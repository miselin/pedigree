#define _GNU_SOURCE
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "contract.h"
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/wait.h>

int64_t tf_now(void) {
  struct timespec now;
  return clock_gettime(CLOCK_MONOTONIC, &now) ? -1 : (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}
void tf_pause(int milliseconds) {
  struct timespec delay = {milliseconds / 1000, (milliseconds % 1000) * 1000000L};
  while (nanosleep(&delay, &delay) && errno == EINTR) {
  }
}
int tf_wait(volatile int* flag, int milliseconds) {
  const int64_t until = tf_now() + (int64_t)milliseconds * 1000000;
  while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE)) {
    if (tf_now() >= until)
      return -1;
    tf_pause(2);
  }
  return 0;
}
int tf_reap(pid_t child, int milliseconds) {
  const int64_t until = tf_now() + (int64_t)milliseconds * 1000000;
  while (tf_now() < until) {
    int status;
    pid_t result = waitpid(child, &status, WNOHANG);
    if (result == child) {
      int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
      if (code)
        fprintf(stderr, "TRANSFER-CONTRACT: child=%ld status=%d\n", (long)child, code);
      return code;
    }
    if (result < 0 && errno != EINTR)
      return -1;
    tf_pause(5);
  }
  fprintf(stderr, "TRANSFER-CONTRACT: child=%ld timeout\n", (long)child);
  kill(child, SIGKILL);
  while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {
  }
  return -1;
}
unsigned char tf_pattern(size_t offset, int seed) {
  return seed == TF_FILLER ? 0x7a : ((offset * 17) ^ (offset >> 9) ^ seed) & 255;
}
int tf_create(struct tf_file* file, const char* directory, size_t bytes, int seed) {
  static unsigned sequence;
  snprintf(file->path, sizeof(file->path), "%s/transfer-%ld-%u", directory, (long)getpid(),
           ++sequence);
  file->fd = open(file->path, O_CREAT | O_EXCL | O_RDWR, 0600);
  if (file->fd < 0)
    return -1;
  unsigned char buffer[4096];
  for (size_t offset = 0; offset < bytes;) {
    size_t count = bytes - offset < sizeof(buffer) ? bytes - offset : sizeof(buffer);
    for (size_t n = 0; n < count; ++n)
      buffer[n] = tf_pattern(offset + n, seed);
    if (pwrite(file->fd, buffer, count, offset) != (ssize_t)count)
      return -1;
    offset += count;
  }
  return 0;
}
void tf_destroy(struct tf_file* file) {
  if (file->fd >= 0)
    close(file->fd);
  if (*file->path)
    unlink(file->path);
  file->fd = -1;
}
int tf_verify(int fd, off_t offset, size_t length, size_t source_offset, int seed) {
  unsigned char buffer[4096];
  for (size_t done = 0; done < length;) {
    size_t count = length - done < sizeof(buffer) ? length - done : sizeof(buffer);
    if (pread(fd, buffer, count, offset + done) != (ssize_t)count)
      return -1;
    for (size_t n = 0; n < count; ++n)
      if (buffer[n] != tf_pattern(source_offset + done + n, seed))
        return -1;
    done += count;
  }
  return 0;
}
int tf_socket_read(int fd, size_t length, size_t source_offset, int seed) {
  unsigned char buffer[4096];
  const int64_t until = tf_now() + 5000000000;
  for (size_t done = 0; done < length;) {
    struct pollfd entry = {.fd = fd, .events = POLLIN};
    if (tf_now() >= until)
      return -1;
    int ready = poll(&entry, 1, 100);
    if (ready < 0 && errno == EINTR)
      continue;
    if (ready < 0)
      return -1;
    if (!ready)
      continue;
    size_t count = length - done < sizeof(buffer) ? length - done : sizeof(buffer);
    ssize_t received = read(fd, buffer, count);
    if (received <= 0)
      return -1;
    for (ssize_t n = 0; n < received; ++n)
      if (buffer[n] != tf_pattern(source_offset + done + n, seed))
        return -1;
    done += received;
  }
  return 0;
}
ssize_t tf_saturate(int fd) {
  int flags = fcntl(fd, F_GETFL);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK))
    return -1;
  char bytes[4096];
  memset(bytes, 0x7a, sizeof(bytes));
  ssize_t total = 0;
  while (total < 1024 * 1024) {
    ssize_t result = write(fd, bytes, sizeof(bytes));
    if (result < 0 && errno == EAGAIN) {
      if (fcntl(fd, F_SETFL, flags))
        return -1;
      return total;
    }
    if (result <= 0)
      break;
    total += result;
  }
  fcntl(fd, F_SETFL, flags);
  return -1;
}
ssize_t tf_copy(int kind, int input, off_t* input_offset, int output, off_t* output_offset,
                size_t count) {
  return kind == TF_SENDFILE
             ? sendfile(output, input, input_offset, count)
             : copy_file_range(input, input_offset, output, output_offset, count, 0);
}
void* tf_worker(void* argument) {
  struct tf_transfer* transfer = argument;
  __atomic_store_n(&transfer->ready, 1, __ATOMIC_RELEASE);
  if (!tf_wait(&transfer->gate, 5000)) {
    transfer->started = tf_now();
    errno = 0;
    transfer->result = tf_copy(transfer->kind, transfer->input, transfer->input_offset,
                               transfer->output, transfer->output_offset, transfer->count);
    transfer->error = errno;
    transfer->finished = tf_now();
  } else
    transfer->error = ETIMEDOUT;
  __atomic_store_n(&transfer->done, 1, __ATOMIC_RELEASE);
  return NULL;
}
void tf_diagnostic(const struct tf_transfer* transfer) {
  int done = __atomic_load_n(&transfer->done, __ATOMIC_ACQUIRE);
  fprintf(stderr,
          "TRANSFER-CONTRACT: kind=%d count=%zu done=%d result=%ld/%d start=%lld end=%lld\n",
          transfer->kind, transfer->count, done, done ? (long)transfer->result : -2,
          done ? transfer->error : -2, done ? (long long)transfer->started : -1,
          done ? (long long)transfer->finished : -1);
}
int tf_join(pthread_t thread, struct tf_transfer* transfer) {
  __atomic_store_n(&transfer->gate, 1, __ATOMIC_RELEASE);
  if (tf_wait(&transfer->done, 5000)) {
    tf_diagnostic(transfer);
    _exit(60);
  }
  return pthread_join(thread, NULL);
}
static int run(const char* name, int (*test)(void)) {
  printf("TRANSFER-CONTRACT: BEGIN %s\n", name);
  fflush(stdout);
  pid_t child = fork();
  if (child < 0)
    return -1;
  if (!child) {
    alarm(40);
    int result = test();
    fflush(stdout);
    fflush(stderr);
    _exit(result ? 1 : 0);
  }
  const int status = tf_reap(child, 45000);
  printf("TRANSFER-CONTRACT: %s %s status=%d\n", status ? "FAIL" : "PASS", name, status);
  fflush(stdout);
  return status;
}
int main(int argc, char** argv) {
  if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
    return 1;
  const struct {
    const char* name;
    int (*test)(void);
  } suites[] = {{"file-copy", transfer_file_copy},
                {"offsets", transfer_offsets},
                {"stream", transfer_stream},
                {"lifetime", transfer_lifetime},
                {"races", transfer_races}};
  int selected = 0;
  for (unsigned n = 0; n < sizeof(suites) / sizeof(suites[0]); ++n) {
    if (argc > 1 && strcmp(argv[1], suites[n].name))
      continue;
    selected = 1;
    if (run(suites[n].name, suites[n].test))
      return 1;
  }
  if (!selected)
    return 2;
  puts("TRANSFER-CONTRACT: END PASS");
  return 0;
}
