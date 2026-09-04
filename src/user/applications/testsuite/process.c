/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>

extern void fail(void) __attribute__((noreturn));

static volatile sig_atomic_t signalHandled = 0;
static int signalReportFd = -1;
static const char* execSignalProgram = 0;
static volatile sig_atomic_t sigchldExpectedChild = -1;
static volatile sig_atomic_t sigchldHandlerCalls = 0;
static volatile sig_atomic_t sigchldHandlerSignal = 0;
static volatile sig_atomic_t sigchldWaitResult = -1;
static volatile sig_atomic_t sigchldWaitStatus = 0;

enum {
  processStopGateAttempts = 20000,
  processStopGateQuietYields = 512,
  futexRequeueAttempts = 100000,
  futexWait = 0,
  futexWake = 1,
  futexRequeue = 3,
  futexPrivate = 128,
  futexRequeueWaiters = 3,
  condBroadcastWaiters = 4,
};

struct processStopGateProbe {
  int heartbeatFd;
  volatile int ready[2];
  volatile int stop;
  volatile int failure;
};

struct processStopGateWorker {
  struct processStopGateProbe* probe;
  int index;
};

struct futexRequeueProbe {
  int source;
  int destination;
  volatile int ready;
  volatile int woke;
  volatile int failure;
};

struct condBroadcastProbe {
  pthread_mutex_t mutex;
  pthread_cond_t condition;
  volatile int ready;
  int release;
  volatile int woke;
};

static void handleSignal(int signalNumber) {
  if (signalNumber == SIGUSR1) {
    signalHandled = 1;
    if (signalReportFd >= 0) {
      const char token = 's';
      (void)write(signalReportFd, &token, sizeof(token));
    }
  }
}

static void handleExecSignal(int signalNumber) {
  if (signalNumber != SIGUSR1 || !execSignalProgram || kill(getpid(), SIGTERM))
    _exit(123);

  char* const arguments[] = {(char*)execSignalProgram, (char*)"--exec-signal-child", 0};
  execv(execSignalProgram, arguments);
  _exit(124);
}

static void handleSigchld(int signalNumber) {
  int savedErrno = errno;
  if (!sigchldHandlerCalls) {
    int statusCode = 0;
    sigchldHandlerSignal = signalNumber;
    pid_t expectedChild = (pid_t)sigchldExpectedChild;
    sigchldWaitResult = expectedChild > 0 ? waitpid(expectedChild, &statusCode, WNOHANG) : -1;
    sigchldWaitStatus = statusCode;
  }
  ++sigchldHandlerCalls;
  errno = savedErrno;
}

static void status(const char* message) {
  puts(message);
  fflush(stdout);
}

static pid_t waitpid_bounded(pid_t child, int* statusCode, int options) {
  for (size_t attempt = 0; attempt < processStopGateAttempts; ++attempt) {
    pid_t result = waitpid(child, statusCode, options | WNOHANG);
    if (result == child)
      return result;
    if (result < 0) {
      if (errno == EINTR)
        continue;
      return result;
    }
    sched_yield();
  }
  return 0;
}

static int wait_for_atomic_value(volatile int* value, int expected) {
  for (size_t attempt = 0; attempt < futexRequeueAttempts; ++attempt) {
    if (__atomic_load_n(value, __ATOMIC_ACQUIRE) == expected)
      return 0;
    sched_yield();
  }
  return -1;
}

static void* run_futex_requeue_waiter(void* parameter) {
  struct futexRequeueProbe* probe = parameter;
  __atomic_add_fetch(&probe->ready, 1, __ATOMIC_RELEASE);

  for (;;) {
    errno = 0;
    long result = syscall(SYS_futex, &probe->source, futexWait | futexPrivate, 0, 0, 0, 0);
    if (!result)
      break;
    if (errno == EINTR)
      continue;
    __atomic_store_n(&probe->failure, 1, __ATOMIC_RELEASE);
    return 0;
  }

  __atomic_add_fetch(&probe->woke, 1, __ATOMIC_RELEASE);
  return 0;
}

static int run_raw_futex_requeue_child(void) {
  struct futexRequeueProbe probe = {0};
  pthread_t waiters[futexRequeueWaiters];

  for (size_t i = 0; i < futexRequeueWaiters; ++i) {
    if (pthread_create(&waiters[i], 0, run_futex_requeue_waiter, &probe))
      return 10;
  }
  if (wait_for_atomic_value(&probe.ready, futexRequeueWaiters))
    return 11;

  // Accumulate every waiter on the destination. This closes the publication
  // race without relying on delays or scheduler timing.
  int staged = 0;
  for (size_t attempt = 0; attempt < futexRequeueAttempts && staged < futexRequeueWaiters;
       ++attempt) {
    long result = syscall(SYS_futex, &probe.source, futexRequeue | futexPrivate, 0,
                          futexRequeueWaiters - staged, &probe.destination, 0);
    if (result < 0)
      return 12;
    staged += (int)result;
    if (staged < futexRequeueWaiters)
      sched_yield();
  }
  if (staged != futexRequeueWaiters)
    return 13;

  if (syscall(SYS_futex, &probe.destination, futexRequeue | futexPrivate, 0, futexRequeueWaiters,
              &probe.source, 0) != futexRequeueWaiters)
    return 14;

  // Plain FUTEX_REQUEUE permits equal keys and counts each selected waiter,
  // even though retagging it to the same key is a no-op.
  if (syscall(SYS_futex, &probe.source, futexRequeue | futexPrivate, 0, futexRequeueWaiters,
              &probe.source, 0) != futexRequeueWaiters)
    return 15;

#if UINTPTR_MAX > UINT32_MAX
  const uintptr_t extendedRequeueCount = ((uintptr_t)1 << 32) | 1;
  if (syscall(SYS_futex, &probe.source, futexRequeue | futexPrivate, 0, extendedRequeueCount,
              &probe.destination, 0) != 1 ||
      syscall(SYS_futex, &probe.destination, futexRequeue | futexPrivate, 0, 1, &probe.source, 0) !=
          1)
    return 16;
#endif

  errno = 0;
  if (syscall(SYS_futex, (char*)&probe.source + 1, futexRequeue | futexPrivate, 0, 0,
              &probe.destination, 0) != -1 ||
      errno != EINVAL)
    return 17;
  errno = 0;
  if (syscall(SYS_futex, &probe.source, futexRequeue | futexPrivate, 0, 0,
              (char*)&probe.destination + 1, 0) != -1 ||
      errno != EINVAL)
    return 18;
  errno = 0;
  if (syscall(SYS_futex, &probe.source, futexRequeue | futexPrivate, -1, 0, &probe.destination,
              0) != -1 ||
      errno != EINVAL)
    return 19;
  errno = 0;
  if (syscall(SYS_futex, &probe.source, futexRequeue | futexPrivate, 0, -1, &probe.destination,
              0) != -1 ||
      errno != EINVAL)
    return 20;
  errno = 0;
  if (syscall(SYS_futex, &probe.source, futexRequeue | futexPrivate, 0, 0, 0, 0) != -1 ||
      errno != EFAULT)
    return 21;

  if (syscall(SYS_futex, &probe.source, futexRequeue | futexPrivate, 1, 2, &probe.destination, 0) !=
      futexRequeueWaiters)
    return 22;
  if (syscall(SYS_futex, &probe.source, futexWake | futexPrivate, INT_MAX, 0, 0, 0) != 0)
    return 23;
  if (syscall(SYS_futex, &probe.destination, futexWake | futexPrivate, INT_MAX, 0, 0, 0) != 2)
    return 24;

  for (size_t i = 0; i < futexRequeueWaiters; ++i) {
    if (pthread_join(waiters[i], 0))
      return 25;
  }
  if (__atomic_load_n(&probe.failure, __ATOMIC_ACQUIRE) ||
      __atomic_load_n(&probe.woke, __ATOMIC_ACQUIRE) != futexRequeueWaiters)
    return 26;
  return 0;
}

static void* run_cond_broadcast_waiter(void* parameter) {
  struct condBroadcastProbe* probe = parameter;
  if (pthread_mutex_lock(&probe->mutex))
    return (void*)1;
  __atomic_add_fetch(&probe->ready, 1, __ATOMIC_RELEASE);
  while (!probe->release) {
    if (pthread_cond_wait(&probe->condition, &probe->mutex)) {
      pthread_mutex_unlock(&probe->mutex);
      return (void*)1;
    }
  }
  __atomic_add_fetch(&probe->woke, 1, __ATOMIC_RELEASE);
  if (pthread_mutex_unlock(&probe->mutex))
    return (void*)1;
  return 0;
}

static int run_cond_broadcast_child(void) {
  struct condBroadcastProbe probe = {
      .mutex = PTHREAD_MUTEX_INITIALIZER,
      .condition = PTHREAD_COND_INITIALIZER,
      .ready = 0,
      .release = 0,
      .woke = 0,
  };
  pthread_t waiters[condBroadcastWaiters];

  for (size_t i = 0; i < condBroadcastWaiters; ++i) {
    if (pthread_create(&waiters[i], 0, run_cond_broadcast_waiter, &probe))
      return 30;
  }
  if (wait_for_atomic_value(&probe.ready, condBroadcastWaiters) || pthread_mutex_lock(&probe.mutex))
    return 31;

  // Every waiter registered itself with the condition variable before it
  // released this mutex. Give that stable cohort time to enter the barrier
  // futexes which musl will hand off with FUTEX_REQUEUE.
  for (size_t i = 0; i < 128; ++i)
    sched_yield();
  probe.release = 1;
  if (pthread_cond_broadcast(&probe.condition) || pthread_mutex_unlock(&probe.mutex))
    return 32;

  for (size_t i = 0; i < condBroadcastWaiters; ++i) {
    void* result = 0;
    if (pthread_join(waiters[i], &result) || result)
      return 33;
  }
  if (__atomic_load_n(&probe.woke, __ATOMIC_ACQUIRE) != condBroadcastWaiters ||
      pthread_cond_destroy(&probe.condition) || pthread_mutex_destroy(&probe.mutex))
    return 34;
  return 0;
}

static void run_bounded_thread_child(int (*childTest)(void)) {
  pid_t child = fork();
  if (child < 0)
    fail();
  if (!child) {
    alarm(10);
    _exit(childTest());
  }

  int statusCode = 0;
  pid_t waited;
  do {
    waited = waitpid(child, &statusCode, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited != child)
    fail();
  if (!WIFEXITED(statusCode) || WEXITSTATUS(statusCode))
    fail();
}

static void* run_process_stop_gate_worker(void* parameter) {
  struct processStopGateWorker* worker = parameter;
  struct processStopGateProbe* probe = worker->probe;
  const char token = (char)('a' + worker->index);
  volatile unsigned long spin = (unsigned long)(worker->index + 1);
  __atomic_store_n(&probe->ready[worker->index], 1, __ATOMIC_RELEASE);

  while (!__atomic_load_n(&probe->stop, __ATOMIC_ACQUIRE)) {
    for (size_t i = 0; i < 4096; ++i)
      spin = (spin * 33) ^ i;

    ssize_t written = write(probe->heartbeatFd, &token, sizeof(token));
    if (written == sizeof(token) ||
        (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)))
      continue;
    __atomic_store_n(&probe->failure, 1, __ATOMIC_RELEASE);
    break;
  }

  return 0;
}

static void test_proc_self_fd(void) {
  status("Testing /proc/self/fd readlink...");

  int fd = open("/dev/null", O_RDONLY);
  if (fd < 0)
    fail();

  char procname[64];
  snprintf(procname, sizeof(procname), "/proc/self/fd/%d", fd);

  char target[PATH_MAX];
  ssize_t length = readlink(procname, target, sizeof(target));
  if (length <= 0 || (size_t)length >= sizeof(target))
    fail();
  target[length] = '\0';

  struct stat pathStat;
  struct stat descriptorStat;
  if (stat(target, &pathStat) || fstat(fd, &descriptorStat) ||
      pathStat.st_dev != descriptorStat.st_dev || pathStat.st_ino != descriptorStat.st_ino)
    fail();

  char truncated[2];
  if (readlink(procname, truncated, sizeof(truncated)) != (ssize_t)sizeof(truncated) ||
      memcmp(target, truncated, sizeof(truncated)))
    fail();

  close(fd);
  errno = 0;
  if (readlink(procname, target, sizeof(target)) != -1 || errno != ENOENT)
    fail();

  if (isatty(STDIN_FILENO)) {
    char tty[PATH_MAX];
    if (ttyname_r(STDIN_FILENO, tty, sizeof(tty)))
      fail();
  }

  status("OK");
}

static void test_vfork(void) {
  status("Testing vfork syscall compatibility...");

  pid_t child = vfork();
  if (child < 0)
    fail();
  if (!child)
    _exit(0);

  int statusCode = 0;
  if (waitpid(child, &statusCode, 0) != child || !WIFEXITED(statusCode) || WEXITSTATUS(statusCode))
    fail();

  status("OK");
}

void test_posix_spawn(const char* program) {
  status("Testing posix_spawn process isolation...");

  const pid_t parent = getpid();
  pid_t child = -1;
  char* const arguments[] = {(char*)program, (char*)"--exec-shebang-unexpected-child", 0};
  char* const environment[] = {0};
  if (posix_spawn(&child, program, 0, 0, arguments, environment) || child <= 0 || child == parent ||
      getpid() != parent)
    fail();

  int statusCode = 0;
  if (waitpid_bounded(child, &statusCode, 0) != child || !WIFEXITED(statusCode) ||
      WEXITSTATUS(statusCode) != 120 || getpid() != parent)
    fail();

  errno = 0;
  if (waitpid(child, &statusCode, WNOHANG) != -1 || errno != ECHILD)
    fail();

  pid_t missingChild = -1;
  char* const missingArguments[] = {(char*)"/posix-spawn-missing", 0};
  if (posix_spawn(&missingChild, missingArguments[0], 0, 0, missingArguments, environment) !=
          ENOENT ||
      getpid() != parent)
    fail();

  errno = 0;
  if (waitpid(-1, &statusCode, WNOHANG) != -1 || errno != ECHILD)
    fail();

  status("OK");
}

static void test_resource_compatibility(void) {
  status("Testing Linux resource compatibility syscalls...");

  struct rlimit limit = {0};
  if (getrlimit(RLIMIT_CPU, &limit) || limit.rlim_cur != RLIM_INFINITY ||
      limit.rlim_max != RLIM_INFINITY)
    fail();

  if (getrlimit(RLIMIT_RTPRIO, &limit) || limit.rlim_cur != 0 || limit.rlim_max != 0)
    fail();

  if (getrlimit(RLIMIT_RTTIME, &limit) || limit.rlim_cur != RLIM_INFINITY ||
      limit.rlim_max != RLIM_INFINITY)
    fail();

  memset(&limit, 0, sizeof(limit));
  if (syscall(SYS_prlimit64, 0, RLIMIT_NOFILE, 0, &limit) || limit.rlim_cur != 16384 ||
      limit.rlim_max != 16384)
    fail();

  memset(&limit, 0, sizeof(limit));
  if (syscall(SYS_prlimit64, getpid(), RLIMIT_NOFILE, 0, &limit) || limit.rlim_cur != 16384 ||
      limit.rlim_max != 16384)
    fail();

  if (syscall(SYS_prlimit64, 0, RLIMIT_NOFILE, 0, 0))
    fail();

  errno = 0;
  if (syscall(SYS_prlimit64, 0, RLIMIT_NOFILE, 0, (void*)UINTPTR_MAX) != -1 || errno != EFAULT)
    fail();

  errno = 0;
  if (syscall(SYS_prlimit64, 0, RLIMIT_NOFILE, (void*)UINTPTR_MAX, 0) != -1 || errno != ENOSYS)
    fail();

  errno = 0;
  if (syscall(SYS_prlimit64, 0, RLIMIT_NOFILE, &limit, 0) != -1 || errno != ENOSYS)
    fail();

  errno = 0;
  if (syscall(SYS_prlimit64, -1, RLIMIT_NOFILE, 0, &limit) != -1 || errno != ESRCH)
    fail();

  errno = 0;
  if (syscall(SYS_prlimit64, 0, -1, 0, &limit) != -1 || errno != EINVAL)
    fail();

  errno = 0;
  if (syscall(SYS_setrlimit, RLIMIT_NOFILE, (void*)UINTPTR_MAX) != -1 || errno != ENOSYS)
    fail();

  errno = 0;
  if (syscall(SYS_setrlimit, RLIMIT_NOFILE, &limit) != -1 || errno != ENOSYS)
    fail();

  memset(&limit, 0, sizeof(limit));
  if (syscall(SYS_getrlimit, RLIMIT_NOFILE, &limit) || limit.rlim_cur != 16384 ||
      limit.rlim_max != 16384)
    fail();

  if (syscall(SYS_membarrier, 0, 0, 0) != 0)
    fail();

  errno = 0;
  if (syscall(SYS_membarrier, 1, 0, 0) != -1 || errno != EINVAL)
    fail();

  errno = 0;
  if (syscall(SYS_membarrier, 0, 1, 0) != -1 || errno != EINVAL)
    fail();

  status("OK");
}

static void test_futex_requeue(void) {
  status("Testing raw futex requeue semantics...");
  run_bounded_thread_child(run_raw_futex_requeue_child);
  status("OK");
}

static void test_cond_broadcast(void) {
  status("Testing pthread condition broadcast requeue...");
  run_bounded_thread_child(run_cond_broadcast_child);
  status("OK");
}

static void test_signal_return(void) {
  status("Testing signal handler return...");

  struct sigaction action = {0};
  action.sa_handler = handleSignal;
  if (sigemptyset(&action.sa_mask) || sigaction(SIGUSR1, &action, 0) || kill(getpid(), SIGUSR1) ||
      !signalHandled || signal(SIGUSR1, SIG_DFL) == SIG_ERR)
    fail();

  status("OK");
}

static void test_default_signal_termination(void) {
  status("Testing default signal termination status...");

  pid_t child = fork();
  if (child < 0)
    fail();
  if (!child) {
    if (signal(SIGUSR1, SIG_DFL) == SIG_ERR || kill(getpid(), SIGUSR1))
      _exit(126);
    _exit(127);
  }

  int statusCode = 0;
  if (waitpid(child, &statusCode, 0) != child || !WIFSIGNALED(statusCode) ||
      WTERMSIG(statusCode) != SIGUSR1)
    fail();

  status("OK");
}

static void test_sigchld_wait_status(void) {
  status("Testing SIGCHLD wait status publication...");

  sigset_t sigchldSet;
  sigset_t originalMask;
  if (sigemptyset(&sigchldSet) || sigaddset(&sigchldSet, SIGCHLD) ||
      sigprocmask(SIG_BLOCK, &sigchldSet, &originalMask))
    fail();

  struct sigaction action = {0};
  struct sigaction previousAction = {0};
  action.sa_handler = handleSigchld;
  if (sigemptyset(&action.sa_mask) || sigaction(SIGCHLD, &action, &previousAction)) {
    sigprocmask(SIG_SETMASK, &originalMask, 0);
    fail();
  }

  int gate[2];
  if (pipe(gate)) {
    sigaction(SIGCHLD, &previousAction, 0);
    sigprocmask(SIG_SETMASK, &originalMask, 0);
    fail();
  }

  sigchldExpectedChild = -1;
  sigchldHandlerCalls = 0;
  sigchldHandlerSignal = 0;
  sigchldWaitResult = -1;
  sigchldWaitStatus = 0;

  pid_t child = fork();
  if (!child) {
    close(gate[1]);
    char token = 0;
    ssize_t received;
    do {
      received = read(gate[0], &token, sizeof(token));
    } while (received < 0 && errno == EINTR);
    close(gate[0]);
    _exit(received == sizeof(token) && token == 'x' ? 37 : 125);
  }

  int failed = child < 0;
  close(gate[0]);
  sigchldExpectedChild = child;

  sigset_t deliveryMask = originalMask;
  if (sigdelset(&deliveryMask, SIGCHLD) || sigprocmask(SIG_SETMASK, &deliveryMask, 0))
    failed = 1;

  if (child > 0) {
    const char token = 'x';
    ssize_t written;
    do {
      written = write(gate[1], &token, sizeof(token));
    } while (written < 0 && errno == EINTR);
    if (written != sizeof(token))
      failed = 1;
  }
  close(gate[1]);

  if (child > 0) {
    for (size_t attempt = 0; attempt < 1000 && !sigchldHandlerCalls; ++attempt)
      sched_yield();
  }

  if (sigprocmask(SIG_BLOCK, &sigchldSet, 0))
    failed = 1;

  int handlerStatus = (int)sigchldWaitStatus;
  if (child <= 0 || sigchldHandlerCalls != 1 || sigchldHandlerSignal != SIGCHLD ||
      (pid_t)sigchldWaitResult != child || !WIFEXITED(handlerStatus) ||
      WEXITSTATUS(handlerStatus) != 37) {
    failed = 1;
    if (child > 0 && (pid_t)sigchldWaitResult != child) {
      int rescueStatus = 0;
      pid_t rescued = 0;
      for (size_t attempt = 0; attempt < 1000 && !rescued; ++attempt) {
        errno = 0;
        rescued = waitpid(child, &rescueStatus, WNOHANG);
        if (rescued < 0 && errno == EINTR)
          rescued = 0;
        if (!rescued)
          sched_yield();
      }
      if (!rescued) {
        do {
          rescued = waitpid(child, &rescueStatus, 0);
        } while (rescued < 0 && errno == EINTR);
      }
      if (rescued != child && (rescued >= 0 || errno != ECHILD))
        failed = 1;
    }
  }

  struct sigaction ignoredAction = {0};
  ignoredAction.sa_handler = SIG_IGN;
  if (sigemptyset(&ignoredAction.sa_mask) || sigaction(SIGCHLD, &ignoredAction, 0))
    failed = 1;
  sigchldExpectedChild = -1;
  if (sigaction(SIGCHLD, &previousAction, 0))
    failed = 1;
  if (sigprocmask(SIG_SETMASK, &originalMask, 0))
    failed = 1;

  if (failed)
    fail();
  status("OK");
}

int process_exec_signal_child(void) {
  struct sigaction action = {0};
  if (sigaction(SIGUSR1, 0, &action) || action.sa_handler != SIG_DFL ||
      sigaction(SIGTERM, 0, &action) || action.sa_handler != SIG_DFL ||
      sigaction(SIGUSR2, 0, &action) || action.sa_handler != SIG_IGN ||
      (action.sa_flags & SA_RESTART) || sigismember(&action.sa_mask, SIGCHLD) != 0)
    return 125;

  stack_t alternate = {0};
  if (sigaltstack(0, &alternate) || !(alternate.ss_flags & SS_DISABLE))
    return 126;

  sigset_t currentMask;
  if (sigprocmask(SIG_SETMASK, 0, &currentMask) || sigismember(&currentMask, SIGUSR1) != 1 ||
      sigismember(&currentMask, SIGTERM) != 1)
    return 127;

  sigset_t terminate;
  if (sigemptyset(&terminate) || sigaddset(&terminate, SIGTERM) ||
      sigprocmask(SIG_UNBLOCK, &terminate, 0))
    return 128;

  for (size_t attempt = 0; attempt < 1000; ++attempt)
    sched_yield();
  return 129;
}

static void test_exec_signal_state(const char* program) {
  status("Testing exec signal state replacement...");

  pid_t child = fork();
  if (child < 0)
    fail();
  if (!child) {
    char alternateMemory[SIGSTKSZ];
    stack_t alternate = {0};
    alternate.ss_sp = alternateMemory;
    alternate.ss_size = sizeof(alternateMemory);
    if (sigaltstack(&alternate, 0))
      _exit(120);

    struct sigaction ignored = {0};
    ignored.sa_handler = SIG_IGN;
    ignored.sa_flags = SA_RESTART;
    if (sigemptyset(&ignored.sa_mask) || sigaddset(&ignored.sa_mask, SIGCHLD) ||
        sigaction(SIGUSR2, &ignored, 0))
      _exit(121);

    struct sigaction pending = {0};
    pending.sa_handler = handleSignal;
    if (sigemptyset(&pending.sa_mask) || sigaction(SIGTERM, &pending, 0))
      _exit(122);

    struct sigaction invoke = {0};
    invoke.sa_handler = handleExecSignal;
    invoke.sa_flags = SA_ONSTACK;
    if (sigemptyset(&invoke.sa_mask) || sigaddset(&invoke.sa_mask, SIGTERM) ||
        sigaction(SIGUSR1, &invoke, 0))
      _exit(123);

    execSignalProgram = program;
    if (raise(SIGUSR1))
      _exit(124);
    _exit(125);
  }

  int statusCode = 0;
  if (waitpid(child, &statusCode, 0) != child || !WIFSIGNALED(statusCode) ||
      WTERMSIG(statusCode) != SIGTERM)
    fail();

  status("OK");
}

static int write_exec_image(const char* path, const unsigned char* image, size_t imageSize) {
  int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0700);
  if (fd < 0)
    return -1;

  size_t written = 0;
  while (written < imageSize) {
    ssize_t result = write(fd, image + written, imageSize - written);
    if (result < 0 && errno == EINTR)
      continue;
    if (result <= 0) {
      close(fd);
      unlink(path);
      return -1;
    }
    written += result;
  }

  if (close(fd) || chmod(path, 0700)) {
    unlink(path);
    return -1;
  }
  return 0;
}

static int write_exec_fixture(const char* path, uintptr_t loadAddress, const char* interpreter,
                              int duplicateInterpreter) {
  unsigned char image[512] = {0};
  Elf64_Ehdr header = {0};
  Elf64_Phdr programHeaders[3] = {0};
  const size_t programHeaderCount = 1 + (interpreter ? 1 : 0) + duplicateInterpreter;
  const size_t interpreterOffset = sizeof(header) + sizeof(programHeaders);
  const size_t interpreterSize = interpreter ? strlen(interpreter) + 1 : 0;

  if (interpreterSize > sizeof(image) - interpreterOffset)
    return -1;

  memcpy(header.e_ident, ELFMAG, SELFMAG);
  header.e_ident[EI_CLASS] = ELFCLASS64;
  header.e_ident[EI_DATA] = ELFDATA2LSB;
  header.e_ident[EI_VERSION] = EV_CURRENT;
  header.e_type = ET_EXEC;
  header.e_machine = EM_X86_64;
  header.e_version = EV_CURRENT;
  header.e_entry = loadAddress;
  header.e_phoff = sizeof(header);
  header.e_ehsize = sizeof(header);
  header.e_phentsize = sizeof(programHeaders[0]);
  header.e_phnum = programHeaderCount;

  programHeaders[0].p_type = PT_LOAD;
  programHeaders[0].p_flags = PF_R | PF_X;
  programHeaders[0].p_offset = 0;
  programHeaders[0].p_vaddr = loadAddress;
  programHeaders[0].p_paddr = programHeaders[0].p_vaddr;
  programHeaders[0].p_filesz = sizeof(image);
  programHeaders[0].p_memsz = sizeof(image);
  programHeaders[0].p_align = 4096;

  if (interpreter) {
    programHeaders[1].p_type = PT_INTERP;
    programHeaders[1].p_flags = PF_R;
    programHeaders[1].p_offset = interpreterOffset;
    programHeaders[1].p_filesz = interpreterSize;
    programHeaders[1].p_memsz = interpreterSize;
    programHeaders[1].p_align = 1;

    if (duplicateInterpreter)
      programHeaders[2] = programHeaders[1];
  }

  memcpy(image, &header, sizeof(header));
  memcpy(image + sizeof(header), programHeaders, programHeaderCount * sizeof(programHeaders[0]));
  if (interpreter)
    memcpy(image + interpreterOffset, interpreter, interpreterSize);

  return write_exec_image(path, image, sizeof(image));
}

static int write_partial_bss_fixture(const char* path) {
  static const size_t imageSize = 4096;
  static const size_t fileSize = 0x200;
  static const size_t memorySize = 0x300;
  static const size_t entryOffset = 0x180;
  static const size_t probeOffset = 0x380;
  static const uintptr_t loadAddress = 0x400000;
  static const unsigned char code[] = {
      0x0f, 0xb6, 0x3d, 0xf9, 0x01, 0x00, 0x00,  // movzbl probe(%rip), %edi
      0xb8, 0xe7, 0x00, 0x00, 0x00,              // mov $231, %eax
      0x0f, 0x05,                                // syscall
  };

  unsigned char image[imageSize];
  memset(image, 0, sizeof(image));
  Elf64_Ehdr* header = (Elf64_Ehdr*)image;
  Elf64_Phdr* programHeader = (Elf64_Phdr*)(image + sizeof(*header));

  memcpy(header->e_ident, ELFMAG, SELFMAG);
  header->e_ident[EI_CLASS] = ELFCLASS64;
  header->e_ident[EI_DATA] = ELFDATA2LSB;
  header->e_ident[EI_VERSION] = EV_CURRENT;
  header->e_type = ET_EXEC;
  header->e_machine = EM_X86_64;
  header->e_version = EV_CURRENT;
  header->e_entry = loadAddress + entryOffset;
  header->e_phoff = sizeof(*header);
  header->e_ehsize = sizeof(*header);
  header->e_phentsize = sizeof(*programHeader);
  header->e_phnum = 1;

  programHeader->p_type = PT_LOAD;
  programHeader->p_flags = PF_R | PF_X;
  programHeader->p_offset = 0;
  programHeader->p_vaddr = loadAddress;
  programHeader->p_paddr = loadAddress;
  programHeader->p_filesz = fileSize;
  programHeader->p_memsz = memorySize;
  programHeader->p_align = 4096;

  memcpy(image + entryOffset, code, sizeof(code));
  image[probeOffset] = 73;
  return write_exec_image(path, image, sizeof(image));
}

static void expect_exec_failure(const char* path, int expectedErrno, int failureBase) {
  pid_t child = fork();
  if (child < 0)
    fail();
  if (!child) {
    char* const arguments[] = {(char*)path, 0};
    errno = 0;
    if (execv(path, arguments) != -1 || errno != expectedErrno)
      _exit(failureBase);

    struct sigaction action = {0};
    action.sa_handler = handleSignal;
    signalHandled = 0;
    if (sigemptyset(&action.sa_mask) || sigaction(SIGUSR1, &action, 0) || raise(SIGUSR1) ||
        !signalHandled)
      _exit(failureBase + 1);
    _exit(0);
  }

  int statusCode = 0;
  if (waitpid(child, &statusCode, 0) != child || !WIFEXITED(statusCode) || WEXITSTATUS(statusCode))
    fail();
}

static void test_exec_partial_page_bss(void) {
  status("Testing exec partial-page BSS zeroing...");

  char path[PATH_MAX];
  snprintf(path, sizeof(path), "/tmp/exec-partial-bss-%d", getpid());
  if (write_partial_bss_fixture(path))
    fail();

  pid_t child = fork();
  if (child < 0)
    fail();
  if (!child) {
    char* const arguments[] = {path, 0};
    execv(path, arguments);
    _exit(120);
  }

  int statusCode = 0;
  if (waitpid(child, &statusCode, 0) != child || unlink(path) || !WIFEXITED(statusCode) ||
      WEXITSTATUS(statusCode))
    fail();

  status("OK");
}

static void test_exec_failure_boundary(void) {
  status("Testing exec failure boundary...");

  char missingInterpreter[PATH_MAX];
  char invalidLoad[PATH_MAX];
  char malformedInterpreter[PATH_MAX];
  char malformedInterpreterTarget[PATH_MAX];
  char duplicateInterpreter[PATH_MAX];
  char missingInterpreterTarget[PATH_MAX];
  snprintf(missingInterpreter, sizeof(missingInterpreter), "/tmp/exec-missing-interpreter-%d",
           getpid());
  snprintf(invalidLoad, sizeof(invalidLoad), "/tmp/exec-invalid-load-%d", getpid());
  snprintf(malformedInterpreter, sizeof(malformedInterpreter), "/tmp/exec-malformed-interp-%d",
           getpid());
  snprintf(malformedInterpreterTarget, sizeof(malformedInterpreterTarget),
           "/tmp/exec-malformed-interp-target-%d", getpid());
  snprintf(duplicateInterpreter, sizeof(duplicateInterpreter), "/tmp/exec-duplicate-interp-%d",
           getpid());
  snprintf(missingInterpreterTarget, sizeof(missingInterpreterTarget),
           "/tmp/exec-missing-interp-target-%d", getpid());

  unlink(missingInterpreterTarget);
  if (write_exec_fixture(missingInterpreter, 0x400000, missingInterpreterTarget, 0))
    fail();
  expect_exec_failure(missingInterpreter, ENOENT, 120);
  if (unlink(missingInterpreter))
    fail();

  if (write_exec_fixture(invalidLoad, 0, 0, 0))
    fail();
  expect_exec_failure(invalidLoad, ENOEXEC, 122);
  if (unlink(invalidLoad))
    fail();

  static const char malformedContents[] = "not an ELF interpreter";
  int malformedFd = open(malformedInterpreterTarget, O_CREAT | O_TRUNC | O_WRONLY, 0700);
  if (malformedFd < 0)
    fail();
  if (write(malformedFd, malformedContents, sizeof(malformedContents)) !=
          sizeof(malformedContents) ||
      close(malformedFd))
    fail();
  if (chmod(malformedInterpreterTarget, 0700) ||
      write_exec_fixture(malformedInterpreter, 0x400000, malformedInterpreterTarget, 0))
    fail();
  expect_exec_failure(malformedInterpreter, ELIBBAD, 124);
  if (unlink(malformedInterpreter) || unlink(malformedInterpreterTarget))
    fail();

  if (write_exec_fixture(duplicateInterpreter, 0x400000, missingInterpreterTarget, 1))
    fail();
  expect_exec_failure(duplicateInterpreter, EINVAL, 126);
  if (unlink(duplicateInterpreter))
    fail();

  status("OK");
}

static void test_wait_stop_continue(void) {
  status("Testing stopped and continued wait status...");

  int gate[2];
  int signalReport[2];
  if (pipe(gate) || pipe(signalReport))
    fail();

  pid_t child = fork();
  if (child < 0)
    fail();
  if (!child) {
    close(gate[1]);
    close(signalReport[0]);
    signalReportFd = signalReport[1];
    signalHandled = 0;
    struct sigaction action = {0};
    action.sa_handler = handleSignal;
    if (sigemptyset(&action.sa_mask) || sigaction(SIGUSR1, &action, 0) || raise(SIGSTOP))
      _exit(125);

    signalReportFd = -1;
    close(signalReport[1]);
    if (!signalHandled)
      _exit(124);

    char token;
    if (read(gate[0], &token, sizeof(token)) != sizeof(token))
      _exit(126);
    close(gate[0]);
    _exit(0);
  }

  close(gate[0]);
  close(signalReport[1]);
  int reportFlags = fcntl(signalReport[0], F_GETFL);
  if (reportFlags < 0 || fcntl(signalReport[0], F_SETFL, reportFlags | O_NONBLOCK) < 0)
    fail();
  int statusCode = 0;
  if (waitpid(child, &statusCode, WUNTRACED) != child || !WIFSTOPPED(statusCode) ||
      WSTOPSIG(statusCode) != SIGSTOP)
    fail();

  if (kill(child, SIGUSR1))
    fail();
  char signalToken = 0;
  for (size_t attempt = 0; attempt < 1000; ++attempt) {
    errno = 0;
    ssize_t received = read(signalReport[0], &signalToken, sizeof(signalToken));
    if (received >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
      fail();
    sched_yield();
  }

  if (kill(child, SIGCONT) || waitpid(child, &statusCode, WCONTINUED) != child ||
      !WIFCONTINUED(statusCode))
    fail();

  ssize_t received = -1;
  for (size_t attempt = 0; attempt < 1000 && received < 0; ++attempt) {
    errno = 0;
    received = read(signalReport[0], &signalToken, sizeof(signalToken));
    if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
      fail();
    sched_yield();
  }
  if (received != sizeof(signalToken) || signalToken != 's' || close(signalReport[0]))
    fail();

  const char token = 'x';
  if (write(gate[1], &token, sizeof(token)) != sizeof(token) || close(gate[1]))
    fail();
  if (waitpid(child, &statusCode, 0) != child || !WIFEXITED(statusCode) || WEXITSTATUS(statusCode))
    fail();

  status("OK");
}

static int wait_for_process_stop_gate_quiet(int heartbeatFd) {
  size_t quietYields = 0;
  for (size_t attempt = 0; attempt < processStopGateAttempts; ++attempt) {
    char heartbeats[128];
    errno = 0;
    ssize_t received = read(heartbeatFd, heartbeats, sizeof(heartbeats));
    if (received > 0) {
      quietYields = 0;
    } else if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      if (++quietYields == processStopGateQuietYields)
        return 0;
    } else if (received < 0 && errno == EINTR) {
      continue;
    } else {
      return -1;
    }
    sched_yield();
  }
  return -1;
}

static int wait_for_process_stop_gate_heartbeat(int heartbeatFd) {
  for (size_t attempt = 0; attempt < processStopGateAttempts; ++attempt) {
    char heartbeat;
    errno = 0;
    ssize_t received = read(heartbeatFd, &heartbeat, sizeof(heartbeat));
    if (received == sizeof(heartbeat))
      return 0;
    if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
      return -1;
    if (!received)
      return -1;
    sched_yield();
  }
  return -1;
}

static void test_multithreaded_process_stop_gate(void) {
  status("Testing multithreaded process stop gate...");

  int heartbeat[2];
  int release[2];
  if (pipe(heartbeat) || pipe(release))
    fail();

  pid_t child = fork();
  if (child < 0)
    fail();
  if (!child) {
    close(heartbeat[0]);
    close(release[1]);

    int flags = fcntl(heartbeat[1], F_GETFL);
    if (flags < 0 || fcntl(heartbeat[1], F_SETFL, flags | O_NONBLOCK) < 0)
      _exit(120);

    struct processStopGateProbe probe = {
        .heartbeatFd = heartbeat[1],
        .ready = {0, 0},
        .stop = 0,
        .failure = 0,
    };
    struct processStopGateWorker workers[2] = {
        {&probe, 0},
        {&probe, 1},
    };
    pthread_t threads[2];
    if (pthread_create(&threads[0], 0, run_process_stop_gate_worker, &workers[0]))
      _exit(121);
    if (pthread_create(&threads[1], 0, run_process_stop_gate_worker, &workers[1])) {
      __atomic_store_n(&probe.stop, 1, __ATOMIC_RELEASE);
      pthread_join(threads[0], 0);
      _exit(122);
    }

    size_t attempt = 0;
    while ((!__atomic_load_n(&probe.ready[0], __ATOMIC_ACQUIRE) ||
            !__atomic_load_n(&probe.ready[1], __ATOMIC_ACQUIRE)) &&
           attempt++ < processStopGateAttempts) {
      sched_yield();
    }
    if (!__atomic_load_n(&probe.ready[0], __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&probe.ready[1], __ATOMIC_ACQUIRE) || raise(SIGSTOP)) {
      __atomic_store_n(&probe.stop, 1, __ATOMIC_RELEASE);
      pthread_join(threads[0], 0);
      pthread_join(threads[1], 0);
      _exit(123);
    }

    char token = 0;
    ssize_t received;
    do {
      received = read(release[0], &token, sizeof(token));
    } while (received < 0 && errno == EINTR);
    __atomic_store_n(&probe.stop, 1, __ATOMIC_RELEASE);
    const int firstJoin = pthread_join(threads[0], 0);
    const int secondJoin = pthread_join(threads[1], 0);
    close(release[0]);
    close(heartbeat[1]);
    _exit(received == sizeof(token) && token == 'x' && !firstJoin && !secondJoin &&
                  !__atomic_load_n(&probe.failure, __ATOMIC_ACQUIRE)
              ? 0
              : 124);
  }

  close(heartbeat[1]);
  close(release[0]);
  int failed = 0;
  int flags = fcntl(heartbeat[0], F_GETFL);
  if (flags < 0 || fcntl(heartbeat[0], F_SETFL, flags | O_NONBLOCK) < 0)
    failed = 1;

  int statusCode = 0;
  pid_t waited = waitpid_bounded(child, &statusCode, WUNTRACED);
  if (waited != child || !WIFSTOPPED(statusCode) || WSTOPSIG(statusCode) != SIGSTOP)
    failed = 1;

  if (!failed && wait_for_process_stop_gate_quiet(heartbeat[0]))
    failed = 1;

  if (!failed &&
      (kill(child, SIGCONT) || waitpid_bounded(child, &statusCode, WCONTINUED) != child ||
       !WIFCONTINUED(statusCode) || wait_for_process_stop_gate_heartbeat(heartbeat[0]))) {
    failed = 1;
  }

  const char token = 'x';
  if (!failed && write(release[1], &token, sizeof(token)) != sizeof(token))
    failed = 1;
  close(release[1]);

  waited = waitpid_bounded(child, &statusCode, 0);
  if (waited != child || !WIFEXITED(statusCode) || WEXITSTATUS(statusCode))
    failed = 1;

  if (waited != child) {
    (void)kill(child, SIGCONT);
    (void)kill(child, SIGKILL);
    (void)waitpid_bounded(child, &statusCode, 0);
  }
  close(heartbeat[0]);

  if (failed)
    fail();
  status("OK");
}

static void test_stopped_process_sigkill(void) {
  status("Testing SIGKILL of stopped process...");

  pid_t child = fork();
  if (child < 0)
    fail();
  if (!child) {
    if (raise(SIGSTOP))
      _exit(125);
    _exit(126);
  }

  int failed = 0;
  int statusCode = 0;
  pid_t waited = waitpid_bounded(child, &statusCode, WUNTRACED);
  if (waited != child || !WIFSTOPPED(statusCode) || WSTOPSIG(statusCode) != SIGSTOP)
    failed = 1;

  if (!failed && kill(child, SIGKILL))
    failed = 1;

  waited = waitpid_bounded(child, &statusCode, 0);
  if (waited != child || !WIFSIGNALED(statusCode) || WTERMSIG(statusCode) != SIGKILL)
    failed = 1;

  if (waited != child) {
    (void)kill(child, SIGKILL);
    (void)kill(child, SIGCONT);
    (void)waitpid_bounded(child, &statusCode, 0);
  }

  if (failed)
    fail();
  status("OK");
}

static void test_thread_signal_syscalls(void) {
  status("Testing thread-directed signal syscalls...");

  struct sigaction action = {0};
  struct sigaction previousAction = {0};
  action.sa_handler = handleSignal;
  if (sigemptyset(&action.sa_mask) || sigaction(SIGUSR1, &action, &previousAction))
    fail();

  long tid = syscall(SYS_gettid);
  if (tid <= 0 || syscall(SYS_tkill, tid, 0) || syscall(SYS_tgkill, getpid(), tid, 0))
    fail();

  signalHandled = 0;
  if (syscall(SYS_tkill, tid, SIGUSR1))
    fail();
  for (size_t attempt = 0; attempt < 1000 && !signalHandled; ++attempt)
    sched_yield();
  if (!signalHandled)
    fail();

  signalHandled = 0;
  if (syscall(SYS_tgkill, getpid(), tid, SIGUSR1))
    fail();
  for (size_t attempt = 0; attempt < 1000 && !signalHandled; ++attempt)
    sched_yield();
  if (!signalHandled)
    fail();

  signalHandled = 0;
  if (raise(SIGUSR1) || !signalHandled)
    fail();

  errno = 0;
  if (syscall(SYS_tkill, 0, 0) != -1 || errno != EINVAL)
    fail();
  errno = 0;
  if (syscall(SYS_tgkill, 0, tid, 0) != -1 || errno != EINVAL)
    fail();
  errno = 0;
  if (syscall(SYS_tgkill, getpid(), 0, 0) != -1 || errno != EINVAL)
    fail();
  errno = 0;
  if (syscall(SYS_tkill, INT_MAX, 32) != -1 || errno != ESRCH)
    fail();
  errno = 0;
  if (syscall(SYS_tkill, tid, 32) != -1 || errno != EINVAL)
    fail();
  errno = 0;
  if (syscall(SYS_tgkill, getpid(), INT_MAX, 32) != -1 || errno != ESRCH)
    fail();
  errno = 0;
  if (syscall(SYS_tgkill, INT_MAX, tid, 32) != -1 || errno != ESRCH)
    fail();

  if (sigaction(SIGUSR1, &previousAction, 0))
    fail();
  status("OK");
}

static void test_sigsuspend(void) {
  status("Testing sigsuspend temporary mask...");

  struct sigaction action = {0};
  struct sigaction previousAction = {0};
  action.sa_handler = handleSignal;
  if (sigemptyset(&action.sa_mask) || sigaction(SIGUSR1, &action, &previousAction))
    fail();

  sigset_t blocked;
  sigset_t original;
  sigset_t temporary;
  if (sigemptyset(&blocked) || sigaddset(&blocked, SIGUSR1) ||
      sigprocmask(SIG_BLOCK, &blocked, &original))
    fail();
  temporary = original;
  if (sigdelset(&temporary, SIGUSR1))
    fail();

  signalHandled = 0;
  if (kill(getpid(), SIGUSR1))
    fail();
  errno = 0;
  const int result = sigsuspend(&temporary);
  const int suspendError = errno;

  sigset_t restored;
  if (sigprocmask(SIG_SETMASK, 0, &restored) || sigprocmask(SIG_SETMASK, &original, 0) ||
      sigaction(SIGUSR1, &previousAction, 0))
    fail();
  if (result != -1 || suspendError != EINTR || !signalHandled ||
      sigismember(&restored, SIGUSR1) != 1)
    fail();

  status("OK");
}

void test_process(const char* program) {
  printf("Testing process compatibility...\n");
  test_proc_self_fd();
  test_vfork();
  test_posix_spawn(program);
  test_resource_compatibility();
  test_futex_requeue();
  test_cond_broadcast();
  test_signal_return();
  test_default_signal_termination();
  test_sigchld_wait_status();
  test_exec_signal_state(program);
  test_exec_partial_page_bss();
  test_exec_failure_boundary();
  test_wait_stop_continue();
  test_multithreaded_process_stop_gate();
  test_stopped_process_sigkill();
  test_thread_signal_syscalls();
  test_sigsuspend();
}
