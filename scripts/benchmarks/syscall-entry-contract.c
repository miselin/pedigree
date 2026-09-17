#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#define WORKERS 4
#define ITERATIONS 4096
#define ARCH_SET_GS 0x1001
#define ARCH_GET_GS 0x1004

static _Thread_local volatile uint64_t tls_token;
static _Alignas(64) uint64_t gs_slots[WORKERS][8];
static volatile sig_atomic_t signal_count, signal_error, gs_enabled;
static volatile sig_atomic_t child_count;
static pid_t children[WORKERS];
static int worker = -1;
static unsigned iteration;
static long expected_uid;
static uintptr_t expected_gs;
static uint64_t expected_magic;
static unsigned short expected_selector;
static int* errno_address;

static inline long raw6(long number, long a1, long a2, long a3, long a4, long a5, long a6) {
  register long r10 __asm__("r10") = a4;
  register long r8 __asm__("r8") = a5;
  register long r9 __asm__("r9") = a6;
  __asm__ volatile("syscall"
                   : "+a"(number)
                   : "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                   : "rcx", "r11", "memory", "cc");
  return number;
}

static unsigned short gs_selector(void) {
  unsigned short selector;
  __asm__ volatile("movw %%gs, %0" : "=r"(selector));
  return selector;
}

static int gs_valid(void) {
  if (gs_selector() != expected_selector)
    return 0;
  if (!gs_enabled)
    return 1;
  uintptr_t base = 0;
  if (raw6(SYS_arch_prctl, ARCH_GET_GS, (long)&base, 0, 0, 0, 0) != 0 || base != expected_gs)
    return 0;
  uint64_t magic;
  __asm__ volatile("movq %%gs:0, %0" : "=r"(magic) : : "memory");
  return magic == expected_magic;
}

static void timeout_handler(int signum) {
  (void)signum;
  static const char message[] = "ENTRY-CONTRACT FAIL operation=timeout\n";
  (void)raw6(SYS_write, STDERR_FILENO, (long)message, sizeof(message) - 1, 0, 0, 0);
  for (sig_atomic_t i = 0; i < child_count; ++i)
    (void)raw6(SYS_kill, children[i], SIGKILL, 0, 0, 0, 0);
  _exit(124);
}

static void require(int valid, const char* operation) {
  if (valid)
    return;
  fprintf(stderr, "ENTRY-CONTRACT FAIL worker=%d iteration=%u operation=%s errno=%d\n", worker,
          iteration, operation, errno);
  for (sig_atomic_t i = 0; i < child_count; ++i)
    (void)kill(children[i], SIGKILL);
  _exit(1);
}

static void check_state(uint64_t token, int error) {
  require(tls_token == token && &errno == errno_address && errno == error, "tls-errno");
  require(gs_valid(), "gs-roundtrip");
  require(tls_token == token && errno == error, "state-after-gs-query");
}

static void user_signal(int signum) {
  const int old_errno = errno;
  const uint64_t old_token = tls_token;
  const uint64_t handler_token = old_token ^ UINT64_C(0xb937facde0861254);
  if (signum != SIGUSR1 || &errno != errno_address || !gs_valid())
    signal_error = 1;
  tls_token = handler_token;
  errno = EOVERFLOW;
  for (unsigned i = 0; i < 16; ++i) {
    if (raw6(SYS_getuid, 0, 0, 0, 0, 0, 0) != expected_uid || tls_token != handler_token ||
        errno != EOVERFLOW || &errno != errno_address)
      signal_error = 2;
  }
  if (raw6(SYS_sched_yield, 0, 0, 0, 0, 0, 0) != 0 || !gs_valid())
    signal_error = 3;
  if (tls_token != handler_token || errno != EOVERFLOW)
    signal_error = 4;
  tls_token = old_token;
  errno = old_errno;
  ++signal_count;
}

static unsigned hardware_apic(void) {
  unsigned eax = 1, ebx, ecx = 0, edx;
  __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx) : : "memory");
  return ebx >> 24;
}

static void write_page(int fd, unsigned char* page, size_t size) {
  for (size_t done = 0; done < size;) {
    ssize_t n = write(fd, page + done, size - done);
    if (n < 0 && errno == EINTR)
      continue;
    require(n > 0, "fixture-write");
    done += (size_t)n;
  }
}

static void run_worker(int slot) {
  worker = slot;
  child_count = 0;
  alarm(90);
  errno_address = &errno;
  expected_selector = gs_selector();
  expected_uid = getuid();
  const pid_t pid = getpid();
  uintptr_t original_gs = 0;
  const long get_gs = raw6(SYS_arch_prctl, ARCH_GET_GS, (long)&original_gs, 0, 0, 0, 0);
  expected_magic = UINT64_C(0x6a579ef3b40d8200) + (unsigned)slot;
  gs_slots[slot][0] = expected_magic;
  expected_gs = (uintptr_t)&gs_slots[slot][0];
  const long set_gs = raw6(SYS_arch_prctl, ARCH_SET_GS, (long)expected_gs, 0, 0, 0, 0);
  if (set_gs == -EINVAL || set_gs == -ENOSYS) {
    require(get_gs == 0 || get_gs == -EINVAL || get_gs == -ENOSYS, "get-gs-unexpected-error");
    printf("ENTRY-CONTRACT SKIP worker=%d feature=nonzero-gs set_rc=%ld get_rc=%ld\n", slot, set_gs,
           get_gs);
  } else {
    require(set_gs == 0 && get_gs == 0, "set-get-gs");
    gs_enabled = 1;
    require(gs_valid(), "initial-gs");
  }

  struct sigaction action = {0};
  action.sa_handler = user_signal;
  require(sigemptyset(&action.sa_mask) == 0 && sigaction(SIGUSR1, &action, NULL) == 0,
          "signal-setup");
  const long page_size = sysconf(_SC_PAGESIZE);
  require(page_size >= 4096 && page_size <= 65536, "page-size");
  unsigned char* page = malloc((size_t)page_size);
  require(page != NULL, "fixture-buffer");
  char path[] = "/tmp/entry-contract-XXXXXX";
  int fd = mkstemp(path);
  require(fd >= 0 && unlink(path) == 0, "fixture-open");
  memset(page, 0x17, (size_t)page_size);
  write_page(fd, page, (size_t)page_size);
  const unsigned char second = (unsigned char)(0x70 + slot);
  memset(page, second, (size_t)page_size);
  write_page(fd, page, (size_t)page_size);
  free(page);

  unsigned char cpu_seen[256] = {0}, apic_seen[256] = {0};
  for (iteration = 0; iteration < ITERATIONS; ++iteration) {
    const uint64_t token = UINT64_C(0x875ace04913b2600) ^ ((uint64_t)slot << 32) ^ iteration;
    tls_token = token;
    errno = EDOM;
    require(raw6(SYS_getuid, 0, 0, 0, 0, 0, 0) == expected_uid, "raw-getuid");
    check_state(token, EDOM);
    if (!(iteration % 32)) {
      unsigned cpu = UINT32_MAX, node = UINT32_MAX;
      require(raw6(SYS_getcpu, (long)&cpu, (long)&node, 0, 0, 0, 0) == 0 && cpu < 256,
              "getcpu-record");
      cpu_seen[cpu] = 1;
      apic_seen[hardware_apic()] = 1;
      require(raw6(SYS_sched_yield, 0, 0, 0, 0, 0, 0) == 0, "yield");
      check_state(token, EDOM);
    }
    if (!(iteration % 64)) {
      // The file descriptor and nonzero offset make arguments five and six observable.
      long result =
          raw6(SYS_mmap, 0, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, page_size);
      require((unsigned long)result < (unsigned long)-4095, "six-argument-file-mmap");
      volatile unsigned char* mapping = (void*)result;
      for (long n = 0; n < page_size; ++n)
        require(mapping[n] == second, "mmap-offset-contents");
      mapping[0] = (unsigned char)(second ^ 0x3f);
      require(raw6(SYS_munmap, result, page_size, 0, 0, 0, 0) == 0, "file-munmap");
      result =
          raw6(SYS_mmap, 0, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      require((unsigned long)result < (unsigned long)-4095, "anonymous-mmap");
      mapping = (void*)result;
      for (long n = 0; n < page_size; ++n)
        require(mapping[n] == 0, "anonymous-zero");
      mapping[page_size - 1] = (unsigned char)(slot + 1);
      require(mapping[page_size - 1] == slot + 1, "anonymous-write");
      require(raw6(SYS_munmap, result, page_size, 0, 0, 0, 0) == 0, "anonymous-munmap");
      check_state(token, EDOM);
    }
    if (!(iteration % 128)) {
      sig_atomic_t before = signal_count;
      require(raw6(SYS_kill, pid, SIGUSR1, 0, 0, 0, 0) == 0, "signal-send");
      for (unsigned wait = 0; signal_count == before && wait < 4096; ++wait)
        require(raw6(SYS_sched_yield, 0, 0, 0, 0, 0, 0) == 0, "signal-wait");
      require(signal_count == before + 1 && !signal_error, "signal-handler-return");
      check_state(token, EDOM);
    }
  }
  unsigned char retained = 0;
  require(pread(fd, &retained, 1, page_size) == 1 && retained == second, "private-map-file-intact");
  require(close(fd) == 0, "fixture-close");
  for (unsigned cpu = 0; cpu < 256; ++cpu) {
    if (cpu_seen[cpu])
      printf("ENTRY-CONTRACT VISIT worker=%d kind=logical cpu=%u\n", slot, cpu);
    if (apic_seen[cpu])
      printf("ENTRY-CONTRACT VISIT worker=%d kind=apic cpu=%u\n", slot, cpu);
  }
  require(signal_count == ITERATIONS / 128 && !signal_error, "signal-count");
  if (gs_enabled)
    require(raw6(SYS_arch_prctl, ARCH_SET_GS, (long)original_gs, 0, 0, 0, 0) == 0, "restore-gs");
  alarm(0);
  printf("ENTRY-CONTRACT WORKER PASS worker=%d getuid=%d signals=%d gs=%s\n", slot, ITERATIONS,
         (int)signal_count, gs_enabled ? "PASS" : "SKIP");
  _exit(0);
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  struct sigaction action = {0};
  action.sa_handler = timeout_handler;
  require(sigemptyset(&action.sa_mask) == 0 && sigaction(SIGALRM, &action, NULL) == 0,
          "timeout-setup");
  alarm(120);
  printf("ENTRY-CONTRACT BEGIN workers=%d iterations=%d\n", WORKERS, ITERATIONS);
  for (int slot = 0; slot < WORKERS; ++slot) {
    pid_t pid = fork();
    require(pid >= 0, "fork");
    if (!pid)
      run_worker(slot);
    children[child_count++] = pid;
  }
  for (int slot = 0; slot < WORKERS; ++slot) {
    int status = 0;
    pid_t result;
    do {
      result = waitpid(children[slot], &status, 0);
    } while (result < 0 && errno == EINTR);
    require(result == children[slot] && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "worker-exit");
  }
  child_count = 0;
  alarm(0);
  puts("ENTRY-CONTRACT PASS END workers=4");
  return 0;
}
