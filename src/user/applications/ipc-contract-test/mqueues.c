#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define CHECK(condition)                                                           \
  do {                                                                             \
    if (!(condition)) {                                                            \
      fprintf(stderr, "mqueues:%d: %s (errno=%d)\n", __LINE__, #condition, errno); \
      failed = 1;                                                                  \
      goto out;                                                                    \
    }                                                                              \
  } while (0)

static struct timespec deadline_after(long milliseconds) {
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_nsec += milliseconds * 1000000;
  deadline.tv_sec += deadline.tv_nsec / 1000000000;
  deadline.tv_nsec %= 1000000000;
  return deadline;
}

static volatile sig_atomic_t signal_count, signal_code, signal_value;
static sem_t callback_done;
static void notified(int signal, siginfo_t* info, void* context) {
  (void)signal;
  (void)context;
  signal_code = info->si_code;
  signal_value = info->si_value.sival_int;
  ++signal_count;
}
static void callback(union sigval value) {
  signal_value = value.sival_int;
  sem_post(&callback_done);
}
static volatile sig_atomic_t interrupt_calls, hold_handler;
static struct timespec handler_until, handler_finished;
static void interrupted(int signal) {
  (void)signal;
  __atomic_add_fetch(&interrupt_calls, 1, __ATOMIC_RELEASE);
  if (hold_handler) {
    struct timespec now, pause = {0, 10000000};
    do {
      nanosleep(&pause, NULL);
      clock_gettime(CLOCK_REALTIME, &now);
    } while (now.tv_sec < handler_until.tv_sec ||
             (now.tv_sec == handler_until.tv_sec && now.tv_nsec < handler_until.tv_nsec));
    clock_gettime(CLOCK_MONOTONIC, &handler_finished);
  }
}

struct queue_wait {
  mqd_t queue;
  sem_t entered;
  int sending, returned;
  struct timespec deadline, finished;
  ssize_t result;
  int error;
  char buffer[32];
};
static void* interrupted_queue_call(void* argument) {
  struct queue_wait* wait = argument;
  sem_post(&wait->entered);
  wait->result = wait->sending ? mq_timedsend(wait->queue, "restart", 8, 1, &wait->deadline)
                               : mq_timedreceive(wait->queue, wait->buffer, sizeof(wait->buffer),
                                                 NULL, &wait->deadline);
  wait->error = errno;
  clock_gettime(CLOCK_MONOTONIC, &wait->finished);
  __atomic_store_n(&wait->returned, 1, __ATOMIC_RELEASE);
  return NULL;
}

static int interruption_contract(mqd_t queue, int sending, int restart, int expires) {
  struct sigaction action = {.sa_handler = interrupted, .sa_flags = restart ? SA_RESTART : 0};
  struct queue_wait wait = {.queue = queue, .sending = sending};
  struct mq_attr attr;
  char buffer[32];
  int failed = 0;
  hold_handler = expires;
  __atomic_store_n(&interrupt_calls, 0, __ATOMIC_RELEASE);
  if (sigaction(SIGUSR2, &action, NULL) || sem_init(&wait.entered, 0, 0))
    return 1;
  if (sending) {
    for (int n = 0; n < 3; ++n) {
      if (mq_send(queue, "full", 5, 0))
        return 1;
    }
  }
  wait.deadline = deadline_after(expires ? 500 : 2000);
  handler_until = deadline_after(900);
  pthread_t worker;
  if (pthread_create(&worker, NULL, interrupted_queue_call, &wait))
    return 1;
  struct timespec gate_deadline = deadline_after(1000);
  if (sem_timedwait(&wait.entered, &gate_deadline))
    failed = 1;
  // Repeated acknowledged deliveries cover the publication-to-syscall-entry
  // window, while each call still has a bounded absolute timeout.
  for (int round = 0; !failed && round < (expires ? 1 : 3); ++round) {
    usleep(20000);
    if (__atomic_load_n(&wait.returned, __ATOMIC_ACQUIRE))
      break;
    int previous = __atomic_load_n(&interrupt_calls, __ATOMIC_ACQUIRE);
    if (pthread_kill(worker, SIGUSR2)) {
      failed = 1;
      break;
    }
    for (int n = 0; n < 100 && __atomic_load_n(&interrupt_calls, __ATOMIC_ACQUIRE) == previous; ++n)
      usleep(1000);
    if (__atomic_load_n(&interrupt_calls, __ATOMIC_ACQUIRE) == previous)
      failed = 1;
    if (restart && !expires && __atomic_load_n(&wait.returned, __ATOMIC_ACQUIRE))
      failed = 1;
  }
  if (!expires) {
    if (sending ? mq_receive(queue, buffer, sizeof(buffer), NULL) < 0
                : mq_send(queue, "restart", 8, 1) != 0)
      failed = 1;
  }
  if (pthread_join(worker, NULL))
    failed = 1;
  if (!__atomic_load_n(&interrupt_calls, __ATOMIC_ACQUIRE))
    failed = 1;
  if (!restart) {
    if (wait.result != -1 || wait.error != EINTR)
      failed = 1;
  } else if (expires) {
    long long after_handler = (wait.finished.tv_sec - handler_finished.tv_sec) * 1000000000LL +
                              wait.finished.tv_nsec - handler_finished.tv_nsec;
    if (wait.result != -1 || wait.error != ETIMEDOUT || after_handler < 0 ||
        after_handler > 300000000LL)
      failed = 1;
  } else if (wait.result != (sending ? 0 : 8) || (!sending && strcmp(wait.buffer, "restart"))) {
    failed = 1;
  }
  hold_handler = 0;
  sem_destroy(&wait.entered);
  while (!mq_getattr(queue, &attr) && attr.mq_curmsgs) {
    if (mq_receive(queue, buffer, sizeof(buffer), NULL) < 0) {
      failed = 1;
      break;
    }
  }
  if (failed)
    fprintf(stderr, "mq interruption send=%d restart=%d expired=%d result=%ld errno=%d\n", sending,
            restart, expires, (long)wait.result, wait.error);
  return failed;
}

int ipc_test_mqueue_exec(int argc, char** argv) {
  if (argc != 3)
    return 1;
  int descriptor = atoi(argv[2]);
  struct sigevent event = {.sigev_notify = SIGEV_NONE};
  struct mq_attr attr;
  alarm(5);
  if (fcntl(descriptor, F_GETFD) != 0 || mq_getattr(descriptor, &attr))
    return 2;
  if (mq_notify(descriptor, &event))
    return 3;
  return mq_notify(descriptor, NULL) || mq_close(descriptor) ? 4 : 0;
}

static int child_result(pid_t child, int killed) {
  int status;
  for (int n = 0; n < 500; ++n) {
    pid_t result = waitpid(child, &status, WNOHANG);
    if (result == child)
      return killed ? !(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL)
                    : !(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    if (result < 0 && errno != EINTR)
      return 1;
    usleep(10000);
  }
  kill(child, SIGKILL);
  waitpid(child, &status, 0);
  return 1;
}

int ipc_test_mqueues(void) {
  int failed = 0, alias = -1, child_status = 0, epoll = -1;
  mqd_t queue = (mqd_t)-1, replacement = (mqd_t)-1, read_only = (mqd_t)-1;
  char name[80], buffer[32];
  unsigned priority = 0;
  struct mq_attr attr = {.mq_maxmsg = 3, .mq_msgsize = sizeof(buffer)};
  struct mq_attr old, flags = {0};
  struct sigaction action = {0}, previous_usr1 = {0}, previous_usr2 = {0};
  int signals_installed = 0, callback_initialized = 0;
  void* bad = MAP_FAILED;
  snprintf(name, sizeof(name), "/ipc-contract-mq-%ld", (long)getpid());
  mq_unlink(name);
  alarm(30);

  queue = mq_open(name, O_CREAT | O_EXCL | O_RDWR | O_NONBLOCK, 0600, &attr);
  CHECK(queue != (mqd_t)-1);
  CHECK(fcntl(queue, F_GETFD) & FD_CLOEXEC);
  CHECK(mq_open(name, O_CREAT | O_EXCL | O_RDWR, 0600, &attr) == (mqd_t)-1 && errno == EEXIST);
  CHECK(mq_getattr(queue, &old) == 0 && old.mq_maxmsg == 3 && old.mq_msgsize == 32 &&
        old.mq_curmsgs == 0 && old.mq_flags == O_NONBLOCK);
  CHECK(mq_open("/bad/name", O_CREAT | O_RDWR, 0600, &attr) == (mqd_t)-1 && errno == EACCES);
  alias = dup(queue);
  CHECK(alias >= 0);
  CHECK(mq_setattr(alias, &flags, &old) == 0 && old.mq_flags == O_NONBLOCK);
  CHECK(!(fcntl(queue, F_GETFL) & O_NONBLOCK));
  CHECK(fcntl(alias, F_SETFL, O_NONBLOCK) == 0);
  CHECK(mq_getattr(queue, &old) == 0 && old.mq_flags == O_NONBLOCK);
  CHECK(mq_receive(queue, buffer, sizeof(buffer), NULL) == -1 && errno == EAGAIN);

  CHECK(mq_send(queue, "low", 4, 2) == 0);
  CHECK(mq_send(queue, "first", 6, 7) == 0);
  CHECK(mq_send(queue, "second", 7, 7) == 0);
  CHECK(mq_send(queue, "full", 5, 0) == -1 && errno == EAGAIN);
  CHECK(mq_send(queue, "bad", 4, 32768) == -1 && errno == EINVAL);
  CHECK(mq_receive(queue, buffer, sizeof(buffer) - 1, NULL) == -1 && errno == EMSGSIZE);
  struct pollfd pollfd = {.fd = queue, .events = POLLIN | POLLOUT};
  CHECK(poll(&pollfd, 1, 0) == 1 && (pollfd.revents & POLLIN) && !(pollfd.revents & POLLOUT));
  epoll = epoll_create1(EPOLL_CLOEXEC);
  CHECK(epoll >= 0);
  struct epoll_event watch = {.events = EPOLLIN | EPOLLOUT, .data.u64 = 17}, ready;
  CHECK(epoll_ctl(epoll, EPOLL_CTL_ADD, queue, &watch) == 0);
  CHECK(epoll_wait(epoll, &ready, 1, 0) == 1 && ready.data.u64 == 17 && (ready.events & EPOLLIN) &&
        !(ready.events & EPOLLOUT));

  bad = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(bad != MAP_FAILED);
  CHECK(mq_receive(queue, buffer, sizeof(buffer), bad) == -1 && errno == EFAULT);
  CHECK(mq_getattr(queue, &old) == 0 && old.mq_curmsgs == 3);
  CHECK(mq_setattr(queue, &flags, bad) == -1 && errno == EFAULT);
  CHECK(mq_getattr(queue, &old) == 0 && old.mq_flags == O_NONBLOCK);
  CHECK(mq_receive(queue, bad, sizeof(buffer), NULL) == -1 && errno == EFAULT);
  CHECK(mq_receive(queue, buffer, sizeof(buffer), &priority) == 6 && priority == 7 &&
        !strcmp(buffer, "first"));
  CHECK(mq_receive(queue, buffer, sizeof(buffer), &priority) == 7 && priority == 7 &&
        !strcmp(buffer, "second"));
  CHECK(mq_receive(queue, buffer, sizeof(buffer), &priority) == 4 && priority == 2 &&
        !strcmp(buffer, "low"));
  CHECK(epoll_wait(epoll, &ready, 1, 0) == 1 && !(ready.events & EPOLLIN) &&
        (ready.events & EPOLLOUT));
  CHECK(close(epoll) == 0);
  epoll = -1;
  CHECK(mq_send(queue, NULL, 0, 0) == 0);
  CHECK(mq_receive(queue, buffer, sizeof(buffer), &priority) == 0 && priority == 0);

  read_only = mq_open(name, O_RDONLY | O_NONBLOCK);
  CHECK(read_only != (mqd_t)-1);
  CHECK(mq_send(read_only, "x", 1, 0) == -1 && errno == EBADF);
  CHECK(mq_close(read_only) == 0);
  read_only = (mqd_t)-1;
  if (geteuid() == 0) {
    pid_t denied_child = fork();
    CHECK(denied_child >= 0);
    if (!denied_child) {
      if (setgid(65534) || setuid(65534))
        _exit(1);
      mqd_t denied = mq_open(name, O_RDONLY);
      if (denied != (mqd_t)-1 || errno != EACCES)
        _exit(2);
      _exit(mq_unlink(name) == -1 && errno == EACCES ? 0 : 3);
    }
    CHECK(waitpid(denied_child, &child_status, 0) == denied_child && WIFEXITED(child_status) &&
          WEXITSTATUS(child_status) == 0);
  }
  CHECK(mq_unlink(name) == 0);
  CHECK(mq_open(name, O_RDWR) == (mqd_t)-1 && errno == ENOENT);
  replacement = mq_open(name, O_CREAT | O_EXCL | O_RDWR | O_NONBLOCK, 0600, &attr);
  CHECK(replacement != (mqd_t)-1);
  CHECK(mq_send(queue, "unlinked", 9, 1) == 0);
  CHECK(mq_receive(replacement, buffer, sizeof(buffer), NULL) == -1 && errno == EAGAIN);
  CHECK(mq_receive(alias, buffer, sizeof(buffer), NULL) == 9 && !strcmp(buffer, "unlinked"));

  CHECK(mq_setattr(queue, &flags, NULL) == 0);
  struct timespec deadline = deadline_after(60);
  CHECK(mq_timedreceive(queue, buffer, sizeof(buffer), NULL, &deadline) == -1 &&
        errno == ETIMEDOUT);
  deadline.tv_nsec = 1000000000;
  CHECK(mq_timedreceive(queue, buffer, sizeof(buffer), NULL, &deadline) == -1 && errno == EINVAL);
  CHECK(mq_send(queue, "1", 2, 0) == 0 && mq_send(queue, "2", 2, 0) == 0 &&
        mq_send(queue, "3", 2, 0) == 0);
  deadline = deadline_after(60);
  CHECK(mq_timedsend(queue, "4", 2, 0, &deadline) == -1 && errno == ETIMEDOUT);
  for (int n = 0; n < 3; ++n)
    CHECK(mq_receive(queue, buffer, sizeof(buffer), NULL) == 2);

  action.sa_sigaction = notified;
  action.sa_flags = SA_SIGINFO;
  sigemptyset(&action.sa_mask);
  CHECK(sigaction(SIGUSR1, &action, &previous_usr1) == 0);
  action.sa_handler = interrupted;
  action.sa_flags = 0;
  CHECK(sigaction(SIGUSR2, &action, &previous_usr2) == 0);
  signals_installed = 1;
  for (int sending = 0; sending < 2; ++sending) {
    CHECK(interruption_contract(queue, sending, 0, 0) == 0);
    CHECK(interruption_contract(queue, sending, 1, 0) == 0);
    CHECK(interruption_contract(queue, sending, 1, 1) == 0);
  }

  struct sigevent event = {.sigev_notify = SIGEV_SIGNAL, .sigev_signo = SIGUSR1};
  event.sigev_value.sival_int = 0x1357;
  signal_count = 0;
  CHECK(mq_notify(queue, &event) == 0);
  CHECK(mq_notify(alias, &event) == -1 && errno == EBUSY);
  CHECK(mq_send(queue, "signal", 7, 0) == 0);
  for (int n = 0; n < 100 && !signal_count; ++n)
    usleep(1000);
  CHECK(signal_count == 1 && signal_code == SI_MESGQ && signal_value == 0x1357);
  CHECK(mq_receive(queue, buffer, sizeof(buffer), NULL) == 7);
  CHECK(mq_send(queue, "once", 5, 0) == 0 && signal_count == 1);
  CHECK(mq_receive(queue, buffer, sizeof(buffer), NULL) == 5);
  event.sigev_notify = SIGEV_NONE;
  CHECK(mq_notify(queue, &event) == 0);
  CHECK(mq_notify(alias, &event) == -1 && errno == EBUSY);
  CHECK(mq_notify(queue, NULL) == 0);

  CHECK(sem_init(&callback_done, 0, 0) == 0);
  callback_initialized = 1;
  event.sigev_notify = SIGEV_THREAD;
  event.sigev_notify_function = callback;
  event.sigev_value.sival_int = 0x2468;
  CHECK(mq_notify(queue, &event) == 0);
  CHECK(mq_send(queue, "thread", 7, 0) == 0);
  deadline = deadline_after(2000);
  CHECK(sem_timedwait(&callback_done, &deadline) == 0 && signal_value == 0x2468);
  CHECK(mq_receive(queue, buffer, sizeof(buffer), NULL) == 7);
  CHECK(mq_notify(queue, &event) == 0);
  CHECK(mq_notify(queue, NULL) == 0);
  usleep(50000);
  CHECK(sem_trywait(&callback_done) == -1 && errno == EAGAIN);

  event.sigev_notify = SIGEV_SIGNAL;
  event.sigev_signo = SIGUSR1;
  CHECK(mq_notify(queue, &event) == 0);
  CHECK(close(alias) == 0);
  alias = -1;
  CHECK(mq_notify(queue, &event) == 0);
  CHECK(mq_notify(queue, NULL) == 0);
  for (int close_original = 0; close_original < 2; ++close_original) {
    pid_t exec_child = fork();
    CHECK(exec_child >= 0);
    if (!exec_child) {
      int retained = fcntl(queue, F_DUPFD, 64);
      if (retained < 0 || (close_original && mq_close(queue)))
        _exit(1);
      struct sigevent none = {.sigev_notify = SIGEV_NONE};
      if (mq_notify(retained, &none))
        _exit(2);
      char descriptor[32];
      snprintf(descriptor, sizeof(descriptor), "%d", retained);
      execl("/applications/ipc-contract-test", "ipc-contract-test", "mq-exec", descriptor, NULL);
      _exit(127);
    }
    CHECK(child_result(exec_child, 0) == 0);
  }

  int gate[2];
  CHECK(pipe(gate) == 0);
  pid_t killed_child = fork();
  CHECK(killed_child >= 0);
  if (!killed_child) {
    close(gate[0]);
    alarm(5);
    if (write(gate[1], "r", 1) != 1)
      _exit(1);
    close(gate[1]);
    mq_receive(queue, buffer, sizeof(buffer), NULL);
    _exit(2);
  }
  close(gate[1]);
  struct pollfd gate_poll = {.fd = gate[0], .events = POLLIN};
  CHECK(poll(&gate_poll, 1, 1000) == 1 && read(gate[0], buffer, 1) == 1);
  close(gate[0]);
  usleep(50000);
  CHECK(kill(killed_child, SIGKILL) == 0 && child_result(killed_child, 1) == 0);
  sig_atomic_t before_kill_notification = signal_count;
  CHECK(mq_notify(queue, &event) == 0 && mq_send(queue, "alive", 6, 0) == 0);
  for (int n = 0; n < 100 && signal_count == before_kill_notification; ++n)
    usleep(1000);
  CHECK(signal_count == before_kill_notification + 1);
  CHECK(mq_receive(queue, buffer, sizeof(buffer), NULL) == 6 && !strcmp(buffer, "alive"));

  pid_t child = fork();
  CHECK(child >= 0);
  if (!child)
    _exit(mq_send(queue, "child", 6, 3) == 0 ? 0 : 1);
  deadline = deadline_after(2000);
  CHECK(mq_timedreceive(queue, buffer, sizeof(buffer), &priority, &deadline) == 6 &&
        priority == 3 && !strcmp(buffer, "child"));
  CHECK(waitpid(child, &child_status, 0) == child && WIFEXITED(child_status) &&
        WEXITSTATUS(child_status) == 0);

out:
  if (epoll >= 0)
    close(epoll);
  if (queue != (mqd_t)-1)
    mq_notify(queue, NULL);
  if (alias >= 0)
    close(alias);
  if (read_only != (mqd_t)-1)
    mq_close(read_only);
  if (queue != (mqd_t)-1)
    mq_close(queue);
  if (replacement != (mqd_t)-1)
    mq_close(replacement);
  mq_unlink(name);
  if (bad != MAP_FAILED)
    munmap(bad, 4096);
  if (callback_initialized)
    sem_destroy(&callback_done);
  if (signals_installed) {
    sigaction(SIGUSR1, &previous_usr1, NULL);
    sigaction(SIGUSR2, &previous_usr2, NULL);
  }
  alarm(0);
  if (!failed)
    puts("ipc-contract-test: mqueues passed");
  return failed;
}
