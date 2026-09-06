#define _GNU_SOURCE
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      fprintf(stderr, "SYSINFO-CONTRACT: line=%d errno=%d\n", __LINE__, errno); \
      goto fail;                                                                \
    }                                                                           \
  } while (0)

static uint64_t seconds(void) {
  struct timespec now;
  return clock_gettime(CLOCK_MONOTONIC, &now) ? UINT64_MAX : (uint64_t)now.tv_sec;
}

int main(void) {
  struct sysinfo first, second;
  int result = 1, gates[2] = {-1, -1};
  pid_t child = -1;
  const long page = sysconf(_SC_PAGESIZE);
  void* boundary = MAP_FAILED;
  void* memory = MAP_FAILED;
  CHECK(page >= 112);
  _Static_assert(offsetof(struct sysinfo, mem_unit) == 104, "amd64 sysinfo prefix");
  memset(&first, 0xa5, sizeof(first));
  const uint64_t before = seconds();
  CHECK(sysinfo(&first) == 0 && first.mem_unit == 1 && first.procs > 0);
  const uint64_t after = seconds();
  CHECK(first.totalram > 0 && first.freeram <= first.totalram && first.freeswap <= first.totalswap);
  CHECK(before != UINT64_MAX && after != UINT64_MAX);
  CHECK(first.uptime >= before && first.uptime <= after + 1);
  for (size_t i = 112; i < sizeof(first); ++i)
    CHECK(((const unsigned char*)&first)[i] == 0xa5);
  errno = 0;
  CHECK(sysinfo(NULL) == -1 && errno == EFAULT);
  boundary = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(boundary != MAP_FAILED);
  CHECK(mprotect((char*)boundary + page, page, PROT_NONE) == 0);
  CHECK(sysinfo((struct sysinfo*)((char*)boundary + page - 112)) == 0);
  errno = 0;
  CHECK(sysinfo((struct sysinfo*)((char*)boundary + page - 111)) == -1 && errno == EFAULT);
  CHECK(mprotect(boundary, page, PROT_READ) == 0);
  errno = 0;
  CHECK(sysinfo(boundary) == -1 && errno == EFAULT);
  memory = mmap(NULL, 32 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(memory != MAP_FAILED);
  for (size_t offset = 0; offset < 32 * (size_t)page; offset += page)
    ((volatile unsigned char*)memory)[offset] = 0x5c;
  CHECK(sysinfo(&second) == 0 && second.totalram == first.totalram &&
        second.freeram <= second.totalram && second.uptime >= first.uptime);
  CHECK(pipe(gates) == 0);
  CHECK(sysinfo(&first) == 0);
  child = fork();
  CHECK(child >= 0);
  if (!child) {
    close(gates[0]);
    if (write(gates[1], "r", 1) != 1)
      _exit(2);
    const uint64_t started = seconds();
    if (started == UINT64_MAX)
      _exit(3);
    const uint64_t end = started + 7;
    volatile uint64_t work = 0;
    while (seconds() < end) {
      for (unsigned i = 0; i < 100000; ++i)
        work += i;
    }
    _exit(0);
  }
  char ready;
  CHECK(read(gates[0], &ready, 1) == 1);
  CHECK(sysinfo(&second) == 0 && second.procs > first.procs);
  struct timespec delay = {6, 0};
  int slept;
  do {
    slept = nanosleep(&delay, &delay);
  } while (slept && errno == EINTR);
  CHECK(slept == 0);
  CHECK(sysinfo(&second) == 0 && second.loads[0] > 0 && second.loads[1] > 0 && second.loads[2] > 0);
  int status = 0;
  CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  child = -1;
  result = 0;
fail:
  if (child > 0)
    waitpid(child, NULL, 0);
  if (gates[0] >= 0)
    close(gates[0]);
  if (gates[1] >= 0)
    close(gates[1]);
  if (boundary != MAP_FAILED)
    munmap(boundary, 2 * page);
  if (memory != MAP_FAILED)
    munmap(memory, 32 * page);
  puts(result ? "SYSINFO-CONTRACT: FAIL" : "SYSINFO-CONTRACT: PASS");
  return result;
}
