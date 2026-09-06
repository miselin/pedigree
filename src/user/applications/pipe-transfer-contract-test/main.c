#define _GNU_SOURCE
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>

int64_t pt_now(void) {
  struct timespec now;
  return clock_gettime(CLOCK_MONOTONIC, &now) ? -1 : (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}
void pt_pause(int milliseconds) {
  struct timespec pause = {milliseconds / 1000, (milliseconds % 1000) * 1000000L};
  while (nanosleep(&pause, &pause) && errno == EINTR) {
  }
}
int pt_wait(volatile int* flag, int milliseconds) {
  int64_t until = pt_now() + (int64_t)milliseconds * 1000000;
  while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE)) {
    if (pt_now() >= until)
      return -1;
    pt_pause(2);
  }
  return 0;
}
int pt_reap(pid_t child, int milliseconds) {
  int64_t until = pt_now() + (int64_t)milliseconds * 1000000;
  while (pt_now() < until) {
    int status;
    pid_t result = waitpid(child, &status, WNOHANG);
    if (result == child) {
      int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
      if (code)
        fprintf(stderr, "PIPE-TRANSFER-CONTRACT: child=%ld status=%d\n", (long)child, code);
      return code;
    }
    if (result < 0 && errno != EINTR)
      return -1;
    pt_pause(5);
  }
  fprintf(stderr, "PIPE-TRANSFER-CONTRACT: child=%ld timeout\n", (long)child);
  kill(child, SIGKILL);
  while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {
  }
  return -1;
}
int pt_ready(int fd, short events, int milliseconds) {
  struct pollfd entry = {.fd = fd, .events = events};
  int result;
  do
    result = poll(&entry, 1, milliseconds);
  while (result < 0 && errno == EINTR);
  return result < 0 ? -1 : !!(entry.revents & events);
}
int pt_wait_readiness(int fd, short events, int expected) {
  int64_t until = pt_now() + 3000000000;
  do {
    int ready = pt_ready(fd, events, 0);
    if (ready < 0 || ready == expected)
      return ready < 0 ? -1 : 0;
    pt_pause(2);
  } while (pt_now() < until);
  return -1;
}
unsigned char pt_pattern(size_t offset, int seed) {
  return seed == PT_FILLER ? 0x7a : ((offset * 17) ^ (offset >> 9) ^ seed) & 255;
}
int pt_fill(int fd, size_t count, int seed) {
  unsigned char bytes[PT_CAPACITY];
  if (count > sizeof(bytes))
    return -1;
  for (size_t n = 0; n < count; ++n)
    bytes[n] = pt_pattern(n, seed);
  return write(fd, bytes, count) == (ssize_t)count ? 0 : -1;
}
int pt_read(int fd, size_t count, size_t source_offset, int seed) {
  unsigned char bytes[PT_CAPACITY];
  size_t done = 0;
  while (done < count) {
    if (pt_ready(fd, POLLIN, 3000) != 1)
      return -1;
    size_t amount = count - done < sizeof(bytes) ? count - done : sizeof(bytes);
    ssize_t received = read(fd, bytes, amount);
    if (received <= 0)
      return -1;
    for (ssize_t n = 0; n < received; ++n)
      if (bytes[n] != pt_pattern(source_offset + done + n, seed))
        return -1;
    done += received;
  }
  return 0;
}
int pt_file(size_t count, int seed) {
  int fd = memfd_create("pipe-transfer", MFD_ALLOW_SEALING);
  if (fd < 0)
    return -1;
  unsigned char bytes[PT_CAPACITY];
  for (size_t offset = 0; offset < count;) {
    size_t amount = count - offset < sizeof(bytes) ? count - offset : sizeof(bytes);
    for (size_t n = 0; n < amount; ++n)
      bytes[n] = pt_pattern(offset + n, seed);
    if (pwrite(fd, bytes, amount, offset) != (ssize_t)amount) {
      close(fd);
      return -1;
    }
    offset += amount;
  }
  return fd;
}
int pt_verify(int fd, off_t offset, size_t count, size_t source_offset, int seed) {
  unsigned char bytes[PT_CAPACITY];
  if (count > sizeof(bytes) || pread(fd, bytes, count, offset) != (ssize_t)count)
    return -1;
  for (size_t n = 0; n < count; ++n)
    if (bytes[n] != pt_pattern(source_offset + n, seed))
      return -1;
  return 0;
}
ssize_t pt_socket_full(int fd) {
  int flags = fcntl(fd, F_GETFL);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK))
    return -1;
  char bytes[4096];
  memset(bytes, 0x7a, sizeof(bytes));
  ssize_t total = 0;
  while (total < 1024 * 1024) {
    ssize_t written = write(fd, bytes, sizeof(bytes));
    if (written < 0 && errno == EAGAIN)
      return fcntl(fd, F_SETFL, flags) ? -1 : total;
    if (written <= 0)
      break;
    total += written;
  }
  fcntl(fd, F_SETFL, flags);
  return -1;
}
int pt_send_fd(int socket, int fd) {
  char byte = 'f';
  struct iovec vector = {&byte, 1};
  union {
    struct cmsghdr align;
    char bytes[CMSG_SPACE(sizeof(int))];
  } control = {0};
  struct msghdr message = {.msg_iov = &vector,
                           .msg_iovlen = 1,
                           .msg_control = control.bytes,
                           .msg_controllen = sizeof(control.bytes)};
  struct cmsghdr* item = CMSG_FIRSTHDR(&message);
  item->cmsg_level = SOL_SOCKET;
  item->cmsg_type = SCM_RIGHTS;
  item->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(item), &fd, sizeof(fd));
  return sendmsg(socket, &message, 0) == 1 ? 0 : -1;
}
int pt_receive_fd(int socket) {
  char byte;
  struct iovec vector = {&byte, 1};
  union {
    struct cmsghdr align;
    char bytes[CMSG_SPACE(sizeof(int))];
  } control = {0};
  struct msghdr message = {.msg_iov = &vector,
                           .msg_iovlen = 1,
                           .msg_control = control.bytes,
                           .msg_controllen = sizeof(control.bytes)};
  if (pt_ready(socket, POLLIN, 3000) != 1 || recvmsg(socket, &message, MSG_CMSG_CLOEXEC) != 1 ||
      (message.msg_flags & MSG_CTRUNC))
    return -1;
  struct cmsghdr* item = CMSG_FIRSTHDR(&message);
  if (!item || item->cmsg_level != SOL_SOCKET || item->cmsg_type != SCM_RIGHTS ||
      item->cmsg_len != CMSG_LEN(sizeof(int)))
    return -1;
  int fd;
  memcpy(&fd, CMSG_DATA(item), sizeof(fd));
  return fd;
}
void* pt_worker(void* argument) {
  struct pt_call* call = argument;
  __atomic_store_n(&call->ready, 1, __ATOMIC_RELEASE);
  if (!pt_wait(&call->gate, 5000)) {
    errno = 0;
    __atomic_store_n(&call->entered, 1, __ATOMIC_RELEASE);
    if (call->kind == PT_TEE)
      call->result = tee(call->input, call->output, call->count, call->flags);
    else if (call->kind == PT_VMSPLICE)
      call->result = vmsplice(call->input, call->vectors, call->vector_count, call->flags);
    else
      call->result = splice(call->input, call->input_offset, call->output, call->output_offset,
                            call->count, call->flags);
    call->error = errno;
  } else
    call->error = ETIMEDOUT;
  __atomic_store_n(&call->done, 1, __ATOMIC_RELEASE);
  return NULL;
}
void pt_diagnostic(const struct pt_call* call) {
  int done = __atomic_load_n(&call->done, __ATOMIC_ACQUIRE);
  fprintf(stderr, "PIPE-TRANSFER-CONTRACT: kind=%d count=%zu done=%d result=%ld/%d\n", call->kind,
          call->count, done, done ? (long)call->result : -2, done ? call->error : -2);
}
int pt_join(pthread_t worker, struct pt_call* call) {
  __atomic_store_n(&call->gate, 1, __ATOMIC_RELEASE);
  if (pt_wait(&call->done, 5000)) {
    pt_diagnostic(call);
    _exit(60);
  }
  return pthread_join(worker, NULL);
}
static int run(const char* name, int (*test)(void)) {
  printf("PIPE-TRANSFER-CONTRACT: BEGIN %s\n", name);
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
  int status = pt_reap(child, 45000);
  printf("PIPE-TRANSFER-CONTRACT: %s %s status=%d\n", status ? "FAIL" : "PASS", name, status);
  fflush(stdout);
  return status;
}
int main(int argc, char** argv) {
  if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
    return 1;
  const struct {
    const char* name;
    int (*test)(void);
  } suites[] = {{"splice", pipe_transfer_splice},       {"tee", pipe_transfer_tee},
                {"vmsplice", pipe_transfer_vmsplice},   {"lifetime", pipe_transfer_lifetime},
                {"readiness", pipe_transfer_readiness}, {"concurrency", pipe_transfer_concurrency}};
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
  puts("PIPE-TRANSFER-CONTRACT: END PASS");
  return 0;
}
