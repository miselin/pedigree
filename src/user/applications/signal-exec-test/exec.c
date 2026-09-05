#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "contracts.h"
#include <sys/stat.h>
#include <sys/syscall.h>

static volatile sig_atomic_t delivered_signals;
static volatile sig_atomic_t delivered_process_signals;
static volatile sig_atomic_t delivered_private_signals;
static volatile sig_atomic_t delivered_sibling_signals;
static volatile sig_atomic_t invalid_signal_info;

struct exec_probe {
  const char* program;
  pthread_mutex_t mutex;
  pthread_cond_t condition;
  int gate[2];
  int inherited;
  int cloexec;
  int stop;
  int pending_signals;
  volatile int read_ready;
  volatile int condition_ready;
  volatile int main_ready;
  volatile int read_done;
  volatile int condition_done;
  volatile int race_start;
  volatile int clone_ready;
  volatile int signals_queued;
  volatile int sibling_mask_checked;
  long read_tid;
  long condition_tid;
};

static void* fresh_thread(void* argument) {
  return argument;
}

static void caught_signal(int number) {
  (void)number;
}

static void pending_signal(int number, siginfo_t* info, void* context) {
  const int saved_errno = errno;
  (void)context;
  if (number == SIGHUP) {
    ++delivered_sibling_signals;
    errno = saved_errno;
    return;
  }
  if (number != SIGUSR1 || !info || info->si_signo != SIGUSR1 || info->si_errno ||
      info->si_pid != getpid() || info->si_uid != getuid()) {
    invalid_signal_info = 1;
  } else if (info->si_code == SI_USER) {
    ++delivered_process_signals;
  } else if (info->si_code == SI_TKILL) {
    ++delivered_private_signals;
  } else {
    invalid_signal_info = 1;
  }
  ++delivered_signals;
  errno = saved_errno;
}

static int pending_delivery_contract(const sigset_t* original, int expect_private) {
  struct sigaction action = {.sa_sigaction = pending_signal, .sa_flags = SA_SIGINFO};
  sigset_t unblocked = *original;
  const long long start = test_milliseconds();
  if (start < 0 || sigemptyset(&action.sa_mask) || sigaction(SIGUSR1, &action, 0) ||
      sigaction(SIGHUP, &action, 0) || sigdelset(&unblocked, SIGUSR1) ||
      sigdelset(&unblocked, SIGHUP) || pthread_sigmask(SIG_SETMASK, &unblocked, 0)) {
    return 107;
  }
  const int expected = 1 + expect_private;
  while (delivered_signals < expected) {
    const long long now = test_milliseconds();
    if (now < 0 || now - start >= 3000) {
      break;
    }
    test_pause();
  }
  for (int grace = 0; grace < 4; ++grace) {
    test_pause();
  }
  if (pthread_sigmask(SIG_SETMASK, original, 0)) {
    return 108;
  }
  if (delivered_signals != expected || delivered_process_signals != 1 ||
      delivered_private_signals != expect_private || invalid_signal_info ||
      delivered_sibling_signals) {
    fprintf(stderr, "pending delivery: total=%d process=%d private=%d sibling=%d invalid=%d\n",
            delivered_signals, delivered_process_signals, delivered_private_signals,
            delivered_sibling_signals, invalid_signal_info);
    return 109;
  }
  return 0;
}

static int set_exec_signals(int blocked_signal, int pending_signals) {
  struct sigaction caught = {.sa_handler = caught_signal, .sa_flags = SA_RESTART};
  struct sigaction ignored = {.sa_handler = SIG_IGN};
  sigset_t mask;
  return sigemptyset(&caught.sa_mask) || sigemptyset(&ignored.sa_mask) ||
         sigaction(SIGUSR1, &caught, 0) || sigaction(SIGUSR2, &ignored, 0) || sigemptyset(&mask) ||
         sigaddset(&mask, blocked_signal) ||
         (pending_signals && (sigaction(SIGHUP, &caught, 0) || sigaddset(&mask, SIGUSR1) ||
                              sigaddset(&mask, SIGHUP))) ||
         pthread_sigmask(SIG_SETMASK, &mask, 0);
}

static void* blocked_read(void* argument) {
  struct exec_probe* probe = argument;
  char token = 0;
  probe->read_tid = syscall(SYS_gettid);
  __atomic_store_n(&probe->read_ready, 1, __ATOMIC_RELEASE);
  if (probe->pending_signals) {
    sigset_t mask;
    if (test_wait_flag(&probe->signals_queued) || pthread_sigmask(SIG_SETMASK, 0, &mask) ||
        sigismember(&mask, SIGHUP) != 1 || sigismember(&mask, SIGUSR1) != 1) {
      _exit(96);
    }
    __atomic_store_n(&probe->sibling_mask_checked, 1, __ATOMIC_RELEASE);
  }
  const int valid = read(probe->gate[0], &token, 1) == 1 && token == 'x';
  __atomic_store_n(&probe->read_done, valid ? 1 : -1, __ATOMIC_RELEASE);
  return 0;
}

static void* blocked_condition(void* argument) {
  struct exec_probe* probe = argument;
  if (pthread_mutex_lock(&probe->mutex)) {
    _exit(70);
  }
  probe->condition_tid = syscall(SYS_gettid);
  __atomic_store_n(&probe->condition_ready, 1, __ATOMIC_RELEASE);
  while (!probe->stop) {
    if (pthread_cond_wait(&probe->condition, &probe->mutex)) {
      _exit(71);
    }
  }
  pthread_mutex_unlock(&probe->mutex);
  __atomic_store_n(&probe->condition_done, 1, __ATOMIC_RELEASE);
  return 0;
}

static int replace_image(struct exec_probe* probe, int blocked_signal, int racing) {
  char inherited[24], cloexec[24], expected_mask[24], read_tid[24], condition_tid[24], caller[24];
  snprintf(inherited, sizeof(inherited), "%d", probe->inherited);
  snprintf(cloexec, sizeof(cloexec), "%d", probe->cloexec);
  snprintf(expected_mask, sizeof(expected_mask), "%d", blocked_signal);
  snprintf(read_tid, sizeof(read_tid), "%ld", probe->read_tid);
  snprintf(condition_tid, sizeof(condition_tid), "%ld", probe->condition_tid);
  snprintf(caller, sizeof(caller), "%ld", syscall(SYS_gettid));
  char* arguments[] = {(char*)probe->program,
                       (char*)"--exec-image",
                       inherited,
                       cloexec,
                       expected_mask,
                       read_tid,
                       condition_tid,
                       caller,
                       0};
  if (racing) {
    arguments[1] = (char*)"--exec-race-image";
    arguments[5] = 0;
  } else if (probe->pending_signals) {
    arguments[1] = (char*)"--exec-pending-image";
  }
  while (1) {
    execv(probe->program, arguments);
    if (!racing || errno != EAGAIN) {
      break;
    }
    test_pause();
  }
  fprintf(stderr, "threaded exec: execv errno=%d\n", errno);
  _exit(72);
}

static void* exec_worker(void* argument) {
  struct exec_probe* probe = argument;
  if (set_exec_signals(SIGTERM, probe->pending_signals) || test_wait_flag(&probe->main_ready) ||
      pthread_mutex_lock(&probe->mutex)) {
    _exit(73);
  }
  // The main thread publishes readiness with this mutex held, so acquiring
  // it establishes that the main thread has entered its condition wait.
  pthread_mutex_unlock(&probe->mutex);
  _exit(replace_image(probe, SIGTERM, 0));
}

static void* competing_exec(void* argument) {
  struct exec_probe* probe = argument;
  if (test_wait_flag(&probe->race_start) || test_wait_flag(&probe->clone_ready)) {
    _exit(74);
  }
  _exit(replace_image(probe, SIGUSR1, 1));
}

static void* competing_clone(void* argument) {
  struct exec_probe* probe = argument;
  if (test_wait_flag(&probe->race_start)) {
    _exit(75);
  }
  for (int attempt = 0; attempt < 16; ++attempt) {
    pthread_t worker;
    const int error = pthread_create(&worker, 0, fresh_thread, probe);
    if (error && error != EAGAIN) {
      _exit(76);
    }
    void* result = 0;
    if (!error && (pthread_join(worker, &result) || result != probe)) {
      _exit(77);
    }
    if (!error) {
      __atomic_store_n(&probe->clone_ready, 1, __ATOMIC_RELEASE);
    }
  }
  return 0;
}

static int race_contract(struct exec_probe* probe) {
  pthread_t first, second, cloner;
  if (pthread_create(&first, 0, competing_exec, probe) ||
      pthread_create(&second, 0, competing_exec, probe) ||
      pthread_create(&cloner, 0, competing_clone, probe)) {
    _exit(78);
  }
  __atomic_store_n(&probe->race_start, 1, __ATOMIC_RELEASE);
  while (1) {
    test_pause();
  }
}

static int failed_exec_contract(struct exec_probe* probe, pthread_t reader, pthread_t waiter) {
  char* const arguments[] = {(char*)probe->program, 0};
  errno = 0;
  execv("/signal-exec-test-does-not-exist", arguments);
  if (errno != ENOENT) {
    _exit(80);
  }

  char path[96];
  snprintf(path, sizeof(path), "/tmp/signal-exec-invalid-%ld", (long)getpid());
  const int fixture = open(path, O_CREAT | O_EXCL | O_WRONLY, 0700);
  if (fixture < 0) {
    _exit(81);
  }
  static const char contents[] = "not an executable\n";
  if (write(fixture, contents, sizeof(contents) - 1) != sizeof(contents) - 1 || close(fixture)) {
    unlink(path);
    _exit(82);
  }
  errno = 0;
  execv(path, arguments);
  const int error = errno;
  if (unlink(path) || error != ENOEXEC) {
    _exit(83);
  }

  struct sigaction current;
  sigset_t mask;
  if (sigaction(SIGUSR1, 0, &current) || current.sa_handler != caught_signal ||
      !(current.sa_flags & SA_RESTART) || pthread_sigmask(SIG_SETMASK, 0, &mask) ||
      sigismember(&mask, SIGUSR1) != 1 || fcntl(probe->cloexec, F_GETFD) != FD_CLOEXEC ||
      __atomic_load_n(&probe->read_done, __ATOMIC_ACQUIRE) ||
      __atomic_load_n(&probe->condition_done, __ATOMIC_ACQUIRE)) {
    _exit(84);
  }
  if (write(probe->gate[1], "x", 1) != 1 || pthread_mutex_lock(&probe->mutex)) {
    _exit(85);
  }
  probe->stop = 1;
  pthread_cond_broadcast(&probe->condition);
  pthread_mutex_unlock(&probe->mutex);
  if (test_wait_flag(&probe->read_done) || test_wait_flag(&probe->condition_done) ||
      pthread_join(reader, 0) || pthread_join(waiter, 0) || probe->read_done != 1 ||
      probe->condition_done != 1) {
    _exit(86);
  }
  return close(probe->gate[0]) || close(probe->gate[1]) || close(probe->inherited) ||
                 close(probe->cloexec) || pthread_cond_destroy(&probe->condition) ||
                 pthread_mutex_destroy(&probe->mutex)
             ? 87
             : 0;
}

static void queue_exec_signals(struct exec_probe* probe, pthread_t reader) {
  // Queue the private copy first so coalescing must retain the later process
  // signal when the original main thread retires during worker exec.
  if (pthread_kill(pthread_self(), SIGUSR1) || pthread_kill(reader, SIGUSR1) ||
      pthread_kill(reader, SIGHUP) || kill(getpid(), SIGUSR1)) {
    _exit(97);
  }
  __atomic_store_n(&probe->signals_queued, 1, __ATOMIC_RELEASE);
  if (test_wait_flag(&probe->sibling_mask_checked)) {
    _exit(98);
  }
  test_pause();
  if (__atomic_load_n(&probe->read_done, __ATOMIC_ACQUIRE)) {
    _exit(99);
  }
}

struct exit_race_probe {
  const char* program;
  volatile int exec_ready;
  volatile int exit_ready;
  volatile int start;
};

static void* race_exec_with_exit(void* argument) {
  struct exit_race_probe* probe = argument;
  char* const arguments[] = {(char*)probe->program, (char*)"--exec-exit-image", 0};
  __atomic_store_n(&probe->exec_ready, 1, __ATOMIC_RELEASE);
  if (test_wait_flag(&probe->start)) {
    _exit(110);
  }
  while (1) {
    execv(probe->program, arguments);
    if (errno != EAGAIN) {
      _exit(111);
    }
    test_pause();
  }
}

static void* race_exit_with_exec(void* argument) {
  struct exit_race_probe* probe = argument;
  __atomic_store_n(&probe->exit_ready, 1, __ATOMIC_RELEASE);
  if (test_wait_flag(&probe->start)) {
    _exit(113);
  }
  _exit(37);
}

static int exec_exit_contract(const char* program) {
  struct exit_race_probe probe = {.program = program};
  pthread_t exec_worker, exit_worker;
  if (pthread_create(&exec_worker, 0, race_exec_with_exit, &probe) ||
      pthread_create(&exit_worker, 0, race_exit_with_exec, &probe) ||
      test_wait_flag(&probe.exec_ready) || test_wait_flag(&probe.exit_ready)) {
    _exit(114);
  }
  __atomic_store_n(&probe.start, 1, __ATOMIC_RELEASE);
  while (1) {
    test_pause();
  }
}

int exec_contract(const char* program, const char* name) {
  if (!strcmp(name, "exec-exit-race")) {
    return exec_exit_contract(program);
  }
  struct exec_probe probe = {.program = program,
                             .mutex = PTHREAD_MUTEX_INITIALIZER,
                             .condition = PTHREAD_COND_INITIALIZER,
                             .pending_signals = !strncmp(name, "exec-pending-", 13)};
  if (set_exec_signals(SIGUSR1, probe.pending_signals) || pipe(probe.gate)) {
    return 90;
  }
  probe.inherited = open("/dev/null", O_RDONLY);
  probe.cloexec = open("/dev/null", O_RDONLY | O_CLOEXEC);
  if (probe.inherited < 0 || probe.cloexec < 0) {
    return 91;
  }
  pthread_t reader, waiter;
  if (pthread_create(&reader, 0, blocked_read, &probe) ||
      pthread_create(&waiter, 0, blocked_condition, &probe) || test_wait_flag(&probe.read_ready) ||
      test_wait_flag(&probe.condition_ready) || pthread_mutex_lock(&probe.mutex)) {
    _exit(92);
  }
  pthread_mutex_unlock(&probe.mutex);
  test_pause();
  if (probe.read_tid <= 0 || probe.condition_tid <= 0 ||
      __atomic_load_n(&probe.read_done, __ATOMIC_ACQUIRE) ||
      __atomic_load_n(&probe.condition_done, __ATOMIC_ACQUIRE)) {
    _exit(93);
  }
  if (!strcmp(name, "exec-failure")) {
    return failed_exec_contract(&probe, reader, waiter);
  }
  if (!strcmp(name, "exec-main") || !strcmp(name, "exec-pending-main")) {
    if (probe.pending_signals) {
      queue_exec_signals(&probe, reader);
    }
    return replace_image(&probe, SIGUSR1, 0);
  }
  if (!strcmp(name, "exec-race")) {
    return race_contract(&probe);
  }
  pthread_t worker;
  if (pthread_create(&worker, 0, exec_worker, &probe) || pthread_mutex_lock(&probe.mutex)) {
    _exit(94);
  }
  if (probe.pending_signals) {
    queue_exec_signals(&probe, reader);
  }
  __atomic_store_n(&probe.main_ready, 1, __ATOMIC_RELEASE);
  while (1) {
    if (pthread_cond_wait(&probe.condition, &probe.mutex)) {
      _exit(95);
    }
  }
}

int exec_image_contract(int argc, char* argv[]) {
  if (!strcmp(argv[1], "--exec-exit-image")) {
    return argc == 2 && syscall(SYS_gettid) == getpid() ? 0 : 112;
  }
  const int racing = !strcmp(argv[1], "--exec-race-image");
  const int pending_signals = !strcmp(argv[1], "--exec-pending-image");
  if (argc != (racing ? 5 : 8) || syscall(SYS_gettid) != getpid()) {
    return 100;
  }
  const int inherited = atoi(argv[2]);
  const int cloexec = atoi(argv[3]);
  const int blocked_signal = atoi(argv[4]);
  struct sigaction action;
  sigset_t mask;
  if (sigaction(SIGUSR1, 0, &action) || action.sa_handler != SIG_DFL ||
      sigaction(SIGUSR2, 0, &action) || action.sa_handler != SIG_IGN ||
      pthread_sigmask(SIG_SETMASK, 0, &mask) || sigismember(&mask, blocked_signal) != 1 ||
      sigismember(&mask, SIGTERM) != (blocked_signal == SIGTERM) ||
      sigismember(&mask, SIGUSR1) != (pending_signals || blocked_signal == SIGUSR1)) {
    return 101;
  }
  if (pending_signals) {
    if (sigaction(SIGHUP, 0, &action) || action.sa_handler != SIG_DFL ||
        sigismember(&mask, SIGHUP) != 1) {
      return 106;
    }
    const int delivery_result = pending_delivery_contract(&mask, blocked_signal == SIGUSR1);
    if (delivery_result) {
      return delivery_result;
    }
  }
  char byte;
  if (fcntl(inherited, F_GETFD) != 0 || read(inherited, &byte, 1) != 0) {
    return 102;
  }
  errno = 0;
  if (fcntl(cloexec, F_GETFD) != -1 || errno != EBADF) {
    return 103;
  }
  for (int i = 5; i < argc; ++i) {
    const long old_tid = atol(argv[i]);
    if (old_tid == getpid()) {
      continue;
    }
    errno = 0;
    if (syscall(SYS_tgkill, getpid(), old_tid, 0) != -1 || errno != ESRCH) {
      return 104;
    }
  }
  pthread_t worker;
  void* result = 0;
  if (pthread_create(&worker, 0, fresh_thread, &mask) || pthread_join(worker, &result) ||
      result != &mask || close(inherited)) {
    return 105;
  }
  return 0;
}
