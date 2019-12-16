#include "kbdysch.h"

#include "internal-defs.h"
#include "invoker-utils.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/sysmacros.h>
#include <syscall.h>

#ifdef USE_LKL

// Unfortunately, printk does not have `state` argument...
static int patching_was_performed = 0;

static void kernel_dump_all_pertitions_if_requested(struct fuzzer_state *state)
{
  if (!get_bool_knob("DUMP", 0))
    return;

  for (int part = 0; part < state->constant_state.part_count; ++part) {
    char dump_file_name[128];
    snprintf(dump_file_name, sizeof(dump_file_name), "dump_%s.img",
             state->partitions[part].fstype);

    fprintf(stderr, "Dumping current %s state to %s... ",
            state->partitions[part].fstype, dump_file_name);

    int dump_fd = open(dump_file_name, O_CREAT | O_WRONLY, S_IRUSR);
    CHECK_THAT(dump_fd >= 0);
    size_t dump_size = state->partitions[part].size;
    CHECK_THAT(write(dump_fd, state->partitions[part].data, dump_size) == dump_size);
    close(dump_fd);
    fprintf(stderr, "OK\n");
  }
}

static void set_part_to_disk(struct lkl_disk *disk, partition_t *partition)
{
  disk->handle = partition;
}

static partition_t *part_from_disk(struct lkl_disk disk)
{
  return (partition_t *)disk.handle;
}

static int mem_get_capacity(struct lkl_disk disk, unsigned long long *res) {
  *res = part_from_disk(disk)->size;
  return LKL_DEV_BLK_STATUS_OK;
}

static int mem_request(struct lkl_disk disk, struct lkl_blk_req *req)
{
  int reading_requested;
  switch(req->type) {
    case LKL_DEV_BLK_TYPE_READ:
      reading_requested = 1;
      break;
    case LKL_DEV_BLK_TYPE_WRITE:
      reading_requested = 0;
      break;
    case LKL_DEV_BLK_TYPE_FLUSH:
    case LKL_DEV_BLK_TYPE_FLUSH_OUT:
      // no-op
      return LKL_DEV_BLK_STATUS_OK;
    default:
      return LKL_DEV_BLK_STATUS_UNSUP;
  }

  partition_t * const partition = part_from_disk(disk);
  size_t offset = req->sector * 512;

  if (offset >= partition->size)
      return LKL_DEV_BLK_STATUS_IOERR;

  for (int i = 0; i < req->count; ++i)
  {
    size_t len = req->buf[i].iov_len;
    if (offset + len > partition->size)
      return LKL_DEV_BLK_STATUS_IOERR;

    partition->off_start[partition->off_cur] = offset;
    partition->off_end  [partition->off_cur] = offset + len;
    partition->off_cur++;
    if (partition->off_cur >= ACCESS_HISTORY_LEN)
      partition->off_cur = 0;
    partition->access_count++;

    if (reading_requested)
      memcpy(req->buf[i].iov_base, partition->data + offset, len);
    else
      memcpy(partition->data + offset, req->buf[i].iov_base, len);
    offset += len;
  }
  return LKL_DEV_BLK_STATUS_OK;
}

void kernel_setup_disk(struct fuzzer_state *state, const char *filename, const char *fstype)
{
  partition_t *partition = &state->partitions[state->constant_state.part_count];

  set_part_to_disk(&partition->disk, partition);

  int image_fd = open(filename, O_RDONLY);
  if(image_fd == -1) {
      fprintf(stderr, "Cannot open '%s': %s\n", filename, strerror(errno));
      abort();
  }
  off_t size = lseek(image_fd, 0, SEEK_END);
  if (size == (off_t)-1) {
    perror("setup_disk: cannot get disk size");
    abort();
  }

  partition->size = size;
  strncpy(partition->fstype, fstype, sizeof(partition->fstype));

  // First, try with HugeTLB enabled to speed up forkserver
  fprintf(stderr, "Loading %s with HugeTLB... ", filename);
  partition->data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
  if (partition->data != MAP_FAILED) {
    fprintf(stderr, "OK\n");
    CHECK_THAT(pread(image_fd, partition->data, size, 0) == size);
  } else {
    fprintf(stderr, "FAILED (%s)\n", strerror(errno));
    fprintf(stderr, "Loading %s without HugeTLB... ", filename);
    partition->data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE, image_fd, 0);
    if (partition->data == MAP_FAILED) {
      fprintf(stderr, "FAILED (%s)", strerror(errno));
      abort();
    } else {
      fprintf(stderr, "OK\n");
    }
  }
  close(image_fd);

  state->constant_state.part_count += 1;
}


static void add_all_disks(struct fuzzer_state *state)
{
  static struct lkl_dev_blk_ops blk_ops;
  blk_ops.get_capacity = mem_get_capacity;
  blk_ops.request = mem_request;

  for (int i = 0; i < state->constant_state.part_count; ++i)
  {
    state->partitions[i].disk.ops = &blk_ops;
    int disk_id = lkl_disk_add(&state->partitions[i].disk);
    CHECK_THAT(disk_id >= 0);
    state->partitions[i].disk_id = disk_id;
  }
}

static void unmount_all(struct fuzzer_state *state) {
  if (state->constant_state.diskless)
    return;

  CHECK_THAT(state->constant_state.native_mode == 0);
  fprintf(stderr, "Unmounting all...\n");

  res_close_all_fds(state);

  // perform umount()
  for (int part = 0; part < state->constant_state.part_count; ++part) {
    int ret = lkl_umount_dev(state->partitions[part].disk_id, 0, 0, 1);
    if (ret) {
      // TODO
      fprintf(stderr, "WARN: cannot unmount #%d, type = %s (%s), just exiting...\n",
              part, state->partitions[part].fstype, lkl_strerror(ret));
      exit(1);
    }
  }
}

static void mount_all(struct fuzzer_state *state)
{
  if (state->constant_state.diskless)
    return;

  CHECK_THAT (state->constant_state.native_mode == 0);

  kernel_dump_all_pertitions_if_requested(state);

  fprintf(stderr, "Mounting all...\n");

  for (int part = 0; part < state->constant_state.part_count; ++part)
  {
    int mount_flags = get_bool_knob("READ_ONLY", 0) ? MS_RDONLY : MS_MGC_VAL;
    const char *mount_options;

    if (strcmp(state->partitions[part].fstype, "ext4") == 0) {
      mount_options = "errors=remount-ro";
    } else {
      mount_options = get_string_knob("MOUNT_OPTIONS", NULL);
    }

    int ret = lkl_mount_dev(
      state->partitions[part].disk_id,
      0,
      state->partitions[part].fstype,
      mount_flags,
      mount_options,
      state->partitions[part].mount_point,
      MOUNT_POINT_LEN);

    if (ret) {
      fprintf(stderr, "Cannot mount partition #%d, type = %s: %s\n",
              part, state->partitions[part].fstype, lkl_strerror(ret));

      if (state->mutable_state.patch_was_invoked) {
        fprintf(stderr, "Exiting cleanly because PATCH was invoked previously.\n");
      } else {
        abort();
      }
    }

    state->partitions[part].registered_fds[0] = -1; // -1 is an "invalid FD" for any partition
    state->partitions[part].registered_fds_count = 1;

    fprintf(stderr, "Successfully mounted partition #%d, type = %s\n",
            part, state->partitions[part].fstype);
  }
}

static void patch_one(struct fuzzer_state *state, partition_t *partition)
{
  const unsigned param1 = res_get_u16(state) % partition->access_count;
  const int access_nr = ((partition->off_cur - (int)param1) % ACCESS_HISTORY_LEN + ACCESS_HISTORY_LEN) % ACCESS_HISTORY_LEN;

  const unsigned char op = res_get_u8(state);
  const int32_t arg = res_get_u32(state);
  const int patch_size = (op & 0x70) >> 4;
  const int patch_local_range = partition->off_end[access_nr] - partition->off_start[access_nr] - patch_size + 1;

  if (patch_local_range <= 0)
    return;

  const int32_t param2 = res_get_u32(state);
  const int patch_local_offset = (param2 > 0) ? (param2 % patch_local_range) : (param2 % patch_local_range + patch_local_range);
  void *patch_destination = partition->data + partition->off_start[access_nr] + patch_local_offset;

  uint64_t patched_data;
  memcpy(&patched_data, patch_destination, patch_size);

  switch(op & 0x07) {
  case 0:
  case 1:
    patched_data += (int64_t)arg;
    break;
  case 2:
  case 3:
    patched_data = (int64_t)arg;
    break;
  case 4:
    patched_data &= arg;
    break;
  case 5:
    patched_data |= arg;
    break;
  case 6:
  case 7:
    patched_data ^= arg;
    break;
  }
  memcpy(patch_destination, &patched_data, patch_size);
}

static void my_printk(const char *msg, int len)
{
  static char print_buf[65536];
  int do_print = !get_bool_knob("NO_PRINTK", 0);
  uint64_t nbw = get_int_knob("NO_BAD_WORDS", 0);

  if (do_print) {
    fwrite(msg, len, 1, stderr);
  }

  if (!patching_was_performed && nbw != (uint64_t)-1LL) {
    memcpy(print_buf, msg, len);
    print_buf[len] = 0;
    if (strcasestr(print_buf, "errors=remount-ro")) {
      return;
    }
    for (int i = 0; i < sizeof(BAD_WORDS) / sizeof(BAD_WORDS[0]); ++i) {
      if ((nbw & (1 << i)) == 0 && strcasestr(print_buf, BAD_WORDS[i])) {
        abort();
      }
    }
  }
}

// path is relative to current mount point (cwd for the time of scanning)!
static char file_scanner_tmp_buf[MAX_FILE_NAME_LEN];
// stack of visited inodes, to handle hard linked directory loops
static ino64_t inode_stack[128];
static int inode_stack_size;

static void recurse_into_directory(struct fuzzer_state *state, int part, struct lkl_dir *dir)
{
  struct lkl_linux_dirent64 *dirent;
  size_t old_len = strlen(file_scanner_tmp_buf); // might be optimized, but not on hot path anyway
  while ((dirent = lkl_readdir(dir)) != NULL) {
    // is this "." or ".."?
    if (strcmp(dirent->d_name, ".") == 0 || strcmp(dirent->d_name, "..") == 0) {
      continue;
    }

    // are we on a directory loop?
    int dir_loop = 0;
    for (int i = 0; i < inode_stack_size; ++i) {
      if (inode_stack[i] == dirent->d_ino) {
        dir_loop = 1;
        break;
      }
    }
    if (dir_loop) {
      continue;
    }

    // temporarily adding new path component
    snprintf(file_scanner_tmp_buf + old_len, sizeof(file_scanner_tmp_buf) - old_len, "/%s", dirent->d_name);
    state->mutable_state.file_names[state->mutable_state.file_name_count++] = strdup(file_scanner_tmp_buf);
    inode_stack[inode_stack_size++] = dirent->d_ino;

    // is this a directory itself?
    struct lkl_stat statbuf;
    CHECK_INVOKER_ERRNO(state, lkl_sys_stat(file_scanner_tmp_buf, &statbuf));
    if (S_ISDIR(statbuf.st_mode)) {
      int err;
      struct lkl_dir *dir_to_recurse = lkl_opendir(file_scanner_tmp_buf, &err);
      CHECK_THAT(dir_to_recurse != NULL);
      recurse_into_directory(state, part, dir_to_recurse);
      CHECK_INVOKER_ERRNO(state, lkl_closedir(dir_to_recurse));
    }

    // dropping path component
    file_scanner_tmp_buf[old_len] = '\0';
    inode_stack_size--;
  }
}

#else // USE_LKL

void kernel_setup_disk(struct fuzzer_state *state, const char *filename, const char *fstype)
{
  warn_lkl_not_supported();
}

#endif // USE_LKL

void kernel_setup_dummy_disk(struct fuzzer_state *state)
{
  if (!state->constant_state.native_mode) {
    fprintf(stderr, "Refused to setup dummy disk in the current directory in LKL mode!\n");
    abort();
  }

  state->constant_state.diskless = 0;
  state->constant_state.part_count = 1;
  strcpy(state->partitions[0].fstype, "<dummy>");
  strcpy(state->partitions[0].mount_point, "./");
  state->partitions[0].registered_fds[0] = -1; // invalid FD
  state->partitions[0].registered_fds_count = 1;
}

void kernel_configure_diskless(struct fuzzer_state *state)
{
  state->constant_state.diskless = 1;
  state->constant_state.part_count = 1;
  partition_t *root_pseudo_partition = &state->partitions[0];

  root_pseudo_partition->size = 0;
  root_pseudo_partition->data = NULL;
  strncpy(root_pseudo_partition->fstype,  "<root pseudo partition>", sizeof(root_pseudo_partition->fstype));
}

void kernel_perform_remount(struct fuzzer_state *state)
{
  if (state->constant_state.native_mode) {
    fprintf(stderr, "REMOUNT requested in native mode, exiting.\n");
    exit(1);
  }
#ifdef USE_LKL
  unmount_all(state);
  mount_all(state);
#endif
}

void kernel_perform_patching(struct fuzzer_state *state)
{
  if (state->constant_state.native_mode) {
    fprintf(stderr, "PATCH requested in native mode, exiting.\n");
    exit(1);
  }
  if (state->constant_state.part_count > 1) {
    fprintf(stderr, "PATCH requested in comparison mode, exiting.\n");
    exit(1);
  }
  if (get_bool_knob("NO_PATCH", 0)) {
    fprintf(stderr, "PATCH requested, but is explicitly disabled, exiting.\n");
    exit(1);
  }

#ifdef USE_LKL
  // Now, we have exactly one real partition
  partition_t *partition = &state->partitions[0];

  if (partition->access_count == 0)
    return;

  const int count = res_get_u8(state) % 32;
  fprintf(stderr, "PATCH count requested = %d\n", count);
  unmount_all(state);
  state->mutable_state.patch_was_invoked = 1;
  patching_was_performed = 1;
  for(int i = 0; i < count; ++i) {
    patch_one(state, partition);
  }
  mount_all(state);
#endif
}

void kernel_boot(struct fuzzer_state *state, const char *cmdline)
{
  if (state->constant_state.native_mode) {
    fprintf(stderr, "Refusing to boot LKL in native mode!\n");
    abort();
  }
#ifdef USE_LKL
  add_all_disks(state);
  lkl_host_ops.print = my_printk;
  lkl_host_ops.panic = abort;
  lkl_start_kernel(&lkl_host_ops, cmdline);
  lkl_mount_fs("sysfs");
  mount_all(state);
#endif
}

void kernel_write_to_file(struct fuzzer_state *state, const char *filename, const char *data)
{
  fprintf(stderr, "Writing [%s] to %s... ", data, filename);
  int len = strlen(data);
  int fd = INVOKE_SYSCALL(state, openat, AT_FDCWD, (long)filename, O_WRONLY, 0);
  CHECK_THAT(fd >= 0);
  CHECK_THAT(INVOKE_SYSCALL(state, write, fd, (long)data, len) == len);
  INVOKE_SYSCALL(state, close, fd);
  fprintf(stderr, "OK\n");
}

int kernel_open_char_dev_by_sysfs_name(struct fuzzer_state *state, const char *name, const char *sysfs_id)
{
  char sysfs_name[128];
  char dev_name[128];
  if (is_native_invoker(state)) {
    sprintf(sysfs_name, "/sysfs/class/%s/dev", sysfs_id);
    sprintf(dev_name, "/tmp/%s", name);
  } else {
    sprintf(sysfs_name, "/sys/class/%s/dev", sysfs_id);
    sprintf(dev_name, "/%s", name);
  }

  char str_dev_id[32];
  fprintf(stderr, "Opening the device %s as %s...\n", sysfs_name, dev_name);

  // read device ID as string
  int sysfs_file_fd = INVOKE_SYSCALL(state, openat, AT_FDCWD, (long)sysfs_name, O_RDONLY);
  CHECK_THAT(sysfs_file_fd >= 0);
  int sysfs_read_size = INVOKE_SYSCALL(state, read, sysfs_file_fd, (long)str_dev_id, sizeof(str_dev_id) - 1);
  CHECK_THAT(sysfs_read_size >= 0);
  str_dev_id[sysfs_read_size] = 0;
  fprintf(stderr, "  sysfs returned: %s\n", str_dev_id);

  // parse string ID
  const char *semicolon = strchr(str_dev_id, ':');
  CHECK_THAT(semicolon != NULL);
  int major = atoi(str_dev_id);
  int minor = atoi(str_dev_id + 1);
  fprintf(stderr, "  parsed as: major = %d, minor = %d\n", major, minor);

  // crete device file
  dev_t dev = makedev(major, minor);
  int mknod_result = INVOKE_SYSCALL(state, mknodat, AT_FDCWD, (long)dev_name, S_IFCHR | S_IRUSR | S_IWUSR, dev);
  CHECK_INVOKER_ERRNO(state, mknod_result);
  fprintf(stderr, "  created device file: %s\n", dev_name);

  // open the just created device
  int fd = INVOKE_SYSCALL(state, openat, AT_FDCWD, (long)dev_name, O_RDWR);
  CHECK_THAT(fd >= 0);
  fprintf(stderr, "  opened as fd = %d\n", fd);
  fprintf(stderr, "  DONE\n");

  return fd;
}


int kernel_scan_for_files(struct fuzzer_state *state, int part)
{
  if (state->constant_state.native_mode) {
    fprintf(stderr, "Scanning files in a native mode makes little sense and anyway seems like a bad idea, exiting.\n");
    exit(1);
  }

  const int old_file_count = state->mutable_state.file_name_count;

#ifdef USE_LKL

  CHECK_INVOKER_ERRNO(state, lkl_sys_chdir(state->partitions[part].mount_point));
  // now assuming we are in LKL mode
  int err;
  struct lkl_dir *fs_root_dir = lkl_opendir(".", &err);
  CHECK_THAT(fs_root_dir != NULL);
  file_scanner_tmp_buf[0] = '.';
  file_scanner_tmp_buf[1] = '\0';
  recurse_into_directory(state, part, fs_root_dir);
  CHECK_INVOKER_ERRNO(state, lkl_closedir(fs_root_dir));
  CHECK_INVOKER_ERRNO(state, lkl_sys_chdir("/"));

#endif

  return state->mutable_state.file_name_count - old_file_count;
}

void kernel_dump_file_names(struct fuzzer_state *state)
{
  for (int i = 0; i < state->mutable_state.file_name_count; ++i) {
    fprintf(stderr, "  %s\n", state->mutable_state.file_names[i]);
  }
}
