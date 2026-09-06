#define _GNU_SOURCE
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "contract.h"
#include <sys/syscall.h>
#include <sys/wait.h>

cpu_set_t sc_allowed;
int sc_cpus[CPU_SETSIZE], sc_count;
size_t sc_bytes, sc_page;
_Thread_local unsigned sc_tls;

int sc_init(int expected) {
  sc_page = (size_t)sysconf(_SC_PAGESIZE);
  long bytes = syscall(SYS_sched_getaffinity, 0, sizeof(sc_allowed), &sc_allowed);
  if (bytes <= 0 || bytes > (long)sizeof(sc_allowed) || bytes % sizeof(long)) {
    fprintf(stderr, "raw affinity size=%ld errno=%d\n", bytes, errno);
    return -1;
  }
  sc_bytes = (size_t)bytes;
  if (sched_getaffinity(0, sizeof(sc_allowed), &sc_allowed))
    return -1;
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
    if (CPU_ISSET(cpu, &sc_allowed))
      sc_cpus[sc_count++] = cpu;
  if (!sc_count || (expected && expected != sc_count)) {
    fprintf(stderr, "topology expected=%d allowed=%d\n", expected, sc_count);
    return -1;
  }
  printf("SCHEDULING-CONTRACT: topology cpus=%d mask-bytes=%zu\n", sc_count, sc_bytes);
  if (sc_count == 1)
    puts("SCHEDULING-CONTRACT: SKIP cross-CPU placement (one allowed CPU)");
  return 0;
}
int sc_pin(pid_t tid, int cpu) {
  cpu_set_t mask;
  CPU_ZERO(&mask);
  CPU_SET(cpu, &mask);
  return sched_setaffinity(tid, sizeof(mask), &mask);
}
int sc_mask(pid_t tid, int cpu) {
  cpu_set_t mask;
  return sched_getaffinity(tid, sizeof(mask), &mask) || CPU_COUNT(&mask) != 1 ||
                 !CPU_ISSET(cpu, &mask)
             ? -1
             : 0;
}
int sc_sample(int cpu) {
  unsigned actual = UINT32_MAX, node = UINT32_MAX;
  if (syscall(SYS_getcpu, &actual, &node, NULL) || actual != (unsigned)cpu || node ||
      sched_getcpu() != cpu) {
    fprintf(stderr, "CPU expected=%d actual=%u node=%u errno=%d\n", cpu, actual, node, errno);
    return -1;
  }
  return 0;
}
int sc_register_syscall(long number, long a, long b, long c, long* result) {
#if defined(__x86_64__)
  const uint64_t input[2] = {UINT64_C(0x729bd064a35ecf18), UINT64_C(0xa148f7328d9605eb)};
  uint64_t output[2] = {};
  const long double floating = 0x1.23456789abcdefp+19L;
  long double restored = 0;
  long value = number;
  // Keep live registers across the actual syscall, rather than C ABI spills.
  __asm__ volatile(
      "movdqu %[input], %%xmm7\n\t"
      "fldt %[floating]\n\t"
      "syscall\n\t"
      "movdqu %%xmm7, %[output]\n\t"
      "fstpt %[restored]"
      : "+a"(value), [output] "=m"(output), [restored] "=m"(restored)
      : "D"(a), "S"(b), "d"(c), [input] "m"(input), [floating] "m"(floating)
      : "rcx", "r11", "xmm7", "st", "memory", "cc");
  *result = value;
  return memcmp(input, output, sizeof(input)) || memcmp(&floating, &restored, 10) ? -1 : 0;
#else
  errno = ENOTSUP;
  return -1;
#endif
}
int64_t sc_now(void) {
  struct timespec now;
  return clock_gettime(CLOCK_MONOTONIC, &now) ? -1 : (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}
int sc_wait(atomic_uint* value, unsigned expected) {
  int64_t now = sc_now(), end = now + INT64_C(10000000000);
  while (now >= 0 && now < end) {
    if (atomic_load_explicit(value, memory_order_acquire) == expected)
      return 0;
    sched_yield();
    now = sc_now();
  }
  fprintf(stderr, "generation timeout expected=%u actual=%u\n", expected,
          atomic_load_explicit(value, memory_order_acquire));
  return -1;
}
int sc_read(int fd, void* buffer, size_t length) {
  unsigned char* bytes = buffer;
  while (length) {
    struct pollfd ready = {.fd = fd, .events = POLLIN};
    int count;
    do
      count = poll(&ready, 1, 10000);
    while (count < 0 && errno == EINTR);
    if (count <= 0)
      return -1;
    ssize_t n = read(fd, bytes, length);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return -1;
    bytes += n;
    length -= (size_t)n;
  }
  return 0;
}
int sc_write(int fd, const void* buffer, size_t length) {
  const unsigned char* bytes = buffer;
  while (length) {
    ssize_t n = write(fd, bytes, length);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return -1;
    bytes += n;
    length -= (size_t)n;
  }
  return 0;
}
int sc_send(int fd, char value) {
  return sc_write(fd, &value, 1);
}
int sc_receive(int fd, char value) {
  char actual;
  return sc_read(fd, &actual, 1) || actual != value ? -1 : 0;
}
int sc_reap(pid_t pid, int milliseconds) {
  int64_t now = sc_now(), end = now + (int64_t)milliseconds * 1000000;
  while (now >= 0 && now < end) {
    int status;
    pid_t result = waitpid(pid, &status, WNOHANG);
    if (result == pid)
      return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    if (result < 0 && errno != EINTR)
      return -1;
    struct timespec delay = {0, 5000000};
    nanosleep(&delay, NULL);
    now = sc_now();
  }
  kill(pid, SIGKILL);
  while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
  }
  return -1;
}
int sc_spawn(struct sc_peer* peer, int (*body)(int, int, void*), void* argument) {
  int command[2], report[2];
  if (pipe(command))
    return -1;
  if (pipe(report)) {
    close(command[0]);
    close(command[1]);
    return -1;
  }
  pid_t pid = fork();
  if (!pid) {
    close(command[1]);
    close(report[0]);
    alarm(40);
    int result = body(command[0], report[1], argument);
    fflush(stderr);
    _exit(result ? 1 : 0);
  }
  close(command[0]);
  close(report[1]);
  if (pid < 0) {
    close(command[1]);
    close(report[0]);
    return -1;
  }
  *peer = (struct sc_peer){pid, command[1], report[0]};
  return 0;
}
int sc_join(struct sc_peer* peer) {
  int result = sc_reap(peer->pid, 15000);
  if (result)
    fprintf(stderr, "scheduling child pid=%d status=%d\n", peer->pid, result);
  peer->pid = -1;
  sc_cleanup(peer);
  return result;
}
void sc_cleanup(struct sc_peer* peer) {
  if (peer->pid > 0) {
    kill(peer->pid, SIGKILL);
    sc_reap(peer->pid, 1000);
  }
  if (peer->command >= 0)
    close(peer->command);
  if (peer->report >= 0)
    close(peer->report);
  *peer = (struct sc_peer)SC_PEER_INITIALIZER;
}
