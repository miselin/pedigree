#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/stat.h>
#include <sys/xattr.h>

static int owner_checks(struct xa_file* files, int read_fd) {
  int failed = 0;
  const char* names[] = {"user.keep"};
  CHECK(!xa_unprivileged(60001, 60001, NULL, 0));
  CHECK(fgetxattr(read_fd, "user.keep", NULL, 0) == -1 && errno == EACCES);
  struct xa_file readonly = files[0];
  readonly.fd = read_fd;
  CHECK(!xa_names(&readonly, XA_FD, names, 1));
  CHECK(!fsetxattr(read_fd, "user.keep", "w", 1, XATTR_REPLACE));
  CHECK(!fremovexattr(read_fd, "user.keep"));
  CHECK(!setxattr(files[0].path, "user.path", "p", 1, 0));
  CHECK(!lremovexattr(files[0].path, "user.path"));
  CHECK(!xa_value(&files[1], XA_FD, "user.keep", "k", 1));
  CHECK(fsetxattr(files[1].fd, "user.keep", "x", 1, 0) == -1 && errno == EACCES);
  CHECK(removexattr(files[1].path, "user.keep") == -1 && errno == EACCES);
  CHECK(fgetxattr(files[2].fd, "user.keep", NULL, 0) == -1 && errno == EACCES);
  CHECK(fsetxattr(files[2].fd, "user.keep", "x", 1, 0) == -1 && errno == EACCES);
  CHECK(!xa_names(&files[2], XA_FD, names, 1));
  CHECK(!xa_names(&files[2], XA_PATH, names, 1));
out:
  return failed;
}

static int current_permissions(int backend) {
  int failed = 0, read_fd = -1;
  pid_t child = -1;
  struct xa_file files[3] = {{.fd = -1}, {.fd = -1}, {.fd = -1}};
  const mode_t modes[] = {0200, 0400, 0000};
  for (int n = 0; n < 3; ++n) {
    CHECK(!xa_create(&files[n], backend, 0));
    CHECK(!fsetxattr(files[n].fd, "user.keep", "k", 1, 0));
    if (!n)
      CHECK((read_fd = open(files[n].path, O_RDONLY | O_CLOEXEC)) >= 0);
    CHECK(!fchown(files[n].fd, 60001, 60001));
    CHECK(!fchmod(files[n].fd, modes[n]));
    struct stat status;
    CHECK(!fstat(files[n].fd, &status));
    CHECK(status.st_uid == 60001 && status.st_gid == 60001 && (status.st_mode & 0777) == modes[n]);
  }
  CHECK((child = fork()) >= 0);
  if (!child)
    _exit(owner_checks(files, read_fd));
  int result = xa_reap(child, 8000);
  child = -1;
  CHECK(!result);
  CHECK(fgetxattr(files[0].fd, "user.keep", NULL, 0) == -1 && errno == ENODATA);
  CHECK(!xa_value(&files[1], XA_FD, "user.keep", "k", 1));
out:
  if (child > 0) {
    kill(child, SIGKILL);
    xa_reap(child, 1000);
  }
  if (read_fd >= 0)
    close(read_fd);
  for (int n = 0; n < 3; ++n)
    xa_close(&files[n]);
  return failed;
}

static int group_checks(struct xa_file* file, int member) {
  int failed = 0;
  const gid_t group = 60003;
  CHECK(!xa_unprivileged(60001, 60001, member ? &group : NULL, member ? 1 : 0));
  if (member) {
    CHECK(!xa_value(file, XA_FD, "user.group", "g", 1));
    CHECK(!setxattr(file->path, "user.group", "G", 1, XATTR_REPLACE));
  } else {
    CHECK(fgetxattr(file->fd, "user.group", NULL, 0) == -1 && errno == EACCES);
    CHECK(fsetxattr(file->fd, "user.group", "x", 1, 0) == -1 && errno == EACCES);
    CHECK(fremovexattr(file->fd, "user.group") == -1 && errno == EACCES);
  }
out:
  return failed;
}

static int supplementary_group(int backend) {
  int failed = 0;
  pid_t child = -1;
  struct xa_file file = {.fd = -1};
  CHECK(!xa_create(&file, backend, 0));
  CHECK(!fsetxattr(file.fd, "user.group", "g", 1, 0));
  CHECK(!fchown(file.fd, 60002, 60003));
  CHECK(!fchmod(file.fd, 0660));
  for (int member = 0; member < 2; ++member) {
    CHECK((child = fork()) >= 0);
    if (!child)
      _exit(group_checks(&file, member));
    int result = xa_reap(child, 8000);
    child = -1;
    CHECK(!result);
  }
  CHECK(!xa_value(&file, XA_FD, "user.group", "G", 1));
out:
  if (child > 0) {
    kill(child, SIGKILL);
    xa_reap(child, 1000);
  }
  xa_close(&file);
  return failed;
}

static int directory_child(struct xa_file* sticky, struct xa_file* owned, struct xa_file* hidden) {
  int failed = 0;
  const char* names[] = {"user.directory"};
  CHECK(!xa_unprivileged(60001, 60001, NULL, 0));
  CHECK(!xa_value(sticky, XA_PATH, "user.directory", "d", 1));
  CHECK(!xa_names(sticky, XA_LINK, names, 1));
  CHECK(fsetxattr(sticky->fd, "user.directory", "x", 1, 0) == -1 && errno == EPERM);
  CHECK(lremovexattr(sticky->path, "user.directory") == -1 && errno == EPERM);
  CHECK(!fsetxattr(owned->fd, "user.owner", "o", 1, 0));
  CHECK(!xa_value(owned, XA_PATH, "user.owner", "o", 1));
  CHECK(!lremovexattr(owned->path, "user.owner"));
  CHECK(getxattr(hidden->path, "user.hidden", NULL, 0) == -1 && errno == EACCES);
  CHECK(llistxattr(hidden->path, NULL, 0) == -1 && errno == EACCES);
  CHECK(!xa_value(hidden, XA_FD, "user.hidden", "h", 1));
out:
  return failed;
}

static int directories(int backend) {
  int failed = 0;
  pid_t child = -1;
  struct xa_file sticky = {.fd = -1}, owned = {.fd = -1}, parent = {.fd = -1};
  struct xa_file hidden = {.fd = -1};
  CHECK(!xa_create(&sticky, backend, 1));
  CHECK(!xa_create(&owned, backend, 1));
  CHECK(!xa_create(&parent, backend, 1));
  for (int how = XA_PATH; how <= XA_FD; ++how) {
    CHECK(!xa_set(&sticky, how, "user.directory", "d", 1, XATTR_CREATE));
    CHECK(!xa_value(&sticky, how, "user.directory", "d", 1));
    CHECK(!xa_remove(&sticky, how, "user.directory"));
  }
  CHECK(!fsetxattr(sticky.fd, "user.directory", "d", 1, 0));
  CHECK(!fchmod(sticky.fd, 01777));
  CHECK(!fchown(owned.fd, 60001, 60001));
  CHECK(!fchmod(owned.fd, 01777));
  hidden.backend = backend;
  CHECK(snprintf(hidden.path, sizeof(hidden.path), "%s/child", parent.path) <
        (int)sizeof(hidden.path));
  CHECK((hidden.fd = open(hidden.path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600)) >= 0);
  CHECK(!fchmod(hidden.fd, 0666));
  CHECK(!fsetxattr(hidden.fd, "user.hidden", "h", 1, 0));
  CHECK((child = fork()) >= 0);
  if (!child)
    _exit(directory_child(&sticky, &owned, &hidden));
  int result = xa_reap(child, 8000);
  child = -1;
  CHECK(!result);
out:
  if (child > 0) {
    kill(child, SIGKILL);
    xa_reap(child, 1000);
  }
  xa_close(&hidden);
  xa_close(&parent);
  xa_close(&owned);
  xa_close(&sticky);
  return failed;
}

static int fifo_policy(void) {
  int failed = 0;
  char path[96];
  snprintf(path, sizeof(path), "/tmp/xattr-fifo-%ld", (long)getpid());
  CHECK(!mkfifo(path, 0600));
  CHECK(getxattr(path, "user.fifo", NULL, 0) == -1 && errno == ENODATA);
  CHECK(setxattr(path, "user.fifo", "f", 1, 0) == -1 && errno == EPERM);
  CHECK(lremovexattr(path, "user.fifo") == -1 && errno == EPERM);
out:
  unlink(path);
  return failed;
}

int xa_permissions(void) {
  for (int backend = XA_RAMFS; backend <= XA_EXT2; ++backend)
    if (current_permissions(backend) || supplementary_group(backend) || directories(backend))
      return 1;
  return fifo_policy();
}
