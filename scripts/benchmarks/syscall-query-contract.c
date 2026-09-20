#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#define WORKERS 4
#define ITERATIONS 16384
#define SIGNALS 16

static pid_t children[WORKERS + 1];
static volatile sig_atomic_t child_count;
static long expected_pid, expected_uid;
static int worker = -1;
static struct loop_state {
  unsigned iterations;
  unsigned signals;
  unsigned error;
}* loop;

static inline long raw0(long number) {
  __asm__ volatile("syscall" : "+a"(number) : : "rcx", "r11", "memory", "cc");
  return number;
}

static void kill_children(void) {
  for (sig_atomic_t i = 0; i < child_count; ++i) {
    if (children[i] > 0)
      (void)kill(children[i], SIGKILL);
  }
}

static void timeout_handler(int signum) {
  (void)signum;
  static const char message[] = "QUERY-CONTRACT FAIL operation=timeout\n";
  (void)write(STDERR_FILENO, message, sizeof(message) - 1);
  kill_children();
  _exit(124);
}

static void require(int valid, const char* operation) {
  if (valid)
    return;
  fprintf(stderr, "QUERY-CONTRACT FAIL worker=%d operation=%s errno=%d\n", worker, operation,
          errno);
  kill_children();
  _exit(1);
}

static int queries_valid(void) {
  return raw0(SYS_getuid) == expected_uid && raw0(SYS_getpid) == expected_pid &&
         raw0(SYS_gettid) == expected_pid;
}

static int wait_child(pid_t pid, int options) {
  int status = 0;
  pid_t result;
  do {
    result = waitpid(pid, &status, options);
  } while (result < 0 && errno == EINTR);
  require(result == pid, "waitpid");
  return status;
}

struct report {
  int slot;
  pid_t pid;
  uid_t uid;
};

static void run_worker(int slot, pid_t parent, int report_fd) {
  worker = slot;
  child_count = 0;
  alarm(30);
  expected_pid = getpid();
  expected_uid = 20001 + slot * 3;
  require(expected_pid > 0 && expected_pid != parent, "child-pid");
  require(setresuid(expected_uid, expected_uid + 1, expected_uid + 2) == 0, "setresuid");
  uid_t real, effective, saved;
  require(getresuid(&real, &effective, &saved) == 0 && real == expected_uid &&
              effective == expected_uid + 1 && saved == expected_uid + 2,
          "nonzero-distinct-credentials");
  require(geteuid() == effective, "effective-uid-fallback");

  errno = 0;
  require(syscall(0xffffL) == -1 && errno == ENOSYS, "unimplemented-fallback");
  require(queries_valid() && errno == ENOSYS, "query-after-enosys");
  require(close(-1) == -1 && errno == EBADF, "error-fallback");
  require(queries_valid() && errno == EBADF, "query-after-ebadf");
  for (unsigned i = 0; i < ITERATIONS; ++i) {
    require(queries_valid() && errno == EBADF, "query-values-and-errno");
    if (!(i % 256))
      require(raw0(SYS_sched_yield) == 0 && errno == EBADF, "yield-fallback");
  }
  struct report report = {slot, (pid_t)expected_pid, (uid_t)expected_uid};
  require(write(report_fd, &report, sizeof(report)) == (ssize_t)sizeof(report), "report-write");
  _exit(0);
}

static void user_signal(int signum) {
  const int saved_errno = errno;
  if (signum != SIGUSR1 || !queries_valid() || errno != saved_errno)
    __atomic_store_n(&loop->error, 1, __ATOMIC_RELEASE);
  errno = saved_errno;
  __atomic_add_fetch(&loop->signals, 1, __ATOMIC_RELEASE);
}

static void run_query_loop(void) {
  worker = WORKERS;
  child_count = 0;
  alarm(30);
  expected_pid = getpid();
  expected_uid = getuid();
  struct sigaction action = {0};
  action.sa_handler = user_signal;
  require(sigemptyset(&action.sa_mask) == 0 && sigaction(SIGUSR1, &action, NULL) == 0,
          "signal-setup");
  errno = EDOM;
  // Shared counters let the parent coordinate without any blocking or yielding here.
  for (unsigned i = 1;; ++i) {
    if (!queries_valid() || errno != EDOM)
      __atomic_store_n(&loop->error, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&loop->iterations, i, __ATOMIC_RELEASE);
  }
}

static unsigned read_counter(unsigned* counter) {
  return __atomic_load_n(counter, __ATOMIC_ACQUIRE);
}

static void wait_counter(unsigned* counter, unsigned previous, const char* operation) {
  for (unsigned i = 0; i < 5000; ++i) {
    require(!read_counter(&loop->error), "loop-query-state");
    if (read_counter(counter) != previous)
      return;
    (void)usleep(1000);
  }
  require(0, operation);
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  struct sigaction action = {0};
  action.sa_handler = timeout_handler;
  require(sigemptyset(&action.sa_mask) == 0 && sigaction(SIGALRM, &action, NULL) == 0,
          "timeout-setup");
  alarm(60);
  require(getuid() == 0 && geteuid() == 0, "root-fixture-required");
  printf("QUERY-CONTRACT BEGIN workers=%d iterations=%d signals=%d\n", WORKERS, ITERATIONS,
         SIGNALS);
  const pid_t parent = getpid();
  int reports[2];
  require(pipe(reports) == 0, "report-pipe");
  for (int slot = 0; slot < WORKERS; ++slot) {
    pid_t pid = fork();
    require(pid >= 0, "fork-worker");
    if (!pid) {
      close(reports[0]);
      run_worker(slot, parent, reports[1]);
    }
    children[child_count++] = pid;
  }
  close(reports[1]);
  unsigned seen = 0;
  for (int i = 0; i < WORKERS; ++i) {
    struct report report;
    size_t done = 0;
    while (done < sizeof(report)) {
      ssize_t result = read(reports[0], (char*)&report + done, sizeof(report) - done);
      if (result < 0 && errno == EINTR)
        continue;
      require(result > 0, "report-read");
      done += (size_t)result;
    }
    require(report.slot >= 0 && report.slot < WORKERS, "report-slot");
    require(!(seen & (1U << report.slot)) && report.pid == children[report.slot] &&
                report.uid == (uid_t)(20001 + report.slot * 3),
            "parent-verified-query-values");
    seen |= 1U << report.slot;
    printf("QUERY-CONTRACT WORKER PASS worker=%d pid=%d uid=%u queries=%d\n", report.slot,
           report.pid, report.uid, ITERATIONS * 3);
  }
  close(reports[0]);
  for (int slot = 0; slot < WORKERS; ++slot) {
    int status = wait_child(children[slot], 0);
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "worker-exit");
    children[slot] = 0;
  }

  // Pedigree supports shared file mappings, but not shared anonymous mappings.
  const size_t loop_size = 4096;
  char path[] = "/tmp/query-contract-XXXXXX";
  int loop_fd = mkstemp(path);
  require(loop_fd >= 0, "shared-loop-file");
  require(unlink(path) == 0 && ftruncate(loop_fd, loop_size) == 0, "shared-loop-file-size");
  loop = mmap(NULL, loop_size, PROT_READ | PROT_WRITE, MAP_SHARED, loop_fd, 0);
  require(loop != MAP_FAILED, "shared-loop-state");
  require(close(loop_fd) == 0, "shared-loop-file-close");
  *loop = (struct loop_state){0};
  pid_t pid = fork();
  require(pid >= 0, "fork-query-loop");
  if (!pid)
    run_query_loop();
  children[child_count++] = pid;
  wait_counter(&loop->iterations, 0, "loop-start");
  for (unsigned i = 0; i < SIGNALS; ++i) {
    unsigned before = read_counter(&loop->iterations);
    wait_counter(&loop->iterations, before, "loop-progress");
    require(kill(pid, SIGUSR1) == 0, "async-signal-send");
    wait_counter(&loop->signals, i, "async-signal-delivery");
    require(read_counter(&loop->signals) == i + 1, "async-signal-count");
  }
  require(kill(pid, SIGSTOP) == 0, "stop-send");
  int status = wait_child(pid, WUNTRACED);
  require(WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP, "stopped-status");
  unsigned stopped = read_counter(&loop->iterations);
  (void)usleep(10000);
  require(read_counter(&loop->iterations) == stopped, "stopped-loop-quiescent");
  require(kill(pid, SIGCONT) == 0, "continue-send");
  status = wait_child(pid, WCONTINUED);
  require(WIFCONTINUED(status), "continued-status");
  wait_counter(&loop->iterations, stopped, "continued-loop-progress");
  require(kill(pid, SIGUSR1) == 0, "continued-signal-send");
  wait_counter(&loop->signals, SIGNALS, "continued-signal-delivery");
  require(read_counter(&loop->signals) == SIGNALS + 1 && !read_counter(&loop->error),
          "continued-query-state");
  require(kill(pid, SIGKILL) == 0, "kill-send");
  status = wait_child(pid, 0);
  require(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL, "killed-status");
  children[WORKERS] = 0;
  printf("QUERY-CONTRACT LOOP PASS queries=%u signals=%u stop=PASS continue=PASS kill=PASS\n",
         read_counter(&loop->iterations) * 3, read_counter(&loop->signals));
  require(munmap(loop, loop_size) == 0, "shared-state-unmap");
  alarm(0);
  puts("QUERY-CONTRACT PASS END workers=4");
  return 0;
}
