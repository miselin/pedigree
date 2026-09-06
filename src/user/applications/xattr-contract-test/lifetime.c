#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/xattr.h>

static int aliases(int backend) {
  int failed = 0, alias = -1, duplicate = -1, reused = -1;
  pid_t child = -1;
  struct xa_file file = {.fd = -1}, replacement = {.fd = -1};
  struct xa_file retained = {.fd = -1};
  char moved[224] = {0};
  CHECK(!xa_create(&file, backend, 0));
  CHECK(!fsetxattr(file.fd, "user.slot", "old", 3, 0));
  CHECK((alias = xa_open_alias(&file)) >= 0);
  CHECK((duplicate = dup(file.fd)) >= 0);
  retained = file;
  retained.fd = duplicate;
  struct stat first, second;
  CHECK(!fstat(file.fd, &first) && !fstat(alias, &second));
  CHECK(first.st_ino == second.st_ino && first.st_dev == second.st_dev);
  CHECK(!fsetxattr(alias, "user.slot", "alias", 5, XATTR_REPLACE));
  CHECK(!xa_value(&retained, XA_FD, "user.slot", "alias", 5));
  CHECK((child = fork()) >= 0);
  if (!child) {
    int result = xa_value(&retained, XA_FD, "user.slot", "alias", 5) ||
                 fsetxattr(duplicate, "user.slot", "child", 5, XATTR_REPLACE);
    _exit(result ? 1 : 0);
  }
  int result = xa_reap(child, 8000);
  child = -1;
  CHECK(!result);
  CHECK(!xa_value(&file, XA_FD, "user.slot", "child", 5));
  if (file.path[0]) {
    snprintf(moved, sizeof(moved), "%s.moved", file.path);
    CHECK(!rename(file.path, moved));
    file.path[0] = 0;
    char value[5];
    CHECK(getxattr(moved, "user.slot", value, sizeof(value)) == 5 && !memcmp(value, "child", 5));
    CHECK(!unlink(moved));
    moved[0] = 0;
  }
  CHECK(!xa_create(&replacement, XA_MEMFD, 0));
  CHECK(!fsetxattr(replacement.fd, "user.slot", "new", 3, 0));
  int number = file.fd;
  CHECK(!close(file.fd));
  file.fd = -1;
  CHECK((reused = dup2(replacement.fd, number)) == number);
  struct xa_file current = replacement;
  current.fd = reused;
  CHECK(!xa_value(&current, XA_FD, "user.slot", "new", 3));
  CHECK(!xa_value(&retained, XA_FD, "user.slot", "child", 5));
  CHECK(!close(alias));
  alias = -1;
  CHECK(!fsetxattr(duplicate, "user.slot", "last", 4, XATTR_REPLACE));
  CHECK(!xa_value(&retained, XA_FD, "user.slot", "last", 4));
out:
  if (child > 0) {
    kill(child, SIGKILL);
    xa_reap(child, 1000);
  }
  if (reused >= 0)
    close(reused);
  if (duplicate >= 0)
    close(duplicate);
  if (alias >= 0)
    close(alias);
  if (moved[0])
    unlink(moved);
  xa_close(&replacement);
  xa_close(&file);
  return failed;
}

static int send_descriptor(int socket, int fd) {
  char byte = 'f';
  struct iovec vector = {&byte, 1};
  union {
    struct cmsghdr aligned;
    unsigned char bytes[CMSG_SPACE(sizeof(int))];
  } control = {0};
  struct msghdr message = {.msg_iov = &vector,
                           .msg_iovlen = 1,
                           .msg_control = control.bytes,
                           .msg_controllen = sizeof(control.bytes)};
  struct cmsghdr* header = CMSG_FIRSTHDR(&message);
  header->cmsg_level = SOL_SOCKET;
  header->cmsg_type = SCM_RIGHTS;
  header->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(header), &fd, sizeof(fd));
  return sendmsg(socket, &message, 0) == 1 ? 0 : -1;
}

static int receive_descriptor(int socket) {
  char byte = 0;
  struct iovec vector = {&byte, 1};
  union {
    struct cmsghdr aligned;
    unsigned char bytes[CMSG_SPACE(sizeof(int))];
  } control = {0};
  struct msghdr message = {.msg_iov = &vector,
                           .msg_iovlen = 1,
                           .msg_control = control.bytes,
                           .msg_controllen = sizeof(control.bytes)};
  if (recvmsg(socket, &message, MSG_CMSG_CLOEXEC) != 1 || byte != 'f' ||
      (message.msg_flags & MSG_CTRUNC))
    return -1;
  struct cmsghdr* header = CMSG_FIRSTHDR(&message);
  if (!header || header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
      header->cmsg_len != CMSG_LEN(sizeof(int)))
    return -1;
  int fd;
  memcpy(&fd, CMSG_DATA(header), sizeof(fd));
  return fd;
}

static int queued_descriptor(int backend) {
  int failed = 0, sockets[2] = {-1, -1}, ready[2] = {-1, -1}, gate[2] = {-1, -1};
  pid_t child = -1;
  struct xa_file file = {.fd = -1};
  CHECK(!xa_create(&file, backend, 0));
  CHECK(!fsetxattr(file.fd, "user.queued", "queued", 6, 0));
  CHECK(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets));
  CHECK(!pipe(ready) && !pipe(gate));
  CHECK((child = fork()) >= 0);
  if (!child) {
    close(sockets[0]);
    close(ready[0]);
    close(gate[1]);
    close(file.fd);
    if (xa_send(ready[1], 'r') || xa_receive(gate[0], 'g'))
      _exit(2);
    file.fd = receive_descriptor(sockets[1]);
    if (file.fd < 0 || xa_value(&file, XA_FD, "user.queued", "queued", 6) ||
        fsetxattr(file.fd, "user.queued", "received", 8, XATTR_REPLACE) ||
        xa_value(&file, XA_FD, "user.queued", "received", 8))
      _exit(3);
    close(file.fd);
    _exit(0);
  }
  close(sockets[1]);
  sockets[1] = -1;
  close(ready[1]);
  ready[1] = -1;
  close(gate[0]);
  gate[0] = -1;
  CHECK(!xa_receive(ready[0], 'r'));
  CHECK(!send_descriptor(sockets[0], file.fd));
  xa_close(&file);
  CHECK(!xa_send(gate[1], 'g'));
  int result = xa_reap(child, 8000);
  child = -1;
  CHECK(!result);
out:
  if (child > 0) {
    kill(child, SIGKILL);
    xa_reap(child, 1000);
  }
  for (int n = 0; n < 2; ++n) {
    if (sockets[n] >= 0)
      close(sockets[n]);
    if (ready[n] >= 0)
      close(ready[n]);
    if (gate[n] >= 0)
      close(gate[n]);
  }
  xa_close(&file);
  return failed;
}

static int rename_replacement(void) {
  int failed = 0;
  struct xa_file source = {.fd = -1}, victim = {.fd = -1};
  CHECK(!xa_create(&source, XA_EXT2, 0));
  CHECK(!xa_create(&victim, XA_EXT2, 0));
  CHECK(!fsetxattr(source.fd, "user.inode", "source", 6, 0));
  CHECK(!fsetxattr(victim.fd, "user.inode", "victim", 6, 0));
  CHECK(!rename(source.path, victim.path));
  source.path[0] = 0;
  CHECK(!xa_value(&victim, XA_PATH, "user.inode", "source", 6));
  CHECK(!xa_value(&victim, XA_FD, "user.inode", "victim", 6));
  CHECK(!fremovexattr(victim.fd, "user.inode"));
  CHECK(!xa_value(&source, XA_FD, "user.inode", "source", 6));
out:
  xa_close(&victim);
  xa_close(&source);
  return failed;
}

static int sealed_memfd(void) {
  int failed = 0;
  struct xa_file file = {.fd = -1};
  const int seals = F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_FUTURE_WRITE;
  CHECK(!xa_create(&file, XA_MEMFD, 0));
  CHECK(write(file.fd, "content", 7) == 7);
  CHECK(!fsetxattr(file.fd, "user.sealed", "before", 6, 0));
  CHECK(!fcntl(file.fd, F_ADD_SEALS, seals));
  CHECK(fcntl(file.fd, F_GET_SEALS) == seals);
  CHECK(pwrite(file.fd, "x", 1, 0) == -1 && errno == EPERM);
  CHECK(!fsetxattr(file.fd, "user.sealed", "after", 5, XATTR_REPLACE));
  CHECK(!xa_value(&file, XA_FD, "user.sealed", "after", 5));
  CHECK(!fsetxattr(file.fd, "user.new", NULL, 0, XATTR_CREATE));
  CHECK(!fremovexattr(file.fd, "user.sealed"));
  const char* names[] = {"user.new"};
  CHECK(!xa_names(&file, XA_FD, names, 1));
out:
  xa_close(&file);
  return failed;
}

int xa_lifetime(void) {
  for (int backend = XA_MEMFD; backend <= XA_EXT2; ++backend)
    if (aliases(backend) || queued_descriptor(backend))
      return 1;
  return rename_replacement() || sealed_memfd();
}
