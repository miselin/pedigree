#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

static const char* scratch = "/pedigree-io-bench.bin";
static int scratch_created;
static int keep_scratch;
static void cleanup(void) {
  if (scratch_created && !keep_scratch)
    unlink(scratch);
}
static void die(const char* phase) {
  fprintf(stderr, "IOBENCH FAIL phase=%s errno=%d error=%s\n", phase, errno, strerror(errno));
  exit(1);
}
static uint64_t now_ns(void) {
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t))
    die("clock");
  return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}
static void metric(const char* phase, uint64_t start, size_t bytes) {
  uint64_t elapsed = now_ns() - start;
  printf("IOBENCH metric phase=%s elapsed_us=%llu bytes=%zu\n", phase,
         (unsigned long long)(elapsed / 1000), bytes);
}
static void full_write(int fd, const unsigned char* p, size_t n) {
  while (n) {
    ssize_t r = write(fd, p, n);
    if (r < 0 && errno == EINTR)
      continue;
    if (r <= 0)
      die("write");
    p += r;
    n -= (size_t)r;
  }
}
static unsigned char pattern(size_t i) {
  return (unsigned char)((i * 37U + (i >> 8) * 17U + 0x53U) & 255U);
}
static void run_child(const char* phase, const char* path, char* const argv[]) {
  printf("IOBENCH phase_start phase=%s\n", phase);
  uint64_t start = now_ns();
  pid_t child = fork();
  if (child < 0)
    die("fork");
  if (!child) {
    if (!path)
      _exit(0);
    int fd = open("/dev/null", O_WRONLY);
    if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0)
      _exit(125);
    if (fd != STDOUT_FILENO)
      close(fd);
    execv(path, argv);
    _exit(126);
  }
  int status;
  pid_t r;
  do {
    r = waitpid(child, &status, 0);
  } while (r < 0 && errno == EINTR);
  if (r < 0)
    die("waitpid");
  metric(phase, start, 0);
  printf("IOBENCH child phase=%s status=%d\n", phase, status);
  if (!WIFEXITED(status) || WEXITSTATUS(status)) {
    fprintf(stderr, "IOBENCH FAIL phase=%s child_status=%d\n", phase, status);
    exit(1);
  }
}
static const char* find_binary(const char* a, const char* b) {
  if (!access(a, X_OK))
    return a;
  if (b && !access(b, X_OK))
    return b;
  return NULL;
}
struct Interval {
  uint64_t begin, end;
};
static struct Interval read_file(const char* phase, const char* path, int with_checksum) {
  unsigned char buf[16384];
  size_t total = 0;
  uint64_t sum = 0, start = now_ns();
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    die("read-open");
  for (;;) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0)
      die("read");
    if (!n)
      break;
    total += (size_t)n;
    if (with_checksum) {
      for (ssize_t i = 0; i < n; ++i)
        sum += buf[i];
    }
  }
  if (close(fd))
    die("read-close");
  struct Interval interval = {start, now_ns()};
  metric(phase, start, total);
  if (with_checksum)
    printf("IOBENCH checksum phase=%s value=%llu\n", phase, (unsigned long long)sum);
  if (!total) {
    errno = EIO;
    die("empty-read");
  }
  return interval;
}
static void verify_scratch(size_t length, int mapped) {
  unsigned char buf[16384];
  size_t pos = 0;
  uint64_t start = now_ns();
  int fd = open(scratch, O_RDONLY);
  if (fd < 0)
    die("verify-open");
  while (pos < length) {
    size_t want = length - pos;
    if (want > sizeof(buf))
      want = sizeof(buf);
    ssize_t n = read(fd, buf, want);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      die("verify-read");
    for (ssize_t i = 0; i < n; ++i) {
      size_t at = pos + (size_t)i;
      unsigned char expected = pattern(at);
      if (mapped && at % 4096 == 17)
        expected ^= 0xa5;
      if (buf[i] != expected) {
        fprintf(stderr, "IOBENCH FAIL phase=verify offset=%zu expected=%u actual=%u\n", at,
                expected, buf[i]);
        exit(1);
      }
    }
    pos += (size_t)n;
  }
  if (read(fd, buf, 1) != 0) {
    errno = EIO;
    die("verify-length");
  }
  if (close(fd))
    die("verify-close");
  metric(mapped ? "mmap_read_verify" : "write_read_verify", start, length);
}
static void read_pipe(int fd, void* buffer, size_t length) {
  unsigned char* p = buffer;
  while (length) {
    ssize_t n = read(fd, p, length);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0) {
      if (!n)
        errno = EPIPE;
      die("sync-pipe-read");
    }
    p += n;
    length -= (size_t)n;
  }
}
static void read_under_sync(int fd, size_t length, const char* bash) {
  int release[2], result[2];
  if (pipe(release) || pipe(result))
    die("sync-pipe");
  printf("IOBENCH phase_start phase=read_under_sync\n");
  uint64_t total_start = now_ns();
  pid_t child = fork();
  if (child < 0)
    die("sync-fork");
  if (!child) {
    /* Only the parent owns scratch cleanup, including child error paths. */
    scratch_created = 0;
    close(release[1]);
    close(result[0]);
    for (int i = 0; i < 3; ++i) {
      unsigned char token;
      read_pipe(release[0], &token, 1);
      struct Interval sync;
      full_write(result[1], &token, 1);
      sync.begin = now_ns();
      if (fsync(fd))
        die("concurrent-fsync");
      sync.end = now_ns();
      full_write(result[1], (const unsigned char*)&sync, sizeof(sync));
    }
    close(release[0]);
    close(result[1]);
    _exit(0);
  }
  close(release[0]);
  close(result[1]);
  uint64_t read_total = 0, sync_total = 0, overlap_total = 0;
  for (int i = 0; i < 3; ++i) {
    unsigned char token = 1;
    full_write(release[1], &token, 1);
    read_pipe(result[0], &token, 1);
    char phase[40];
    snprintf(phase, sizeof(phase), "bash_read_under_sync_%d", i);
    struct Interval reader = read_file(phase, bash, 1), sync;
    read_pipe(result[0], &sync, sizeof(sync));
    uint64_t begin = reader.begin > sync.begin ? reader.begin : sync.begin;
    uint64_t end = reader.end < sync.end ? reader.end : sync.end;
    uint64_t overlap = end > begin ? end - begin : 0;
    read_total += reader.end - reader.begin;
    sync_total += sync.end - sync.begin;
    overlap_total += overlap;
    printf("IOBENCH metric phase=concurrent_fsync_%d elapsed_us=%llu bytes=%zu\n", i,
           (unsigned long long)((sync.end - sync.begin) / 1000), length);
    printf("IOBENCH overlap iteration=%d elapsed_us=%llu\n", i,
           (unsigned long long)(overlap / 1000));
  }
  close(release[1]);
  close(result[0]);
  int status;
  pid_t waited;
  do {
    waited = waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited < 0)
    die("sync-waitpid");
  printf("IOBENCH child phase=read_under_sync status=%d\n", status);
  if (!WIFEXITED(status) || WEXITSTATUS(status)) {
    fprintf(stderr, "IOBENCH FAIL phase=read_under_sync child_status=%d\n", status);
    exit(1);
  }
  printf("IOBENCH concurrent_totals read_us=%llu sync_us=%llu overlap_us=%llu\n",
         (unsigned long long)(read_total / 1000), (unsigned long long)(sync_total / 1000),
         (unsigned long long)(overlap_total / 1000));
  metric("read_under_sync_total", total_start, length * 3);
}
int main(int argc, char** argv) {
  if (argc == 2 && !strcmp(argv[1], "--exec-probe"))
    return 0;
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  atexit(cleanup);
  const char* scratch_override = getenv("IOBENCH_SCRATCH");
  if (scratch_override && *scratch_override)
    scratch = scratch_override;
  int read_only = 0, verify_existing = 0, size_set = 0, concurrent_sync = 0, read_diagnostics = 0;
  size_t mib = 1;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "read-only"))
      read_only = 1;
    else if (!strcmp(argv[i], "verify-existing"))
      verify_existing = 1;
    else if (!strcmp(argv[i], "--read-under-sync"))
      concurrent_sync = 1;
    else if (!strcmp(argv[i], "--read-diagnostics"))
      read_diagnostics = 1;
    else if (!strcmp(argv[i], "--keep-scratch"))
      keep_scratch = 1;
    else {
      char* end;
      unsigned long parsed = strtoul(argv[i], &end, 10);
      if (!*argv[i] || *end || parsed < 1 || parsed > 8 || size_set) {
        fprintf(stderr,
                "usage: %s [read-only|verify-existing] [1..8] [--keep-scratch] "
                "[--read-under-sync] [--read-diagnostics]\n",
                argv[0]);
        return 2;
      }
      mib = (size_t)parsed;
      size_set = 1;
    }
  }
  if (read_only && verify_existing) {
    fprintf(stderr, "read-only and verify-existing are exclusive\n");
    return 2;
  }
  if (concurrent_sync && (read_only || verify_existing)) {
    fprintf(stderr, "--read-under-sync requires the full workload\n");
    return 2;
  }
  if (read_diagnostics && verify_existing) {
    fprintf(stderr, "--read-diagnostics is incompatible with verify-existing\n");
    return 2;
  }
  printf("IOBENCH BEGIN mode=%s size_mib=%zu keep_scratch=%d\n",
         verify_existing ? "verify-existing" : (read_only ? "read-only" : "full"), mib,
         keep_scratch);
  if (verify_existing) {
    verify_scratch(mib * 1024 * 1024, 1);
    printf("IOBENCH PASS END\n");
    return 0;
  }
  uint64_t start = now_ns();
  const char* ls = find_binary("/usr/bin/ls", "/bin/ls");
  const char* bash = find_binary("/usr/bin/bash", "/bin/bash");
  const char* nano = find_binary("/usr/bin/nano", "/bin/nano");
  if (!ls || !bash) {
    errno = ENOENT;
    die("required-binary");
  }
  for (int i = 0; i < 3; ++i) {
    char phase[32];
    snprintf(phase, sizeof(phase), "ls_long_%d", i);
    char* args[] = {(char*)ls, "-l", "/", NULL};
    run_child(phase, ls, args);
  }
  if (nano) {
    for (int i = 0; i < 3; ++i) {
      char phase[32];
      snprintf(phase, sizeof(phase), "nano_version_%d", i);
      char* args[] = {(char*)nano, "--version", NULL};
      run_child(phase, nano, args);
    }
  } else
    printf("IOBENCH SKIP phase=nano_version reason=not-installed\n");
  /* First pass is only cold if the caller starts from a fresh guest/cache. */
  read_file("bash_read_first", bash, 1);
  read_file("bash_read_warm", bash, 1);
  if (read_diagnostics) {
    read_file("bash_read_without_checksum", bash, 0);
    for (int i = 0; i < 3; ++i) {
      char phase[32];
      snprintf(phase, sizeof(phase), "fork_exit_%d", i);
      run_child(phase, NULL, NULL);
      snprintf(phase, sizeof(phase), "exec_self_%d", i);
      char* args[] = {argv[0], "--exec-probe", NULL};
      run_child(phase, argv[0], args);
    }
    uint64_t t = now_ns();
    for (int i = 0; i < 100; ++i) {
      struct stat st;
      if (stat("/", &st))
        die("stat-root");
    }
    metric("stat_root_100", t, 0);
  }
  if (!read_only) {
    size_t length = mib * 1024 * 1024;
    unsigned char buf[16384];
    uint64_t t = now_ns();
    int fd = open(scratch, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
      die("scratch-create");
    scratch_created = 1;
    for (size_t pos = 0; pos < length; pos += sizeof(buf)) {
      for (size_t i = 0; i < sizeof(buf); ++i)
        buf[i] = pattern(pos + i);
      full_write(fd, buf, sizeof(buf));
    }
    metric("scratch_write", t, length);
    t = now_ns();
    if (fsync(fd))
      die("scratch-fsync");
    metric("scratch_fsync", t, length);
    verify_scratch(length, 0);
    if (concurrent_sync)
      read_under_sync(fd, length, bash);
    t = now_ns();
    unsigned char* p = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED)
      die("mmap");
    for (size_t at = 17; at < length; at += 4096)
      p[at] ^= 0xa5;
    metric("mmap_mutate", t, length);
    t = now_ns();
    if (msync(p, length, MS_SYNC))
      die("msync");
    metric("mmap_msync", t, length);
    t = now_ns();
    if (munmap(p, length))
      die("munmap");
    metric("mmap_munmap", t, length);
    if (close(fd))
      die("scratch-close");
    verify_scratch(length, 1);
    if (keep_scratch) {
      printf("IOBENCH retained path=%s bytes=%zu\n", scratch, length);
    } else {
      t = now_ns();
      if (unlink(scratch))
        die("scratch-unlink");
      scratch_created = 0;
      metric("scratch_unlink", t, 0);
    }
  }
  metric("total", start, 0);
  printf("IOBENCH PASS END\n");
  return 0;
}
