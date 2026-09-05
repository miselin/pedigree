#define _GNU_SOURCE
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>

static sigset_t one_signal(int signal) {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, signal);
  return set;
}
static int queued(int signal, uintptr_t value) {
  return sigqueue(getpid(), signal, (union sigval){.sival_ptr = (void*)value});
}

static int records_and_updates(void) {
  int failed = 0, fd = -1, alias = -1, legacy = -1, pipefd[2] = {-1, -1};
  long page = sysconf(_SC_PAGESIZE);
  unsigned char* area =
      mmap(NULL, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  sigset_t set = one_signal(SIGUSR1), before, after, pending;
  struct signalfd_siginfo records[3] = {0};
  struct stat info, shared_info;
  char path[64], link[64];
  const uintptr_t value = UINT64_C(0x12345678abcdef01);
  CHECK(sizeof(records[0]) == 128 && area != MAP_FAILED);
  CHECK(mprotect(area + page, page, PROT_NONE) == 0);
  sigaddset(&set, SIGRTMIN);
  sigaddset(&set, SIGRTMAX);
  CHECK(pthread_sigmask(SIG_SETMASK, NULL, &before) == 0);
  CHECK((fd = signalfd(-1, &set, SFD_NONBLOCK | SFD_CLOEXEC)) >= 0);
  CHECK(pthread_sigmask(SIG_SETMASK, NULL, &after) == 0);
  CHECK(!memcmp(&before, &after, 8));
  CHECK((fcntl(fd, F_GETFL) & O_NONBLOCK) && (fcntl(fd, F_GETFD) & FD_CLOEXEC));
  CHECK(fstat(fd, &info) == 0 && info.st_ino != 0 && info.st_nlink == 1 &&
        (info.st_mode & 0777) == 0600 && info.st_size == 0);
  CHECK(lseek(fd, 0, SEEK_SET) == 0 && lseek(fd, 123, SEEK_SET) == 0);
  CHECK(lseek(fd, 0, 123) == -1 && errno == EINVAL);
  snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
  CHECK(readlink(path, link, sizeof(link)) == (ssize_t)strlen("anon_inode:[signalfd]") &&
        !memcmp(link, "anon_inode:[signalfd]", strlen("anon_inode:[signalfd]")));
  CHECK(write(fd, records, 128) == -1 && errno == EINVAL);
  CHECK(signalfd(-1, &set, 0x40000000) == -1 && errno == EINVAL);
  CHECK(syscall(SYS_signalfd4, -1, &set, 16, 0) == -1 && errno == EINVAL);
  CHECK(syscall(SYS_signalfd, -1, &set, 0) == -1 && errno == EINVAL);
  CHECK(signalfd(-1, (sigset_t*)(area + page), 0) == -1 && errno == EFAULT);
  CHECK(signalfd(-2, &set, 0) == -1 && errno == EBADF);
  CHECK(pipe(pipefd) == 0);
  CHECK(signalfd(pipefd[0], &set, 0) == -1 && errno == EINVAL);
  CHECK((legacy = syscall(SYS_signalfd, -1, &set, 8)) >= 0);
  close(legacy);
  legacy = -1;
  CHECK(read(fd, records, sizeof(records)) == -1 && errno == EAGAIN);
  CHECK(kill(getpid(), SIGUSR1) == 0 && kill(getpid(), SIGUSR1) == 0);
  CHECK(queued(SIGRTMIN, value) == 0 && queued(SIGRTMIN, 102) == 0);
  CHECK(queued(SIGRTMAX, 640) == 0);
  CHECK(read(fd, records, 127) == -1 && errno == EINVAL);
  CHECK(read(fd, area + page, 128) == -1 && errno == EFAULT);
  CHECK(sigpending(&pending) == 0 && sigismember(&pending, SIGUSR1) == 1);
  memset(records, 0xa5, sizeof(records));
  CHECK(read(fd, records, 128 * 2 + 63) == 256);
  CHECK(records[0].ssi_signo == SIGUSR1 && records[0].ssi_code == SI_USER &&
        records[0].ssi_pid == (unsigned)getpid() && records[0].ssi_uid == getuid());
  CHECK(records[1].ssi_signo == (unsigned)SIGRTMIN && records[1].ssi_code == SI_QUEUE &&
        records[1].ssi_ptr == value && records[1].ssi_pid == (unsigned)getpid() &&
        records[1].ssi_uid == getuid());
  CHECK(((unsigned char*)&records[2])[0] == 0xa5);
  CHECK((alias = dup(fd)) >= 0 && !(fcntl(alias, F_GETFD) & FD_CLOEXEC));
  CHECK(fstat(alias, &shared_info) == 0 && shared_info.st_ino == info.st_ino);
  set = one_signal(SIGRTMAX);
  CHECK(signalfd(alias, &set, 0) == alias);
  CHECK((fcntl(alias, F_GETFL) & O_NONBLOCK) && (fcntl(fd, F_GETFD) & FD_CLOEXEC));
  CHECK(read(fd, records, sizeof(records)) == 128 && records[0].ssi_signo == SIGRTMAX &&
        records[0].ssi_int == 640);
  CHECK(read(fd, records, sizeof(records)) == -1 && errno == EAGAIN);
  set = one_signal(SIGRTMIN);
  int nonblock = 0;
  CHECK(ioctl(alias, FIONBIO, &nonblock) == 0 && !(fcntl(fd, F_GETFL) & O_NONBLOCK));
  CHECK(signalfd(alias, &set, SFD_CLOEXEC | SFD_NONBLOCK) == alias);
  CHECK(!(fcntl(alias, F_GETFD) & FD_CLOEXEC) && !(fcntl(fd, F_GETFL) & O_NONBLOCK));
  CHECK(fcntl(alias, F_SETFL, O_NONBLOCK) == 0 && (fcntl(fd, F_GETFL) & O_NONBLOCK));
  struct iovec vectors[2] = {{records, 31}, {(unsigned char*)records + 31, 97}};
  CHECK(readv(alias, vectors, 2) == 128 && records[0].ssi_int == 102);
  CHECK(queued(SIGRTMIN, 201) == 0 && queued(SIGRTMIN, 202) == 0);
  CHECK(signalfd(fd, (sigset_t*)(area + page), 0) == -1 && errno == EFAULT);
  CHECK(read(fd, area + page - 128, 256) == 128);
  memcpy(records, area + page - 128, 128);
  CHECK(records[0].ssi_int == 201);
  CHECK(read(fd, records, sizeof(records)) == 128 && records[0].ssi_int == 202);
  CHECK(read(fd, records, sizeof(records)) == -1 && errno == EAGAIN);
  CHECK(queued(SIGRTMIN, 203) == 0 && queued(SIGRTMIN, 204) == 0);
  vectors[0] = (struct iovec){records, 128};
  vectors[1] = (struct iovec){area + page, 128};
  CHECK(readv(fd, vectors, 2) == 128 && records[0].ssi_int == 203);
  CHECK(sigpending(&pending) == 0 && sigismember(&pending, SIGRTMIN) == 1);
  CHECK(read(fd, records, sizeof(records)) == 128 && records[0].ssi_int == 204);
  CHECK(read(fd, records, sizeof(records)) == -1 && errno == EAGAIN);
  sigaddset(&set, SIGKILL);
  sigaddset(&set, SIGSTOP);
  CHECK(signalfd(fd, &set, 0) == fd);
  CHECK(pthread_sigmask(SIG_SETMASK, NULL, &after) == 0 && !memcmp(&before, &after, 8));
out:
  if (legacy >= 0)
    close(legacy);
  if (alias >= 0)
    close(alias);
  if (fd >= 0)
    close(fd);
  for (int n = 0; n < 2; ++n)
    if (pipefd[n] >= 0)
      close(pipefd[n]);
  if (area != MAP_FAILED)
    munmap(area, page * 2);
  return failed;
}

static void* producer(void* ignored) {
  (void)ignored;
  ed_pause(30);
  return (void*)(intptr_t)queued(SIGRTMIN, 301);
}
static int readiness(void) {
  int failed = 0, fd = -1, epoll = -1, active = 0;
  pthread_t thread;
  void* result;
  sigset_t set = one_signal(SIGRTMIN);
  struct signalfd_siginfo record;
  struct epoll_event interest = {.events = EPOLLIN, .data.u64 = 0x1234}, ready;
  CHECK((fd = signalfd(-1, &set, SFD_NONBLOCK)) >= 0);
  CHECK((epoll = epoll_create1(EPOLL_CLOEXEC)) >= 0);
  CHECK(epoll_ctl(epoll, EPOLL_CTL_ADD, fd, &interest) == 0);
  CHECK(ed_readable(fd, 0) == 0 && epoll_wait(epoll, &ready, 1, 0) == 0);
  CHECK(pthread_create(&thread, NULL, producer, NULL) == 0);
  active = 1;
  CHECK(epoll_wait(epoll, &ready, 1, 2000) == 1 && ready.data.u64 == 0x1234 &&
        (ready.events & EPOLLIN));
  CHECK(pthread_join(thread, &result) == 0);
  active = 0;
  CHECK((intptr_t)result == 0);
  CHECK(ed_readable(fd, 0) == 1 && epoll_wait(epoll, &ready, 1, 0) == 1);
  CHECK(read(fd, &record, sizeof(record)) == 128 && record.ssi_int == 301);
  CHECK(ed_readable(fd, 0) == 0 && epoll_wait(epoll, &ready, 1, 0) == 0);
  interest.events = EPOLLIN | EPOLLET;
  CHECK(epoll_ctl(epoll, EPOLL_CTL_MOD, fd, &interest) == 0);
  CHECK(queued(SIGRTMIN, 302) == 0 && queued(SIGRTMIN, 303) == 0);
  CHECK(epoll_wait(epoll, &ready, 1, 1000) == 1);
  CHECK(epoll_wait(epoll, &ready, 1, 0) == 0);
  CHECK(read(fd, &record, sizeof(record)) == 128 && record.ssi_int == 302);
  CHECK(ed_readable(fd, 0) == 1 && epoll_wait(epoll, &ready, 1, 0) == 0);
  CHECK(read(fd, &record, sizeof(record)) == 128 && record.ssi_int == 303);
  CHECK(queued(SIGRTMIN, 304) == 0);
  CHECK(epoll_wait(epoll, &ready, 1, 1000) == 1);
  CHECK(read(fd, &record, sizeof(record)) == 128 && record.ssi_int == 304);
out:
  if (active)
    pthread_join(thread, NULL);
  if (epoll >= 0)
    close(epoll);
  if (fd >= 0)
    close(fd);
  return failed;
}

struct epoll_registrar {
  int fd, epoll, error;
  pthread_t waiter;
  volatile int ready, send, finish;
};
static void* epoll_registrar(void* argument) {
  struct epoll_registrar* registrar = argument;
  sigset_t set = one_signal(SIGRTMIN);
  struct epoll_event interest = {.events = EPOLLIN, .data.u64 = 351};
  registrar->fd = signalfd(-1, &set, SFD_NONBLOCK);
  registrar->epoll = epoll_create1(0);
  registrar->error = registrar->fd < 0 || registrar->epoll < 0 ||
                     epoll_ctl(registrar->epoll, EPOLL_CTL_ADD, registrar->fd, &interest);
  __atomic_store_n(&registrar->ready, 1, __ATOMIC_RELEASE);
  if (registrar->error || ed_wait(&registrar->send, 3000))
    return (void*)1;
  ed_pause(30);
  if (pthread_kill(registrar->waiter, SIGRTMIN) || ed_wait(&registrar->finish, 3000))
    return (void*)1;
  return NULL;
}
static int epoll_waiter_lifetime(void) {
  int failed = 0, active = 0;
  pthread_t thread;
  void* result;
  struct epoll_registrar registrar = {.fd = -1, .epoll = -1, .waiter = pthread_self()};
  struct epoll_event ready;
  struct signalfd_siginfo record;
  CHECK(pthread_create(&thread, NULL, epoll_registrar, &registrar) == 0);
  active = 1;
  CHECK(ed_wait(&registrar.ready, 1000) == 0 && registrar.error == 0);
  CHECK(epoll_wait(registrar.epoll, &ready, 1, 0) == 0);
  __atomic_store_n(&registrar.send, 1, __ATOMIC_RELEASE);
  CHECK(epoll_wait(registrar.epoll, &ready, 1, 2000) == 1 && ready.events == EPOLLIN &&
        ready.data.u64 == 351);
  CHECK(read(registrar.fd, &record, 128) == 128 && record.ssi_signo == SIGRTMIN &&
        record.ssi_code == SI_TKILL);
  CHECK(epoll_wait(registrar.epoll, &ready, 1, 0) == 0);
  __atomic_store_n(&registrar.finish, 1, __ATOMIC_RELEASE);
  CHECK(pthread_join(thread, &result) == 0);
  active = 0;
  CHECK(result == NULL);
  CHECK(queued(SIGRTMIN, 352) == 0);
  CHECK(epoll_wait(registrar.epoll, &ready, 1, 2000) == 1 && ready.events == EPOLLIN &&
        ready.data.u64 == 351);
  CHECK(read(registrar.fd, &record, 128) == 128 && record.ssi_code == SI_QUEUE &&
        record.ssi_int == 352);
  CHECK(epoll_wait(registrar.epoll, &ready, 1, 0) == 0);
out:
  if (active) {
    __atomic_store_n(&registrar.send, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&registrar.finish, 1, __ATOMIC_RELEASE);
    pthread_join(thread, NULL);
  }
  if (registrar.epoll >= 0)
    close(registrar.epoll);
  if (registrar.fd >= 0)
    close(registrar.fd);
  return failed;
}

static int inherited_epoll(void) {
  int failed = 0, fd = -1, epoll = -1;
  pid_t child = -1;
  sigset_t set = one_signal(SIGRTMIN);
  struct signalfd_siginfo record;
  struct epoll_event interest = {.events = EPOLLIN}, ready;
  CHECK((fd = signalfd(-1, &set, SFD_NONBLOCK)) >= 0);
  CHECK((epoll = epoll_create1(0)) >= 0 && epoll_ctl(epoll, EPOLL_CTL_ADD, fd, &interest) == 0);
  CHECK((child = fork()) >= 0);
  if (!child) {
    if (queued(SIGRTMIN, 350) || epoll_wait(epoll, &ready, 1, 0) != 0)
      _exit(10);
    int own_epoll = epoll_create1(0);
    if (own_epoll < 0 || epoll_ctl(own_epoll, EPOLL_CTL_ADD, fd, &interest) ||
        epoll_wait(own_epoll, &ready, 1, 1000) != 1 || read(fd, &record, 128) != 128 ||
        record.ssi_int != 350)
      _exit(11);
    close(own_epoll);
    _exit(0);
  }
  int status = ed_reap(child, 3000);
  child = -1;
  CHECK(status == 0 && epoll_wait(epoll, &ready, 1, 0) == 0);
  CHECK(read(fd, &record, 128) == -1 && errno == EAGAIN);
out:
  if (child > 0) {
    kill(child, SIGKILL);
    ed_reap(child, 1000);
  }
  if (epoll >= 0)
    close(epoll);
  if (fd >= 0)
    close(fd);
  return failed;
}

struct private_reader {
  struct ed_reader reader;
  volatile int ready, go;
};
static void* private_reader(void* argument) {
  struct private_reader* target = argument;
  __atomic_store_n(&target->ready, 1, __ATOMIC_RELEASE);
  if (ed_wait(&target->go, 2000))
    return NULL;
  return ed_read_thread(&target->reader);
}
static int caller_and_sharing(void) {
  int failed = 0, fd = -1, alias = -1, sockets[2] = {-1, -1}, active = 0;
  pid_t child = -1;
  pthread_t thread;
  sigset_t set = one_signal(SIGRTMIN);
  struct signalfd_siginfo record;
  struct private_reader target = {.reader = {.size = 128}};
  CHECK((fd = signalfd(-1, &set, SFD_NONBLOCK)) >= 0);
  target.reader.fd = fd;
  CHECK(pthread_create(&thread, NULL, private_reader, &target) == 0);
  active = 1;
  CHECK(ed_wait(&target.ready, 1000) == 0 && pthread_kill(thread, SIGRTMIN) == 0);
  CHECK(ed_readable(fd, 0) == 0 && read(fd, &record, 128) == -1 && errno == EAGAIN);
  __atomic_store_n(&target.go, 1, __ATOMIC_RELEASE);
  CHECK(ed_wait(&target.reader.done, 2000) == 0 && pthread_join(thread, NULL) == 0);
  active = 0;
  memcpy(&record, target.reader.bytes, sizeof(record));
  CHECK(target.reader.result == 128 && record.ssi_signo == SIGRTMIN && record.ssi_code == SI_TKILL);
  CHECK((alias = dup(fd)) >= 0 && socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  CHECK(queued(SIGRTMIN, 401) == 0);
  CHECK((child = fork()) >= 0);
  if (!child) {
    close(sockets[0]);
    close(fd);
    close(alias);
    int received = ed_receive_fd(sockets[1]);
    if (received < 0 || !(fcntl(received, F_GETFD) & FD_CLOEXEC))
      _exit(10);
    set = one_signal(SIGRTMAX);
    if (signalfd(received, &set, 0) != received || queued(SIGRTMAX, 402) ||
        read(received, &record, 128) != 128 || record.ssi_int != 402 ||
        record.ssi_pid != (unsigned)getpid())
      _exit(11);
    close(received);
    close(sockets[1]);
    _exit(0);
  }
  CHECK(ed_send_fd(sockets[0], fd) == 0);
  int status = ed_reap(child, 3000);
  child = -1;
  CHECK(status == 0);
  CHECK(read(alias, &record, 128) == -1 && errno == EAGAIN);
  CHECK(queued(SIGRTMAX, 403) == 0);
  CHECK(read(fd, &record, 128) == 128 && record.ssi_int == 403);
  set = one_signal(SIGRTMIN);
  CHECK(signalfd(alias, &set, 0) == alias);
  CHECK(read(fd, &record, 128) == 128 && record.ssi_int == 401);
out:
  if (active) {
    __atomic_store_n(&target.go, 1, __ATOMIC_RELEASE);
    pthread_join(thread, NULL);
  }
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

static int source_metadata(void) {
  int failed = 0, fd = -1, live = 0;
  pid_t child = -1;
  timer_t timer;
  sigset_t set = one_signal(SIGCHLD);
  siginfo_t discarded;
  struct timespec zero = {0};
  struct signalfd_siginfo record;
  struct sigevent event = {
      .sigev_notify = SIGEV_SIGNAL, .sigev_signo = SIGRTMAX, .sigev_value.sival_int = 501};
  struct itimerspec setting = {.it_value = {0, 30000000}, .it_interval = {0, 30000000}};
  while (sigtimedwait(&set, &discarded, &zero) >= 0) {
  }
  sigaddset(&set, SIGRTMAX);
  CHECK((fd = signalfd(-1, &set, SFD_NONBLOCK)) >= 0);
  CHECK((child = fork()) >= 0);
  if (!child)
    _exit(42);
  CHECK(ed_readable(fd, 2000) == 1);
  CHECK(read(fd, &record, 128) == 128 && record.ssi_signo == SIGCHLD &&
        record.ssi_code == CLD_EXITED && record.ssi_pid == (unsigned)child &&
        record.ssi_status == 42);
  int status = ed_reap(child, 1000);
  child = -1;
  CHECK(status == 42);
  CHECK(timer_create(CLOCK_MONOTONIC, &event, &timer) == 0);
  live = 1;
  CHECK(timer_settime(timer, 0, &setting, NULL) == 0);
  ed_pause(160);
  CHECK(read(fd, &record, 128) == 128 && record.ssi_signo == SIGRTMAX &&
        record.ssi_code == SI_TIMER && record.ssi_int == 501 &&
        record.ssi_tid == (uint32_t)(intptr_t)timer && record.ssi_overrun >= 1);
  CHECK(timer_getoverrun(timer) == (int)record.ssi_overrun);
out:
  if (live)
    timer_delete(timer);
  if (child > 0) {
    kill(child, SIGKILL);
    ed_reap(child, 1000);
  }
  if (fd >= 0)
    close(fd);
  return failed;
}

int event_descriptor_signalfd_exec(int argc, char** argv) {
  if (argc != 4)
    return 2;
  int keep = atoi(argv[2]), drop = atoi(argv[3]);
  struct signalfd_siginfo record;
  sigset_t blocked;
  if (fcntl(drop, F_GETFD) != -1 || errno != EBADF || fcntl(keep, F_GETFD) < 0 ||
      pthread_sigmask(SIG_SETMASK, NULL, &blocked) || sigismember(&blocked, SIGRTMIN) != 1 ||
      read(keep, &record, sizeof(record)) != 128 || record.ssi_int != 601 ||
      record.ssi_signo != SIGRTMIN || record.ssi_code != SI_QUEUE)
    return 1;
  close(keep);
  return 0;
}
static int closing_and_exec(void) {
  int failed = 0, fd = -1, active = 0;
  pid_t child = -1;
  pthread_t thread;
  sigset_t set = one_signal(SIGRTMIN);
  struct ed_reader reader = {.size = 128};
  CHECK((fd = signalfd(-1, &set, 0)) >= 0);
  reader.fd = fd;
  CHECK(pthread_create(&thread, NULL, ed_read_thread, &reader) == 0);
  active = 1;
  CHECK(ed_wait(&reader.started, 1000) == 0);
  ed_pause(30);
  CHECK(!__atomic_load_n(&reader.done, __ATOMIC_ACQUIRE));
  CHECK(close(fd) == 0);
  fd = -1;
  CHECK(ed_wait(&reader.done, 2000) == 0 && pthread_join(thread, NULL) == 0);
  active = 0;
  CHECK(reader.result == -1 && reader.error == EBADF);
  CHECK((child = fork()) >= 0);
  if (!child) {
    int keep = signalfd(-1, &set, SFD_NONBLOCK);
    int drop = signalfd(-1, &set, SFD_NONBLOCK | SFD_CLOEXEC);
    char kept[24], dropped[24];
    if (keep < 0 || drop < 0 || queued(SIGRTMIN, 601))
      _exit(10);
    snprintf(kept, sizeof(kept), "%d", keep);
    snprintf(dropped, sizeof(dropped), "%d", drop);
    execl(EVENT_DESCRIPTOR_APP, EVENT_DESCRIPTOR_APP, "signalfd-exec", kept, dropped, (char*)NULL);
    _exit(11);
  }
  int status = ed_reap(child, 4000);
  child = -1;
  CHECK(status == 0);
out:
  if (fd >= 0)
    close(fd);
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

int event_descriptor_test_signalfd(void) {
  sigset_t set = one_signal(SIGUSR1), original;
  siginfo_t ignored;
  struct timespec zero = {0};
  sigaddset(&set, SIGRTMIN);
  sigaddset(&set, SIGRTMAX);
  sigaddset(&set, SIGCHLD);
  if (pthread_sigmask(SIG_BLOCK, &set, &original))
    return 1;
  int failed = records_and_updates() || readiness() || epoll_waiter_lifetime() ||
               inherited_epoll() || caller_and_sharing() || source_metadata() || closing_and_exec();
  while (sigtimedwait(&set, &ignored, &zero) >= 0) {
  }
  if (pthread_sigmask(SIG_SETMASK, &original, NULL))
    failed = 1;
  return failed;
}
