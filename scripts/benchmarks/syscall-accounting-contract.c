#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>

struct usage {
  uint64_t user, system;
};

struct samples {
  struct usage initial_self, initial_thread;
  struct usage busy_self, busy_thread;
  struct usage final_self, final_thread;
};

static void fail(const char* where) {
  dprintf(1, "RUSAGE-PROBE FAIL where=%s errno=%d\n", where, errno);
  _exit(1);
}

static void timeout(int signal_number) {
  (void)signal_number;
  static const char text[] = "RUSAGE-PROBE FAIL timeout\n";
  write(1, text, sizeof(text) - 1);
  _exit(124);
}

static uint64_t now_ns(void) {
  struct timespec value;
  if (clock_gettime(CLOCK_MONOTONIC, &value))
    fail("clock_gettime");
  return (uint64_t)value.tv_sec * 1000000000ULL + value.tv_nsec;
}

static struct usage unpack(const struct rusage* value) {
  if (value->ru_utime.tv_sec < 0 || value->ru_stime.tv_sec < 0 || value->ru_utime.tv_usec < 0 ||
      value->ru_utime.tv_usec >= 1000000 || value->ru_stime.tv_usec < 0 ||
      value->ru_stime.tv_usec >= 1000000)
    fail("timeval-range");
  return (struct usage){(uint64_t)value->ru_utime.tv_sec * 1000000 + value->ru_utime.tv_usec,
                        (uint64_t)value->ru_stime.tv_sec * 1000000 + value->ru_stime.tv_usec};
}

static struct usage sample(int who) {
  struct rusage value = {0};
  if (syscall(SYS_getrusage, who, &value))
    fail("raw-getrusage");
  return unpack(&value);
}

static void print_usage(const char* phase, struct usage value) {
  dprintf(1, "RUSAGE-PROBE phase=%s user_us=%llu system_us=%llu\n", phase,
          (unsigned long long)value.user, (unsigned long long)value.system);
}

static int follows(struct usage newer, struct usage older) {
  return newer.user >= older.user && newer.system >= older.system;
}

int main(void) {
  if (signal(SIGALRM, timeout) == SIG_ERR)
    fail("signal");
  alarm(45);
  struct usage children_before = sample(RUSAGE_CHILDREN);
  int descriptors[2];
  if (pipe(descriptors))
    fail("pipe");
  pid_t child = fork();
  if (child < 0)
    fail("fork");
  if (!child) {
    close(descriptors[0]);
    alarm(40);
    struct samples values;
    values.initial_self = sample(RUSAGE_SELF);
    values.initial_thread = sample(RUSAGE_THREAD);
    volatile uint64_t work = 1;
    uint64_t begin = now_ns();
    do {
      for (unsigned i = 0; i < 65536; ++i)
        work = work * 6364136223846793005ULL + 1;
    } while (now_ns() - begin < 100000000ULL);
    values.busy_self = sample(RUSAGE_SELF);
    values.busy_thread = sample(RUSAGE_THREAD);
    for (unsigned i = 0; i < 1000000; ++i) {
      if (syscall(SYS_getuid) < 0)
        fail("raw-getuid");
    }
    values.final_self = sample(RUSAGE_SELF);
    values.final_thread = sample(RUSAGE_THREAD);
    const char* bytes = (const char*)&values;
    size_t left = sizeof(values);
    while (left) {
      ssize_t count = write(descriptors[1], bytes, left);
      if (count < 0 && errno == EINTR)
        continue;
      if (count <= 0)
        fail("write-samples");
      bytes += count;
      left -= count;
    }
    close(descriptors[1]);
    _exit(0);
  }
  close(descriptors[1]);
  struct samples values;
  char* bytes = (char*)&values;
  size_t left = sizeof(values);
  while (left) {
    ssize_t count = read(descriptors[0], bytes, left);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      fail("read-samples");
    bytes += count;
    left -= count;
  }
  close(descriptors[0]);
  struct rusage waited_usage = {0};
  int status = 0;
  long waited;
  do {
    waited = syscall(SYS_wait4, child, &status, 0, &waited_usage);
  } while (waited < 0 && errno == EINTR);
  if (waited != child || !WIFEXITED(status) || WEXITSTATUS(status))
    fail("raw-wait4");
  struct usage waited_values = unpack(&waited_usage);
  struct usage children_after = sample(RUSAGE_CHILDREN);
  if (!follows(children_after, children_before))
    fail("children-monotonic");
  struct usage child_delta = {children_after.user - children_before.user,
                              children_after.system - children_before.system};
  print_usage("initial-self", values.initial_self);
  print_usage("initial-thread", values.initial_thread);
  print_usage("after-user-self", values.busy_self);
  print_usage("after-user-thread", values.busy_thread);
  print_usage("after-getuid-self", values.final_self);
  print_usage("after-getuid-thread", values.final_thread);
  print_usage("wait4", waited_values);
  print_usage("children-delta", child_delta);
  if (!follows(values.busy_self, values.initial_self) ||
      !follows(values.busy_thread, values.initial_thread) ||
      !follows(values.final_self, values.busy_self) ||
      !follows(values.final_thread, values.busy_thread) ||
      !follows(waited_values, values.final_self) || !follows(waited_values, values.final_thread))
    fail("accounting-monotonic");
  if (values.busy_self.user <= values.initial_self.user ||
      values.busy_thread.user <= values.initial_thread.user ||
      values.final_self.system <= values.busy_self.system ||
      values.final_thread.system <= values.busy_thread.system)
    fail("accounting-did-not-advance");
  if (child_delta.user != waited_values.user || child_delta.system != waited_values.system)
    fail("wait4-children-disagree");
  alarm(0);
  dprintf(1, "RUSAGE-PROBE PASS\n");
  return 0;
}
