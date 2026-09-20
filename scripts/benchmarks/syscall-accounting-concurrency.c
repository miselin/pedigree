#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>

#define WORKERS 4
#define OBSERVATIONS 20
#define TIMER_EXPIRIES 3
#define QUERY_LIMIT 4000000

struct usage {
  uint64_t user, system;
};
static struct worker {
  unsigned slot, rounds;
  uint64_t user, system;
  struct usage initial, final;
} workers[WORKERS];
static unsigned start, stop, virtual_expiries, profile_expiries;
static long expected_uid;

static void require(int valid, const char* where) {
  if (valid)
    return;
  dprintf(2, "ACCOUNTING-CONCURRENCY FAIL where=%s errno=%d\n", where, errno);
  _exit(1);
}

static void timeout(int signal_number) {
  (void)signal_number;
  static const char message[] = "ACCOUNTING-CONCURRENCY FAIL where=timeout\n";
  (void)write(2, message, sizeof(message) - 1);
  _exit(124);
}

static unsigned load(unsigned* value) {
  return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static void raw_query(void) {
  long number = SYS_getuid;
  __asm__ volatile("syscall" : "+a"(number) : : "rcx", "r11", "memory", "cc");
  require(number == expected_uid, "query-value");
}

static struct usage sample(int who) {
  struct rusage value = {0};
  require(syscall(SYS_getrusage, who, &value) == 0, "getrusage");
  require(value.ru_utime.tv_sec >= 0 && value.ru_stime.tv_sec >= 0 && value.ru_utime.tv_usec >= 0 &&
              value.ru_utime.tv_usec < 1000000 && value.ru_stime.tv_usec >= 0 &&
              value.ru_stime.tv_usec < 1000000,
          "timeval-range");
  return (struct usage){(uint64_t)value.ru_utime.tv_sec * 1000000 + value.ru_utime.tv_usec,
                        (uint64_t)value.ru_stime.tv_sec * 1000000 + value.ru_stime.tv_usec};
}

static int covers(struct usage total, struct usage lower) {
  return total.user >= lower.user && total.system >= lower.system;
}

static void pause_ms(unsigned milliseconds) {
  struct timespec delay = {milliseconds / 1000, (milliseconds % 1000) * 1000000L};
  while (nanosleep(&delay, &delay))
    require(errno == EINTR, "nanosleep");
}

static uint64_t now_us(void) {
  struct timespec value;
  require(clock_gettime(CLOCK_MONOTONIC, &value) == 0, "monotonic-clock");
  return (uint64_t)value.tv_sec * 1000000 + value.tv_nsec / 1000;
}

static void* account_worker(void* argument) {
  struct worker* worker = argument;
  worker->initial = sample(RUSAGE_THREAD);
  while (!load(&start))
    raw_query();
  uint64_t checksum = worker->slot + 1;
  struct usage previous = worker->initial;
  for (unsigned round = 1; !load(&stop); ++round) {
    if (worker->slot & 1) {
      for (unsigned i = 0; i < 65536; ++i) {
        checksum ^= checksum << 13;
        checksum ^= checksum >> 7;
        checksum ^= checksum << 17;
      }
      __asm__ volatile("" : "+r"(checksum) : : "memory");
    } else {
      for (unsigned i = 0; i < 257; ++i)
        raw_query();
    }
    struct usage current = sample(RUSAGE_THREAD);
    require(covers(current, previous), "worker-monotonic");
    previous = current;
    __atomic_store_n(&worker->user, current.user, __ATOMIC_RELEASE);
    __atomic_store_n(&worker->system, current.system, __ATOMIC_RELEASE);
    __atomic_store_n(&worker->rounds, round, __ATOMIC_RELEASE);
  }
  worker->final = sample(RUSAGE_THREAD);
  require(covers(worker->final, previous), "worker-final-monotonic");
  return NULL;
}

static void aggregate_threads(void) {
  pthread_t threads[WORKERS];
  struct usage previous = sample(RUSAGE_SELF);
  for (unsigned i = 0; i < WORKERS; ++i) {
    workers[i].slot = i;
    require(pthread_create(&threads[i], NULL, account_worker, &workers[i]) == 0, "worker-create");
  }
  __atomic_store_n(&start, 1, __ATOMIC_RELEASE);
  for (unsigned attempt = 0;; ++attempt) {
    unsigned ready = 0;
    for (unsigned i = 0; i < WORKERS; ++i)
      ready += load(&workers[i].rounds) >= 10;
    if (ready == WORKERS)
      break;
    require(attempt < 5000, "workers-progress");
    pause_ms(1);
  }
  for (unsigned observation = 0; observation < OBSERVATIONS; ++observation) {
    struct usage reported = {0};
    for (unsigned i = 0; i < WORKERS; ++i) {
      reported.user += __atomic_load_n(&workers[i].user, __ATOMIC_ACQUIRE);
      reported.system += __atomic_load_n(&workers[i].system, __ATOMIC_ACQUIRE);
    }
    // Sample after reading the workers: every reported value is already public.
    struct usage current = sample(RUSAGE_SELF);
    require(covers(current, previous), "live-process-monotonic");
    require(covers(current, reported), "live-process-covers-threads");
    previous = current;
    pause_ms(2);
  }
  __atomic_store_n(&stop, 1, __ATOMIC_RELEASE);
  struct usage reported = {0};
  for (unsigned i = 0; i < WORKERS; ++i) {
    require(pthread_join(threads[i], NULL) == 0, "worker-join");
    require((i & 1) ? workers[i].final.user > workers[i].initial.user
                    : workers[i].final.system > workers[i].initial.system,
            "worker-mode-progress");
    reported.user += workers[i].final.user;
    reported.system += workers[i].final.system;
  }
  struct usage final = sample(RUSAGE_SELF);
  require(covers(final, previous) && covers(final, reported), "joined-process-covers-threads");
  printf(
      "ACCOUNTING-CONCURRENCY PASS phase=aggregation observations=%d user_us=%llu "
      "system_us=%llu\n",
      OBSERVATIONS, (unsigned long long) final.user, (unsigned long long) final.system);
}

static void sleeping_time(void) {
  struct usage before = sample(RUSAGE_THREAD);
  uint64_t begin = now_us();
  pause_ms(200);
  uint64_t elapsed = now_us() - begin;
  struct usage after = sample(RUSAGE_THREAD);
  require(covers(after, before), "sleep-monotonic");
  uint64_t cpu = after.user - before.user + after.system - before.system;
  require(elapsed >= 150000 && cpu < elapsed * 3 / 4, "sleep-is-not-cpu-time");
  printf("ACCOUNTING-CONCURRENCY PASS phase=sleep wall_us=%llu cpu_us=%llu\n",
         (unsigned long long)elapsed, (unsigned long long)cpu);
}

static void expired(int signal_number) {
  if (signal_number == SIGVTALRM)
    __atomic_add_fetch(&virtual_expiries, 1, __ATOMIC_RELEASE);
  if (signal_number == SIGPROF)
    __atomic_add_fetch(&profile_expiries, 1, __ATOMIC_RELEASE);
}

static void arm_timers(int milliseconds) {
  struct itimerval setting = {.it_value = {0, milliseconds * 1000},
                              .it_interval = {0, milliseconds * 1000}};
  require(setitimer(ITIMER_VIRTUAL, &setting, NULL) == 0, "virtual-timer");
  require(setitimer(ITIMER_PROF, &setting, NULL) == 0, "profile-timer");
}

struct timer_result {
  unsigned queries, virtual_count, profile_count;
};

static struct timer_result query_until_expiry(void) {
  unsigned queries = 0;
  // No usage/clock query, yield, or blocking call may flush accounting in this loop.
  do {
    raw_query();
    ++queries;
  } while (queries < QUERY_LIMIT &&
           (load(&virtual_expiries) < TIMER_EXPIRIES || load(&profile_expiries) < TIMER_EXPIRIES));
  return (struct timer_result){queries, load(&virtual_expiries), load(&profile_expiries)};
}

static void timer_result(const char* phase, struct timer_result result) {
  // Use the pre-disarm snapshot: disarming itself must not make the test pass.
  require(result.virtual_count >= TIMER_EXPIRIES && result.profile_count >= TIMER_EXPIRIES,
          "query-only-timer-delivery");
  printf("ACCOUNTING-CONCURRENCY PASS phase=%s queries=%u virtual=%u profile=%u\n", phase,
         result.queries, result.virtual_count, result.profile_count);
}

static unsigned timer_ready, timer_start;
static struct timer_result worker_timers;

static void* timer_worker(void* argument) {
  const sigset_t* signals = argument;
  require(pthread_sigmask(SIG_UNBLOCK, signals, NULL) == 0, "worker-unblock-timers");
  __atomic_store_n(&timer_ready, 1, __ATOMIC_RELEASE);
  while (!load(&timer_start))
    raw_query();
  worker_timers = query_until_expiry();
  return NULL;
}

static void cpu_timers(void) {
  struct sigaction action = {.sa_handler = expired};
  sigemptyset(&action.sa_mask);
  require(sigaction(SIGVTALRM, &action, NULL) == 0 && sigaction(SIGPROF, &action, NULL) == 0,
          "timer-signal-setup");
  arm_timers(20);
  struct timer_result result = query_until_expiry();
  arm_timers(0);
  timer_result("query-timers", result);

  sigset_t signals, original;
  sigemptyset(&signals);
  sigaddset(&signals, SIGVTALRM);
  sigaddset(&signals, SIGPROF);
  require(pthread_sigmask(SIG_BLOCK, &signals, &original) == 0, "parent-block-timers");
  pthread_t thread;
  require(pthread_create(&thread, NULL, timer_worker, &signals) == 0, "timer-worker-create");
  for (unsigned attempt = 0; !load(&timer_ready); ++attempt) {
    require(attempt < 5000, "timer-worker-ready");
    pause_ms(1);
  }
  __atomic_store_n(&virtual_expiries, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&profile_expiries, 0, __ATOMIC_RELEASE);
  arm_timers(20);
  __atomic_store_n(&timer_start, 1, __ATOMIC_RELEASE);
  require(pthread_join(thread, NULL) == 0, "timer-worker-join");
  arm_timers(0);
  require(pthread_sigmask(SIG_SETMASK, &original, NULL) == 0, "parent-restore-signals");
  timer_result("cross-thread-timers", worker_timers);
}

static struct usage burn_to(struct usage target) {
  volatile uint64_t work = 1;
  struct usage current = sample(RUSAGE_SELF);
  while (!covers(current, target)) {
    for (unsigned i = 0; i < 32768; ++i)
      work = work * 6364136223846793005ULL + 1;
    for (unsigned i = 0; i < 1024; ++i)
      raw_query();
    struct usage next = sample(RUSAGE_SELF);
    require(covers(next, current), "lifetime-burn-monotonic");
    current = next;
  }
  return current;
}

static void fork_exec_lifetime(const char* executable) {
  // Establish a broad CPU-time margin; wall-clock scheduling delays are irrelevant.
  const struct usage parent = burn_to((struct usage){200000, 200000});
  pid_t child = fork();
  require(child >= 0, "lifetime-fork");
  if (!child) {
    alarm(30);
    struct usage fresh = sample(RUSAGE_SELF);
    require(fresh.user < parent.user / 2 && fresh.system < parent.system / 2,
            "fork-does-not-inherit-cpu-totals");
    printf("ACCOUNTING-CONCURRENCY PASS phase=fork-fresh user_us=%llu system_us=%llu\n",
           (unsigned long long)fresh.user, (unsigned long long)fresh.system);
    struct usage before = burn_to((struct usage){fresh.user + 20000, fresh.system + 20000});
    char user[32], system[32];
    snprintf(user, sizeof(user), "%llu", (unsigned long long)before.user);
    snprintf(system, sizeof(system), "%llu", (unsigned long long)before.system);
    execl(executable, executable, "--after-exec", user, system, NULL);
    execlp(executable, executable, "--after-exec", user, system, NULL);
    require(0, "lifetime-exec");
  }
  int status = 0;
  pid_t result;
  do {
    result = waitpid(child, &status, 0);
  } while (result < 0 && errno == EINTR);
  require(result == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "lifetime-child-exit");
  require(covers(sample(RUSAGE_SELF), parent), "parent-totals-retained");
  puts("ACCOUNTING-CONCURRENCY PASS phase=fork-exec");
}

static uint64_t lower_bound(const char* argument) {
  char* end;
  errno = 0;
  uint64_t value = strtoull(argument, &end, 10);
  require(end != argument && !*end && errno != ERANGE && value > 0, "exec-lower-bound");
  return value;
}

int main(int argc, char** argv) {
  const int after_exec = argc > 1 && !strcmp(argv[1], "--after-exec");
  setvbuf(stdout, NULL, _IONBF, 0);
  require(signal(SIGALRM, timeout) != SIG_ERR, "timeout-setup");
  alarm(after_exec ? 20 : 60);
  expected_uid = getuid();
  if (after_exec) {
    require(argc == 4, "exec-arguments");
    struct usage before = {lower_bound(argv[2]), lower_bound(argv[3])};
    struct usage after = sample(RUSAGE_SELF);
    require(covers(after, before), "exec-retains-cpu-totals");
    printf("ACCOUNTING-CONCURRENCY PASS phase=exec-retained user_us=%llu system_us=%llu\n",
           (unsigned long long)after.user, (unsigned long long)after.system);
    return 0;
  }
  puts("ACCOUNTING-CONCURRENCY BEGIN workers=4");
  aggregate_threads();
  sleeping_time();
  cpu_timers();
  fork_exec_lifetime(argv[0]);
  alarm(0);
  puts("ACCOUNTING-CONCURRENCY PASS END");
  return 0;
}
