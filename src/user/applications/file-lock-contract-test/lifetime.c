#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include "contract.h"
#include <sys/socket.h>
#include <sys/stat.h>

static void* unlock_thread(void* argument) {
  int* result = argument;
  *result = fl_lock(*result, FL_CLASSIC, F_UNLCK, 0);
  return NULL;
}
static int process_lifetime(void) {
  int failed = 0, a = -1, b = -1, alias = -1, other = -1;
  char path[128] = {0}, other_path[128] = {0};
  pid_t child = -1;
  CHECK((a = fl_file(path, "/tmp")) >= 0 && (b = open(path, O_RDWR)) >= 0);
  CHECK((other = fl_file(other_path, "/tmp")) >= 0);
  for (int mode = 0; mode < 2; ++mode) {
    CHECK((alias = open(path, O_RDWR)) >= 0);
    CHECK(fl_lock(a, FL_CLASSIC, F_WRLCK, 0) == 0);
    CHECK(fl_query(b, F_OFD_GETLK, 0, 1, F_WRLCK, 0, 0, getpid()) == 0);
    if (!mode) {
      CHECK(close(alias) == 0);
      alias = -1;
    } else {
      CHECK(dup2(other, alias) == alias);
    }
    CHECK(fl_query(b, F_OFD_GETLK, 0, 1, F_UNLCK, 0, 0, 0) == 0);
    if (alias >= 0) {
      close(alias);
      alias = -1;
    }
  }
  CHECK(fl_lock(a, FL_CLASSIC, F_WRLCK, 0) == 0);
  int thread_result = a;
  pthread_t worker;
  CHECK(pthread_create(&worker, NULL, unlock_thread, &thread_result) == 0);
  CHECK(pthread_join(worker, NULL) == 0 && thread_result == 0);
  CHECK(fl_query(b, F_OFD_GETLK, 0, 1, F_UNLCK, 0, 0, 0) == 0);
  CHECK(fl_lock(a, FL_CLASSIC, F_WRLCK, 0) == 0);
  const pid_t owner = getpid();
  CHECK((child = fork()) >= 0);
  if (!child) {
    alarm(5);
    if (fl_query(b, F_GETLK, 0, 1, F_WRLCK, 0, 0, owner) ||
        fl_lock(a, FL_CLASSIC, F_WRLCK, 0) != -1 || errno != EAGAIN)
      _exit(10);
    close(a);
    close(b);
    _exit(0);
  }
  CHECK(fl_reap(child, 6000) == 0);
  child = -1;
  CHECK(fl_query(b, F_OFD_GETLK, 0, 1, F_WRLCK, 0, 0, owner) == 0);
out:
  if (child > 0) {
    kill(child, SIGKILL);
    fl_reap(child, 1000);
  }
  if (alias >= 0)
    close(alias);
  if (other >= 0)
    close(other);
  if (b >= 0)
    close(b);
  if (a >= 0)
    close(a);
  if (*path)
    unlink(path);
  if (*other_path)
    unlink(other_path);
  return failed;
}

static int shared_fork(int kind) {
  int failed = 0, a = -1, b = -1, alias = -1, pair[2] = {-1, -1};
  pid_t child = -1;
  char path[128] = {0};
  CHECK((a = fl_file(path, "/tmp")) >= 0 && (b = open(path, O_RDWR)) >= 0);
  CHECK(fl_lock(a, kind, F_WRLCK, 0) == 0 && (alias = dup(a)) >= 0);
  CHECK(close(a) == 0);
  a = -1;
  CHECK(fl_lock(b, kind, F_WRLCK, 0) == -1 && errno == EAGAIN);
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0 && (child = fork()) >= 0);
  if (!child) {
    alarm(6);
    close(pair[0]);
    close(b);
    if (fl_lock(alias, kind, F_WRLCK, 0) || write(pair[1], "h", 1) != 1 || fl_byte(pair[1], 'x'))
      _exit(10);
    close(alias);
    _exit(0);
  }
  close(pair[1]);
  pair[1] = -1;
  CHECK(fl_byte(pair[0], 'h') == 0);
  CHECK(close(alias) == 0);
  alias = -1;
  CHECK(fl_lock(b, kind, F_WRLCK, 0) == -1 && errno == EAGAIN);
  CHECK(write(pair[0], "x", 1) == 1 && fl_reap(child, 7000) == 0);
  child = -1;
  CHECK(fl_lock(b, kind, F_WRLCK, 0) == 0);
out:
  if (child > 0) {
    kill(child, SIGKILL);
    fl_reap(child, 1000);
  }
  for (int n = 0; n < 2; ++n)
    if (pair[n] >= 0)
      close(pair[n]);
  if (alias >= 0)
    close(alias);
  if (b >= 0)
    close(b);
  if (a >= 0)
    close(a);
  if (*path)
    unlink(path);
  return failed;
}

static int shared_rights(int kind) {
  int failed = 0, a = -1, b = -1, control[2] = {-1, -1}, transport[2] = {-1, -1};
  pid_t child = -1;
  char path[128] = {0};
  CHECK((a = fl_file(path, "/tmp")) >= 0 && (b = open(path, O_RDWR)) >= 0);
  CHECK(fl_lock(a, kind, F_WRLCK, 0) == 0);
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, control) == 0);
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, transport) == 0 && (child = fork()) >= 0);
  if (!child) {
    alarm(8);
    close(a);
    close(b);
    close(control[0]);
    close(transport[0]);
    if (write(control[1], "r", 1) != 1 || fl_byte(control[1], 'g'))
      _exit(10);
    int received = fl_receive_fd(transport[1]);
    if (received < 0 || fl_lock(received, kind, F_WRLCK, 0) || write(control[1], "h", 1) != 1 ||
        fl_byte(control[1], 'x'))
      _exit(11);
    close(received);
    _exit(0);
  }
  close(control[1]);
  control[1] = -1;
  close(transport[1]);
  transport[1] = -1;
  CHECK(fl_byte(control[0], 'r') == 0 && fl_send_fd(transport[0], a) == 0);
  CHECK(close(a) == 0);
  a = -1;
  CHECK(fl_lock(b, kind, F_WRLCK, 0) == -1 && errno == EAGAIN);
  CHECK(write(control[0], "g", 1) == 1 && fl_byte(control[0], 'h') == 0);
  CHECK(fl_lock(b, kind, F_WRLCK, 0) == -1 && errno == EAGAIN);
  CHECK(write(control[0], "x", 1) == 1 && fl_reap(child, 9000) == 0);
  child = -1;
  CHECK(fl_lock(b, kind, F_WRLCK, 0) == 0);
out:
  if (child > 0) {
    kill(child, SIGKILL);
    fl_reap(child, 1000);
  }
  for (int n = 0; n < 2; ++n) {
    if (control[n] >= 0)
      close(control[n]);
    if (transport[n] >= 0)
      close(transport[n]);
  }
  if (b >= 0)
    close(b);
  if (a >= 0)
    close(a);
  if (*path)
    unlink(path);
  return failed;
}

int file_lock_exec(int argc, char** argv) {
  if (argc != 5)
    return 20;
  alarm(6);
  const int fd = atoi(argv[2]), socket = atoi(argv[3]), closed = atoi(argv[4]);
  const int result = fcntl(fd, F_GETFD, 0);
  if (closed ? result != -1 || errno != EBADF : result < 0)
    return 21;
  return write(socket, "e", 1) == 1 && fl_byte(socket, 'x') == 0 ? 0 : 22;
}
static int exec_lifetime(int kind, int cloexec) {
  int failed = 0, a = -1, probe = -1, pair[2] = {-1, -1};
  pid_t child = -1;
  char path[128] = {0};
  CHECK((a = fl_file(path, "/tmp")) >= 0 && (probe = open(path, O_RDWR)) >= 0);
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0 && (child = fork()) >= 0);
  if (!child) {
    alarm(6);
    close(pair[0]);
    close(probe);
    if (cloexec == 2 && open(path, O_RDONLY | O_CLOEXEC) < 0)
      _exit(12);
    if (fl_lock(a, kind, F_WRLCK, 0) || fcntl(a, F_SETFD, cloexec == 1 ? FD_CLOEXEC : 0))
      _exit(10);
    char descriptor[20], socket[20], flag[4];
    snprintf(descriptor, sizeof(descriptor), "%d", a);
    snprintf(socket, sizeof(socket), "%d", pair[1]);
    snprintf(flag, sizeof(flag), "%d", cloexec == 1);
    execl(LOCK_APP, LOCK_APP, "lock-exec", descriptor, socket, flag, (char*)NULL);
    _exit(11);
  }
  close(pair[1]);
  pair[1] = -1;
  close(a);
  a = -1;
  CHECK(fl_byte(pair[0], 'e') == 0);
  if (cloexec)
    CHECK(fl_lock(probe, kind, F_WRLCK, 0) == 0);
  else
    CHECK(fl_lock(probe, kind, F_WRLCK, 0) == -1 && errno == EAGAIN);
  CHECK(write(pair[0], "x", 1) == 1 && fl_reap(child, 7000) == 0);
  child = -1;
  CHECK(fl_lock(probe, kind, F_WRLCK, 0) == 0);
out:
  if (child > 0) {
    kill(child, SIGKILL);
    fl_reap(child, 1000);
  }
  for (int n = 0; n < 2; ++n)
    if (pair[n] >= 0)
      close(pair[n]);
  if (probe >= 0)
    close(probe);
  if (a >= 0)
    close(a);
  if (*path)
    unlink(path);
  return failed;
}

static int inode_aliases(void) {
  int failed = 0, a = -1, b = -1, c = -1;
  char path[128] = {0}, linked[160] = {0}, renamed[160] = {0};
  struct stat first, second;
  if (geteuid()) {
    puts("FILE-LOCK-CONTRACT: SKIP disk hard-link fixture requires root");
    return 0;
  }
  CHECK((a = fl_file(path, "")) >= 0);
  snprintf(linked, sizeof(linked), "%s.link", path);
  snprintf(renamed, sizeof(renamed), "%s.renamed", path);
  CHECK(link(path, linked) == 0 && (b = open(linked, O_RDWR)) >= 0);
  CHECK(fstat(a, &first) == 0 && fstat(b, &second) == 0 && first.st_dev == second.st_dev &&
        first.st_ino == second.st_ino);
  CHECK(fl_lock(a, FL_OFD, F_WRLCK, 0) == 0);
  CHECK(fl_lock(b, FL_OFD, F_WRLCK, 0) == -1 && errno == EAGAIN);
  CHECK(rename(path, renamed) == 0 && unlink(linked) == 0);
  CHECK((c = open(renamed, O_RDWR)) >= 0 && unlink(renamed) == 0);
  CHECK(fl_query(c, F_GETLK, 0, 1, F_WRLCK, 0, 0, -1) == 0);
  CHECK(fl_lock(a, FL_OFD, F_UNLCK, 0) == 0);
  CHECK(fl_lock(a, FL_CLASSIC, F_WRLCK, 0) == 0);
  CHECK(close(b) == 0);
  b = -1;
  CHECK(fl_query(c, F_OFD_GETLK, 0, 1, F_UNLCK, 0, 0, 0) == 0);
  CHECK(fl_lock(a, FL_FLOCK, F_WRLCK, 0) == 0);
  CHECK(fl_lock(c, FL_FLOCK, F_WRLCK, 0) == -1 && errno == EAGAIN);
  CHECK(close(a) == 0);
  a = -1;
  CHECK(fl_lock(c, FL_FLOCK, F_WRLCK, 0) == 0);
out:
  if (c >= 0)
    close(c);
  if (b >= 0)
    close(b);
  if (a >= 0)
    close(a);
  if (*renamed)
    unlink(renamed);
  if (*linked)
    unlink(linked);
  if (*path)
    unlink(path);
  return failed;
}
int file_lock_lifetime(void) {
  if (process_lifetime() || exec_lifetime(FL_CLASSIC, 2))
    return 1;
  for (int kind = 0; kind < 3; ++kind) {
    if (kind != FL_CLASSIC && (shared_fork(kind) || shared_rights(kind)))
      return 1;
    if (exec_lifetime(kind, 0) || exec_lifetime(kind, 1))
      return 1;
  }
  return inode_aliases();
}
