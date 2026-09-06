#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#define CHECK(expression)                                                          \
  do {                                                                             \
    if (!(expression)) {                                                           \
      fprintf(stderr, "PIVOT-ROOT-CONTRACT: line=%d errno=%d\n", __LINE__, errno); \
      return 0;                                                                    \
    }                                                                              \
  } while (0)
#define ERROR(expression, expected)                   \
  do {                                                \
    errno = 0;                                        \
    CHECK((expression) == -1 && errno == (expected)); \
  } while (0)

_Static_assert(SYS_pivot_root == 155 && SYS_mount == 165 && SYS_umount2 == 166,
               "Linux amd64 mount routes");

struct Fixture {
  char base[128], newRoot[160], putOld[192], oldCwd[160], park[160], restoreTarget[192];
  int originalCwd, oldRoot, baseFd, newFd, childFd, oldFile;
  int command[2], response[2];
  pid_t observer;
  int created, mounted, pivoted, restored;
};

static int write_marker(int directory, const char* name, char value) {
  int fd = openat(directory, name, O_CREAT | O_EXCL | O_WRONLY, 0600);
  CHECK(fd >= 0);
  int good = write(fd, &value, 1) == 1;
  CHECK(close(fd) == 0 && good);
  return 1;
}

static int marker(int directory, const char* name, char expected) {
  int fd = openat(directory, name, O_RDONLY);
  CHECK(fd >= 0);
  char value = 0;
  int good = read(fd, &value, 1) == 1 && value == expected;
  CHECK(close(fd) == 0 && good);
  return 1;
}

static int receive(int fd, char expected) {
  struct pollfd ready = {fd, POLLIN, 0};
  CHECK(poll(&ready, 1, 10000) == 1 && (ready.revents & POLLIN));
  char value = 0;
  CHECK(read(fd, &value, 1) == 1 && value == expected);
  return 1;
}

static int wait_child(pid_t child) {
  int status = 0;
  for (unsigned attempt = 0; attempt < 100; ++attempt) {
    pid_t result = waitpid(child, &status, WNOHANG);
    CHECK(result >= 0);
    if (result == child) {
      CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
      return 1;
    }
    const struct timespec delay = {0, 100000000};
    CHECK(nanosleep(&delay, NULL) == 0);
  }
  kill(child, SIGKILL);
  waitpid(child, &status, 0);
  CHECK(0);
}

static int setup(struct Fixture* fixture) {
  CHECK(geteuid() == 0);
  fixture->originalCwd = open(".", O_PATH | O_DIRECTORY);
  CHECK(fixture->originalCwd >= 0 && chdir("/") == 0);
  fixture->oldRoot = open("/", O_PATH | O_DIRECTORY);
  CHECK(fixture->oldRoot >= 0);
  snprintf(fixture->base, sizeof(fixture->base), "/tmp/pivot-contract-%ld", (long)getpid());
  snprintf(fixture->newRoot, sizeof(fixture->newRoot), "%s/new", fixture->base);
  snprintf(fixture->putOld, sizeof(fixture->putOld), "%s/old", fixture->newRoot);
  snprintf(fixture->oldCwd, sizeof(fixture->oldCwd), "%s/cwd", fixture->base);
  snprintf(fixture->park, sizeof(fixture->park), "%s/park", fixture->base);
  snprintf(fixture->restoreTarget, sizeof(fixture->restoreTarget), "/old%s", fixture->park);
  CHECK(mkdir(fixture->base, 0700) == 0);
  fixture->created = 1;
  fixture->baseFd = open(fixture->base, O_PATH | O_DIRECTORY);
  CHECK(fixture->baseFd >= 0 && write_marker(fixture->baseFd, "original", 'O'));
  CHECK(mkdirat(fixture->baseFd, "cwd", 0700) == 0 && mkdirat(fixture->baseFd, "new", 0700) == 0 &&
        mkdirat(fixture->baseFd, "park", 0700) == 0);
  int cwd = openat(fixture->baseFd, "cwd", O_PATH | O_DIRECTORY);
  CHECK(cwd >= 0 && write_marker(cwd, "old-marker", 'S') && close(cwd) == 0);
  fixture->oldFile = openat(fixture->baseFd, "original", O_RDONLY);
  CHECK(fixture->oldFile >= 0);
  CHECK(mount("none", fixture->newRoot, "tmpfs", 0, NULL) == 0);
  fixture->mounted = 1;
  fixture->newFd = open(fixture->newRoot, O_PATH | O_DIRECTORY);
  CHECK(fixture->newFd >= 0);
  const char* directories[] = {"old", "child", "proc", "dev", "mapped"};
  for (size_t i = 0; i < sizeof(directories) / sizeof(directories[0]); ++i)
    CHECK(mkdirat(fixture->newFd, directories[i], 0755) == 0);
  CHECK(write_marker(fixture->newFd, "new-marker", 'N'));
  CHECK(symlinkat("/new-marker", fixture->newFd, "absolute-link") == 0 &&
        symlinkat("../new-marker", fixture->newFd, "relative-link") == 0);
  for (unsigned i = 0; i <= 40; ++i) {
    char name[32], target[32];
    snprintf(name, sizeof(name), "chain-%u", i);
    if (i == 40)
      snprintf(target, sizeof(target), "new-marker");
    else
      snprintf(target, sizeof(target), "chain-%u", i + 1);
    CHECK(symlinkat(target, fixture->newFd, name) == 0);
  }
  CHECK(symlinkat("child/", fixture->newFd, "directory-target") == 0 &&
        symlinkat("directory-target", fixture->newFd, "directory-chain") == 0);
  char path[192];
  snprintf(path, sizeof(path), "%s/proc", fixture->newRoot);
  CHECK(mount("none", path, "proc", 0, NULL) == 0);
  snprintf(path, sizeof(path), "%s/child", fixture->newRoot);
  CHECK(mount("none", path, "tmpfs", 0, NULL) == 0);
  fixture->childFd = open(path, O_PATH | O_DIRECTORY);
  CHECK(fixture->childFd >= 0 && write_marker(fixture->childFd, "child-marker", 'C'));
  return 1;
}

static int unprivileged(const struct Fixture* fixture) {
  CHECK(setgid(42424) == 0 && setuid(42424) == 0);
  ERROR(syscall(SYS_pivot_root, fixture->newRoot, fixture->putOld), EPERM);
  return 1;
}

static int confined(const struct Fixture* fixture) {
  CHECK(chroot(fixture->newRoot) == 0 && chdir("/") == 0);
  CHECK(marker(AT_FDCWD, "/new-marker", 'N') && marker(AT_FDCWD, "/../../new-marker", 'N'));
  ERROR(open("/dev/tty", O_RDWR | O_NOCTTY), ENOENT);
  ERROR(open(fixture->base, O_PATH | O_DIRECTORY), ENOENT);
  return 1;
}

static int before_pivot(struct Fixture* fixture) {
  char file[192], byte = 0;
  snprintf(file, sizeof(file), "%s/new-marker", fixture->newRoot);
  ERROR(syscall(SYS_pivot_root, (const char*)1, fixture->putOld), EFAULT);
  ERROR(syscall(SYS_pivot_root, fixture->newRoot, (const char*)1), EFAULT);
  ERROR(syscall(SYS_pivot_root, file, fixture->putOld), ENOTDIR);
  ERROR(syscall(SYS_pivot_root, fixture->oldCwd, fixture->putOld), EINVAL);
  ERROR(syscall(SYS_pivot_root, fixture->newRoot, fixture->base), EINVAL);
  ERROR(syscall(SYS_pivot_root, fixture->newRoot, fixture->newRoot), EOPNOTSUPP);
  ERROR(mount("none", fixture->newRoot, "tmpfs", 0, NULL), EOPNOTSUPP);
  ERROR(mount("none", fixture->park, "tmpfs", MS_RDONLY, NULL), EOPNOTSUPP);
  ERROR(mount("none", fixture->park, "tmpfs", 0, "size=4096"), EOPNOTSUPP);
  ERROR(syscall(SYS_umount2, fixture->newRoot, 0x40000000), EOPNOTSUPP);
  ERROR(read(fixture->newFd, &byte, 1), EBADF);
  ERROR(linkat(fixture->newFd, "old", fixture->newFd, "directory-link", 0), EPERM);
  pid_t child = fork();
  CHECK(child >= 0);
  if (!child)
    _exit(unprivileged(fixture) ? 0 : 1);
  CHECK(wait_child(child));
  child = fork();
  CHECK(child >= 0);
  if (!child)
    _exit(confined(fixture) ? 0 : 1);
  CHECK(wait_child(child));
  char cwd[PATH_MAX];
  CHECK(getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, "/") && marker(fixture->baseFd, "original", 'O'));
  return 1;
}

static int observe_pivot(const struct Fixture* fixture) {
  close(fixture->command[1]);
  close(fixture->response[0]);
  CHECK(chdir(fixture->oldCwd) == 0 && write(fixture->response[1], "R", 1) == 1);
  CHECK(receive(fixture->command[0], 'P'));
  char cwd[PATH_MAX], expected[192];
  snprintf(expected, sizeof(expected), "/old%s", fixture->oldCwd);
  CHECK(getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, expected));
  CHECK(marker(AT_FDCWD, "old-marker", 'S') && marker(AT_FDCWD, "/new-marker", 'N'));
  CHECK(write(fixture->response[1], "G", 1) == 1);
  return 1;
}

static int symlink_paths(const struct Fixture* fixture) {
  CHECK(marker(AT_FDCWD, "/chain-10", 'N') && marker(AT_FDCWD, "/chain-1", 'N'));
  ERROR(open("/chain-0", O_RDONLY), ELOOP);
  int fd = open("/chain-0", O_PATH | O_NOFOLLOW);
  struct stat attributes;
  CHECK(fd >= 0 && fstat(fd, &attributes) == 0 && S_ISLNK(attributes.st_mode));
  CHECK(close(fd) == 0);
  CHECK(marker(AT_FDCWD, "/directory-chain/child-marker", 'C'));
  fd = open("/directory-chain/", O_PATH | O_DIRECTORY);
  CHECK(fd >= 0 && close(fd) == 0);
  ERROR(open("/chain-10/", O_PATH), ENOTDIR);
  char magic[96];
  snprintf(magic, sizeof(magic), "/proc/self/fd/%d/chain-10", fixture->newFd);
  CHECK(marker(AT_FDCWD, magic, 'N'));
  return 1;
}

static int mount_report(void) {
  char contents[32768];
  int fd = open("/proc/mounts", O_RDONLY);
  CHECK(fd >= 0);
  size_t used = 0;
  while (used < sizeof(contents) - 1) {
    ssize_t count = read(fd, contents + used, sizeof(contents) - 1 - used);
    CHECK(count >= 0);
    if (!count)
      break;
    used += count;
  }
  CHECK(close(fd) == 0 && used < sizeof(contents) - 1);
  contents[used] = 0;
  unsigned found = 0;
  char* state = NULL;
  for (char* line = strtok_r(contents, "\n", &state); line; line = strtok_r(NULL, "\n", &state)) {
    char source[256], path[512], type[64];
    CHECK(sscanf(line, "%255s %511s %63s", source, path, type) == 3);
    if (!strcmp(path, "/"))
      found |= 1;
    if (!strcmp(path, "/old"))
      found |= 2;
    if (!strcmp(path, "/child"))
      found |= 4;
    if (!strcmp(path, "/proc"))
      found |= 8;
  }
  CHECK(found == 15);
  return 1;
}

static int mapped_attachment(void) {
  CHECK(mount("none", "/mapped", "tmpfs", 0, NULL) == 0);
  const long page = sysconf(_SC_PAGESIZE);
  CHECK(page > 0);
  int fd = open("/mapped/data", O_CREAT | O_EXCL | O_RDWR, 0600);
  CHECK(fd >= 0 && ftruncate(fd, page) == 0);
  char* mapping = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  CHECK(mapping != MAP_FAILED && close(fd) == 0);
  mapping[0] = 'M';
  ERROR(umount2("/mapped", 0), EBUSY);
  int command[2], response[2];
  CHECK(pipe(command) == 0 && pipe(response) == 0);
  pid_t child = fork();
  CHECK(child >= 0);
  if (!child) {
    close(command[1]);
    close(response[0]);
    const int good = mapping[0] == 'M' && write(response[1], "R", 1) == 1 &&
                     receive(command[0], 'X') && mapping[0] == 'M';
    // Exit must release the inherited VMA's last attachment reference.
    _exit(good ? 0 : 1);
  }
  close(command[0]);
  close(response[1]);
  CHECK(receive(response[0], 'R'));
  CHECK(munmap(mapping, page) == 0);
  ERROR(umount2("/mapped", 0), EBUSY);
  CHECK(write(command[1], "X", 1) == 1 && wait_child(child));
  CHECK(close(command[1]) == 0 && close(response[0]) == 0);
  CHECK(umount2("/mapped", 0) == 0);
  ERROR(open("/mapped/data", O_RDONLY), ENOENT);
  return 1;
}

static int after_pivot(struct Fixture* fixture) {
  CHECK(write(fixture->command[1], "P", 1) == 1 && receive(fixture->response[0], 'G'));
  CHECK(wait_child(fixture->observer));
  fixture->observer = -1;
  char cwd[PATH_MAX], value = 0;
  CHECK(getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, "/"));
  CHECK(marker(AT_FDCWD, "/new-marker", 'N') && marker(AT_FDCWD, "/../../new-marker", 'N') &&
        marker(AT_FDCWD, "/absolute-link", 'N') && marker(AT_FDCWD, "/relative-link", 'N'));
  CHECK(pread(fixture->oldFile, &value, 1, 0) == 1 && value == 'O');
  CHECK(marker(fixture->baseFd, "original", 'O') && marker(fixture->newFd, "../new-marker", 'N'));
  CHECK(fchdir(fixture->oldRoot) == 0 && getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, "/old"));
  CHECK(chdir("/") == 0);
  ERROR(open(fixture->base, O_PATH | O_DIRECTORY), ENOENT);
  int ns = open("/proc/self/ns/uts", O_RDONLY);
  CHECK(ns >= 0 && close(ns) == 0);
  ssize_t length = readlink("/proc/self/ns/uts", cwd, sizeof(cwd));
  CHECK(length > 5 && !memcmp(cwd, "uts:[", 5));
  CHECK(mount_report());
  CHECK(symlink_paths(fixture));
  CHECK(mapped_attachment());
  ERROR(umount2("/old", 0), EBUSY);
  ERROR(umount2("/child", 0), EBUSY);
  CHECK(syscall(SYS_umount2, "/child", MNT_DETACH) == 0);
  ERROR(open("/child/child-marker", O_RDONLY), ENOENT);
  CHECK(marker(fixture->childFd, "child-marker", 'C'));
  CHECK(fchdir(fixture->childFd) == 0 && marker(AT_FDCWD, "child-marker", 'C'));
  ERROR(getcwd(cwd, sizeof(cwd)) ? 0 : -1, ENOENT);
  CHECK(chdir("/") == 0);
  CHECK(syscall(SYS_mount, "none", "/child", "ramfs", 0, NULL) == 0);
  int replacement = open("/child", O_PATH | O_DIRECTORY);
  CHECK(replacement >= 0 && write_marker(replacement, "child-marker", 'R'));
  CHECK(marker(replacement, "child-marker", 'R') && marker(fixture->childFd, "child-marker", 'C'));
  CHECK(close(replacement) == 0);
  return 1;
}

static int restore(struct Fixture* fixture) {
  if (!fixture->pivoted)
    return 1;
  if (syscall(SYS_pivot_root, "/old", fixture->restoreTarget) < 0) {
    fprintf(stderr, "PIVOT-ROOT-CONTRACT: restoration errno=%d\n", errno);
    return 0;
  }
  fixture->pivoted = 0;
  fixture->restored = 1;
  CHECK(chdir("/") == 0 && marker(fixture->baseFd, "original", 'O'));
  char path[160];
  snprintf(path, sizeof(path), "%s/original", fixture->base);
  CHECK(marker(AT_FDCWD, path, 'O'));
  ERROR(open("/new-marker", O_RDONLY), ENOENT);
  return 1;
}

int main(void) {
  struct Fixture fixture = {.originalCwd = -1,
                            .oldRoot = -1,
                            .baseFd = -1,
                            .newFd = -1,
                            .childFd = -1,
                            .oldFile = -1,
                            .command = {-1, -1},
                            .response = {-1, -1},
                            .observer = -1};
  int success = 0;
  const mode_t previousMask = umask(0);
  setvbuf(stdout, NULL, _IOLBF, 0);
  puts("PIVOT-ROOT-CONTRACT: BEGIN");
  if (!setup(&fixture) || !before_pivot(&fixture))
    goto cleanup;
  if (pipe(fixture.command) || pipe(fixture.response))
    goto cleanup;
  fixture.observer = fork();
  if (!fixture.observer)
    _exit(observe_pivot(&fixture) ? 0 : 1);
  if (fixture.observer < 0 || !receive(fixture.response[0], 'R'))
    goto cleanup;
  if (syscall(SYS_pivot_root, fixture.newRoot, fixture.putOld) < 0)
    goto cleanup;
  fixture.pivoted = 1;
  puts("PIVOT-ROOT-CONTRACT: pivoted");
  if (!after_pivot(&fixture) || !restore(&fixture))
    goto cleanup;
  success = 1;
cleanup:
  if (fixture.observer > 0) {
    kill(fixture.observer, SIGKILL);
    waitpid(fixture.observer, NULL, 0);
  }
  if (!restore(&fixture))
    success = 0;
  if (!fixture.pivoted && fixture.mounted &&
      umount2(fixture.restored ? fixture.park : fixture.newRoot, MNT_DETACH) < 0)
    success = 0;
  if (!fixture.pivoted && fixture.originalCwd >= 0 && fchdir(fixture.originalCwd) < 0)
    success = 0;
  if (fixture.baseFd >= 0) {
    unlinkat(fixture.baseFd, "original", 0);
    unlinkat(fixture.baseFd, "cwd/old-marker", 0);
    unlinkat(fixture.baseFd, "cwd", AT_REMOVEDIR);
    unlinkat(fixture.baseFd, "new", AT_REMOVEDIR);
    unlinkat(fixture.baseFd, "park", AT_REMOVEDIR);
  }
  int descriptors[] = {fixture.originalCwd, fixture.oldRoot,    fixture.baseFd,
                       fixture.newFd,       fixture.childFd,    fixture.oldFile,
                       fixture.command[0],  fixture.command[1], fixture.response[0],
                       fixture.response[1]};
  for (size_t i = 0; i < sizeof(descriptors) / sizeof(descriptors[0]); ++i)
    if (descriptors[i] >= 0)
      close(descriptors[i]);
  if (!fixture.pivoted && fixture.created)
    rmdir(fixture.base);
  umask(previousMask);
  printf("PIVOT-ROOT-CONTRACT: %s restored=%d\n", success ? "PASS" : "FAIL", fixture.restored);
  return success ? 0 : 1;
}
