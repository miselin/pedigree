/* Copyright (c) 2026, Pedigree Developers. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "contract.h"
#include <sys/stat.h>
#include <sys/wait.h>

size_t ef_page;
char ef_directory[PATH_MAX];
char ef_self[PATH_MAX];
int ef_ext2;

int ef_create(const char* name, char path[PATH_MAX]) {
  snprintf(path, PATH_MAX, "%s/%s", ef_directory, name);
  int fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0700);
  if (fd < 0)
    return -1;
  unsigned char* bytes = malloc(3 * ef_page);
  if (bytes)
    memset(bytes, 0x31, 3 * ef_page);
  if (!bytes || write(fd, bytes, 3 * ef_page) != (ssize_t)(3 * ef_page)) {
    free(bytes);
    close(fd);
    unlink(path);
    return -1;
  }
  free(bytes);
  return fd;
}

int ef_copy(const char* source, const char* destination) {
  int failed = 0;
  int input = open(source, O_RDONLY | O_CLOEXEC);
  int output = open(destination, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0700);
  CHECK(input >= 0 && output >= 0);
  char bytes[8192];
  ssize_t count;
  while ((count = read(input, bytes, sizeof(bytes))) > 0) {
    ssize_t done = 0;
    while (done < count) {
      ssize_t amount = write(output, bytes + done, count - done);
      CHECK(amount > 0);
      done += amount;
    }
  }
  CHECK(count == 0);
out:
  if (input >= 0)
    close(input);
  if (output >= 0)
    close(output);
  return failed;
}

int ef_receive(int fd, char expected) {
  struct pollfd item = {.fd = fd, .events = POLLIN};
  int result;
  do {
    result = poll(&item, 1, 5000);
  } while (result < 0 && errno == EINTR);
  char byte = 0;
  return result > 0 && read(fd, &byte, 1) == 1 && byte == expected ? 0 : -1;
}

int ef_send(int fd, char value) {
  return write(fd, &value, 1) == 1 ? 0 : -1;
}

int ef_reap(pid_t child) {
  for (int n = 0; n < 1000; ++n) {
    int status;
    pid_t result = waitpid(child, &status, WNOHANG);
    if (result == child)
      return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    if (result < 0 && errno != EINTR)
      return -1;
    struct timespec pause = {0, 5000000};
    nanosleep(&pause, NULL);
  }
  kill(child, SIGKILL);
  while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {
  }
  return -1;
}

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  alarm(60);
  if (argc == 4 && !strcmp(argv[1], "--load"))
    return ef_load(argv[2], atoi(argv[3]));
  if (argc > 2) {
    fprintf(stderr, "usage: %s [/tmp|/]\n", argv[0]);
    return 2;
  }
  int failed = 0;
  const char* base = argc == 2 ? argv[1] : "/tmp";
  ef_ext2 = strcmp(base, "/tmp") != 0;
  long page = sysconf(_SC_PAGESIZE);
  CHECK(page > 0);
  ef_page = (size_t)page;
  ssize_t count = readlink("/proc/self/exe", ef_self, sizeof(ef_self) - 1);
  CHECK(count > 0 && (size_t)count < sizeof(ef_self) - 1);
  ef_self[count] = 0;
  snprintf(ef_directory, sizeof(ef_directory), "%s/executable-files-XXXXXX", base);
  CHECK(mkdtemp(ef_directory));
  printf("EXECUTABLE-FILE-CONTRACT: BEGIN %s\n", base);
  CHECK(!ef_mutations());
  puts("EXECUTABLE-FILE-CONTRACT: PASS mutation-denial");
  CHECK(!ef_lifetime());
  puts("EXECUTABLE-FILE-CONTRACT: PASS fork-split-lifetime");
  CHECK(!ef_conflicts());
  puts("EXECUTABLE-FILE-CONTRACT: PASS shared-conflicts");
  CHECK(!ef_ordinary());
  puts("EXECUTABLE-FILE-CONTRACT: PASS ordinary-private");
  CHECK(!ef_library());
  puts("EXECUTABLE-FILE-CONTRACT: PASS library-replacement");
out:
  if (*ef_directory)
    rmdir(ef_directory);
  puts(failed ? "EXECUTABLE-FILE-CONTRACT: END FAIL" : "EXECUTABLE-FILE-CONTRACT: END PASS");
  return failed;
}
