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

#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>

static int serial_fd = -1;
static int stdio_mode;
static int trace_link;
static int link_only;
static int quick;
static const char* profile_phase;
static unsigned profile_rows;
extern char** environ;

static char* child_environment[] = {
    "PATH=/usr/bin:/bin", "LC_ALL=C", "HOME=/root", "TMPDIR=/tmp",
    "CPLUS_INCLUDE_PATH=/usr/include/c++/15.3.0:/usr/include/c++/15.3.0/x86_64-pedigree", NULL};
static const char* inputs[] = {"which.cc", "which.ii", "which.s", "which.o", "tiny.cc"};
static const char* cases[] = {"tiny", "tiny-pipe", "preprocess", "syntax", "codegen",
                            "assemble", "link", "full", "full-pipe"};
static const char* rounds[] = {"warm", "r1", "r2", "r3"};
static const char* link_rounds[] = {"warm", "r1", "r2", "r3", "r4", "r5"};
static const unsigned quick_cases[] = {0, 2, 6, 7};

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
    if (n == 1 && c == 'g') {
      printf("COMPILEBENCH ACK phase=%s\n", phase);
      return;
    }
    if (n == 1 || (n < 0 && errno == EINTR))
      continue;
    if ((!n && stdio_mode) || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
      fail("gate-read");
    struct pollfd p = {serial_fd, POLLIN, 0};
    int rc;
    do {
      // The Pedigree serial device does not publish readiness edges.
      rc = poll(&p, 1, 10);
    } while (rc < 0 && errno == EINTR);
    if (rc < 0 || (p.revents & (POLLERR | POLLNVAL | POLLHUP)))
      fail("gate-poll");
  }
}

/* Distinct bodies preserve plugin gates without invoking the kernel. */
__attribute__((noinline, noclone)) void profile_compile_begin(void) {
  __asm__ volatile("nop" ::: "memory");
}

__attribute__((noinline, noclone)) void profile_compile_end(void) {
  __asm__ volatile("nop; nop" ::: "memory");
}

static int wants_profile(const char* phase) {
  int selected = profile_phase && !strcmp(profile_phase, phase);
  if (selected)
    ++profile_rows;
  return selected;
}

static void metric(const char* phase, uint64_t start, uint64_t end, int rc,
                   const struct rusage* usage, uint64_t checksum) {
  printf("COMPILEBENCH metric phase=%s total_us=%llu rc=%d user_us=%llu system_us=%llu "
         "minor_faults=%ld major_faults=%ld in_blocks=%ld out_blocks=%ld "
         "voluntary_switches=%ld involuntary_switches=%ld checksum=%llu\n",
         phase, (unsigned long long)((end - start) / 1000), rc,
         (unsigned long long)timeval_us(usage->ru_utime),
         (unsigned long long)timeval_us(usage->ru_stime), usage->ru_minflt, usage->ru_majflt,
         usage->ru_inblock, usage->ru_oublock, usage->ru_nvcsw, usage->ru_nivcsw,
         (unsigned long long)checksum);
  printf("COMPILEBENCH DONE phase=%s\n", phase);
}

static int run_child(char* const args[], int log_fd, int null_fd, struct rusage* usage,
                     uint64_t* start, uint64_t* end, int profile, int trace) {
  *start = now_ns();
  if (profile)
    profile_compile_begin();
  pid_t child = fork();
  if (child < 0)
    fail("fork");
  if (!child) {
    if (dup2(null_fd, STDIN_FILENO) < 0 || dup2(log_fd, STDOUT_FILENO) < 0 ||
        dup2(log_fd, STDERR_FILENO) < 0)
      _exit(125);
    if (null_fd > STDERR_FILENO)
      close(null_fd);
    if (log_fd > STDERR_FILENO)
      close(log_fd);
    if (serial_fd > STDERR_FILENO)
      close(serial_fd);
    environ = child_environment;
    // Arm only the compiler child, so warmup and output verification stay untraced.
    if (trace && syscall(SYS_syslog, 20, NULL, 1) != 0) {
      dprintf(STDERR_FILENO, "trace gate unsupported or failed: errno=%d\n", errno);
      _exit(126);
    }
    execvp(args[0], args);
    dprintf(STDERR_FILENO, "exec %s failed: errno=%d\n", args[0], errno);
    _exit(127);
  }
  int status;
  pid_t waited;
  do {
    // GCC reaps its tool children before exiting, carrying their usage here.
    waited = wait4(child, &status, 0, usage);
  } while (waited < 0 && errno == EINTR);
  if (profile)
    profile_compile_end();
  *end = now_ns();
  if (waited != child)
    fail("wait4");
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

static void command(const char* phase, char* const args[], int measured) {
  char log_path[128];
  snprintf(log_path, sizeof(log_path), "matrix-%s%s.log", phase, measured ? "" : ".verify");
  int log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  int null_fd = open("/dev/null", O_RDONLY);
  if (log_fd < 0 || null_fd < 0)
    fail("command-files");
  printf("COMPILEBENCH command phase=%s log=%s argv=", phase, log_path);
  for (unsigned i = 0; args[i]; ++i)
    printf("%s%s", i ? " " : "", args[i]);
  printf("\n");
  if (measured)
    gate(phase);
  struct rusage usage = {0};
  uint64_t start, end;
  int rc = run_child(args, log_fd, null_fd, &usage, &start, &end,
                    measured && wants_profile(phase),
                    measured && trace_link && !strcmp(phase, "trace-link"));
  if (close(log_fd) || close(null_fd))
    fail("command-close");
  if (measured)
    metric(phase, start, end, rc, &usage, 0);
  else
    printf("COMPILEBENCH verify phase=%s rc=%d\n", phase, rc);
  if (rc)
    fail(phase);
}

struct identity {
  uint64_t bytes;
  uint64_t hash;
};

static struct identity identify(const char* path, const char* stage) {
  struct stat st;
  int fd = open(path, O_RDONLY);
  if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0)
    fail(path);
  struct identity id = {0, 14695981039346656037ULL};
  unsigned char buffer[4096];
  for (;;) {
    ssize_t n = read(fd, buffer, sizeof(buffer));
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0)
      fail("identity-read");
    if (!n)
      break;
    id.bytes += n;
    for (ssize_t i = 0; i < n; ++i) {
      id.hash ^= buffer[i];
      id.hash *= 1099511628211ULL;
    }
  }
  if (close(fd) || id.bytes != (uint64_t)st.st_size)
    fail("identity-size");
  printf("COMPILEBENCH input stage=%s path=%s bytes=%llu fnv1a64=%016llx\n", stage, path,
         (unsigned long long)id.bytes, (unsigned long long)id.hash);
  return id;
}

static void unchanged(const char* path, struct identity before) {
  struct identity after = identify(path, "after");
  if (before.bytes != after.bytes || before.hash != after.hash)
    fail("input-changed");
}

static void cpu_control(const char* phase) {
  gate(phase);
  struct rusage before, after;
  if (getrusage(RUSAGE_SELF, &before))
    fail("getrusage");
  int profile = wants_profile(phase);
  uint64_t start = now_ns();
  if (profile)
    profile_compile_begin();
  uint64_t value = 0x123456789abcdefULL;
  for (uint64_t i = 0; i < 100000000; ++i) {
    value ^= value << 13;
    value ^= value >> 7;
    value ^= value << 17;
  }
  if (profile)
    profile_compile_end();
  uint64_t end = now_ns();
  if (getrusage(RUSAGE_SELF, &after))
    fail("getrusage");
  uint64_t user = timeval_us(after.ru_utime) - timeval_us(before.ru_utime);
  uint64_t system = timeval_us(after.ru_stime) - timeval_us(before.ru_stime);
  after.ru_utime.tv_sec = user / 1000000;
  after.ru_utime.tv_usec = user % 1000000;
  after.ru_stime.tv_sec = system / 1000000;
  after.ru_stime.tv_usec = system % 1000000;
  after.ru_minflt -= before.ru_minflt;
  after.ru_majflt -= before.ru_majflt;
  after.ru_inblock -= before.ru_inblock;
  after.ru_oublock -= before.ru_oublock;
  after.ru_nvcsw -= before.ru_nvcsw;
  after.ru_nivcsw -= before.ru_nivcsw;
  metric(phase, start, end, 0, &after, value);
}

static void run_case(const char* round, unsigned index) {
  char phase[64], output[96];
  snprintf(phase, sizeof(phase), "%s-%s", round, cases[index]);
  const char* suffix = index == 2 ? ".ii" : index == 4 ? ".s" : index == 5 ? ".o" : "";
  // Warmup uses the same output path lifecycle as every measured round.
  snprintf(output, sizeof(output), "matrix-%s%s", cases[index], suffix);
  char* args[20] = {"gcc"};
  unsigned n = 1;
  if (index <= 1) {
    args[n++] = "-nostdinc";
    args[n++] = "-nostdlib";
    args[n++] = "-static";
    args[n++] = "-fno-stack-protector";
    args[n++] = "-fno-pie";
    args[n++] = "-no-pie";
    if (index == 1)
      args[n++] = "-pipe";
    args[n++] = "tiny.cc";
  } else if (index <= 5) {
    static char* flags[] = {"-E", "-fsyntax-only", "-S", "-c"};
    args[n++] = flags[index - 2];
    args[n++] = index == 2 ? "which.cc" : index == 5 ? "which.s" : "which.ii";
  } else {
    if (index == 8)
      args[n++] = "-pipe";
    args[n++] = index == 6 ? "which.o" : "which.cc";
    args[n++] = "-lstdc++";
  }
  if (index != 3) {
    args[n++] = "-o";
    args[n++] = output;
  }
  args[n] = NULL;
  command(phase, args, 1);
  if (index != 3) {
    struct stat st;
    if (stat(output, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0)
      fail(output);
    printf("COMPILEBENCH file path=%s bytes=%lld\n", output, (long long)st.st_size);
  }
  if (index <= 1 || index >= 6) {
    char executable[100];
    snprintf(executable, sizeof(executable), "./%s", output);
    char* verify[] = {executable, index <= 1 ? NULL : "gcc", NULL};
    command(phase, verify, 0);
  }
}

static void prepare(void) {
  struct identity source = identify(inputs[0], "before");
  struct identity tiny = identify(inputs[4], "before");
  // Reserve all destinations before launching GCC; preparation never replaces a fixture.
  for (unsigned i = 1; i <= 3; ++i) {
    struct stat st;
    if (!lstat(inputs[i], &st) || errno != ENOENT)
      fail("prepare-existing-input");
  }
  for (unsigned i = 1; i <= 3; ++i) {
    int fd = open(inputs[i], O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0 || close(fd))
      fail("prepare-reserve");
  }
  char* preprocess[] = {"gcc", "-E", "which.cc", "-o", "which.ii", NULL};
  char* codegen[] = {"gcc", "-S", "which.ii", "-o", "which.s", NULL};
  char* assemble[] = {"gcc", "-c", "which.s", "-o", "which.o", NULL};
  command("prepare-preprocess", preprocess, 1);
  identify(inputs[1], "prepared");
  command("prepare-codegen", codegen, 1);
  identify(inputs[2], "prepared");
  command("prepare-assemble", assemble, 1);
  identify(inputs[3], "prepared");
  unchanged(inputs[0], source);
  unchanged(inputs[4], tiny);
}

static int valid_profile(int preparing) {
  if (!profile_phase)
    return 1;
  if (trace_link)
    return !strcmp(profile_phase, "warm-link") || !strcmp(profile_phase, "trace-link");
  if (link_only) {
    for (unsigned round = 0; round < 6; ++round) {
      char phase[64];
      snprintf(phase, sizeof(phase), "%s-link", link_rounds[round]);
      if (!strcmp(profile_phase, phase))
        return 1;
    }
    return 0;
  }
  if (preparing)
    return !strcmp(profile_phase, "prepare-preprocess") ||
           !strcmp(profile_phase, "prepare-codegen") ||
           !strcmp(profile_phase, "prepare-assemble");
  if (!strcmp(profile_phase, "cpu-before") || !strcmp(profile_phase, "cpu-after"))
    return 1;
  if (quick) {
    for (unsigned i = 0; i < 4; ++i) {
      char phase[64];
      snprintf(phase, sizeof(phase), "r1-%s", cases[quick_cases[i]]);
      if (!strcmp(profile_phase, phase)) {
        return 1;
      }
    }
    return 0;
  }
  for (unsigned round = 0; round < 4; ++round)
    for (unsigned i = 0; i < 9; ++i) {
      char phase[64];
      snprintf(phase, sizeof(phase), "%s-%s", rounds[round], cases[i]);
      if (!strcmp(profile_phase, phase))
        return 1;
    }
  return 0;
}

static void module_addresses(void) {
#ifdef SYS_syslog
  static char log[256 * 1024];
  long size = syscall(SYS_syslog, 3, log, sizeof(log) - 1);
  if (size <= 0)
    return;
  log[size] = 0;
  for (char* line = strtok(log, "\n"); line; line = strtok(NULL, "\n")) {
    char* module = strstr(line, "KERNELELF: Preloaded module ");
    if (module)
      printf("COMPILEBENCH %s\n", module);
  }
#endif
}

static int mode_marker(const char* path, const char* mode) {
  FILE* file = fopen(path, "r");
  if (!file) {
    if (errno != ENOENT)
      fail(path);
    return 0;
  }
  char selection[16];
  size_t size = fread(selection, 1, sizeof(selection), file);
  if (ferror(file) || fclose(file))
    fail(path);
  const size_t length = strlen(mode);
  if (size < length || memcmp(selection, mode, length) ||
      !(size == length || (size == length + 1 && selection[length] == '\n') ||
        (size == length + 2 && selection[length] == '\r' && selection[length + 1] == '\n'))) {
    errno = EINVAL;
    fail(path);
  }
  return 1;
}

int main(int argc, char** argv) {
  int preparing = 0, have_mode = 0;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--stdio"))
      stdio_mode = 1;
    else if ((!strcmp(argv[i], "--prepare") || !strcmp(argv[i], "--run")) && !have_mode) {
      preparing = !strcmp(argv[i], "--prepare");
      have_mode = 1;
    } else {
      errno = EINVAL;
      fail("arguments");
    }
  }
  serial_fd = stdio_mode ? STDIN_FILENO : open("/dev/ttyS0", O_RDWR | O_NONBLOCK);
  if (serial_fd < 0 || (!stdio_mode && (dup2(serial_fd, STDOUT_FILENO) < 0 ||
                                      dup2(serial_fd, STDERR_FILENO) < 0)))
    fail("serial-open");
  setvbuf(stdout, NULL, _IONBF, 0);
  struct termios t;
  if (!tcgetattr(serial_fd, &t)) {
    t.c_iflag = t.c_oflag = t.c_lflag = 0;
    t.c_cflag = (t.c_cflag & ~(CSIZE | PARENB)) | CS8;
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(serial_fd, TCSANOW, &t))
      fail("serial-termios");
  } else if (errno != ENOTTY)
    fail("serial-termios");
  if (chdir("/root/compile-bench"))
    fail("setup");
  trace_link = mode_marker("matrix-trace", "link");
  link_only = mode_marker("matrix-link-only", "link");
  quick = mode_marker("matrix-quick", "quick");
  if (trace_link + link_only + quick > 1 || (preparing && (trace_link || link_only || quick))) {
    errno = EINVAL;
    fail("link-mode");
  }
  if (trace_link) {
    struct utsname system;
    if (uname(&system) || strcmp(system.sysname, "Pedigree")) {
      errno = EOPNOTSUPP;
      fail("trace-platform");
    }
  }
  char profile_buffer[64];
  profile_phase = getenv("MATRIX_PROFILE");
  if (!profile_phase) {
    FILE* file = fopen("matrix-profile", "r");
    if (file) {
      if (!fgets(profile_buffer, sizeof(profile_buffer), file) ||
          fgetc(file) != EOF || ferror(file))
        fail("profile-file");
      profile_buffer[strcspn(profile_buffer, "\r\n")] = 0;
      if (fclose(file))
        fail("profile-close");
      profile_phase = profile_buffer;
    } else if (errno != ENOENT)
      fail("profile-open");
  }
  if (profile_phase && !*profile_phase)
    profile_phase = NULL;
  if (!valid_profile(preparing)) {
    errno = EINVAL;
    fail("profile-phase");
  }
  printf("COMPILEBENCH BEGIN\n");
  printf("COMPILEBENCH configuration mode=%s profile=%s\n",
         preparing    ? "prepare"
         : trace_link ? "trace-link"
         : link_only  ? "link"
         : quick      ? "quick"
                      : "run",
         profile_phase ? profile_phase : "none");
  module_addresses();
  if (preparing)
    prepare();
  else {
    struct identity before[5];
    for (unsigned i = 0; i < 5; ++i)
      before[i] = identify(inputs[i], "before");
    if (trace_link) {
      run_case("warm", 6);
      run_case("trace", 6);
    } else if (link_only) {
      for (unsigned round = 0; round < 6; ++round)
        run_case(link_rounds[round], 6);
    } else if (quick) {
      cpu_control("cpu-before");
      for (unsigned i = 0; i < 4; ++i) {
        run_case("r1", quick_cases[i]);
      }
      cpu_control("cpu-after");
    } else {
      cpu_control("cpu-before");
      for (unsigned round = 0; round < 4; ++round)
        for (unsigned i = 0; i < 9; ++i)
          run_case(rounds[round], round == 2 ? 8 - i : i);
      cpu_control("cpu-after");
    }
    for (unsigned i = 0; i < 5; ++i)
      unchanged(inputs[i], before[i]);
  }
  if (profile_phase && profile_rows != 1)
    fail("profile-phase");
  printf("COMPILEBENCH PASS END\n");
  return 0;
}
