/* Copyright (c) 2026, Pedigree Developers. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/auxv.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#define CHECK(expression)                                                             \
  do {                                                                                \
    if (!(expression)) {                                                              \
      fprintf(stderr, "EXECVEAT-CONTRACT: FAIL line=%d errno=%d\n", __LINE__, errno); \
      return 1;                                                                       \
    }                                                                                 \
  } while (0)

extern char** environ;
static char self[PATH_MAX];

static int invoke(int fd, const char* path, int flags, char** arguments) {
  return syscall(SYS_execveat, fd, path, arguments, environ, flags);
}

static int run(int fd, const char* path, int flags, int script, int closefd) {
  char descriptor[32], expected[PATH_MAX + 64];
  snprintf(descriptor, sizeof(descriptor), "%d", closefd);
  if (path[0] == '/' || fd == AT_FDCWD)
    snprintf(expected, sizeof(expected), "%s", path);
  else
    snprintf(expected, sizeof(expected), "/dev/fd/%d%s%s", fd, *path ? "/" : "", path);
  char* arguments[] = {"chosen-argv-zero", script ? expected : "--elf-child", descriptor,
                       script ? NULL : expected, NULL};
  pid_t child = fork();
  CHECK(child >= 0);
  if (!child) {
    invoke(fd, path, flags, arguments);
    fprintf(stderr, "EXECVEAT-CONTRACT: exec failed errno=%d path=%s\n", errno, expected);
    _exit(80);
  }
  int status;
  CHECK(waitpid(child, &status, 0) == child);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  return 0;
}

static int copy_image(const char* path) {
  int source = open(self, O_RDONLY), target = open(path, O_CREAT | O_EXCL | O_WRONLY, 0700);
  CHECK(source >= 0 && target >= 0);
  char buffer[8192];
  ssize_t count;
  while ((count = read(source, buffer, sizeof(buffer))) > 0) {
    ssize_t written = 0;
    while (written < count) {
      ssize_t n = write(target, buffer + written, count - written);
      CHECK(n > 0);
      written += n;
    }
  }
  CHECK(count == 0 && close(source) == 0 && close(target) == 0);
  return 0;
}

static int descriptor_paths(const char* image) {
  int fd = open(image, O_RDONLY);
  CHECK(fd >= 0);
  char proc[64], dev[64], name[32];
  snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
  snprintf(dev, sizeof(dev), "/dev/fd/%d", fd);
  snprintf(name, sizeof(name), "%d", fd);
  struct stat backing, link, alias;
  CHECK(fstat(fd, &backing) == 0 && lstat(proc, &link) == 0 && S_ISLNK(link.st_mode));
  CHECK(stat(proc, &alias) == 0 && alias.st_ino == backing.st_ino &&
        alias.st_dev == backing.st_dev);
  CHECK(stat(dev, &alias) == 0 && alias.st_ino == backing.st_ino && alias.st_dev == backing.st_dev);
  int linkfd = open(proc, O_PATH | O_NOFOLLOW);
  CHECK(linkfd >= 0 && fstat(linkfd, &link) == 0 && S_ISLNK(link.st_mode));
  CHECK(open(proc, O_RDONLY | O_NOFOLLOW) == -1 && errno == ELOOP);
  char byte;
  CHECK(read(linkfd, &byte, 1) == -1 && errno == EBADF);
  CHECK(fcntl(linkfd, F_SETFL, O_RDONLY) == -1 && errno == EBADF);
  CHECK(fcntl(linkfd, F_GETFL) & O_PATH);
  char target[PATH_MAX];
  CHECK(readlinkat(linkfd, "", target, sizeof(target)) > 0);
  CHECK(syscall(SYS_readlink, proc, target, 0) == -1 && errno == EINVAL);
  CHECK(readlink(proc, target, 0) == 0);
  DIR* directory = opendir("/proc/self/fd");
  CHECK(directory);
  int found = 0;
  struct dirent* entry;
  while ((entry = readdir(directory)))
    found |= !strcmp(entry->d_name, name);
  CHECK(found && closedir(directory) == 0);
  CHECK(close(fd) == 0);
  CHECK(lstat(proc, &link) == -1 && errno == ENOENT);
  int replacement = open(image, O_RDONLY);
  CHECK(replacement >= 0);
  if (replacement != fd) {
    CHECK(dup2(replacement, fd) == fd && close(replacement) == 0);
  }
  CHECK(stat(proc, &alias) == 0 && alias.st_ino == backing.st_ino);
  CHECK(close(fd) == 0 && close(linkfd) == 0);
  return 0;
}

static int contracts(void) {
  char directory[] = "/tmp/execveat-contract-XXXXXX";
  CHECK(mkdtemp(directory));
  char image[PATH_MAX], script[PATH_MAX], link[PATH_MAX], moved[PATH_MAX];
  snprintf(image, sizeof(image), "%s/image", directory);
  snprintf(script, sizeof(script), "%s/script", directory);
  snprintf(link, sizeof(link), "%s/link", directory);
  snprintf(moved, sizeof(moved), "%s-renamed", directory);
  CHECK(copy_image(image) == 0 && descriptor_paths(image) == 0);
  int dirfd = open(directory, O_PATH | O_DIRECTORY);
  int executable = open(image, O_PATH | O_CLOEXEC);
  CHECK(dirfd >= 0 && executable >= 0);
  CHECK(run(-1, image, 0, 0, -1) == 0);
  CHECK(run(dirfd, "image", 0, 0, -1) == 0);
  CHECK(unlink(image) == 0);
  CHECK(run(executable, "", AT_EMPTY_PATH, 0, executable) == 0);
  char* arguments[] = {"failure", NULL};
  CHECK(invoke(executable, "", 0, arguments) == -1 && errno == ENOENT);
  CHECK(invoke(-1, "", AT_EMPTY_PATH, arguments) == -1 && errno == EBADF);
  CHECK(invoke(executable, "child", 0, arguments) == -1 && errno == ENOTDIR);
  CHECK(invoke(dirfd, "", AT_EMPTY_PATH, arguments) == -1 && errno == EACCES);
  CHECK(invoke(dirfd, "script", 0x40000000, arguments) == -1 && errno == EINVAL);
  CHECK(invoke(dirfd, (void*)1, 0, arguments) == -1 && errno == EFAULT);
  CHECK(close(executable) == 0);

  int output = open(script, O_CREAT | O_EXCL | O_WRONLY, 0700);
  CHECK(output >= 0);
  char contents[PATH_MAX + 64];
  int length = snprintf(contents, sizeof(contents), "#!%s --script-child\nscript-payload\n", self);
  CHECK(write(output, contents, length) == length && close(output) == 0);
  CHECK(symlink("script", link) == 0);
  CHECK(invoke(dirfd, "link", AT_SYMLINK_NOFOLLOW, arguments) == -1 && errno == ELOOP);
  int symlinkfd = open(link, O_PATH | O_NOFOLLOW);
  CHECK(symlinkfd >= 0);
  CHECK(invoke(symlinkfd, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW, arguments) == -1 &&
        errno == ELOOP);
  CHECK(close(symlinkfd) == 0);
  CHECK(run(dirfd, "link", 0, 1, -1) == 0);
  int source = open(script, O_RDONLY | O_CLOEXEC);
  CHECK(source >= 0);
  CHECK(invoke(source, "", AT_EMPTY_PATH, arguments) == -1 && errno == ENOENT);
  CHECK(fcntl(source, F_GETFD) & FD_CLOEXEC);
  CHECK(fcntl(source, F_SETFD, 0) == 0);
  CHECK(rename(directory, moved) == 0);
  CHECK(run(dirfd, "script", 0, 1, -1) == 0);
  CHECK(fcntl(dirfd, F_SETFD, FD_CLOEXEC) == 0);
  CHECK(invoke(dirfd, "script", 0, arguments) == -1 && errno == ENOENT);
  CHECK(fcntl(dirfd, F_SETFD, 0) == 0);
  CHECK(unlinkat(dirfd, "script", 0) == 0);
  CHECK(run(source, "", AT_EMPTY_PATH, 1, -1) == 0);
  CHECK(unlinkat(dirfd, "link", 0) == 0);
  CHECK(close(source) == 0 && close(dirfd) == 0 && rmdir(moved) == 0);
  return 0;
}

static int executable_upgrade(void) {
  char directory[] = "/tmp/exec-upgrade-XXXXXX";
  CHECK(mkdtemp(directory));
  char image[PATH_MAX], replacement[PATH_MAX];
  snprintf(image, sizeof(image), "%s/image", directory);
  snprintf(replacement, sizeof(replacement), "%s/replacement", directory);
  CHECK(copy_image(image) == 0);
  int ready[2], resume[2];
  CHECK(pipe(ready) == 0 && pipe(resume) == 0);
  pid_t child = fork();
  CHECK(child >= 0);
  if (!child) {
    close(ready[0]);
    close(resume[1]);
    char ready_fd[32], resume_fd[32];
    snprintf(ready_fd, sizeof(ready_fd), "%d", ready[1]);
    snprintf(resume_fd, sizeof(resume_fd), "%d", resume[0]);
    execl(image, image, "--upgrade-child", ready_fd, resume_fd, NULL);
    _exit(80);
  }
  close(ready[1]);
  close(resume[0]);
  char byte;
  CHECK(read(ready[0], &byte, 1) == 1);
  int old = open(image, O_RDONLY);
  struct stat before, after;
  CHECK(old >= 0 && fstat(old, &before) == 0);
  int staged = open(replacement, O_CREAT | O_EXCL | O_WRONLY, 0700);
  static const char script[] = "#!/bin/sh\nexit 23\n";
  CHECK(staged >= 0 && write(staged, script, sizeof(script) - 1) == sizeof(script) - 1);
  CHECK(close(staged) == 0 && rename(replacement, image) == 0);
  CHECK(stat(image, &after) == 0 && before.st_ino != after.st_ino);
  char magic[4];
  CHECK(read(old, magic, sizeof(magic)) == sizeof(magic) && !memcmp(magic, "\177ELF", 4));
  CHECK(close(old) == 0 && write(resume[1], "x", 1) == 1);
  int status;
  CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 42);
  close(ready[0]);
  close(resume[1]);
  child = fork();
  CHECK(child >= 0);
  if (!child) {
    execl(image, image, NULL);
    _exit(80);
  }
  CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 23);
  CHECK(unlink(image) == 0 && rmdir(directory) == 0);
  puts("EXECUTABLE-UPGRADE: PASS");
  return 0;
}

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  alarm(30);
  if (argc == 4 && !strcmp(argv[1], "--upgrade-child")) {
    int ready = atoi(argv[2]), resume = atoi(argv[3]);
    char byte;
    CHECK(write(ready, "x", 1) == 1 && read(resume, &byte, 1) == 1);
    return 42;
  }
  if (argc >= 2 && !strcmp(argv[1], "--elf-child")) {
    CHECK(argc == 4);
    int fd = atoi(argv[2]);
    if (fd >= 0)
      CHECK(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
    CHECK(!strcmp((const char*)getauxval(AT_EXECFN), argv[3]));
    return 0;
  }
  if (argc >= 2 && !strcmp(argv[1], "--script-child")) {
    CHECK(argc == 5 && !strcmp(argv[2], argv[3]));
    int fd = open(argv[2], O_RDONLY);
    CHECK(fd >= 0);
    char contents[PATH_MAX + 64];
    ssize_t count = read(fd, contents, sizeof(contents) - 1);
    CHECK(count > 0);
    contents[count] = 0;
    CHECK(strstr(contents, "\nscript-payload\n") && close(fd) == 0);
    return 0;
  }
  CHECK(realpath(argv[0], self));
  CHECK(contracts() == 0);
  CHECK(executable_upgrade() == 0);
  puts("EXECVEAT-CONTRACT: PASS");
  return 0;
}
