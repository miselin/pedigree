#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/swap.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>

static int failure(const char* name) {
  fprintf(stderr, "SWAP-TEST: FAIL %s errno=%d\n", name, errno);
  return 1;
}
static int read_header(const char* path, unsigned char* header) {
  int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0)
    return -1;
  ssize_t received;
  do {
    received = pread(fd, header, 4096, 0);
  } while (received < 0 && errno == EINTR);
  int error = received == 4096 ? 0 : received < 0 ? errno : EIO;
  if (close(fd) && !error)
    error = errno;
  errno = error;
  return error ? -1 : 0;
}
static uint32_t header_word(const unsigned char* data, size_t offset) {
  return (uint32_t)data[offset] | ((uint32_t)data[offset + 1] << 8) |
         ((uint32_t)data[offset + 2] << 16) | ((uint32_t)data[offset + 3] << 24);
}
static int discover_device(char* selected, size_t capacity) {
  DIR* directory = opendir("/dev/block");
  if (!directory)
    return -1;
  size_t matches = 0;
  int error = 0;
  for (;;) {
    errno = 0;
    struct dirent* entry = readdir(directory);
    if (!entry) {
      error = errno;
      break;
    }
    if (strncmp(entry->d_name, "disk", 4) || !entry->d_name[4])
      continue;
    const char* digit = entry->d_name + 4;
    while (*digit >= '0' && *digit <= '9')
      ++digit;
    if (*digit)
      continue;
    char candidate[512];
    int length = snprintf(candidate, sizeof(candidate), "/dev/block/%s", entry->d_name);
    if (length < 0 || (size_t)length >= sizeof(candidate)) {
      error = ENAMETOOLONG;
      break;
    }
    struct stat attributes;
    if (stat(candidate, &attributes)) {
      error = errno;
      break;
    }
    if (!S_ISBLK(attributes.st_mode) || major(attributes.st_rdev) != 241 ||
        attributes.st_size != (off_t)4097 * 4096)
      continue;
    int fd = open(candidate, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
      error = errno;
      break;
    }
    struct stat opened;
    unsigned char header[4096];
    ssize_t received = -1;
    if (fstat(fd, &opened))
      error = errno;
    else if (!S_ISBLK(opened.st_mode) || opened.st_rdev != attributes.st_rdev ||
             opened.st_size != attributes.st_size)
      error = EIO;
    else {
      do {
        received = pread(fd, header, sizeof(header), 0);
      } while (received < 0 && errno == EINTR);
      if (received != (ssize_t)sizeof(header))
        error = received < 0 ? errno : EIO;
    }
    if (close(fd) && !error)
      error = errno;
    if (error)
      break;
    if (memcmp(header + 4086, "SWAPSPACE2", 10) || header_word(header, 1024) != 1 ||
        header_word(header, 1028) != 4096 || header_word(header, 1032))
      continue;
    if (++matches > 1) {
      error = EEXIST;
      break;
    }
    if ((size_t)length >= capacity) {
      error = ENAMETOOLONG;
      break;
    }
    memcpy(selected, candidate, (size_t)length + 1);
  }
  if (closedir(directory) && !error)
    error = errno;
  if (!error && !matches)
    error = ENOENT;
  errno = error;
  return error ? -1 : 0;
}
static unsigned char value(size_t page, size_t byte) {
  return (unsigned char)(page * 29 + byte * 7 + 0x35);
}
static int contents(const unsigned char* p, size_t pages, size_t bytes) {
  for (size_t n = 0; n < pages; ++n)
    for (size_t i = 0; i < bytes; ++i) {
      const unsigned char actual = p[n * bytes + i], expected = value(n, i);
      if (actual != expected) {
        errno = 0;
        fprintf(stderr, "SWAP-TEST: CONTENT page=%zu byte=%zu expected=%u actual=%u\n", n, i,
                (unsigned)expected, (unsigned)actual);
        return 0;
      }
    }
  return 1;
}
static int resident(unsigned char* p, size_t pages, size_t bytes, int expected) {
  unsigned char vector[16];
  if (pages > sizeof(vector)) {
    errno = EINVAL;
    return 0;
  }
  errno = 0;
  if (mincore(p, pages * bytes, vector)) {
    fprintf(stderr, "SWAP-TEST: MINCORE error=%d\n", errno);
    return 0;
  }
  for (size_t i = 0; i < pages; ++i)
    if (!!(vector[i] & 1) != expected) {
      fprintf(stderr, "SWAP-TEST: RESIDENCY page=%zu expected=%d actual=%u\n", i, expected,
              (unsigned)vector[i]);
      return 0;
    }
  return 1;
}
int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  alarm(60);
  if (argc != 2 || geteuid() || sysconf(_SC_PAGESIZE) != 4096)
    return failure("requires root and a prepared disposable device or --discover");
  const size_t page = 4096, pages = 8, bytes = pages * page;
  const char* device = argv[1];
  char discovered[512];
  if (!strcmp(device, "--discover")) {
    if (discover_device(discovered, sizeof(discovered)))
      return failure("discovery requires exactly one prepared disposable disk");
    device = discovered;
  }
  struct stat st;
  if (stat(device, &st) || !S_ISBLK(st.st_mode))
    return failure("physical device selection");
  printf("SWAP-TEST: DEVICE path=%s size=%llu\n", device, (unsigned long long)st.st_size);
  unsigned char originalHeader[4096];
  if (read_header(device, originalHeader))
    return failure("initial header snapshot");
  errno = 0;
  if (swapon((const char*)1, 0) != -1 || errno != EFAULT)
    return failure("path copy");
  errno = 0;
  if (swapon(device, 0x80000000U) != -1 || errno != EINVAL)
    return failure("unknown flags");
  errno = 0;
  if (swapon(device, SWAP_FLAG_DISCARD) != -1 || errno != EOPNOTSUPP)
    return failure("unsupported flags");
  pid_t child = fork();
  if (child < 0)
    return failure("privilege fork");
  if (!child) {
    if (setuid(1000))
      _exit(2);
    errno = 0;
    if (swapon(device, 0) != -1 || errno != EPERM)
      _exit(3);
    errno = 0;
    _exit(swapoff(device) == -1 && errno == EPERM ? 0 : 4);
  }
  int status;
  if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status))
    return failure("privilege gate");
  puts("SWAP-TEST: PASS inputs-permissions");
  unsigned char* p = mmap(0, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED)
    return failure("anonymous mapping");
  for (size_t n = 0; n < pages; ++n)
    for (size_t i = 0; i < page; ++i)
      p[n * page + i] = value(n, i);
  int result = 1, active = 0;
  if (swapon(device, 0)) {
    failure("activate");
    goto done;
  }
  active = 1;
  errno = 0;
  if (swapon(device, 0) != -1 || errno != EBUSY) {
    failure("duplicate activation");
    goto done;
  }
  if (madvise(p, bytes, MADV_PAGEOUT) || !resident(p, pages, page, 0)) {
    failure("pageout residency");
    goto done;
  }
  int nullfd = open("/tmp/swap-contract-copy", O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (nullfd < 0 || write(nullfd, p, bytes) != (ssize_t)bytes) {
    if (nullfd >= 0)
      close(nullfd);
    failure("usercopy fault-in");
    goto done;
  }
  close(nullfd);
  unlink("/tmp/swap-contract-copy");
  if (!resident(p, pages, page, 1)) {
    failure("usercopy residency");
    goto done;
  }
  if (!contents(p, pages, page)) {
    failure("restored contents");
    goto done;
  }
  puts("SWAP-TEST: PASS eviction-usercopy-faultin");
  if (madvise(p, bytes, MADV_PAGEOUT)) {
    failure("fork preparation");
    goto done;
  }
  child = fork();
  if (child < 0) {
    failure("swapped fork");
    goto done;
  }
  if (!child) {
    printf("SWAP-TEST: CHILD stage=read-swapped address=%p pages=%zu\n", (void*)p, pages);
    if (!contents(p, pages, page))
      _exit(5);
    puts("SWAP-TEST: CHILD stage=private-write");
    p[0] ^= 0xff;
    puts("SWAP-TEST: CHILD stage=unmap");
    _exit(munmap(p, bytes) ? 6 : 0);
  }
  errno = 0;
  pid_t reaped = waitpid(child, &status, 0);
  if (reaped != child) {
    fprintf(stderr, "SWAP-TEST: CHILD wait expected=%ld actual=%ld errno=%d\n", (long)child,
            (long)reaped, errno);
    failure("fork wait");
    goto done;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status)) {
    fprintf(stderr, "SWAP-TEST: CHILD status=%#x exited=%d code=%d signalled=%d signal=%d\n",
            status, WIFEXITED(status), WIFEXITED(status) ? WEXITSTATUS(status) : -1,
            WIFSIGNALED(status), WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    failure("fork child completion");
    goto done;
  }
  if (!contents(p, pages, page)) {
    failure("fork parent contents");
    goto done;
  }
  if (madvise(p, bytes, MADV_PAGEOUT)) {
    failure("remap preparation");
    goto done;
  }
  unsigned char* destination = mmap(0, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (destination == MAP_FAILED) {
    failure("remap destination");
    goto done;
  }
  void* moved = mremap(p, bytes, bytes, MREMAP_MAYMOVE | MREMAP_FIXED, destination);
  if (moved == MAP_FAILED) {
    munmap(destination, bytes);
    failure("swapped remap");
    goto done;
  }
  p = moved;
  if (!resident(p, pages, page, 0) || !contents(p, pages, page)) {
    failure("remap contents");
    goto done;
  }
  puts("SWAP-TEST: PASS fork-remap");
  if (madvise(p, bytes, MADV_PAGEOUT) || mlock(p, page) || !resident(p, 1, page, 1)) {
    failure("mlock fault-in");
    goto done;
  }
  errno = 0;
  if (madvise(p, bytes, MADV_PAGEOUT) != -1 || errno != EOPNOTSUPP || !resident(p, 1, page, 1) ||
      munlock(p, page)) {
    failure("locked range admission");
    goto done;
  }
  if (swapoff(device)) {
    failure("drain");
    goto done;
  }
  active = 0;
  if (!resident(p, pages, page, 1) || !contents(p, pages, page)) {
    failure("drain contents");
    goto done;
  }
  errno = 0;
  if (swapoff(device) != -1 || errno != EINVAL || swapon(device, 0)) {
    failure("retirement reuse");
    goto done;
  }
  active = 1;
  if (madvise(p, bytes, MADV_PAGEOUT) || madvise(p, bytes, MADV_DONTNEED) || swapoff(device)) {
    failure("discard releases slots");
    goto done;
  }
  active = 0;
  for (size_t i = 0; i < bytes; ++i)
    if (p[i]) {
      failure("discard zero-fill");
      goto done;
    }
  puts("SWAP-TEST: PASS mlock-swapoff-discard-reuse");
  result = 0;
done:
  munmap(p, bytes);
  if (active) {
    if (swapoff(device))
      result = failure("cleanup swapoff");
    else
      active = 0;
  }
  if (!active) {
    unsigned char currentHeader[4096];
    if (read_header(device, currentHeader))
      result = failure("final header snapshot");
    else {
      for (size_t i = 0; i < sizeof(originalHeader); ++i)
        if (currentHeader[i] != originalHeader[i]) {
          errno = 0;
          fprintf(stderr, "SWAP-TEST: HEADER byte=%zu expected=%u actual=%u\n", i,
                  (unsigned)originalHeader[i], (unsigned)currentHeader[i]);
          result = failure("swap header changed");
          break;
        }
    }
  }
  if (!result)
    puts("SWAP-TEST: END PASS");
  return result;
}
