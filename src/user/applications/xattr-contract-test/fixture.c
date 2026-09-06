#define _GNU_SOURCE
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/xattr.h>

int64_t xa_now(void) {
  struct timespec time;
  return clock_gettime(CLOCK_MONOTONIC, &time) ? -1
                                               : (int64_t)time.tv_sec * 1000000000 + time.tv_nsec;
}

int xa_reap(pid_t child, int milliseconds) {
  int64_t now = xa_now(), deadline = now + (int64_t)milliseconds * 1000000;
  while (now >= 0 && (now = xa_now()) >= 0 && now < deadline) {
    int status;
    pid_t result = waitpid(child, &status, WNOHANG);
    if (result == child)
      return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    if (result < 0 && errno != EINTR)
      return -1;
    struct timespec pause = {0, 5000000};
    nanosleep(&pause, NULL);
  }
  kill(child, SIGKILL);
  while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {
  }
  return -1;
}

int xa_write_all(int fd, const void* buffer, size_t length) {
  const unsigned char* bytes = buffer;
  while (length) {
    ssize_t result = write(fd, bytes, length);
    if (result < 0 && errno == EINTR)
      continue;
    if (result <= 0)
      return -1;
    bytes += result;
    length -= result;
  }
  return 0;
}

int xa_read_all(int fd, void* buffer, size_t length) {
  unsigned char* bytes = buffer;
  while (length) {
    ssize_t result = read(fd, bytes, length);
    if (result < 0 && errno == EINTR)
      continue;
    if (result <= 0)
      return -1;
    bytes += result;
    length -= result;
  }
  return 0;
}

int xa_send(int fd, char byte) {
  return xa_write_all(fd, &byte, 1);
}

int xa_receive(int fd, char expected) {
  int64_t now = xa_now(), deadline = now + 5000000000;
  while (now >= 0 && (now = xa_now()) >= 0 && now < deadline) {
    struct pollfd watch = {.fd = fd, .events = POLLIN};
    int result = poll(&watch, 1, 100);
    if (result < 0 && errno == EINTR)
      continue;
    if (result < 0)
      return -1;
    if (!result)
      continue;
    char byte;
    if (!xa_read_all(fd, &byte, 1) && byte == expected)
      return 0;
    errno = EIO;
    return -1;
  }
  errno = ETIMEDOUT;
  return -1;
}

int xa_create(struct xa_file* file, int backend, int directory) {
  static unsigned sequence;
  memset(file, 0, sizeof(*file));
  file->fd = -1;
  file->backend = backend;
  file->directory = directory;
  if (backend == XA_MEMFD) {
    if (directory) {
      errno = EINVAL;
      return -1;
    }
    file->fd = memfd_create("xattr-contract", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  } else {
    snprintf(file->path, sizeof(file->path), "%s/xattr-%ld-%u", backend == XA_RAMFS ? "/tmp" : "",
             (long)getpid(), ++sequence);
    if (directory && mkdir(file->path, 0700)) {
      file->path[0] = 0;
      return -1;
    }
    file->fd =
        open(file->path,
             directory ? O_RDONLY | O_DIRECTORY | O_CLOEXEC : O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC,
             0600);
    if (file->fd < 0) {
      int saved = errno;
      if (directory)
        rmdir(file->path);
      file->path[0] = 0;
      errno = saved;
    }
  }
  if (file->fd < 0)
    fprintf(stderr, "XATTR-CONTRACT: create backend=%d directory=%d errno=%d\n", backend, directory,
            errno);
  return file->fd < 0 ? -1 : 0;
}

int xa_open_alias(const struct xa_file* file) {
  if (file->directory)
    return open(file->path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (file->backend == XA_EXT2) {
    char alias[224];
    snprintf(alias, sizeof(alias), "%s.alias", file->path);
    if (link(file->path, alias))
      return -1;
    int fd = open(alias, O_RDWR | O_CLOEXEC), saved = errno;
    if (unlink(alias)) {
      saved = errno;
      if (fd >= 0)
        close(fd);
      fd = -1;
    }
    errno = saved;
    return fd;
  }
  return file->path[0] ? open(file->path, O_RDWR | O_CLOEXEC) : dup(file->fd);
}

void xa_close(struct xa_file* file) {
  if (file->fd >= 0)
    close(file->fd);
  if (file->path[0]) {
    if (file->directory)
      rmdir(file->path);
    else
      unlink(file->path);
  }
  file->fd = -1;
  file->path[0] = 0;
}

int xa_set(const struct xa_file* file, int how, const char* name, const void* value, size_t size,
           int flags) {
  if (how == XA_PATH)
    return setxattr(file->path, name, value, size, flags);
  if (how == XA_LINK)
    return lsetxattr(file->path, name, value, size, flags);
  return fsetxattr(file->fd, name, value, size, flags);
}

ssize_t xa_get(const struct xa_file* file, int how, const char* name, void* value, size_t size) {
  if (how == XA_PATH)
    return getxattr(file->path, name, value, size);
  if (how == XA_LINK)
    return lgetxattr(file->path, name, value, size);
  return fgetxattr(file->fd, name, value, size);
}

ssize_t xa_list(const struct xa_file* file, int how, char* names, size_t size) {
  if (how == XA_PATH)
    return listxattr(file->path, names, size);
  if (how == XA_LINK)
    return llistxattr(file->path, names, size);
  return flistxattr(file->fd, names, size);
}

int xa_remove(const struct xa_file* file, int how, const char* name) {
  if (how == XA_PATH)
    return removexattr(file->path, name);
  if (how == XA_LINK)
    return lremovexattr(file->path, name);
  return fremovexattr(file->fd, name);
}

int xa_value(const struct xa_file* file, int how, const char* name, const void* value,
             size_t size) {
  unsigned char* actual = malloc(size + 1);
  if (!actual)
    return -1;
  memset(actual, 0xa5, size + 1);
  int failed = xa_get(file, how, name, NULL, 0) != (ssize_t)size ||
               xa_get(file, how, name, actual, size + 1) != (ssize_t)size ||
               (size && memcmp(actual, value, size)) || actual[size] != 0xa5;
  if (failed)
    fprintf(stderr, "XATTR-CONTRACT: value %s backend=%d how=%d size=%zu errno=%d\n", name,
            file->backend, how, size, errno);
  free(actual);
  return failed ? -1 : 0;
}

int xa_names(const struct xa_file* file, int how, const char* const* names, size_t count) {
  ssize_t length = xa_list(file, how, NULL, 0);
  if (length < 0 || length > 65536 || count > 128)
    return -1;
  char* bytes = malloc((size_t)length + 1);
  if (!bytes)
    return -1;
  unsigned char seen[128] = {0};
  bytes[length] = (char)0xa5;
  int failed = xa_list(file, how, bytes, length) != length || (unsigned char)bytes[length] != 0xa5;
  size_t found = 0;
  for (size_t offset = 0; !failed && offset < (size_t)length;) {
    char* end = memchr(bytes + offset, 0, (size_t)length - offset);
    if (!end || end == bytes + offset) {
      failed = 1;
      break;
    }
    size_t index = 0;
    while (index < count && strcmp(bytes + offset, names[index]))
      ++index;
    if (index == count || seen[index]) {
      failed = 1;
      break;
    }
    seen[index] = 1;
    ++found;
    offset = (size_t)(end - bytes) + 1;
  }
  failed |= found != count;
  if (failed)
    fprintf(stderr, "XATTR-CONTRACT: list backend=%d how=%d bytes=%zd names=%zu expected=%zu\n",
            file->backend, how, length, found, count);
  free(bytes);
  return failed ? -1 : 0;
}

int xa_unprivileged(uid_t uid, gid_t gid, const gid_t* groups, size_t count) {
  if (setgroups(count, groups) || setgid(gid) || setuid(uid))
    return -1;
  if (geteuid() == uid && getegid() == gid)
    return 0;
  errno = EPERM;
  return -1;
}
