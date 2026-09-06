#define _GNU_SOURCE
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>

static int affinity_abi(void) {
  int failed = 0;
  cpu_set_t raw, public, original, singleton;
  void* bad = mmap(NULL, sc_page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(bad != MAP_FAILED);
  CHECK(sched_getaffinity(0, sizeof(original), &original) == 0);
  CHECK(sysconf(_SC_NPROCESSORS_CONF) == sc_count && sysconf(_SC_NPROCESSORS_ONLN) == sc_count);
  CHECK(get_nprocs_conf() == sc_count && get_nprocs() == sc_count);
  memset(&raw, 0xa7, sizeof(raw));
  CHECK(syscall(SYS_sched_getaffinity, 0, sizeof(raw), &raw) == (long)sc_bytes);
  for (size_t n = sc_bytes; n < sizeof(raw); ++n)
    CHECK(((unsigned char*)&raw)[n] == 0xa7);
  memset(&public, 0xa7, sizeof(public));
  CHECK(sched_getaffinity(0, sizeof(public), &public) == 0);
  CHECK(CPU_EQUAL(&original, &public));
  for (size_t n = sc_bytes; n < sizeof(public); ++n)
    CHECK(((unsigned char*)&public)[n] == 0);
  CHECK(pthread_getaffinity_np(pthread_self(), sizeof(public), &public) == 0);
  CHECK(CPU_EQUAL(&original, &public));
  CHECK(syscall(SYS_sched_getaffinity, syscall(SYS_gettid), sc_bytes, &raw) == (long)sc_bytes);
  errno = 0;
  CHECK(syscall(SYS_sched_getaffinity, 0, sc_bytes - 1, &raw) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(syscall(SYS_sched_getaffinity, 0, sc_bytes + 1, &raw) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(syscall(SYS_sched_getaffinity, -1, sizeof(raw), &raw) == -1 && errno == ESRCH);
  errno = 0;
  CHECK(syscall(SYS_sched_getaffinity, 0, sc_bytes, bad) == -1 && errno == EFAULT);
  CHECK(pthread_getaffinity_np(pthread_self(), sc_bytes - 1, &raw) == EINVAL);
  CPU_ZERO(&singleton);
  CPU_SET(sc_cpus[0], &singleton);
  size_t short_size = (size_t)sc_cpus[0] / 8 + 1;
  CHECK(syscall(SYS_sched_setaffinity, 0, short_size, &singleton) == 0);
  CHECK(sc_mask(0, sc_cpus[0]) == 0 && sc_sample(sc_cpus[0]) == 0);
  CHECK(sysconf(_SC_NPROCESSORS_CONF) == 1 && sysconf(_SC_NPROCESSORS_ONLN) == 1);
  CHECK(get_nprocs_conf() == 1 && get_nprocs() == 1);
  // Ignored suffix bytes must not cause an unnecessary user access.
  unsigned char* edge =
      mmap(NULL, 2 * sc_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(edge != MAP_FAILED);
  if (mprotect(edge + sc_page, sc_page, PROT_NONE)) {
    munmap(edge, 2 * sc_page);
    CHECK(0);
  }
  memcpy(edge + sc_page - sc_bytes, &singleton, sc_bytes);
  long copied = syscall(SYS_sched_setaffinity, 0, 128, edge + sc_page - sc_bytes);
  munmap(edge, 2 * sc_page);
  CHECK(copied == 0);
  errno = 0;
  CHECK(syscall(SYS_sched_setaffinity, 0, sc_bytes, bad) == -1 && errno == EFAULT);
  CHECK(sc_mask(0, sc_cpus[0]) == 0);
  CPU_ZERO(&raw);
  errno = 0;
  CHECK(sched_setaffinity(0, sizeof(raw), &raw) == -1 && errno == EINVAL);
  CPU_SET(CPU_SETSIZE - 1, &raw);
  CHECK(!CPU_ISSET(CPU_SETSIZE - 1, &original));
  errno = 0;
  CHECK(sched_setaffinity(0, sizeof(raw), &raw) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(syscall(SYS_sched_setaffinity, 0, 0, NULL) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(syscall(SYS_sched_setaffinity, -1, sizeof(singleton), &singleton) == -1 && errno == ESRCH);
  CHECK(sc_mask(0, sc_cpus[0]) == 0);
  CHECK(pthread_setaffinity_np(pthread_self(), sizeof(original), &original) == 0);
  CHECK(sched_getaffinity(0, sizeof(public), &public) == 0 && CPU_EQUAL(&public, &original));
out:
  if (bad != MAP_FAILED)
    munmap(bad, sc_page);
  return failed;
}

static int cpu_abi(void) {
  int failed = 0;
  unsigned cpu = UINT32_MAX, node = UINT32_MAX;
  void* bad = mmap(NULL, sc_page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(bad != MAP_FAILED && sc_pin(0, sc_cpus[0]) == 0);
  CHECK(syscall(SYS_getcpu, &cpu, &node, bad) == 0 && cpu == (unsigned)sc_cpus[0] && node == 0);
  CHECK(syscall(SYS_getcpu, NULL, NULL, bad) == 0);
  node = UINT32_MAX;
  CHECK(syscall(SYS_getcpu, NULL, &node, NULL) == 0 && node == 0);
  cpu = UINT32_MAX;
  errno = 0;
  CHECK(syscall(SYS_getcpu, &cpu, bad, NULL) == -1 && errno == EFAULT &&
        cpu == (unsigned)sc_cpus[0]);
  node = UINT32_MAX;
  errno = 0;
  CHECK(syscall(SYS_getcpu, bad, &node, NULL) == -1 && errno == EFAULT && node == 0);
  CHECK(sc_sample(sc_cpus[0]) == 0);
out:
  if (bad != MAP_FAILED)
    munmap(bad, sc_page);
  return failed;
}

static atomic_uint entries;
static void* explicit_entry(void* argument) {
  atomic_fetch_add_explicit(&entries, 1, memory_order_relaxed);
  int policy = -1;
  struct sched_param param;
  int error = pthread_getschedparam(pthread_self(), &policy, &param);
  return (void*)(intptr_t)(error || policy != SCHED_OTHER || param.sched_priority ||
                           sc_mask(0, *(int*)argument) || sc_sample(*(int*)argument));
}
static int policy_abi(void) {
  int failed = 0, attr_ready = 0;
  pthread_attr_t attr;
  struct sched_param param = {.sched_priority = 0};
  void* bad = mmap(NULL, sc_page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(bad != MAP_FAILED);
  int words[4] = {-1, 0x126abc, 0x35c9ed, 0x47ad8b};
  CHECK(syscall(SYS_sched_getparam, 0, words) == 0 && words[0] == 0 && words[1] == 0x126abc &&
        words[2] == 0x35c9ed && words[3] == 0x47ad8b);
  CHECK(syscall(SYS_sched_getscheduler, 0) == SCHED_OTHER);
  CHECK(syscall(SYS_sched_setparam, 0, &param) == 0);
  CHECK(syscall(SYS_sched_setscheduler, 0, SCHED_OTHER, &param) == 0);
  errno = 0;
  CHECK(syscall(SYS_sched_getscheduler, -1) == -1 && errno == EINVAL);
  for (size_t i = 0; i < 2; ++i) {
    long number = i ? SYS_sched_setparam : SYS_sched_getparam;
    errno = 0;
    CHECK(syscall(number, 0, NULL) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(syscall(number, -1, &param) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(syscall(number, 0, bad) == -1 && errno == EFAULT);
  }
  errno = 0;
  CHECK(syscall(SYS_sched_setscheduler, 0, -1, bad) == -1 && errno == EINVAL);
  param.sched_priority = 1;
  errno = 0;
  CHECK(syscall(SYS_sched_setparam, 0, &param) == -1 && errno == EINVAL);
  CHECK(pthread_setschedprio(pthread_self(), 1) == EINVAL);
  param.sched_priority = 0;
  CHECK(pthread_setschedparam(pthread_self(), SCHED_OTHER, &param) == 0);
  CHECK(pthread_setschedprio(pthread_self(), 0) == 0);
  const int policies[] = {SCHED_OTHER, SCHED_FIFO, SCHED_RR,
                          SCHED_BATCH, SCHED_IDLE, SCHED_DEADLINE};
  for (size_t i = 0; i < sizeof(policies) / sizeof(policies[0]); ++i) {
    const int realtime = policies[i] == SCHED_FIFO || policies[i] == SCHED_RR;
    CHECK(sched_get_priority_min(policies[i]) == (realtime ? 1 : 0));
    CHECK(sched_get_priority_max(policies[i]) == (realtime ? 99 : 0));
    if (policies[i] == SCHED_OTHER)
      continue;
    param.sched_priority = realtime ? 1 : 0;
    errno = 0;
    CHECK(syscall(SYS_sched_setscheduler, 0, policies[i], &param) == -1 && errno == EOPNOTSUPP);
    param.sched_priority = realtime ? 0 : 1;
    errno = 0;
    CHECK(syscall(SYS_sched_setscheduler, 0, policies[i], &param) == -1 && errno == EINVAL);
  }
  param.sched_priority = 0;
  errno = 0;
  CHECK(syscall(SYS_sched_setscheduler, 0, SCHED_OTHER | SCHED_RESET_ON_FORK, &param) == -1 &&
        errno == EOPNOTSUPP);
  errno = 0;
  CHECK(syscall(SYS_sched_setscheduler, 0, 0x20000000, &param) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(sched_get_priority_min(77) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(sched_get_priority_max(77) == -1 && errno == EINVAL);
  CHECK(syscall(SYS_sched_getscheduler, 0) == SCHED_OTHER);
  CHECK(sc_pin(0, sc_cpus[0]) == 0 && pthread_attr_init(&attr) == 0);
  attr_ready = 1;
  CHECK(pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED) == 0);
  CHECK(pthread_attr_setschedpolicy(&attr, SCHED_OTHER) == 0);
  CHECK(pthread_attr_setschedparam(&attr, &param) == 0);
  pthread_t thread;
  CHECK(pthread_create(&thread, &attr, explicit_entry, &sc_cpus[0]) == 0);
  void* result;
  CHECK(pthread_join(thread, &result) == 0 && result == NULL && atomic_load(&entries) == 1);
  param.sched_priority = 1;
  CHECK(pthread_attr_setschedpolicy(&attr, SCHED_FIFO) == 0);
  CHECK(pthread_attr_setschedparam(&attr, &param) == 0);
  CHECK(pthread_create(&thread, &attr, explicit_entry, &sc_cpus[0]) == EOPNOTSUPP);
  CHECK(atomic_load(&entries) == 1);
out:
  if (attr_ready)
    pthread_attr_destroy(&attr);
  if (bad != MAP_FAILED)
    munmap(bad, sc_page);
  return failed;
}

struct interval_peer {
  atomic_uint ready, stop;
  pid_t tid;
};
struct interval_query {
  pid_t tid;
  struct timespec expected;
};
static void* interval_entry(void* argument) {
  struct interval_peer* peer = argument;
  peer->tid = (pid_t)syscall(SYS_gettid);
  atomic_store_explicit(&peer->ready, 1, memory_order_release);
  while (!atomic_load_explicit(&peer->stop, memory_order_acquire))
    sched_yield();
  return NULL;
}
static int interval_unprivileged(int command, int report, void* argument) {
  (void)command;
  (void)report;
  const struct interval_query* query = argument;
  struct timespec value;
  return setresuid(51991, 51991, 51991) || sched_rr_get_interval(query->tid, &value) ||
         value.tv_sec != query->expected.tv_sec || value.tv_nsec != query->expected.tv_nsec;
}
static int interval_abi(void) {
  _Static_assert(sizeof(struct timespec) == 16, "amd64 Linux timespec ABI");
  int failed = 0, created = 0;
  pthread_t thread;
  struct interval_peer peer = {0};
  struct sc_peer child = SC_PEER_INITIALIZER;
  struct timespec baseline, value;
  void* bad = mmap(NULL, sc_page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(bad != MAP_FAILED);
  CHECK(sched_rr_get_interval(0, &baseline) == 0);
  CHECK(baseline.tv_sec >= 0 && baseline.tv_nsec >= 0 && baseline.tv_nsec < 1000000000 &&
        (baseline.tv_sec || baseline.tv_nsec));
  printf("SCHEDULING-CONTRACT: nominal interval=%llds+%lldns\n", (long long)baseline.tv_sec,
         (long long)baseline.tv_nsec);
  CHECK(pthread_create(&thread, NULL, interval_entry, &peer) == 0);
  created = 1;
  CHECK(sc_wait(&peer.ready, 1) == 0);
  for (int i = 0; i < sc_count; ++i) {
    CHECK(sc_pin(0, sc_cpus[i]) == 0 && sc_pin(peer.tid, sc_cpus[i]) == 0);
    CHECK(sc_sample(sc_cpus[i]) == 0);
    CHECK(sched_rr_get_interval(0, &value) == 0 && value.tv_sec == baseline.tv_sec &&
          value.tv_nsec == baseline.tv_nsec);
    CHECK(sched_rr_get_interval(peer.tid, &value) == 0 && value.tv_sec == baseline.tv_sec &&
          value.tv_nsec == baseline.tv_nsec);
    struct {
      int64_t seconds, nanoseconds;
      uint64_t guard[2];
    } raw = {-1, -1, {UINT64_C(0x48fd8270ab65139c), UINT64_C(0x95814aece263bf70)}};
    CHECK(syscall(SYS_sched_rr_get_interval, peer.tid, &raw) == 0);
    CHECK(raw.seconds == baseline.tv_sec && raw.nanoseconds == baseline.tv_nsec &&
          raw.guard[0] == UINT64_C(0x48fd8270ab65139c) &&
          raw.guard[1] == UINT64_C(0x95814aece263bf70));
  }
  struct interval_query query = {peer.tid, baseline};
  CHECK(sc_spawn(&child, interval_unprivileged, &query) == 0 && sc_join(&child) == 0);
  errno = 0;
  CHECK(sched_rr_get_interval(-1, &value) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(syscall(SYS_sched_rr_get_interval, 0, bad) == -1 && errno == EFAULT);
  errno = 0;
  CHECK(syscall(SYS_sched_rr_get_interval, 0, NULL) == -1 && errno == EFAULT);
  atomic_store_explicit(&peer.stop, 1, memory_order_release);
  CHECK(pthread_join(thread, NULL) == 0);
  created = 0;
  CHECK(sc_wait_for_task_retirement(peer.tid, &baseline) == 0);
  errno = 0;
  CHECK(sched_rr_get_interval(peer.tid, &value) == -1 && errno == ESRCH);
out:
  sc_cleanup(&child);
  if (created) {
    atomic_store_explicit(&peer.stop, 1, memory_order_release);
    pthread_join(thread, NULL);
  }
  if (bad != MAP_FAILED)
    munmap(bad, sc_page);
  return failed;
}

int sc_api(void) {
  return affinity_abi() || cpu_abi() || policy_abi() || interval_abi();
}
