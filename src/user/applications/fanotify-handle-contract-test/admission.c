#define _GNU_SOURCE
#include <grp.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>
#include <sys/stat.h>

static int flags_and_paths(void) {
  int failed = 0, group = -1, accepted = -1, memory = -1, ram = -1;
  struct fh_file file = {.fd = -1};
  struct fh_handle handle;
  char symlink_path[224] = {0}, ram_path[192] = {0};
  int mount_id;
  CHECK(!fh_create(&file));
  errno = 0;
  CHECK(fanotify_init(FAN_REPORT_FID | 0x80000000U, O_RDONLY) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(fanotify_init(FAN_REPORT_FID | FAN_CLASS_CONTENT | FAN_CLASS_PRE_CONTENT, O_RDONLY) == -1 &&
        errno == EINVAL);
  errno = 0;
  CHECK(fanotify_init(0, O_RDONLY) == -1 && errno == EOPNOTSUPP);
  errno = 0;
  CHECK(fanotify_init(FAN_REPORT_FID | FAN_CLASS_CONTENT, O_RDONLY) == -1 && errno == EOPNOTSUPP);
  errno = 0;
  CHECK(fanotify_init(FAN_REPORT_FID, O_ACCMODE) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(fanotify_init(FAN_REPORT_FID, 3) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(fanotify_init(FAN_REPORT_FID, O_PATH) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(fanotify_init(FAN_REPORT_FID, O_CREAT) == -1 && errno == EINVAL);
  accepted = fanotify_init(FAN_REPORT_FID | FAN_NONBLOCK, O_RDWR | O_APPEND | O_NONBLOCK | O_SYNC |
                                                              O_DSYNC | O_CLOEXEC | O_LARGEFILE |
                                                              O_NOATIME);
  CHECK(accepted >= 0);
  close(accepted);
  accepted = -1;
  group = fh_group(1);
  CHECK(group >= 0);
  errno = 0;
  CHECK(fh_mark(group, &file, FAN_MARK_ADD | FAN_MARK_REMOVE, FAN_MODIFY) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(fh_mark(group, &file, FAN_MARK_ADD, FAN_OPEN_PERM) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(fh_mark(group, &file, FAN_MARK_ADD, 1ULL << 63) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(fh_mark(group, &file, FAN_MARK_ADD | FAN_MARK_MOUNT, FAN_MODIFY) == -1 &&
        errno == EOPNOTSUPP);
  errno = 0;
  CHECK(fh_mark(group, &file, FAN_MARK_ADD | FAN_MARK_ONLYDIR, FAN_MODIFY) == -1 &&
        errno == ENOTDIR);
  errno = 0;
  CHECK(fanotify_mark(group, FAN_MARK_ADD, FAN_MODIFY, file.fd, "") == -1 && errno == ENOENT);
  CHECK(!fh_mark(group, &file, FAN_MARK_ADD, FAN_MODIFY));
  errno = 0;
  CHECK(fanotify_mark(group, FAN_MARK_FLUSH | FAN_MARK_DONT_FOLLOW, 0, -1, NULL) == -1 &&
        errno == EINVAL);
  errno = 0;
  CHECK(fanotify_mark(group, FAN_MARK_FLUSH | FAN_MARK_ONLYDIR, 0, -1, NULL) == -1 &&
        errno == EINVAL);
  CHECK(!fh_mark(group, &file, FAN_MARK_REMOVE, FAN_MODIFY));
  CHECK(!fh_mark(group, &file, FAN_MARK_ADD, FAN_MODIFY));
  errno = 0;
  CHECK(fanotify_mark(group, FAN_MARK_ADD, FAN_MODIFY, AT_FDCWD, NULL) == -1 && errno == EBADF);
  CHECK(!fanotify_mark(group, FAN_MARK_FLUSH, UINT64_MAX, -1, (const char*)1));
  snprintf(symlink_path, sizeof(symlink_path), "%s.link", file.path);
  CHECK(!symlink(file.path, symlink_path));
  handle.handle_bytes = sizeof(handle.bytes);
  errno = 0;
  CHECK(name_to_handle_at(AT_FDCWD, symlink_path, (struct file_handle*)&handle, &mount_id, 0) ==
            -1 &&
        errno == EOPNOTSUPP);
  handle.handle_bytes = sizeof(handle.bytes);
  CHECK(!name_to_handle_at(AT_FDCWD, symlink_path, (struct file_handle*)&handle, &mount_id,
                           AT_SYMLINK_FOLLOW));
  CHECK(fh_equal(&handle, &file.handle));
  errno = 0;
  CHECK(fanotify_mark(group, FAN_MARK_ADD | FAN_MARK_DONT_FOLLOW, FAN_MODIFY, AT_FDCWD,
                      symlink_path) == -1 &&
        errno == EOPNOTSUPP);
  CHECK(!fanotify_mark(group, FAN_MARK_ADD, FAN_MODIFY, AT_FDCWD, symlink_path));
  memory = memfd_create("fanotify-unsupported", MFD_CLOEXEC);
  CHECK(memory >= 0);
  errno = 0;
  CHECK(fh_export(memory, &handle, &mount_id) == -1 && errno == EOPNOTSUPP);
  errno = 0;
  CHECK(fanotify_mark(group, FAN_MARK_ADD, FAN_MODIFY, memory, NULL) == -1 && errno == EOPNOTSUPP);
  snprintf(ram_path, sizeof(ram_path), "/tmp/fanotify-unsupported-%ld", (long)getpid());
  ram = open(ram_path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  CHECK(ram >= 0);
  errno = 0;
  CHECK(fh_export(ram, &handle, &mount_id) == -1 && errno == EOPNOTSUPP);
  errno = 0;
  CHECK(open_by_handle_at(file.fd, (struct file_handle*)&file.handle, O_PATH) == -1 &&
        errno == EOPNOTSUPP);
  errno = 0;
  CHECK(open_by_handle_at(-1, (struct file_handle*)&file.handle, O_RDONLY) == -1 && errno == EBADF);
out:
  if (ram >= 0)
    close(ram);
  if (ram_path[0])
    unlink(ram_path);
  if (memory >= 0)
    close(memory);
  if (symlink_path[0])
    unlink(symlink_path);
  if (accepted >= 0)
    close(accepted);
  if (group >= 0)
    close(group);
  fh_close(&file);
  return failed;
}

static int copies(void) {
  int failed = 0, mount_id;
  struct fh_file file = {.fd = -1};
  struct fh_handle handle, saved;
  void* page = MAP_FAILED;
  CHECK(!fh_create(&file));
  page = mmap(NULL, fh_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(page != MAP_FAILED);
  handle = file.handle;
  handle.handle_bytes = 129;
  errno = 0;
  CHECK(name_to_handle_at(file.fd, "", (struct file_handle*)&handle, &mount_id, AT_EMPTY_PATH) ==
            -1 &&
        errno == EINVAL);
  handle = file.handle;
  errno = 0;
  CHECK(name_to_handle_at(file.fd, "", (struct file_handle*)&handle, &mount_id,
                          AT_EMPTY_PATH | 0x40000000) == -1 &&
        errno == EINVAL);
  handle.handle_bytes = 0;
  errno = 0;
  CHECK(open_by_handle_at(file.fd, (struct file_handle*)&handle, O_RDONLY) == -1 &&
        errno == EINVAL);
  CHECK(!mprotect(page, fh_page, PROT_NONE));
  errno = 0;
  CHECK(name_to_handle_at(file.fd, "", page, &mount_id, AT_EMPTY_PATH) == -1 && errno == EFAULT);
  errno = 0;
  CHECK(open_by_handle_at(file.fd, page, O_RDONLY) == -1 && errno == EFAULT);
  memset(&handle, 0x5a, sizeof(handle));
  handle.handle_bytes = sizeof(handle.bytes);
  saved = handle;
  errno = 0;
  CHECK(name_to_handle_at(file.fd, "", (struct file_handle*)&handle, page, AT_EMPTY_PATH) == -1 &&
        errno == EFAULT);
  CHECK(!memcmp(&handle, &saved, sizeof(handle)));
  CHECK(!mprotect(page, fh_page, PROT_READ | PROT_WRITE));
  memcpy(page, &saved, sizeof(saved));
  CHECK(!mprotect(page, fh_page, PROT_READ));
  mount_id = -123;
  errno = 0;
  CHECK(name_to_handle_at(file.fd, "", page, &mount_id, AT_EMPTY_PATH) == -1 && errno == EFAULT);
  CHECK(mount_id == file.mount_id && !memcmp(page, &saved, sizeof(saved)));
out:
  if (page != MAP_FAILED)
    munmap(page, fh_page);
  fh_close(&file);
  return failed;
}

static int permissions(void) {
  int failed = 0, group = -1;
  pid_t child = -1;
  struct fh_file file = {.fd = -1};
  CHECK(!fh_create(&file));
  CHECK(!fchmod(file.fd, 0644));
  group = fh_group(1);
  CHECK(group >= 0);
  child = fork();
  CHECK(child >= 0);
  if (!child) {
    alarm(10);
    struct fh_handle handle;
    int mount_id;
    if (setgroups(0, NULL) || setgid(65534) || setuid(65534))
      _exit(10);
    errno = 0;
    if (fh_group(1) != -1 || errno != EPERM)
      _exit(11);
    handle.handle_bytes = sizeof(handle.bytes);
    if (name_to_handle_at(AT_FDCWD, file.path, (struct file_handle*)&handle, &mount_id, 0) ||
        !fh_equal(&handle, &file.handle))
      _exit(12);
    errno = 0;
    if (open_by_handle_at(file.fd, (struct file_handle*)&handle, O_RDONLY) != -1 || errno != EPERM)
      _exit(13);
    if (fanotify_mark(group, FAN_MARK_ADD, FAN_MODIFY, AT_FDCWD, file.path))
      _exit(14);
    _exit(0);
  }
  CHECK(!fh_reap(child, 12000));
  child = -1;
  CHECK(!fh_mark(group, &file, FAN_MARK_REMOVE, FAN_MODIFY));
  CHECK(!fchmod(file.fd, 0600));
  child = fork();
  CHECK(child >= 0);
  if (!child) {
    alarm(10);
    if (setgroups(0, NULL) || setgid(65534) || setuid(65534))
      _exit(20);
    errno = 0;
    _exit(fh_mark(group, &file, FAN_MARK_ADD, FAN_MODIFY) == -1 && errno == EACCES ? 0 : 21);
  }
  CHECK(!fh_reap(child, 12000));
  child = -1;
out:
  if (child > 0)
    fh_reap(child, 100);
  if (group >= 0)
    close(group);
  fh_close(&file);
  return failed;
}
int fh_admission(void) {
  return flags_and_paths() || copies() || permissions();
}
