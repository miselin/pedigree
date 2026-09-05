#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/uio.h>

static int arm(int fd, int milliseconds, int interval) {
  struct itimerspec setting = {.it_value = ed_timespec((int64_t)milliseconds * 1000000),
                               .it_interval = ed_timespec((int64_t)interval * 1000000)};
  return timerfd_settime(fd, 0, &setting, NULL);
}
static int64_t remaining(const struct itimerspec* setting) {
  return (int64_t)setting->it_value.tv_sec * 1000000000 + setting->it_value.tv_nsec;
}

static int counter_and_settings(void) {
  int failed = 0, fd = -1, pipefd[2] = {-1, -1};
  long page = sysconf(_SC_PAGESIZE);
  void* bad = mmap(NULL, page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  uint64_t counts[2] = {0};
  struct stat info;
  char path[64], link[64];
  struct itimerspec setting, replacement = {.it_value = {0, 1000000}}, old;
  CHECK(bad != MAP_FAILED);
  CHECK((fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)) >= 0);
  CHECK((fcntl(fd, F_GETFL) & O_NONBLOCK) && (fcntl(fd, F_GETFD) & FD_CLOEXEC));
  CHECK(fstat(fd, &info) == 0 && info.st_ino != 0 && info.st_nlink == 1 &&
        (info.st_mode & 0777) == 0600 && info.st_size == 0);
  CHECK(lseek(fd, 0, SEEK_SET) == 0 && lseek(fd, 123, SEEK_SET) == 0);
  CHECK(lseek(fd, 0, 123) == -1 && errno == EINVAL);
  snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
  CHECK(readlink(path, link, sizeof(link)) == (ssize_t)strlen("anon_inode:[timerfd]") &&
        !memcmp(link, "anon_inode:[timerfd]", strlen("anon_inode:[timerfd]")));
  CHECK(write(fd, counts, 8) == -1 && errno == EINVAL);
  CHECK(timerfd_create(-1, 0) == -1 && errno == EINVAL);
  CHECK(timerfd_create(CLOCK_MONOTONIC, 0x40000000) == -1 && errno == EINVAL);
  CHECK(timerfd_create(CLOCK_PROCESS_CPUTIME_ID, 0) == -1 && errno == EINVAL);
  CHECK(timerfd_gettime(fd, &setting) == 0 && remaining(&setting) == 0);
  CHECK(read(fd, counts, 0) == -1 && errno == EINVAL);
  CHECK(read(fd, counts, 7) == -1 && errno == EINVAL);
  CHECK(read(fd, counts, sizeof(counts)) == -1 && errno == EAGAIN);
  CHECK(timerfd_gettime(fd, bad) == -1 && errno == EFAULT);
  CHECK(syscall(SYS_timerfd_settime, fd, 0, NULL, NULL) == -1 && errno == EFAULT);
  CHECK(timerfd_settime(fd, 0, bad, NULL) == -1 && errno == EFAULT);
  CHECK(timerfd_settime(fd, 4, &replacement, NULL) == -1 && errno == EINVAL);
  replacement.it_value.tv_nsec = 1000000000;
  CHECK(timerfd_settime(fd, 0, &replacement, NULL) == -1 && errno == EINVAL);
  replacement.it_value.tv_nsec = 1000000;
  replacement.it_interval.tv_sec = -1;
  CHECK(timerfd_settime(fd, 0, &replacement, NULL) == -1 && errno == EINVAL);
  replacement.it_interval.tv_sec = 0;
  CHECK(pipe(pipefd) == 0);
  CHECK(timerfd_gettime(pipefd[0], &setting) == -1 && errno == EINVAL);
  CHECK(timerfd_gettime(-1, &setting) == -1 && errno == EBADF);
  CHECK(arm(fd, 3000, 77) == 0);
  CHECK(timerfd_settime(fd, 0, &replacement, bad) == -1 && errno == EFAULT);
  CHECK(timerfd_gettime(fd, &setting) == 0 && remaining(&setting) > 1000000000 &&
        setting.it_interval.tv_nsec == 77000000);
  replacement.it_value = ed_timespec(30000000);
  CHECK(timerfd_settime(fd, 0, &replacement, &old) == 0 && remaining(&old) > 1000000000 &&
        old.it_interval.tv_nsec == 77000000);
  CHECK(ed_readable(fd, 2000) == 1);
  CHECK(read(fd, bad, 8) == -1 && errno == EFAULT);
  CHECK(ed_readable(fd, 0) == 1);
  struct iovec faulty_vectors[2] = {{counts, 3}, {bad, 5}};
  CHECK(readv(fd, faulty_vectors, 2) == -1 && errno == EFAULT);
  CHECK(ed_readable(fd, 0) == 1);
  counts[1] = UINT64_C(0xa5a5a5a5a5a5a5a5);
  CHECK(read(fd, counts, sizeof(counts)) == 8 && counts[0] == 1 &&
        counts[1] == UINT64_C(0xa5a5a5a5a5a5a5a5));
  CHECK(timerfd_gettime(fd, &setting) == 0 && remaining(&setting) == 0);
  CHECK(read(fd, counts, 8) == -1 && errno == EAGAIN);
  CHECK(arm(fd, 20, 20) == 0);
  ed_pause(140);
  struct iovec vectors[2] = {{counts, 3}, {(unsigned char*)counts + 3, 5}};
  CHECK(readv(fd, vectors, 2) == 8 && counts[0] >= 3);
  CHECK(timerfd_gettime(fd, &setting) == 0 && remaining(&setting) > 0 &&
        setting.it_interval.tv_nsec == 20000000);
  CHECK(arm(fd, 0, 77) == 0);
  CHECK(timerfd_gettime(fd, &setting) == 0 && remaining(&setting) == 0 &&
        setting.it_interval.tv_sec == 0 && setting.it_interval.tv_nsec == 77000000);
  CHECK(read(fd, counts, 8) == -1 && errno == EAGAIN);
  CHECK(arm(fd, 10, 0) == 0 && ed_readable(fd, 2000) == 1);
  CHECK(arm(fd, 3000, 0) == 0);
  CHECK(read(fd, counts, 8) == -1 && errno == EAGAIN);
  replacement.it_value = ed_timespec(ed_now(CLOCK_MONOTONIC) + 30000000);
  CHECK(timerfd_settime(fd, TFD_TIMER_ABSTIME, &replacement, NULL) == 0);
  CHECK(ed_readable(fd, 2000) == 1 && read(fd, counts, 8) == 8 && counts[0] == 1);
out:
  if (fd >= 0)
    close(fd);
  for (int n = 0; n < 2; ++n)
    if (pipefd[n] >= 0)
      close(pipefd[n]);
  if (bad != MAP_FAILED)
    munmap(bad, page);
  return failed;
}

static int readiness(void) {
  int failed = 0, fd = -1, epoll = -1;
  uint64_t count;
  struct epoll_event interest = {.events = EPOLLIN | EPOLLOUT, .data.u64 = 0x5678}, ready;
  CHECK((fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK)) >= 0);
  CHECK((epoll = epoll_create1(EPOLL_CLOEXEC)) >= 0);
  CHECK(epoll_ctl(epoll, EPOLL_CTL_ADD, fd, &interest) == 0);
  CHECK(ed_readable(fd, 0) == 0 && epoll_wait(epoll, &ready, 1, 0) == 0);
  CHECK(arm(fd, 30, 0) == 0);
  CHECK(epoll_wait(epoll, &ready, 1, 2000) == 1 && ready.data.u64 == 0x5678 &&
        ready.events == EPOLLIN);
  CHECK(ed_readable(fd, 0) == 1 && epoll_wait(epoll, &ready, 1, 0) == 1);
  CHECK(read(fd, &count, 8) == 8 && count == 1);
  CHECK(ed_readable(fd, 0) == 0 && epoll_wait(epoll, &ready, 1, 0) == 0);
  interest.events = EPOLLIN | EPOLLET;
  CHECK(epoll_ctl(epoll, EPOLL_CTL_MOD, fd, &interest) == 0);
  CHECK(arm(fd, 20, 20) == 0);
  CHECK(epoll_wait(epoll, &ready, 1, 2000) == 1);
  ed_pause(80);
  CHECK(ed_readable(fd, 0) == 1 && epoll_wait(epoll, &ready, 1, 0) == 0);
  CHECK(read(fd, &count, 8) == 8 && count >= 3);
  CHECK(epoll_wait(epoll, &ready, 1, 2000) == 1);
  CHECK(read(fd, &count, 8) == 8 && count >= 1);
  CHECK(arm(fd, 0, 0) == 0);
  CHECK(ed_readable(fd, 0) == 0 && epoll_wait(epoll, &ready, 1, 0) == 0);
out:
  if (epoll >= 0)
    close(epoll);
  if (fd >= 0)
    close(fd);
  return failed;
}

static int shared_lifetime(void) {
  int failed = 0, fd = -1, alias = -1, sockets[2] = {-1, -1};
  pid_t child = -1;
  uint64_t count;
  struct itimerspec setting;
  struct stat info, shared_info;
  char byte;
  CHECK((fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)) >= 0);
  CHECK((alias = dup(fd)) >= 0 && !(fcntl(alias, F_GETFD) & FD_CLOEXEC));
  CHECK(fstat(fd, &info) == 0 && fstat(alias, &shared_info) == 0 &&
        info.st_ino == shared_info.st_ino);
  int nonblock = 0;
  CHECK(ioctl(alias, FIONBIO, &nonblock) == 0 && !(fcntl(fd, F_GETFL) & O_NONBLOCK));
  CHECK(fcntl(fd, F_SETFL, O_NONBLOCK) == 0 && (fcntl(alias, F_GETFL) & O_NONBLOCK));
  CHECK((child = fork()) >= 0);
  if (!child) {
    close(fd);
    if (arm(alias, 30, 0))
      _exit(10);
    close(alias);
    _exit(0);
  }
  int status = ed_reap(child, 2000);
  child = -1;
  CHECK(status == 0 && ed_readable(fd, 2000) == 1);
  CHECK(read(alias, &count, 8) == 8 && count == 1);
  CHECK(read(fd, &count, 8) == -1 && errno == EAGAIN);
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  CHECK((child = fork()) >= 0);
  if (!child) {
    alarm(5);
    close(sockets[0]);
    close(fd);
    close(alias);
    int received = ed_receive_fd(sockets[1]);
    if (received < 0 || !(fcntl(received, F_GETFD) & FD_CLOEXEC) || arm(received, 30, 70) ||
        send(sockets[1], "r", 1, 0) != 1 || recv(sockets[1], &byte, 1, 0) != 1 ||
        ed_readable(received, 2000) != 1 || read(received, &count, 8) != 8 || count < 1)
      _exit(11);
    close(received);
    close(sockets[1]);
    _exit(0);
  }
  CHECK(ed_send_fd(sockets[0], fd) == 0 && recv(sockets[0], &byte, 1, 0) == 1 && byte == 'r');
  CHECK(timerfd_gettime(alias, &setting) == 0 && setting.it_interval.tv_nsec == 70000000);
  CHECK(close(fd) == 0);
  fd = -1;
  CHECK(close(alias) == 0);
  alias = -1;
  CHECK(send(sockets[0], "g", 1, 0) == 1);
  status = ed_reap(child, 3000);
  child = -1;
  CHECK(status == 0);
out:
  if (child > 0) {
    kill(child, SIGKILL);
    ed_reap(child, 1000);
  }
  if (alias >= 0)
    close(alias);
  if (fd >= 0)
    close(fd);
  for (int n = 0; n < 2; ++n)
    if (sockets[n] >= 0)
      close(sockets[n]);
  return failed;
}

int event_descriptor_timerfd_exec(int argc, char** argv) {
  if (argc != 4)
    return 2;
  int keep = atoi(argv[2]), drop = atoi(argv[3]);
  uint64_t count;
  struct itimerspec setting;
  if (fcntl(drop, F_GETFD) != -1 || errno != EBADF || fcntl(keep, F_GETFD) < 0 ||
      timerfd_gettime(keep, &setting) || ed_readable(keep, 2000) != 1 ||
      read(keep, &count, sizeof(count)) != 8 || count != 1)
    return 1;
  close(keep);
  return 0;
}
static int closing_and_exec(void) {
  int failed = 0, fd = -1, alias = -1, active = 0;
  pid_t child = -1;
  pthread_t thread;
  struct ed_reader reader = {.size = 8};
  CHECK((fd = timerfd_create(CLOCK_MONOTONIC, 0)) >= 0 && (alias = dup(fd)) >= 0);
  reader.fd = fd;
  CHECK(pthread_create(&thread, NULL, ed_read_thread, &reader) == 0);
  active = 1;
  CHECK(ed_wait(&reader.started, 1000) == 0);
  ed_pause(30);
  CHECK(!__atomic_load_n(&reader.done, __ATOMIC_ACQUIRE));
  CHECK(close(fd) == 0);
  fd = -1;
  ed_pause(30);
  CHECK(!__atomic_load_n(&reader.done, __ATOMIC_ACQUIRE));
  CHECK(close(alias) == 0);
  alias = -1;
  CHECK(ed_wait(&reader.done, 2000) == 0 && pthread_join(thread, NULL) == 0);
  active = 0;
  CHECK(reader.result == -1 && reader.error == EBADF);
  CHECK((child = fork()) >= 0);
  if (!child) {
    int keep = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    int drop = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    char kept[24], dropped[24];
    if (keep < 0 || drop < 0 || arm(keep, 20, 0) || ed_readable(keep, 2000) != 1)
      _exit(10);
    snprintf(kept, sizeof(kept), "%d", keep);
    snprintf(dropped, sizeof(dropped), "%d", drop);
    execl(EVENT_DESCRIPTOR_APP, EVENT_DESCRIPTOR_APP, "timerfd-exec", kept, dropped, (char*)NULL);
    _exit(11);
  }
  int status = ed_reap(child, 4000);
  child = -1;
  CHECK(status == 0);
out:
  if (fd >= 0)
    close(fd);
  if (alias >= 0)
    close(alias);
  if (active) {
    pthread_cancel(thread);
    pthread_join(thread, NULL);
  }
  if (child > 0) {
    kill(child, SIGKILL);
    ed_reap(child, 1000);
  }
  return failed;
}

int event_descriptor_test_timerfd(void) {
  return counter_and_settings() || readiness() || shared_lifetime() || closing_and_exec();
}
