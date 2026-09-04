/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

extern void fail(void) __attribute__((noreturn));

static int write_fixture(const char* path, const char* contents, size_t length, mode_t mode) {
  int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, mode);
  if (fd < 0)
    return -1;

  size_t written = 0;
  while (written < length) {
    ssize_t result = write(fd, contents + written, length - written);
    if (result < 0 && errno == EINTR)
      continue;
    if (result <= 0) {
      close(fd);
      unlink(path);
      return -1;
    }
    written += (size_t)result;
  }

  if (close(fd) || chmod(path, mode)) {
    unlink(path);
    return -1;
  }
  return 0;
}

static int write_script(const char* path, const char* shebang, mode_t mode) {
  return write_fixture(path, shebang, strlen(shebang), mode);
}

int exec_shebang_child(int argc, char* argv[]) {
  if (!strcmp(argv[1], "--exec-shebang-unexpected-child"))
    return 120;

  if (!strcmp(argv[1], "--exec-shebang-child beta\tgamma")) {
    return !(argc == 10 && !strcmp(argv[0], argv[5]) && !strcmp(argv[2], argv[6]) &&
             !strcmp(argv[3], "alpha one") && !strcmp(argv[4], argv[7]) &&
             !strcmp(argv[8], "tail-one") && !strcmp(argv[9], "tail two"));
  }

  if (!strcmp(argv[1], "--exec-shebang-depth-child")) {
    return !(argc == 12 && !strcmp(argv[0], argv[6]) && !strcmp(argv[2], argv[7]) &&
             !strcmp(argv[3], argv[8]) && !strcmp(argv[4], argv[9]) && !strcmp(argv[5], argv[10]) &&
             !strcmp(argv[11], "depth-tail"));
  }

  return 121;
}

static int exec_returns_errno(const char* path, int expected) {
  pid_t child = fork();
  if (child < 0)
    return 0;
  if (!child) {
    char* const arguments[] = {(char*)"wrong-shebang-argv-zero", (char*)"tail", 0};
    errno = 0;
    execv(path, arguments);
    _exit(errno == expected ? 0 : 122);
  }

  int statusCode = 0;
  return waitpid(child, &statusCode, 0) == child && WIFEXITED(statusCode) &&
         !WEXITSTATUS(statusCode);
}

static int exec_succeeds(const char* path, char* const arguments[]) {
  pid_t child = fork();
  if (child < 0)
    return 0;
  if (!child) {
    execv(path, arguments);
    _exit(123);
  }

  int statusCode = 0;
  return waitpid(child, &statusCode, 0) == child && WIFEXITED(statusCode) &&
         !WEXITSTATUS(statusCode);
}

void test_exec_shebang(const char* program) {
  puts("Testing bounded shebang execution...");
  fflush(stdout);

  char permissionScript[PATH_MAX];
  char permissionOuter[PATH_MAX];
  char permissionInterpreter[PATH_MAX];
  char nestedOuter[PATH_MAX];
  char nestedInterpreter[PATH_MAX];
  char depthA[PATH_MAX];
  char depthB[PATH_MAX];
  char depthC[PATH_MAX];
  char depthD[PATH_MAX];
  char depthE[PATH_MAX];
  char truncated[PATH_MAX];
  char unterminated[PATH_MAX];
  char malformed[PATH_MAX];
  const int processId = getpid();
  snprintf(permissionScript, sizeof(permissionScript), "/tmp/exec-shebang-permission-%d",
           processId);
  snprintf(permissionOuter, sizeof(permissionOuter), "/tmp/exec-shebang-permission-outer-%d",
           processId);
  snprintf(permissionInterpreter, sizeof(permissionInterpreter),
           "/tmp/exec-shebang-permission-interpreter-%d", processId);
  snprintf(nestedOuter, sizeof(nestedOuter), "/tmp/exec-shebang-nested-outer-%d", processId);
  snprintf(nestedInterpreter, sizeof(nestedInterpreter), "/tmp/exec-shebang-nested-interpreter-%d",
           processId);
  snprintf(depthA, sizeof(depthA), "/tmp/exec-shebang-depth-a-%d", processId);
  snprintf(depthB, sizeof(depthB), "/tmp/exec-shebang-depth-b-%d", processId);
  snprintf(depthC, sizeof(depthC), "/tmp/exec-shebang-depth-c-%d", processId);
  snprintf(depthD, sizeof(depthD), "/tmp/exec-shebang-depth-d-%d", processId);
  snprintf(depthE, sizeof(depthE), "/tmp/exec-shebang-depth-e-%d", processId);
  snprintf(truncated, sizeof(truncated), "/tmp/exec-shebang-truncated-%d", processId);
  snprintf(unterminated, sizeof(unterminated), "/tmp/exec-shebang-unterminated-%d", processId);
  snprintf(malformed, sizeof(malformed), "/tmp/exec-shebang-malformed-%d", processId);

  const char* fixtures[] = {permissionScript,
                            permissionOuter,
                            permissionInterpreter,
                            nestedOuter,
                            nestedInterpreter,
                            depthA,
                            depthB,
                            depthC,
                            depthD,
                            depthE,
                            truncated,
                            unterminated,
                            malformed};
  const size_t fixtureCount = sizeof(fixtures) / sizeof(fixtures[0]);
  for (size_t i = 0; i < fixtureCount; ++i)
    unlink(fixtures[i]);

  int failed = 0;
  char line[PATH_MAX + 128];
  snprintf(line, sizeof(line), "#!%s\t--exec-shebang-unexpected-child\n", program);
  failed |=
      write_script(permissionScript, line, 0600) || !exec_returns_errno(permissionScript, EACCES);

  failed |= write_script(permissionInterpreter, line, 0600);
  snprintf(line, sizeof(line), "#!%s\n", permissionInterpreter);
  failed |=
      write_script(permissionOuter, line, 0700) || !exec_returns_errno(permissionOuter, EACCES);

  snprintf(line, sizeof(line), "#!\t%s\t--exec-shebang-child beta\tgamma  \t\n", program);
  failed |= write_script(nestedInterpreter, line, 0700);
  snprintf(line, sizeof(line), "#!\t%s\talpha one \t \n", nestedInterpreter);
  failed |= write_script(nestedOuter, line, 0700);
  char* nestedArguments[] = {(char*)"wrong-shebang-argv-zero",
                             (char*)program,
                             nestedInterpreter,
                             nestedOuter,
                             (char*)"tail-one",
                             (char*)"tail two",
                             0};
  if (!failed)
    failed |= !exec_succeeds(nestedOuter, nestedArguments);

  snprintf(line, sizeof(line), "#!%s\t--exec-shebang-depth-child \t\n", program);
  failed |= write_script(depthD, line, 0700);
  snprintf(line, sizeof(line), "#!%s\n", depthD);
  failed |= write_script(depthC, line, 0700);
  snprintf(line, sizeof(line), "#!%s\n", depthC);
  failed |= write_script(depthB, line, 0700);
  snprintf(line, sizeof(line), "#!%s\n", depthB);
  failed |= write_script(depthA, line, 0700);
  char* depthArguments[] = {(char*)"wrong-shebang-depth-zero",
                            (char*)program,
                            depthD,
                            depthC,
                            depthB,
                            depthA,
                            (char*)"depth-tail",
                            0};
  if (!failed)
    failed |= !exec_succeeds(depthA, depthArguments);

  snprintf(line, sizeof(line), "#!%s\t--exec-shebang-unexpected-child\n", program);
  failed |= write_script(depthE, line, 0700);
  snprintf(line, sizeof(line), "#!%s\n", depthE);
  failed |= write_script(depthD, line, 0700);
  if (!failed)
    failed |= !exec_returns_errno(depthA, ELOOP);

  char truncatedContents[320];
  truncatedContents[0] = '#';
  truncatedContents[1] = '!';
  memset(truncatedContents + 2, 'a', sizeof(truncatedContents) - 3);
  truncatedContents[sizeof(truncatedContents) - 1] = '\n';
  failed |= write_fixture(truncated, truncatedContents, sizeof(truncatedContents), 0700) ||
            !exec_returns_errno(truncated, ENOEXEC);
  char unterminatedContents[256];
  unterminatedContents[0] = '#';
  unterminatedContents[1] = '!';
  memset(unterminatedContents + 2, 'a', sizeof(unterminatedContents) - 2);
  failed |= write_fixture(unterminated, unterminatedContents, sizeof(unterminatedContents), 0700) ||
            !exec_returns_errno(unterminated, ENOEXEC);
  failed |= write_script(malformed, "#!\t  \n", 0700) || !exec_returns_errno(malformed, ENOEXEC);

  for (size_t i = 0; i < fixtureCount; ++i) {
    if (unlink(fixtures[i]) && errno != ENOENT)
      failed = 1;
  }

  if (failed)
    fail();
  puts("OK");
  fflush(stdout);
}
