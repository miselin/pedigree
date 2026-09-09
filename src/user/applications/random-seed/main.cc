/* Copyright (c) 2026, Pedigree Developers. See LICENSE. */
#include "pedigree/kernel/utilities/SecureRandom.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/stat.h>

namespace {
constexpr unsigned long AddEntropy = 0x40085203UL;
constexpr char DefaultSeed[] = "/var/lib/pedigree/random-seed";

struct Secrets {
  unsigned char saved[32] = {};
  unsigned char next[32] = {};
  struct {
    int entropyBits = 256;
    int bytes = 32;
    unsigned char seed[32] = {};
  } request;
  ~Secrets() {
    pedigree_random::erase(this, sizeof(*this));
  }
};

int fail(const char* operation) {
  fprintf(stderr, "random-seed: %s: %s; seed not activated\n", operation, strerror(errno));
  return 1;
}

bool privateFile(int fd, bool directory = false) {
  struct stat st;
  if (fstat(fd, &st) != 0)
    return false;
  if (st.st_uid || (st.st_mode & 0777) != (directory ? 0700 : 0600) ||
      (directory ? !S_ISDIR(st.st_mode) : (!S_ISREG(st.st_mode) || st.st_nlink != 1))) {
    errno = EPERM;
    return false;
  }
  return true;
}

bool transfer(int fd, unsigned char* bytes, size_t count, bool writing) {
  size_t done = 0;
  while (done < count) {
    ssize_t n =
        writing ? write(fd, bytes + done, count - done) : read(fd, bytes + done, count - done);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0) {
      if (!n)
        errno = EIO;
      return false;
    }
    done += static_cast<size_t>(n);
  }
  return true;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && !strcmp(argv[1], "--check")) {
    unsigned char bytes[32];
    const bool ready = getrandom(bytes, sizeof(bytes), GRND_NONBLOCK) == sizeof(bytes);
    pedigree_random::erase(bytes, sizeof(bytes));
    puts(ready ? "Secure random generator is ready." : "Secure random generator is not seeded.");
    return ready ? 0 : 1;
  }
  if (argc > 2 || (argc == 2 && argv[1][0] == '-')) {
    fprintf(stderr, "Usage: random-seed [SEED_FILE] | --check\n");
    return 2;
  }
  if (geteuid()) {
    errno = EPERM;
    return fail("requires root");
  }
  const char* path = argc == 2 ? argv[1] : DefaultSeed;
  char parent[4096], name[256], temporary[272], lockname[272];
  const char* slash = strrchr(path, '/');
  if (!slash || slash == path || !slash[1] || static_cast<size_t>(slash - path) >= sizeof(parent) ||
      strlen(slash + 1) >= sizeof(name)) {
    errno = EINVAL;
    return fail("seed path must have a private parent directory");
  }
  memcpy(parent, path, slash - path);
  parent[slash - path] = 0;
  strcpy(name, slash + 1);
  snprintf(temporary, sizeof(temporary), "%s.next", name);
  snprintf(lockname, sizeof(lockname), "%s.lock", name);
  int directory = open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  if (directory < 0 || !privateFile(directory, true))
    return fail("open private seed directory (root:root, mode 0700)");

  // Keep this lock inode across boots. Locking the replaceable seed or its
  // temporary pathname would permit two invocations to consume the same seed.
  int lock = openat(directory, lockname, O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
  if (lock < 0 || !privateFile(lock) || flock(lock, LOCK_EX | LOCK_NB))
    return fail("lock seed transaction");
  int input = openat(directory, name, O_RDONLY | O_NOFOLLOW);
  if (input < 0 || !privateFile(input))
    return fail("open private seed file (root:root, mode 0600)");
  Secrets secrets;
  if (!transfer(input, secrets.saved, sizeof(secrets.saved), false))
    return fail("read 32-byte seed");
  unsigned char extra;
  if (read(input, &extra, 1) != 0) {
    errno = EINVAL;
    return fail("seed must contain exactly 32 bytes");
  }
  if (close(input))
    return fail("close seed input");
  constexpr char KernelDomain[] = "Pedigree kernel seed v1";
  constexpr char SavedDomain[] = "Pedigree saved seed v1";
  pedigree_random::hmac_sha256(secrets.saved, KernelDomain, sizeof(KernelDomain) - 1,
                               secrets.request.seed);
  pedigree_random::hmac_sha256(secrets.saved, SavedDomain, sizeof(SavedDomain) - 1, secrets.next);
  pedigree_random::erase(secrets.saved, sizeof(secrets.saved));

  // An interrupted transaction's .next file requires rescue recovery. Never
  // guess whether a prior boot activated its seed or reuse a bundled default.
  int output = openat(directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  if (output < 0)
    return fail("create successor (recover a stale .next file from rescue Linux)");
  if (!transfer(output, secrets.next, sizeof(secrets.next), true) || fsync(output) || close(output))
    return fail("persist successor");
  if (renameat(directory, temporary, directory, name) || fsync(directory))
    return fail("persist seed replacement");

  // No consumer may obtain bytes derived from this seed until its successor
  // is durable. A crash after this point merely skips an unused generation.
  int random = open("/dev/random", O_RDONLY);
  if (random < 0 || ioctl(random, AddEntropy, &secrets.request))
    return fail("activate kernel seed");
  puts("random-seed: advanced saved seed and initialized secure randomness");
  return 0;
}
