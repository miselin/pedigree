#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>

static int serial_fd = -1;
extern char** environ;

static void fail(const char* operation) {
  dprintf(STDOUT_FILENO, "COMPILEBENCH FAIL operation=%s errno=%d\n", operation, errno);
  exit(1);
}

static uint64_t now_ns(void) {
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t))
    fail("clock");
  return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}

static uint64_t timeval_us(struct timeval t) {
  return (uint64_t)t.tv_sec * 1000000ULL + (uint64_t)t.tv_usec;
}

static void gate(const char* phase) {
  printf("COMPILEBENCH READY phase=%s\n", phase);
  for (;;) {
    char c;
    ssize_t n = read(serial_fd, &c, 1);
    if (n == 1 && c == 'g')
      return;
    if (n == 1 || (n < 0 && errno == EINTR))
      continue;
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
      fail("gate-read");
    struct pollfd p = {serial_fd, POLLIN, 0};
    int rc;
    do {
      rc = poll(&p, 1, -1);
    } while (rc < 0 && errno == EINTR);
    if (rc < 0 || (p.revents & (POLLERR | POLLNVAL | POLLHUP)))
      fail("gate-poll");
  }
}

static void metric(const char* phase, uint64_t start, uint64_t end, int rc,
                   const struct rusage* usage, uint64_t checksum) {
  printf(
      "COMPILEBENCH metric phase=%s total_us=%llu rc=%d user_us=%llu system_us=%llu "
      "minor_faults=%ld major_faults=%ld in_blocks=%ld out_blocks=%ld "
      "voluntary_switches=%ld involuntary_switches=%ld checksum=%llu\n",
      phase, (unsigned long long)((end - start) / 1000), rc,
      (unsigned long long)timeval_us(usage->ru_utime),
      (unsigned long long)timeval_us(usage->ru_stime), usage->ru_minflt, usage->ru_majflt,
      usage->ru_inblock, usage->ru_oublock, usage->ru_nvcsw, usage->ru_nivcsw,
      (unsigned long long)checksum);
  printf("COMPILEBENCH DONE phase=%s\n", phase);
}

static void own_metric(const char* phase, uint64_t start, uint64_t end, const struct rusage* before,
                       uint64_t checksum) {
  struct rusage after;
  if (getrusage(RUSAGE_SELF, &after))
    fail("getrusage");
  uint64_t user = timeval_us(after.ru_utime) - timeval_us(before->ru_utime);
  uint64_t system = timeval_us(after.ru_stime) - timeval_us(before->ru_stime);
  after.ru_utime.tv_sec = user / 1000000;
  after.ru_utime.tv_usec = user % 1000000;
  after.ru_stime.tv_sec = system / 1000000;
  after.ru_stime.tv_usec = system % 1000000;
  after.ru_minflt -= before->ru_minflt;
  after.ru_majflt -= before->ru_majflt;
  after.ru_inblock -= before->ru_inblock;
  after.ru_oublock -= before->ru_oublock;
  after.ru_nvcsw -= before->ru_nvcsw;
  after.ru_nivcsw -= before->ru_nivcsw;
  metric(phase, start, end, 0, &after, checksum);
}

static int command(const char* phase, char* const args[], int permit_failure) {
  printf("COMPILEBENCH command phase=%s argv=", phase);
  for (unsigned i = 0; args[i]; ++i)
    printf("%s%s", i ? " " : "", args[i]);
  printf("\n");
  gate(phase);
  uint64_t start = now_ns();
  pid_t child = fork();
  if (child < 0)
    fail("fork");
  if (!child) {
    int null_fd = open("/dev/null", O_RDONLY);
    if (null_fd < 0 || dup2(null_fd, STDIN_FILENO) < 0)
      _exit(125);
    if (null_fd > STDERR_FILENO)
      close(null_fd);
    if (serial_fd > STDERR_FILENO)
      close(serial_fd);
    char* environment[] = {
        "PATH=/usr/bin:/bin",
        "LC_ALL=C",
        "HOME=/root",
        "TMPDIR=/tmp",
        "CPLUS_INCLUDE_PATH=/usr/include/c++/15.3.0:/usr/include/c++/15.3.0/x86_64-pedigree",
        NULL};
    environ = environment;
    execvp(args[0], args);
    dprintf(STDERR_FILENO, "exec %s failed: errno=%d\n", args[0], errno);
    _exit(127);
  }
  int status;
  struct rusage usage = {0};
  pid_t waited;
  do {
    waited = wait4(child, &status, 0, &usage);
  } while (waited < 0 && errno == EINTR);
  uint64_t end = now_ns();
  if (waited != child)
    fail("wait4");
  int rc = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
  metric(phase, start, end, rc, &usage, 0);
  if (rc && !permit_failure)
    fail(phase);
  return rc;
}

static void require_file(const char* path) {
  struct stat st;
  if (stat(path, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0)
    fail(path);
  printf("COMPILEBENCH file path=%s bytes=%lld\n", path, (long long)st.st_size);
}

static void controls(void) {
  struct rusage before;
  gate("idle");
  if (getrusage(RUSAGE_SELF, &before))
    fail("getrusage");
  uint64_t start = now_ns();
  struct timespec delay = {5, 0};
  while (nanosleep(&delay, &delay)) {
    if (errno != EINTR)
      fail("nanosleep");
  }
  own_metric("idle", start, now_ns(), &before, 0);
  gate("cpu");
  if (getrusage(RUSAGE_SELF, &before))
    fail("getrusage");
  start = now_ns();
  uint64_t value = 0x123456789abcdefULL;
  for (uint64_t i = 0; i < 100000000; ++i) {
    value ^= value << 13;
    value ^= value >> 7;
    value ^= value << 17;
  }
  own_metric("cpu", start, now_ns(), &before, value);
}

static void anonymous_faults(unsigned mib) {
  size_t size = (size_t)mib * 1024 * 1024;
  unsigned char* mapping =
      mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mapping == MAP_FAILED)
    fail("mmap");
  char phase[64];
  snprintf(phase, sizeof(phase), "anon-%umib", mib);
  gate(phase);
  struct rusage before;
  if (getrusage(RUSAGE_SELF, &before))
    fail("getrusage");
  uint64_t start = now_ns();
  volatile unsigned char* touched = mapping;
  for (size_t i = 0; i < size; i += 4096)
    touched[i] = (unsigned char)((i / 4096) * 37U + 0x53U);
  uint64_t end = now_ns(), checksum = 0;
  for (size_t i = 0; i < size; i += 4096) {
    if (mapping[i] != (unsigned char)((i / 4096) * 37U + 0x53U))
      fail("anon-pattern");
    checksum += mapping[i];
  }
  own_metric(phase, start, end, &before, checksum);
  if (munmap(mapping, size))
    fail("munmap");
}

static uint64_t persisted_output(int verify) {
  int fd = open("which", O_RDONLY);
  if (fd < 0)
    fail("persist-open-output");
  uint64_t hash = 14695981039346656037ULL, bytes = 0;
  unsigned char buffer[4096];
  for (;;) {
    ssize_t n = read(fd, buffer, sizeof(buffer));
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0)
      fail("persist-read-output");
    if (!n)
      break;
    bytes += n;
    for (ssize_t i = 0; i < n; ++i) {
      hash ^= buffer[i];
      hash *= 1099511628211ULL;
    }
  }
  close(fd);
  if (!bytes)
    fail("persist-empty-output");
  if (verify) {
    FILE* sentinel = fopen("persisted-output", "r");
    unsigned long long expected_bytes, expected_hash;
    char trailing;
    if (!sentinel ||
        fscanf(sentinel, "COMPILEBENCH-V1 %llu %llx %c", &expected_bytes, &expected_hash,
               &trailing) != 2 ||
        expected_bytes != bytes || expected_hash != hash)
      fail("persist-checksum");
    fclose(sentinel);
  } else {
    // The next boot consumes this sentinel without rewriting it or compiling again.
    fd = open("persisted-output", O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0 ||
        dprintf(fd, "COMPILEBENCH-V1 %llu %016llx\n", (unsigned long long)bytes,
                (unsigned long long)hash) <= 0 ||
        fsync(fd))
      fail("persist-write-sentinel");
    close(fd);
  }
  printf("COMPILEBENCH persisted action=%s bytes=%llu fnv1a64=%016llx\n",
         verify ? "verified" : "stored", (unsigned long long)bytes, (unsigned long long)hash);
  return hash;
}

int main(void) {
  serial_fd = open("/dev/ttyS0", O_RDWR | O_NONBLOCK);
  if (serial_fd < 0 || dup2(serial_fd, STDOUT_FILENO) < 0 || dup2(serial_fd, STDERR_FILENO) < 0)
    fail("serial-open");
  setvbuf(stdout, NULL, _IONBF, 0);
  struct termios t;
  if (!tcgetattr(serial_fd, &t)) {
    t.c_iflag = 0;
    t.c_oflag = 0;
    t.c_lflag = 0;
    t.c_cflag = (t.c_cflag & ~(CSIZE | PARENB)) | CS8;
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(serial_fd, TCSANOW, &t))
      fail("serial-termios");
  } else if (errno != ENOTTY) {
    fail("serial-termios");
  }
  if (chdir("/root/compile-bench"))
    fail("setup");
  printf("COMPILEBENCH BEGIN\n");
  static char kernel_log[256 * 1024];
  long log_size = syscall(SYS_syslog, 3, kernel_log, sizeof(kernel_log) - 1);
  if (log_size > 0) {
    kernel_log[log_size] = 0;
    char* line = strtok(kernel_log, "\n");
    while (line) {
      char* module = strstr(line, "KERNELELF: Preloaded module ");
      if (module)
        printf("COMPILEBENCH %s\n", module);
      line = strtok(NULL, "\n");
    }
  }
  int quick = !access("quick-run", F_OK);
  int skip_sync = !access("no-sync", F_OK);
  int persist = !skip_sync && !access("persist-check", F_OK);
  char* run[] = {"./which", "gcc", NULL};
  if (persist && !access("persisted-output", F_OK)) {
    gate("verify-persisted");
    struct rusage before;
    if (getrusage(RUSAGE_SELF, &before))
      fail("getrusage");
    uint64_t start = now_ns();
    uint64_t hash = persisted_output(1);
    own_metric("verify-persisted", start, now_ns(), &before, hash);
    command("run-persisted", run, 0);
    printf("COMPILEBENCH PASS END\n");
    return 0;
  }
  if (quick && access("link-cxx", F_OK))
    fail("quick-requires-link-cxx");
  require_file("which.cc");
  controls();
  char* exact[] = {"gcc", "-o", "which", "which.cc", NULL};
  char* practical[] = {"gcc", "-o", "which", "which.cc", "-lstdc++", NULL};
  char** successful = exact;
  if (!access("link-cxx", F_OK)) {
    command("compile-cold", practical, 0);
    successful = practical;
  } else if (command("compile-exact", exact, 1)) {
    command("compile-practical", practical, 0);
    successful = practical;
  }
  require_file("which");
  command("compile-warm-1", successful, 0);
  if (!quick) {
    command("compile-warm-2", successful, 0);
    char* preprocess[] = {"gcc", "-E", "which.cc", "-o", "which.ii", NULL};
    char* codegen[] = {"gcc", "-ftime-report", "-S", "which.ii", "-o", "which.s", NULL};
    char* assemble[] = {"gcc", "-c", "which.s", "-o", "which.o", NULL};
    char* link[] = {"gcc", "-o", "which", "which.o", "-lstdc++", NULL};
    command("preprocess", preprocess, 0);
    require_file("which.ii");
    command("codegen", codegen, 0);
    require_file("which.s");
    command("assemble", assemble, 0);
    require_file("which.o");
    command("link", link, 0);
    require_file("which");
  }
  command("run", run, 0);
  if (skip_sync) {
    printf("COMPILEBENCH skipped phase=sync reason=writes-disabled-performance-only\n");
  } else {
    struct rusage before;
    gate("sync");
    if (getrusage(RUSAGE_SELF, &before))
      fail("getrusage");
    uint64_t start = now_ns();
    if (persist)
      persisted_output(0);
    int fd = open("which", O_RDONLY);
    if (fd < 0 || fsync(fd))
      fail("fsync");
    close(fd);
    sync();
    own_metric("sync", start, now_ns(), &before, 0);
  }
  require_file("which");
  if (!quick) {
    anonymous_faults(1);
    anonymous_faults(4);
    anonymous_faults(16);
    anonymous_faults(64);
  }
  char* contract[] = {"./anonymous-contract", "1", NULL};
  command("anon-contract", contract, 0);
  printf("COMPILEBENCH PASS END\n");
  return 0;
}
