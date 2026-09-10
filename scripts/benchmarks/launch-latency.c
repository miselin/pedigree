#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define MAX_ARGS 32
#define PAGE_BYTES 4096U
#define MAX_FIXTURE (32U * 1024U * 1024U)
#define MAX_OUTPUT (1024U * 1024U)
static int serial_fd = -1;
static int config_mode;
static pid_t active_child = -1;
extern char** environ;

static void write_all(int fd, const void* buffer, size_t size);
static void fail(const char* operation) {
  static int failing;
  if (failing++)
    _exit(1);
  int saved_errno = errno;
  if (active_child > 0) {
    (void)kill(active_child, SIGKILL);
    while (waitpid(active_child, NULL, 0) < 0 && errno == EINTR) {
    }
  }
  // Init may have redirected stderr before the config's --serial is parsed.
  int destination = serial_fd;
  if (destination < 0 && config_mode)
    destination = open("/dev/ttyS0", O_WRONLY | O_NONBLOCK);
  if (destination < 0)
    destination = STDERR_FILENO;
  char text[160];
  int n = snprintf(text, sizeof(text), "LAUNCHBENCH FAIL operation=%s errno=%d\n", operation,
                   saved_errno);
  write_all(destination, text, (size_t)n);
  exit(1);
}

static void ready_fd(int fd, short events) {
  struct pollfd p = {fd, events, 0};
  int rc;
  do {
    rc = poll(&p, 1, -1);
  } while (rc < 0 && errno == EINTR);
  if (rc < 0 || (p.revents & (POLLERR | POLLNVAL)) || !(p.revents & events))
    fail("poll");
}

static void write_all(int fd, const void* buffer, size_t size) {
  const unsigned char* p = buffer;
  while (size) {
    ssize_t n = write(fd, p, size);
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      ready_fd(fd, POLLOUT);
      continue;
    }
    if (n <= 0)
      fail("write");
    p += n;
    size -= (size_t)n;
  }
}

static uint64_t now_ns(void) {
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t))
    fail("clock");
  return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}

static void marker(const char* kind, const char* phase) {
  char text[128];
  int n = snprintf(text, sizeof(text), "LAUNCHBENCH %s phase=%s\n", kind, phase);
  write_all(STDOUT_FILENO, text, (size_t)n);
}

static void gate(const char* phase) {
  marker("READY", phase);
  if (serial_fd < 0)
    return;
  for (;;) {
    char c;
    ssize_t n = read(serial_fd, &c, 1);
    if (n == 1 && c == 'g')
      return;
    if (n == 1 || (n < 0 && errno == EINTR))
      continue;
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
      fail("gate-read");
    ready_fd(serial_fd, POLLIN);
  }
}

static void metric(const char* phase, uint64_t start, uint64_t first, uint64_t end, uint64_t bytes,
                   uint64_t checksum) {
  char text[256];
  int n = snprintf(
      text, sizeof(text),
      "LAUNCHBENCH metric phase=%s first_us=%llu total_us=%llu bytes=%llu checksum=%llu\n", phase,
      (unsigned long long)((first - start) / 1000), (unsigned long long)((end - start) / 1000),
      (unsigned long long)bytes, (unsigned long long)checksum);
  write_all(STDOUT_FILENO, text, (size_t)n);
  marker("DONE", phase);
}

static void launch(const char* phase, const char* prefix, char* const args[]) {
  int output[2];
  if (pipe(output))
    fail("pipe");
  gate(phase);
  uint64_t start = now_ns(), first = 0, bytes = 0, checksum = 0;
  pid_t child = fork();
  if (child < 0)
    fail("fork");
  if (!child) {
    close(output[0]);
    int null_fd = open("/dev/null", O_RDWR);
    if (null_fd < 0 || dup2(null_fd, STDIN_FILENO) < 0 || dup2(null_fd, STDERR_FILENO) < 0 ||
        dup2(output[1], STDOUT_FILENO) < 0)
      _exit(125);
    if (null_fd > STDERR_FILENO)
      close(null_fd);
    if (output[1] > STDERR_FILENO)
      close(output[1]);
    if (serial_fd > STDERR_FILENO)
      close(serial_fd);
    serial_fd = -1;
    config_mode = 0;
    if (!args) {
      write_all(STDOUT_FILENO, "probe\n", 6);
      _exit(0);
    }
    char* environment[] = {"PATH=/usr/bin:/bin:/usr/sbin:/sbin", "LC_ALL=C", "HOME=/", NULL};
    environ = environment;
    execvp(args[0], args);
    _exit(127);
  }
  active_child = child;
  close(output[1]);
  size_t prefix_length = strlen(prefix);
  int valid = 1;
  for (;;) {
    unsigned char buffer[PAGE_BYTES];
    ssize_t n = read(output[0], buffer, sizeof(buffer));
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0)
      fail("pipe-read");
    if (!n)
      break;
    if (!first)
      first = now_ns();
    for (ssize_t i = 0; i < n; ++i) {
      if (bytes < prefix_length && buffer[i] != (unsigned char)prefix[bytes])
        valid = 0;
      checksum += buffer[i];
      ++bytes;
    }
    // Drain without retaining unbounded output, even when validation fails.
    if (bytes > MAX_OUTPUT) {
      valid = 0;
      (void)kill(child, SIGKILL);
    }
  }
  close(output[0]);
  int status;
  pid_t waited;
  do {
    waited = waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  uint64_t end = now_ns();
  if (waited != child)
    fail("waitpid");
  active_child = -1;
  if (!WIFEXITED(status) || WEXITSTATUS(status))
    fail("launch-exit");
  if (!first || bytes < prefix_length || !valid)
    fail("launch-output");
  metric(phase, start, first, end, bytes, checksum + bytes);
}

static size_t load_text(const char* path, char text[4097]) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    fail("open-list");
  size_t used = 0;
  while (used < 4097) {
    ssize_t n = read(fd, text + used, 4097 - used);
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0)
      fail("read-list");
    if (!n)
      break;
    used += (size_t)n;
  }
  close(fd);
  if (used > 4096)
    fail("list-too-large");
  if (memchr(text, 0, used))
    fail("list-embedded-nul");
  text[used] = 0;
  return used;
}

static size_t lines(char* text, char** items, size_t capacity) {
  size_t count = 0;
  while (*text) {
    if (count == capacity)
      fail("too-many-lines");
    items[count++] = text;
    char* end = strchr(text, '\n');
    if (!end)
      break;
    *end = 0;
    if (end > text && end[-1] == '\r')
      end[-1] = 0;
    text = end + 1;
  }
  return count;
}

static void prewarm(const char* list) {
  char text[4097];
  char* paths[MAX_ARGS];
  load_text(list, text);
  size_t count = lines(text, paths, MAX_ARGS);
  gate("prewarm");
  uint64_t start = now_ns(), first = 0, bytes = 0;
  for (size_t i = 0; i < count; ++i) {
    int fd = open(paths[i], O_RDONLY);
    if (fd < 0)
      fail("prewarm-open");
    for (;;) {
      unsigned char buffer[PAGE_BYTES];
      ssize_t n = read(fd, buffer, sizeof(buffer));
      if (n < 0 && errno == EINTR)
        continue;
      if (n < 0)
        fail("prewarm-read");
      if (!n)
        break;
      if (!first)
        first = now_ns();
      bytes += (uint64_t)n;
    }
    close(fd);
  }
  uint64_t end = now_ns();
  metric("prewarm", start, first ? first : start, end, bytes, 0);
}

static void read_fixture(const char* path, int permuted, unsigned iterations, size_t read_size) {
  int fd = open(path, O_RDONLY);
  struct stat st;
  if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
      st.st_size > MAX_FIXTURE || st.st_size % PAGE_BYTES)
    fail("fixture-size");
  size_t size = (size_t)st.st_size, pages = size / read_size;
  if (size % read_size)
    fail("fixture-read-size");
  if (permuted && (pages & (pages - 1)))
    fail("fixture-power-of-two");
  unsigned char* buffer = malloc(size);
  if (!buffer)
    fail("fixture-buffer");
  // Touch every destination page before admission; faults are not fixture I/O.
  memset(buffer, 0, size);
  volatile unsigned char* touched = buffer;
  for (size_t i = 0; i < size; i += PAGE_BYTES)
    touched[i] = 0;
  touched[size - 1] = 0;
  for (unsigned iteration = 0; iteration < iterations; ++iteration) {
    char phase[64];
    snprintf(phase, sizeof(phase), "read-%s-%u", permuted ? "permuted" : "sequential", iteration);
    gate(phase);
    uint64_t start = now_ns(), first = 0;
    for (size_t i = 0; i < pages; ++i) {
      // An odd multiplier permutes a power-of-two page count exactly once.
      size_t page = permuted ? (i * 1531U + 17U) % pages : i;
      size_t done = 0, offset = page * read_size;
      while (done < read_size) {
        ssize_t n = pread(fd, buffer + offset + done, read_size - done, (off_t)(offset + done));
        if (n < 0 && errno == EINTR)
          continue;
        if (n <= 0)
          fail("fixture-read");
        done += (size_t)n;
      }
      if (!first)
        first = now_ns();
    }
    uint64_t end = now_ns(), checksum = size;
    for (size_t i = 0; i < size; ++i) {
      if (buffer[i] != (unsigned char)(i * 37U + (i >> 8) * 17U + 0x53U)) {
        errno = EILSEQ;
        fail("fixture-pattern");
      }
      checksum += buffer[i];
    }
    metric(phase, start, first, end, size, checksum);
  }
  free(buffer);
  close(fd);
}

static void mmap_fixture(const char* path, int permuted, unsigned iterations) {
  int fd = open(path, O_RDONLY);
  struct stat st;
  if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
      st.st_size > MAX_FIXTURE || st.st_size % PAGE_BYTES)
    fail("fixture-size");
  size_t size = (size_t)st.st_size, pages = size / PAGE_BYTES;
  if (permuted && (pages & (pages - 1)))
    fail("fixture-power-of-two");
  unsigned char* mapping = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (mapping == MAP_FAILED)
    fail("fixture-mmap");
  close(fd);
  const volatile unsigned char* touched = mapping;
  for (unsigned iteration = 0; iteration < iterations; ++iteration) {
    char phase[64];
    snprintf(phase, sizeof(phase), "mmap-%s-%u", permuted ? "permuted" : "sequential", iteration);
    gate(phase);
    uint64_t start = now_ns(), first = 0, touched_checksum = 0;
    for (size_t i = 0; i < pages; ++i) {
      size_t page = permuted ? (i * 1531U + 17U) % pages : i;
      touched_checksum += touched[page * PAGE_BYTES];
      if (!first)
        first = now_ns();
    }
    uint64_t end = now_ns(), checksum = size, expected_touched = 0;
    // Validation follows the timed fault sequence and leaves this mapping warm.
    for (size_t i = 0; i < size; ++i) {
      unsigned char value = mapping[i];
      if (value != (unsigned char)(i * 37U + (i >> 8) * 17U + 0x53U)) {
        errno = EILSEQ;
        fail("fixture-pattern");
      }
      checksum += value;
      if (!(i % PAGE_BYTES))
        expected_touched += value;
    }
    if (touched_checksum != expected_touched) {
      errno = EILSEQ;
      fail("fixture-touches");
    }
    metric(phase, start, first, end, size, checksum);
  }
  if (munmap(mapping, size))
    fail("fixture-munmap");
}

int main(int argc, char** argv) {
  if (argc == 2 && !strcmp(argv[1], "--probe")) {
    write_all(STDOUT_FILENO, "probe\n", 6);
    return 0;
  }
  config_mode = argc == 1;
  char config[4097], self[4096];
  char* configured[MAX_ARGS + 1];
  if (strchr(argv[0], '/')) {
    if (!realpath(argv[0], self))
      fail("self-path");
  } else {
    const char* original_path = getenv("PATH");
    char* path = strdup(original_path ? original_path : "/usr/bin:/bin");
    if (!path)
      fail("self-path");
    int found = 0;
    char* position = path;
    while (position) {
      char* end = strchr(position, ':');
      if (end)
        *end++ = 0;
      char candidate[4096];
      int n = snprintf(candidate, sizeof(candidate), "%s/%s", *position ? position : ".", argv[0]);
      if (n > 0 && (size_t)n < sizeof(candidate) && !access(candidate, X_OK) &&
          realpath(candidate, self)) {
        found = 1;
        break;
      }
      position = end;
    }
    free(path);
    if (!found)
      fail("self-path");
  }
  if (argc == 1) {
    load_text("/launch-bench.args", config);
    configured[0] = self;
    argc = 1 + (int)lines(config, configured + 1, MAX_ARGS - 1);
    configured[argc] = NULL;
    argv = configured;
  }
  if (argc > MAX_ARGS || chdir("/"))
    fail("arguments");
  unsigned iterations = 3;
  size_t read_size = PAGE_BYTES;
  const char* prewarm_list = NULL;
  int arg = 1;
  while (arg < argc && !strncmp(argv[arg], "--", 2)) {
    if (!strcmp(argv[arg], "--serial")) {
      if (serial_fd >= 0)
        fail("duplicate-serial");
      serial_fd = open("/dev/ttyS0", O_RDWR | O_NONBLOCK);
      if (serial_fd < 0 || dup2(serial_fd, STDOUT_FILENO) < 0 || dup2(serial_fd, STDERR_FILENO) < 0)
        fail("serial-open");
      struct termios t;
      if (!tcgetattr(serial_fd, &t)) {
        t.c_iflag = 0;
        t.c_oflag = 0;
        t.c_lflag = 0;
        t.c_cflag = (t.c_cflag & ~(CSIZE | PARENB)) | CS8;
        t.c_cc[VMIN] = 1;
        t.c_cc[VTIME] = 0;
        (void)tcsetattr(serial_fd, TCSANOW, &t);
      }
      ++arg;
    } else if (!strcmp(argv[arg], "--prewarm") && arg + 1 < argc) {
      prewarm_list = argv[arg + 1];
      arg += 2;
    } else if (!strcmp(argv[arg], "--read-size") && arg + 1 < argc) {
      char* end;
      unsigned long n = strtoul(argv[arg + 1], &end, 10);
      if (!argv[arg + 1][0] || *end || n < PAGE_BYTES || n > 128 * 1024 || (n & (n - 1)))
        fail("read-size");
      read_size = n;
      arg += 2;
    } else if (!strcmp(argv[arg], "--iterations") && arg + 1 < argc) {
      char* end;
      unsigned long n = strtoul(argv[arg + 1], &end, 10);
      if (!argv[arg + 1][0] || *end || !n || n > 100)
        fail("iterations");
      iterations = (unsigned)n;
      arg += 2;
    } else {
      fail("option");
    }
  }
  if (prewarm_list)
    prewarm(prewarm_list);
  if (arg < argc && !strcmp(argv[arg], "launch") && arg + 2 < argc) {
    const char* prefix = argv[arg + 1];
    if (!*prefix || strlen(prefix) > 4096)
      fail("prefix");
    char phase[64];
    for (unsigned i = 0; i < iterations; ++i) {
      snprintf(phase, sizeof(phase), "launch-%u", i);
      launch(phase, prefix, argv + arg + 2);
    }
    char* probe[] = {self, "--probe", NULL};
    for (unsigned i = 0; i < iterations; ++i) {
      snprintf(phase, sizeof(phase), "fork-%u", i);
      launch(phase, "probe\n", NULL);
    }
    for (unsigned i = 0; i < iterations; ++i) {
      snprintf(phase, sizeof(phase), "exec-%u", i);
      launch(phase, "probe\n", probe);
    }
  } else if (arg + 3 == argc && !strcmp(argv[arg], "read") &&
             (!strcmp(argv[arg + 2], "sequential") || !strcmp(argv[arg + 2], "permuted"))) {
    read_fixture(argv[arg + 1], !strcmp(argv[arg + 2], "permuted"), iterations, read_size);
  } else if (arg + 3 == argc && !strcmp(argv[arg], "mmap") &&
             (!strcmp(argv[arg + 2], "sequential") || !strcmp(argv[arg + 2], "permuted"))) {
    mmap_fixture(argv[arg + 1], !strcmp(argv[arg + 2], "permuted"), iterations);
  } else {
    fail("usage");
  }
  const char passed[] = "LAUNCHBENCH PASS END\n";
  write_all(STDOUT_FILENO, passed, sizeof(passed) - 1);
  return 0;
}
