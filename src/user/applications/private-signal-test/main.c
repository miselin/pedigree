#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <sys/membarrier.h>
#include <sys/syscall.h>
#include <sys/wait.h>

enum {
  musl_sig_timer = 32,
  musl_sig_cancel = 33,
  musl_sig_synccall = 34,
  first_realtime_signal = 35,
  last_realtime_signal = 64,
  first_unsupported_signal = 65,
  kernel_sigset_size = 8,
  wait_attempts = 100000,
};

struct kernel_sigaction {
  uint64_t handler;
  uint64_t flags;
  uint64_t restorer;
  uint64_t mask;
};

_Static_assert(sizeof(struct kernel_sigaction) == 32,
               "Linux amd64 kernel sigaction must be 32 bytes");

static int wait_for_value(volatile int* value) {
  for (size_t attempt = 0; attempt < wait_attempts; ++attempt) {
    if (__atomic_load_n(value, __ATOMIC_ACQUIRE)) {
      return 0;
    }
    sched_yield();
  }
  return -1;
}

static int raw_signal_contract(void) {
  alarm(10);

  if (SIGRTMIN != first_realtime_signal || SIGRTMAX != last_realtime_signal) {
    return 1;
  }
  sigset_t public_set;
  if (sigemptyset(&public_set)) {
    return 2;
  }
  for (int signal = musl_sig_timer; signal <= musl_sig_synccall; ++signal) {
    errno = 0;
    if (sigaddset(&public_set, signal) != -1 || errno != EINVAL) {
      return 3;
    }
    errno = 0;
    if (sigdelset(&public_set, signal) != -1 || errno != EINVAL) {
      return 4;
    }
  }
  if (sigfillset(&public_set)) {
    return 5;
  }
  for (int signal = musl_sig_timer; signal <= musl_sig_synccall; ++signal) {
    if (sigismember(&public_set, signal)) {
      return 6;
    }
  }

  for (int signal = musl_sig_timer; signal <= musl_sig_synccall; ++signal) {
    struct sigaction public_action = {0};
    errno = 0;
    if (sigaction(signal, 0, &public_action) != -1 || errno != EINVAL) {
      return 10 + signal - musl_sig_timer;
    }

    struct kernel_sigaction ignored = {
        .handler = 1,
        .mask = UINT64_C(1) << (SIGUSR1 - 1),
    };
    struct kernel_sigaction previous = {
        .handler = UINT64_MAX,
        .flags = UINT64_MAX,
        .restorer = UINT64_MAX,
        .mask = UINT64_MAX,
    };
    if (syscall(SYS_rt_sigaction, signal, &ignored, &previous, kernel_sigset_size) ||
        previous.handler || previous.flags || previous.restorer || previous.mask) {
      return 20 + signal - musl_sig_timer;
    }

    struct kernel_sigaction current = {0};
    if (syscall(SYS_rt_sigaction, signal, 0, &current, kernel_sigset_size) ||
        current.handler != ignored.handler || current.flags != ignored.flags ||
        current.restorer != ignored.restorer || current.mask != ignored.mask) {
      return 30 + signal - musl_sig_timer;
    }

    if (signal == musl_sig_timer && syscall(SYS_tkill, syscall(SYS_gettid), musl_sig_timer)) {
      return 40;
    }

    if (syscall(SYS_rt_sigaction, signal, &previous, 0, kernel_sigset_size)) {
      return 41 + signal - musl_sig_timer;
    }
  }

  errno = 0;
  if (syscall(SYS_rt_sigaction, first_unsupported_signal, 0, 0, kernel_sigset_size) != -1 ||
      errno != EINVAL) {
    return 50;
  }
  errno = 0;
  if (syscall(SYS_rt_sigaction, musl_sig_timer, 0, 0, kernel_sigset_size * 2) != -1 ||
      errno != EINVAL) {
    return 51;
  }
  errno = 0;
  if (kill(getpid(), musl_sig_timer) != -1 || errno != EINVAL) {
    return 52;
  }
  return 0;
}

static volatile int realtime_received;

static void realtime_handler(int signal) {
  __atomic_store_n(&realtime_received, signal, __ATOMIC_RELEASE);
}

static int realtime_signal_contract(void) {
  alarm(10);

  const int signals[] = {first_realtime_signal, last_realtime_signal};
  for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i) {
    const int signal = signals[i];
    struct sigaction action = {.sa_handler = realtime_handler}, previous;
    sigemptyset(&action.sa_mask);
    if (sigaction(signal, &action, &previous)) {
      return 80 + i;
    }

    sigset_t set;
    if (sigemptyset(&set) || sigaddset(&set, signal) || sigismember(&set, signal) != 1 ||
        sigdelset(&set, signal) || sigismember(&set, signal) != 0 || sigfillset(&set) ||
        sigismember(&set, signal) != 1) {
      return 82 + i;
    }

    __atomic_store_n(&realtime_received, 0, __ATOMIC_RELEASE);
    if (syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), signal) ||
        wait_for_value(&realtime_received) ||
        __atomic_load_n(&realtime_received, __ATOMIC_ACQUIRE) != signal) {
      return 84 + i;
    }
    __atomic_store_n(&realtime_received, 0, __ATOMIC_RELEASE);
    if (kill(getpid(), signal) || wait_for_value(&realtime_received) ||
        __atomic_load_n(&realtime_received, __ATOMIC_ACQUIRE) != signal) {
      return 86 + i;
    }
    if (sigaction(signal, &previous, 0)) {
      return 88 + i;
    }
  }

  errno = 0;
  if (syscall(SYS_tkill, syscall(SYS_gettid), first_unsupported_signal) != -1 || errno != EINVAL) {
    return 90;
  }
  errno = 0;
  if (syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), first_unsupported_signal) != -1 ||
      errno != EINVAL) {
    return 91;
  }
  errno = 0;
  if (kill(getpid(), first_unsupported_signal) != -1 || errno != EINVAL) {
    return 92;
  }
  return 0;
}

struct cancel_probe {
  int descriptor;
  volatile int entered;
};

static void* cancellation_target(void* parameter) {
  struct cancel_probe* probe = parameter;
  char value = 0;
  __atomic_store_n(&probe->entered, 1, __ATOMIC_RELEASE);
  (void)read(probe->descriptor, &value, sizeof(value));
  return 0;
}

static int cancellation_contract(void) {
  alarm(10);

  int descriptors[2];
  if (pipe(descriptors)) {
    return 60;
  }

  struct cancel_probe probe = {
      .descriptor = descriptors[0],
      .entered = 0,
  };
  pthread_t thread;
  if (pthread_create(&thread, 0, cancellation_target, &probe) || wait_for_value(&probe.entered)) {
    return 61;
  }

  void* result = 0;
  int error = pthread_cancel(thread);
  if (error) {
    dprintf(STDERR_FILENO, "private-signal cancel: pthread_cancel=%d\n", error);
    return 62;
  }
  error = pthread_join(thread, &result);
  if (error) {
    dprintf(STDERR_FILENO, "private-signal cancel: pthread_join=%d\n", error);
    return 63;
  }
  if (result != PTHREAD_CANCELED) {
    dprintf(STDERR_FILENO, "private-signal cancel: result=%p expected=%p\n", result,
            PTHREAD_CANCELED);
    return 64;
  }
  if (close(descriptors[0]) || close(descriptors[1])) {
    return 65;
  }
  return 0;
}

struct membarrier_probe {
  volatile int ready;
  volatile int stop;
};

static void* membarrier_target(void* parameter) {
  struct membarrier_probe* probe = parameter;
  __atomic_store_n(&probe->ready, 1, __ATOMIC_RELEASE);
  while (!__atomic_load_n(&probe->stop, __ATOMIC_ACQUIRE)) {
    sched_yield();
  }
  return 0;
}

static int membarrier_contract(void) {
  alarm(10);

  struct membarrier_probe probe = {0};
  pthread_t thread;
  if (pthread_create(&thread, 0, membarrier_target, &probe) || wait_for_value(&probe.ready)) {
    return 70;
  }

  const int result = membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0);
  const int saved_errno = errno;
  __atomic_store_n(&probe.stop, 1, __ATOMIC_RELEASE);
  if (pthread_join(thread, 0)) {
    return 71;
  }
  if (result) {
    errno = saved_errno;
    return 72;
  }
  return 0;
}

static int run_bounded(int (*test)(void)) {
  pid_t child = fork();
  if (child < 0) {
    return 255;
  }
  if (!child) {
    _exit(test());
  }

  int status = 0;
  for (size_t attempt = 0; attempt < wait_attempts; ++attempt) {
    pid_t waited = waitpid(child, &status, WNOHANG);
    if (waited == child) {
      return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    }
    if (waited < 0 && errno != EINTR) {
      return 254;
    }
    sched_yield();
  }

  (void)kill(child, SIGKILL);
  (void)waitpid(child, &status, 0);
  return 253;
}

int main(void) {
  int result = run_bounded(raw_signal_contract);
  if (result) {
    printf("PRIVATE-SIGNAL-TEST: FAIL raw=%d\n", result);
    return 1;
  }
  result = run_bounded(realtime_signal_contract);
  if (result) {
    printf("PRIVATE-SIGNAL-TEST: FAIL realtime=%d\n", result);
    return 1;
  }
  result = run_bounded(cancellation_contract);
  if (result) {
    printf("PRIVATE-SIGNAL-TEST: FAIL cancel=%d\n", result);
    return 1;
  }
  result = run_bounded(membarrier_contract);
  if (result) {
    printf("PRIVATE-SIGNAL-TEST: FAIL membarrier=%d\n", result);
    return 1;
  }

  puts("PRIVATE-SIGNAL-TEST: PASS");
  return 0;
}
