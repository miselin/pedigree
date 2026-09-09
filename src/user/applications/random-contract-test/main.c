/* Copyright (c) 2026, Pedigree Developers. See LICENSE. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/auxv.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/uio.h>
#include <sys/wait.h>

#define CHECK(test)                                                    \
  do {                                                                 \
    if (!(test)) {                                                     \
      fprintf(stderr, "RNG FAIL line %d errno %d\n", __LINE__, errno); \
      return 1;                                                        \
    }                                                                  \
  } while (0)

static int test_device_writes(void) {
  const char* paths[] = {"/dev/random", "/dev/urandom"};
  unsigned char payload[513];
  memset(payload, 0xa5, sizeof(payload));
  for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
    int fd = open(paths[i], O_WRONLY);
    CHECK(fd >= 0);
    CHECK(write(fd, payload, 0) == 0);
    CHECK(write(fd, payload, sizeof(payload)) == sizeof(payload));
    struct iovec vectors[] = {{payload, 7}, {payload, 0}, {payload + 7, sizeof(payload) - 7}};
    CHECK(writev(fd, vectors, 3) == sizeof(payload));

    // Dropbear feeds bytes back through stdio while initializing its PRNG.
    FILE* stream = fdopen(fd, "w");
    CHECK(stream != NULL);
    CHECK(fwrite(payload, 1, sizeof(payload), stream) == sizeof(payload));
    CHECK(fflush(stream) == 0);
    CHECK(fclose(stream) == 0);
  }
  puts("RNG-WRITE-PASS");
  return 0;
}

int main(int argc, char** argv) {
  unsigned char first[513], second[513];
#if defined(__x86_64__)
  unsigned a = 1, b, c, d;
  __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
  unsigned rdrand = (c >> 30) & 1;
  a = 7;
  c = 0;
  __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
  printf("RNG-CPU rdrand=%u rdseed=%u\n", rdrand, (b >> 18) & 1);
#endif
  if (argc == 2 && !strcmp(argv[1], "--unseeded")) {
    errno = 0;
    CHECK(getrandom(first, sizeof(first), GRND_NONBLOCK) == -1 && errno == EAGAIN);
    CHECK(test_device_writes() == 0);
    errno = 0;
    CHECK(getrandom(first, sizeof(first), GRND_NONBLOCK) == -1 && errno == EAGAIN);
    puts("RNG-UNSEEDED-PASS");
    return 0;
  }
  CHECK(test_device_writes() == 0);
  CHECK(getauxval(AT_RANDOM) != 0);
  CHECK(getrandom(first, 0, 0) == 0);
  CHECK(getrandom(first, 256, 0) == 256);
  CHECK(getrandom(second, 256, GRND_NONBLOCK) == 256);
  CHECK(memcmp(first, second, 256) != 0);
  errno = 0;
  CHECK(getrandom(first, 32, 0x40000000) == -1 && errno == EINVAL);
  int random = open("/dev/urandom", O_RDONLY);
  CHECK(random >= 0);
  CHECK(read(random, first, sizeof(first)) == sizeof(first));
  CHECK(read(random, second, sizeof(second)) == sizeof(second));
  CHECK(memcmp(first, second, sizeof(first)) != 0);

  struct {
    int bits;
    int size;
    unsigned char seed[32];
  } request = {256, 31, {0}};
  errno = 0;
  CHECK(ioctl(random, 0x40085203UL, &request) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(ioctl(random, 0x40085203UL, (void*)1) == -1 && errno == EFAULT);
  pid_t child = fork();
  CHECK(child >= 0);
  if (!child) {
    CHECK(setuid(65534) == 0);
    request.size = 32;
    errno = 0;
    CHECK(ioctl(random, 0x40085203UL, &request) == -1 && (errno == EPERM || errno == EACCES));
    _exit(0);
  }
  int status;
  CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
  close(random);

  int readers[4];
  pid_t children[4];
  for (int i = 0; i < 4; ++i) {
    int stream[2];
    CHECK(pipe(stream) == 0);
    children[i] = fork();
    CHECK(children[i] >= 0);
    if (!children[i]) {
      close(stream[0]);
      unsigned char sample[64];
      CHECK(getrandom(sample, sizeof(sample), 0) == sizeof(sample));
      CHECK(write(stream[1], sample, sizeof(sample)) == sizeof(sample));
      _exit(0);
    }
    close(stream[1]);
    readers[i] = stream[0];
  }
  unsigned char samples[4][64];
  for (int i = 0; i < 4; ++i) {
    CHECK(read(readers[i], samples[i], sizeof(samples[i])) == sizeof(samples[i]));
    close(readers[i]);
    CHECK(waitpid(children[i], &status, 0) == children[i] && WIFEXITED(status) &&
          !WEXITSTATUS(status));
    for (int j = 0; j < i; ++j)
      CHECK(memcmp(samples[i], samples[j], sizeof(samples[i])) != 0);
  }
  puts("RNG-SEEDED-PASS");
  return 0;
}
