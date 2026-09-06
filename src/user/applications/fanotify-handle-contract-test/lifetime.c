#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>

static int descriptor_surface(void) {
  int failed = 0, group = -1, duplicate = -1, enabled;
  struct stat state, copy;
  char path[96], target[96];
  group = fh_group(1);
  CHECK(group >= 0);
  duplicate = dup(group);
  CHECK(duplicate >= 0 && !fstat(group, &state) && !fstat(duplicate, &copy));
  CHECK(state.st_ino && state.st_ino == copy.st_ino && state.st_nlink == 1 && state.st_size == 0);
  CHECK((state.st_mode & 0777) == 0600);
  CHECK((fcntl(group, F_GETFL) & (O_ACCMODE | O_NONBLOCK)) == (O_RDWR | O_NONBLOCK));
  CHECK((fcntl(group, F_GETFD) & FD_CLOEXEC) && !(fcntl(duplicate, F_GETFD) & FD_CLOEXEC));
  enabled = 0;
  CHECK(!ioctl(duplicate, FIONBIO, &enabled) && !(fcntl(group, F_GETFL) & O_NONBLOCK));
  enabled = 1;
  CHECK(!ioctl(group, FIONBIO, &enabled) && (fcntl(duplicate, F_GETFL) & O_NONBLOCK));
  CHECK(!ioctl(duplicate, FIOCLEX) && (fcntl(duplicate, F_GETFD) & FD_CLOEXEC));
  CHECK(!ioctl(duplicate, FIONCLEX) && !(fcntl(duplicate, F_GETFD) & FD_CLOEXEC));
  CHECK(lseek(group, 17, SEEK_SET) == 0 && lseek(group, -2, SEEK_CUR) == 0);
  errno = 0;
  CHECK(lseek(group, 0, 123) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(write(group, "x", 1) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(ioctl(group, 0x7fff, NULL) == -1 && errno == ENOTTY);
  snprintf(path, sizeof(path), "/proc/self/fd/%d", group);
  ssize_t length = readlink(path, target, sizeof(target));
  CHECK(length == (ssize_t)strlen("anon_inode:[fanotify]"));
  CHECK(!memcmp(target, "anon_inode:[fanotify]", (size_t)length));
out:
  if (duplicate >= 0)
    close(duplicate);
  if (group >= 0)
    close(group);
  return failed;
}

static int queued_transport(void) {
  int failed = 0, group = -1, duplicate = -1, received = -1, sockets[2] = {-1, -1};
  struct fh_file file = {.fd = -1};
  CHECK(!fh_create(&file));
  group = fh_group(1);
  CHECK(group >= 0 && !fh_mark(group, &file, FAN_MARK_ADD, FAN_MODIFY));
  duplicate = dup(group);
  CHECK(duplicate >= 0 && !socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
  CHECK(!fh_send_fd(sockets[0], duplicate));
  CHECK(!close(group));
  group = -1;
  CHECK(!close(duplicate));
  duplicate = -1;
  CHECK(!fh_modify(&file));
  received = fh_receive_fd(sockets[1]);
  CHECK(received >= 0 && (fcntl(received, F_GETFD) & FD_CLOEXEC));
  CHECK(!fh_event(received, &file, FAN_MODIFY, getpid()));
  CHECK(!fh_modify(&file));
  CHECK(!unlink(file.path));
  file.path[0] = 0;
  CHECK(!fh_event(received, &file, FAN_MODIFY, getpid()));
  errno = 0;
  CHECK(open_by_handle_at(file.fd, (struct file_handle*)&file.handle, O_RDONLY) == -1 &&
        errno == ESTALE);
  CHECK(!fh_modify(&file) && fh_readable(received, 0) == 0);
out:
  if (received >= 0)
    close(received);
  if (duplicate >= 0)
    close(duplicate);
  if (group >= 0)
    close(group);
  for (int n = 0; n < 2; ++n)
    if (sockets[n] >= 0)
      close(sockets[n]);
  fh_close(&file);
  return failed;
}

int fh_exec(int argc, char** argv) {
  if (argc != 5)
    return 2;
  alarm(10);
  int kept = atoi(argv[2]), dropped = atoi(argv[3]);
  if (fcntl(kept, F_GETFD) < 0)
    return 10;
  errno = 0;
  if (fcntl(dropped, F_GETFD) != -1 || errno != EBADF)
    return 11;
  struct fh_file file = {.fd = -1};
  file.fd = open(argv[4], O_RDWR | O_CLOEXEC);
  if (file.fd < 0 || fh_export(file.fd, &file.handle, &file.mount_id))
    return 12;
  int result = fh_event(kept, &file, FAN_MODIFY, getppid()) || fh_modify(&file) ||
               fh_event(kept, &file, FAN_MODIFY, getpid()) ||
               fh_mark(kept, &file, FAN_MARK_ADD, FAN_ATTRIB);
  close(file.fd);
  return result ? 13 : 0;
}

static int inherited_exec(void) {
  int failed = 0, group = -1, kept = -1;
  pid_t child = -1;
  struct fh_file file = {.fd = -1};
  CHECK(!fh_create(&file));
  group = fh_group(1);
  CHECK(group >= 0 && !fh_mark(group, &file, FAN_MARK_ADD, FAN_MODIFY));
  kept = dup(group);
  CHECK(kept >= 0 && !fh_modify(&file));
  child = fork();
  CHECK(child >= 0);
  if (!child) {
    char keep_text[32], drop_text[32];
    snprintf(keep_text, sizeof(keep_text), "%d", kept);
    snprintf(drop_text, sizeof(drop_text), "%d", group);
    execl(FH_APP, FH_APP, "fanotify-exec", keep_text, drop_text, file.path, (char*)NULL);
    _exit(20);
  }
  CHECK(!fh_reap(child, 12000));
  child = -1;
  CHECK(fh_readable(group, 0) == 0);
  CHECK(!fchmod(file.fd, 0644));
  CHECK(!fh_event(group, &file, FAN_ATTRIB, getpid()));
  CHECK(!close(kept));
  kept = -1;
  CHECK(!fh_modify(&file) && !fh_event(group, &file, FAN_MODIFY, getpid()));
out:
  if (child > 0)
    fh_reap(child, 100);
  if (kept >= 0)
    close(kept);
  if (group >= 0)
    close(group);
  fh_close(&file);
  return failed;
}
int fh_lifetime(void) {
  return descriptor_surface() || queued_transport() || inherited_exec();
}
