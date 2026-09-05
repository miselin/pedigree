#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "contracts.h"
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/syscall.h>
#include <sys/wait.h>

static volatile int signal_calls;
static volatile sig_atomic_t reset_observed;

static void signal_handler(int number) {
  const int saved_errno = errno;
  if (number == SIGUSR1) {
    __atomic_add_fetch(&signal_calls, 1, __ATOMIC_RELEASE);
  }
  errno = saved_errno;
}

static void reset_handler(int number) {
  const int saved_errno = errno;
  struct sigaction current;
  reset_observed =
      number == SIGUSR1 && !sigaction(SIGUSR1, 0, &current) && current.sa_handler == SIG_DFL;
  errno = saved_errno;
}

static int install_handler(int flags) {
  struct sigaction action = {.sa_handler = signal_handler, .sa_flags = flags};
  signal_calls = 0;
  return sigemptyset(&action.sa_mask) || sigaction(SIGUSR1, &action, 0);
}

enum operation {
  read_operation,
  write_operation,
  wait_operation,
  sleep_operation,
  futex_operation
};

enum { futex_wait = 0, futex_wake = 1, futex_private = 128 };

struct blocking_call {
  enum operation operation;
  int descriptor;
  pid_t child;
  char* bytes;
  size_t length;
  volatile int entered;
  volatile int returned;
  ssize_t result;
  int error;
  int status;
  int futex_word;
  struct timespec remaining;
};

static void* run_blocking_call(void* argument) {
  struct blocking_call* call = argument;
  const struct timespec delay = {.tv_sec = 3};
  errno = 0;
  __atomic_store_n(&call->entered, 1, __ATOMIC_RELEASE);
  switch (call->operation) {
    case read_operation:
      call->result = read(call->descriptor, call->bytes, call->length);
      break;
    case write_operation:
      call->result = write(call->descriptor, call->bytes, call->length);
      break;
    case wait_operation:
      call->result = waitpid(call->child, &call->status, 0);
      break;
    case sleep_operation:
      call->result = nanosleep(&delay, &call->remaining);
      break;
    case futex_operation:
      call->result = syscall(SYS_futex, &call->futex_word, futex_wait | futex_private, 0, 0, 0, 0);
      break;
  }
  call->error = errno;
  __atomic_store_n(&call->returned, 1, __ATOMIC_RELEASE);
  return 0;
}

static int interrupt_call(pthread_t worker, struct blocking_call* call, int restart) {
  if (test_wait_flag(&call->entered)) {
    return -1;
  }
  // Multiple acknowledged deliveries cover the interval between the ready
  // publication and actual syscall entry without relying on one timed signal.
  for (int round = 0; round < 8; ++round) {
    test_pause();
    if (__atomic_load_n(&call->returned, __ATOMIC_ACQUIRE)) {
      return restart ? -1 : 0;
    }
    const int previous = __atomic_load_n(&signal_calls, __ATOMIC_ACQUIRE);
    if (pthread_kill(worker, SIGUSR1)) {
      return -1;
    }
    const long long start = test_milliseconds();
    while (__atomic_load_n(&signal_calls, __ATOMIC_ACQUIRE) == previous) {
      if (__atomic_load_n(&call->returned, __ATOMIC_ACQUIRE)) {
        return restart ? -1 : 0;
      }
      if (start < 0 || test_milliseconds() - start >= 1000) {
        return -1;
      }
      test_pause();
    }
  }
  test_pause();
  return restart ? (__atomic_load_n(&call->returned, __ATOMIC_ACQUIRE) ? -1 : 0)
                 : test_wait_flag(&call->returned);
}

static int read_contract(int restart) {
  int descriptors[2];
  char bytes[2] = {0};
  if (install_handler(restart ? SA_RESTART : 0) || pipe(descriptors)) {
    return 10;
  }
  struct blocking_call call = {.operation = read_operation,
                               .descriptor = descriptors[0],
                               .bytes = bytes,
                               .length = sizeof(bytes)};
  pthread_t worker;
  if (pthread_create(&worker, 0, run_blocking_call, &call) ||
      interrupt_call(worker, &call, restart)) {
    _exit(11);
  }
  if (write(descriptors[1], "xy", 2) != 2 || test_wait_flag(&call.returned) ||
      pthread_join(worker, 0)) {
    _exit(12);
  }
  if (restart) {
    if (call.result != 2 || memcmp(bytes, "xy", 2)) {
      return 13;
    }
  } else if (call.result != -1 || call.error != EINTR || bytes[0] || bytes[1] ||
             read(descriptors[0], bytes, sizeof(bytes)) != 2 || memcmp(bytes, "xy", 2)) {
    return 14;
  }
  if (fcntl(descriptors[0], F_SETFL, O_NONBLOCK)) {
    return 15;
  }
  errno = 0;
  if (read(descriptors[0], bytes, sizeof(bytes)) != -1 || errno != EAGAIN) {
    return 16;
  }
  return close(descriptors[0]) || close(descriptors[1]) ? 17 : 0;
}

static int wait_contract(int restart) {
  int gate[2];
  if (install_handler(restart ? SA_RESTART : 0) || pipe(gate)) {
    return 20;
  }
  const pid_t child = fork();
  if (child < 0) {
    return 21;
  }
  if (!child) {
    alarm(8);
    char token;
    close(gate[1]);
    _exit(read(gate[0], &token, 1) == 1 && token == 'x' ? 42 : 43);
  }
  struct blocking_call call = {.operation = wait_operation, .child = child};
  pthread_t worker;
  int result = 0;
  if (pthread_create(&worker, 0, run_blocking_call, &call) ||
      interrupt_call(worker, &call, restart)) {
    result = 22;
  } else if (write(gate[1], "x", 1) != 1 || test_wait_flag(&call.returned) ||
             pthread_join(worker, 0)) {
    result = 23;
  } else if (restart) {
    if (call.result != child || !WIFEXITED(call.status) || WEXITSTATUS(call.status) != 42) {
      result = 24;
    }
  } else {
    int status;
    if (call.result != -1 || call.error != EINTR || waitpid(child, &status, 0) != child ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 42) {
      result = 25;
    }
  }
  if (result) {
    (void)kill(child, SIGKILL);
    (void)waitpid(child, 0, 0);
    _exit(result);
  }
  close(gate[0]);
  close(gate[1]);
  return result;
}

static int partial_write_contract(void) {
  enum { chunk = 4096, maximum_fill = 1024 * 1024 };
  char bytes[2 * chunk];
  memset(bytes, 'q', sizeof(bytes));
  int descriptors[2];
  if (install_handler(SA_RESTART) || pipe(descriptors) ||
      fcntl(descriptors[1], F_SETFL, O_NONBLOCK)) {
    return 30;
  }
  size_t filled = 0;
  while (filled < maximum_fill) {
    const ssize_t written = write(descriptors[1], bytes, chunk);
    if (written == -1 && errno == EAGAIN) {
      break;
    }
    if (written <= 0) {
      return 31;
    }
    filled += written;
  }
  if (filled < chunk || filled == maximum_fill || read(descriptors[0], bytes, chunk) != chunk ||
      fcntl(descriptors[1], F_SETFL, 0)) {
    return 32;
  }
  memset(bytes, 'x', chunk);
  memset(bytes + chunk, 'y', chunk);
  struct blocking_call call = {.operation = write_operation,
                               .descriptor = descriptors[1],
                               .bytes = bytes,
                               .length = sizeof(bytes)};
  pthread_t worker;
  if (pthread_create(&worker, 0, run_blocking_call, &call) || interrupt_call(worker, &call, 0) ||
      pthread_join(worker, 0) || call.result != chunk || !signal_calls) {
    fprintf(stderr, "partial write: result=%ld errno=%d\n", (long)call.result, call.error);
    _exit(33);
  }
  if (close(descriptors[1])) {
    return 34;
  }
  size_t received = 0;
  ssize_t count;
  while ((count = read(descriptors[0], bytes, sizeof(bytes))) > 0) {
    for (ssize_t i = 0; i < count; ++i, ++received) {
      const char expected = received < filled - chunk ? 'q' : 'x';
      if (received >= filled || bytes[i] != expected) {
        return 35;
      }
    }
  }
  return count || received != filled || close(descriptors[0]) ? 36 : 0;
}

static int sleep_contract(void) {
  if (install_handler(SA_RESTART)) {
    return 40;
  }
  struct blocking_call call = {.operation = sleep_operation};
  pthread_t worker;
  if (pthread_create(&worker, 0, run_blocking_call, &call) || interrupt_call(worker, &call, 0) ||
      pthread_join(worker, 0)) {
    _exit(41);
  }
  return call.result != -1 || call.error != EINTR || call.remaining.tv_sec < 0 ||
                 call.remaining.tv_sec > 3 || call.remaining.tv_nsec < 0 ||
                 call.remaining.tv_nsec >= 1000000000 ||
                 (!call.remaining.tv_sec && !call.remaining.tv_nsec)
             ? 42
             : 0;
}

static int futex_contract(int restart) {
  if (install_handler(restart ? SA_RESTART : 0)) {
    return 43;
  }
  struct blocking_call call = {.operation = futex_operation};
  pthread_t worker;
  if (pthread_create(&worker, 0, run_blocking_call, &call) ||
      interrupt_call(worker, &call, restart)) {
    _exit(44);
  }
  if (restart) {
    __atomic_store_n(&call.futex_word, 1, __ATOMIC_RELEASE);
    if (syscall(SYS_futex, &call.futex_word, futex_wake | futex_private, 1, 0, 0, 0) < 0) {
      _exit(45);
    }
  }
  if (test_wait_flag(&call.returned) || pthread_join(worker, 0)) {
    _exit(46);
  }
  // A restarted wait may observe the changed word before it rejoins the queue.
  return restart ? (call.result != 0 && !(call.result == -1 && call.error == EAGAIN) ? 47 : 0)
                 : (call.result != -1 || call.error != EINTR ? 48 : 0);
}

static int masked_wait_contract(const char* name) {
  sigset_t blocked, original, temporary, after;
  if (install_handler(SA_RESTART) || sigemptyset(&blocked) || sigaddset(&blocked, SIGUSR1) ||
      pthread_sigmask(SIG_BLOCK, &blocked, &original)) {
    return 50;
  }
  temporary = original;
  if (sigdelset(&temporary, SIGUSR1) || raise(SIGUSR1) || signal_calls) {
    return 51;
  }
  const struct timespec timeout = {.tv_sec = 2};
  int result, saved_errno, descriptor = -1;
  errno = 0;
  if (!strcmp(name, "ppoll-eintr")) {
    result = ppoll(0, 0, &timeout, &temporary);
  } else if (!strcmp(name, "pselect-eintr")) {
    result = pselect(0, 0, 0, 0, &timeout, &temporary);
  } else if (!strcmp(name, "sigsuspend-eintr")) {
    result = sigsuspend(&temporary);
  } else {
    descriptor = epoll_create1(EPOLL_CLOEXEC);
    if (descriptor < 0) {
      return 52;
    }
    struct epoll_event event;
    result = epoll_pwait(descriptor, &event, 1, 2000, &temporary);
  }
  saved_errno = errno;
  if (result != -1 || saved_errno != EINTR || signal_calls != 1 ||
      pthread_sigmask(SIG_SETMASK, 0, &after) || sigismember(&after, SIGUSR1) != 1 ||
      pthread_sigmask(SIG_SETMASK, &original, 0)) {
    return 53;
  }
  return descriptor >= 0 && close(descriptor) ? 54 : 0;
}

static int reset_contract(void) {
  struct sigaction action = {.sa_handler = reset_handler, .sa_flags = SA_RESETHAND};
  struct sigaction after;
  if (sigemptyset(&action.sa_mask) || sigaction(SIGUSR1, &action, 0) || raise(SIGUSR1) ||
      !reset_observed || sigaction(SIGUSR1, 0, &after) || after.sa_handler != SIG_DFL) {
    return 60;
  }
  return 0;
}

int signal_contract(const char* name) {
  if (!strncmp(name, "read-", 5)) {
    return read_contract(!strcmp(name, "read-restart"));
  }
  if (!strncmp(name, "wait-", 5)) {
    return wait_contract(!strcmp(name, "wait-restart"));
  }
  if (!strcmp(name, "partial-write")) {
    return partial_write_contract();
  }
  if (!strcmp(name, "nanosleep-eintr")) {
    return sleep_contract();
  }
  if (!strncmp(name, "futex-", 6)) {
    return futex_contract(!strcmp(name, "futex-restart"));
  }
  if (!strcmp(name, "reset-hand")) {
    return reset_contract();
  }
  return masked_wait_contract(name);
}
