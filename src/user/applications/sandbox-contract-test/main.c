/* Copyright (c) 2026, Pedigree Developers. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#ifndef SYS_seccomp
#if defined(__x86_64__)
#define SYS_seccomp 317
#elif defined(__aarch64__)
#define SYS_seccomp 277
#elif defined(__i386__)
#define SYS_seccomp 354
#elif defined(__arm__)
#define SYS_seccomp 383
#endif
#endif
#ifndef SYS_landlock_create_ruleset
#define SYS_landlock_create_ruleset 444
#define SYS_landlock_add_rule 445
#define SYS_landlock_restrict_self 446
#endif

#if defined(__x86_64__)
#define NATIVE_AUDIT_ARCH 0xc000003eU
#elif defined(__aarch64__)
#define NATIVE_AUDIT_ARCH 0xc00000b7U
#elif defined(__i386__)
#define NATIVE_AUDIT_ARCH 0x40000003U
#elif defined(__arm__)
#define NATIVE_AUDIT_ARCH 0x40000028U
#endif

#define PR_GET_SECCOMP 21
#define PR_SET_NO_NEW_PRIVS 38
#define PR_GET_NO_NEW_PRIVS 39
#define SECCOMP_SET_MODE_FILTER 1
#define SECCOMP_GET_ACTION_AVAIL 2
#define SECCOMP_RET_KILL_PROCESS 0x80000000U
#define SECCOMP_RET_ALLOW 0x7fff0000U
#define SECCOMP_RET_ERRNO 0x00050000U
#define LANDLOCK_CREATE_RULESET_VERSION 1
#define LANDLOCK_RULE_PATH_BENEATH 1
#define LANDLOCK_READ ((1ULL << 0) | (1ULL << 2) | (1ULL << 3))
#define LANDLOCK_ABI3_ALL ((1ULL << 15) - 1)

#define CHECK(expression)                                                                    \
  do {                                                                                       \
    if (!(expression)) {                                                                     \
      fprintf(stderr, "SANDBOX-CONTRACT: FAIL line=%d %s errno=%d\n", __LINE__, #expression, \
              errno);                                                                        \
      return 1;                                                                              \
    }                                                                                        \
  } while (0)

#define EXPECT(call, expected, error)                                                            \
  do {                                                                                           \
    errno = 0;                                                                                   \
    long actualResult = (call);                                                                  \
    int actualError = errno;                                                                     \
    if (actualResult != (expected) || (actualResult == -1 && actualError != (error))) {          \
      fprintf(stderr, "SANDBOX-CONTRACT: FAIL line=%d %s result=%ld errno=%d expected=%ld/%d\n", \
              __LINE__, #call, actualResult, actualError, (long)(expected), (error));            \
      return 1;                                                                                  \
    }                                                                                            \
  } while (0)

struct filter_instruction {
  uint16_t code;
  uint8_t jt, jf;
  uint32_t k;
};
struct filter_program {
  uint16_t count;
  const struct filter_instruction* instructions;
};
struct ruleset_attr {
  uint64_t handled_access_fs;
};
struct path_beneath_attr {
  uint64_t allowed_access;
  int32_t parent_fd;
} __attribute__((packed));

struct fixture {
  char root[256];
  char inside[320], outside[320];
  char allowed[384], protected[384], created[384], renamed[384];
  char inside_link[384], outside_link[384], outside_alias[384];
};

static int paths(struct fixture* fixture, const char* root) {
  CHECK(strlen(root) < sizeof(fixture->root));
  strcpy(fixture->root, root);
  snprintf(fixture->inside, sizeof(fixture->inside), "%s/inside", root);
  snprintf(fixture->outside, sizeof(fixture->outside), "%s/outside", root);
  snprintf(fixture->allowed, sizeof(fixture->allowed), "%s/file", fixture->inside);
  snprintf(fixture->protected, sizeof(fixture->protected), "%s/file", fixture->outside);
  snprintf(fixture->created, sizeof(fixture->created), "%s/new", fixture->outside);
  snprintf(fixture->renamed, sizeof(fixture->renamed), "%s/renamed", fixture->outside);
  snprintf(fixture->inside_link, sizeof(fixture->inside_link), "%s/points-outside",
           fixture->inside);
  snprintf(fixture->outside_link, sizeof(fixture->outside_link), "%s/points-inside",
           fixture->outside);
  snprintf(fixture->outside_alias, sizeof(fixture->outside_alias), "%s/alias", fixture->outside);
  return 0;
}

static int prepare(const struct fixture* fixture) {
  CHECK(mkdir(fixture->inside, 0700) == 0);
  CHECK(mkdir(fixture->outside, 0700) == 0);
  int fd = open(fixture->protected, O_CREAT | O_EXCL | O_WRONLY, 0600);
  CHECK(fd >= 0);
  EXPECT(write(fd, "outside", 7), 7, 0);
  EXPECT(close(fd), 0, 0);
  fd = open(fixture->allowed, O_CREAT | O_EXCL | O_WRONLY, 0600);
  CHECK(fd >= 0);
  EXPECT(close(fd), 0, 0);
  EXPECT(link(fixture->allowed, fixture->outside_alias), 0, 0);
  EXPECT(symlink(fixture->protected, fixture->inside_link), 0, 0);
  EXPECT(symlink(fixture->allowed, fixture->outside_link), 0, 0);
  return 0;
}

static int cleanup(const struct fixture* fixture) {
  const char* files[] = {fixture->allowed,      fixture->protected,   fixture->created,
                         fixture->renamed,      fixture->inside_link, fixture->outside_link,
                         fixture->outside_alias};
  int failed = 0;
  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
    if (unlink(files[i]) && errno != ENOENT) {
      fprintf(stderr, "SANDBOX-CONTRACT: cleanup %s errno=%d\n", files[i], errno);
      failed = 1;
    }
  }
  const char* directories[] = {fixture->inside, fixture->outside, fixture->root};
  for (size_t i = 0; i < sizeof(directories) / sizeof(directories[0]); ++i) {
    if (rmdir(directories[i]) && errno != ENOENT) {
      fprintf(stderr, "SANDBOX-CONTRACT: cleanup %s errno=%d\n", directories[i], errno);
      failed = 1;
    }
  }
  return failed;
}

static int no_new_privileges(void) {
  EXPECT(syscall(SYS_prctl, PR_GET_NO_NEW_PRIVS, 1UL, 0UL, 0UL, 0UL), -1, EINVAL);
  EXPECT(syscall(SYS_prctl, PR_SET_NO_NEW_PRIVS, 0UL, 0UL, 0UL, 0UL), -1, EINVAL);
  EXPECT(syscall(SYS_prctl, PR_SET_NO_NEW_PRIVS, 2UL, 0UL, 0UL, 0UL), -1, EINVAL);
  EXPECT(syscall(SYS_prctl, PR_SET_NO_NEW_PRIVS, 1UL, 1UL, 0UL, 0UL), -1, EINVAL);
  EXPECT(syscall(SYS_prctl, PR_SET_NO_NEW_PRIVS, 1UL, 0UL, 0UL, 0UL), 0, 0);
  EXPECT(syscall(SYS_prctl, PR_GET_NO_NEW_PRIVS, 0UL, 0UL, 0UL, 0UL), 1, 0);
  EXPECT(syscall(SYS_prctl, PR_SET_NO_NEW_PRIVS, 0UL, 0UL, 0UL, 0UL), -1, EINVAL);
  EXPECT(syscall(SYS_prctl, PR_GET_NO_NEW_PRIVS, 0UL, 0UL, 0UL, 0UL), 1, 0);
  return 0;
}

static int install_filter(void) {
  const struct filter_instruction instructions[] = {
      {0x20, 0, 0, 4},  // Load architecture.
      {0x15, 1, 0, NATIVE_AUDIT_ARCH},
      {0x06, 0, 0, SECCOMP_RET_KILL_PROCESS},
      {0x20, 0, 0, 0},  // Load syscall number.
      {0x15, 0, 3, SYS_socket},
      {0x20, 0, 0, 16},  // Load socket domain argument.
      {0x15, 0, 1, AF_INET},
      {0x06, 0, 0, SECCOMP_RET_ERRNO | EPERM},
      {0x06, 0, 0, SECCOMP_RET_ALLOW},
  };
  const struct filter_program program = {sizeof(instructions) / sizeof(instructions[0]),
                                         instructions};
  EXPECT(syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &program), 0, 0);
  EXPECT(syscall(SYS_prctl, PR_GET_SECCOMP, 0UL, 0UL, 0UL, 0UL), 2, 0);
  EXPECT(syscall(SYS_socket, AF_INET, SOCK_STREAM, 0), -1, EPERM);

  const struct filter_instruction allow[] = {{0x06, 0, 0, SECCOMP_RET_ALLOW}};
  const struct filter_program second = {1, allow};
  EXPECT(syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &second), 0, 0);
  EXPECT(syscall(SYS_socket, AF_INET, SOCK_STREAM, 0), -1, EPERM);
  return 0;
}

static int filter_checks(void) {
  EXPECT(syscall(SYS_prctl, PR_GET_NO_NEW_PRIVS, 0UL, 0UL, 0UL, 0UL), 1, 0);
  EXPECT(syscall(SYS_prctl, PR_GET_SECCOMP, 0UL, 0UL, 0UL, 0UL), 2, 0);
  EXPECT(syscall(SYS_socket, AF_INET, SOCK_STREAM, 0), -1, EPERM);
  return 0;
}

static void* inherited_thread(void* unused) {
  (void)unused;
  return (void*)(uintptr_t)filter_checks();
}

static void* filtered_thread(void* unused) {
  (void)unused;
  int failed = no_new_privileges() || install_filter();
  if (!failed) {
    pthread_t descendant;
    int error = pthread_create(&descendant, NULL, inherited_thread, NULL);
    if (!error) {
      void* result = NULL;
      error = pthread_join(descendant, &result);
      failed = result != NULL;
    }
    if (error) {
      fprintf(stderr, "SANDBOX-CONTRACT: descendant pthread error=%d\n", error);
      failed = 1;
    }
  }
  return (void*)(uintptr_t)failed;
}

static int thread_isolation(long privileges, long mode) {
  pthread_t worker;
  EXPECT(pthread_create(&worker, NULL, filtered_thread, NULL), 0, 0);
  void* result = NULL;
  EXPECT(pthread_join(worker, &result), 0, 0);
  CHECK(result == NULL);
  EXPECT(syscall(SYS_prctl, PR_GET_NO_NEW_PRIVS, 0UL, 0UL, 0UL, 0UL), privileges, 0);
  EXPECT(syscall(SYS_prctl, PR_GET_SECCOMP, 0UL, 0UL, 0UL, 0UL), mode, 0);
  int fd = syscall(SYS_socket, AF_INET, SOCK_STREAM, 0);
  CHECK(fd >= 0);
  EXPECT(close(fd), 0, 0);
  puts("SANDBOX-CONTRACT: pthread inheritance/isolation PASS");
  return 0;
}

static int add_path(int ruleset, const char* path, uint64_t access) {
  int directory = open(path, O_PATH | O_CLOEXEC);
  CHECK(directory >= 0);
  const struct path_beneath_attr rule = {access, directory};
  errno = 0;
  long result = syscall(SYS_landlock_add_rule, ruleset, LANDLOCK_RULE_PATH_BENEATH, &rule, 0);
  int saved = errno;
  close(directory);
  if (result) {
    fprintf(stderr, "SANDBOX-CONTRACT: add-rule %s result=%ld errno=%d\n", path, result, saved);
    return 1;
  }
  return 0;
}

static int install_domain(const struct fixture* fixture) {
  errno = 0;
  long abi = syscall(SYS_landlock_create_ruleset, NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
  CHECK(abi >= 3);
  const struct ruleset_attr attr = {LANDLOCK_ABI3_ALL};
  int ruleset = syscall(SYS_landlock_create_ruleset, &attr, sizeof(attr), 0);
  CHECK(ruleset >= 0);
  int failed = add_path(ruleset, "/", LANDLOCK_READ) ||
               add_path(ruleset, fixture->inside, LANDLOCK_ABI3_ALL);
  if (!failed) {
    errno = 0;
    long result = syscall(SYS_landlock_restrict_self, ruleset, 0);
    if (result) {
      fprintf(stderr, "SANDBOX-CONTRACT: restrict-self result=%ld errno=%d\n", result, errno);
      failed = 1;
    }
  }
  if (!failed) {
    failed = add_path(ruleset, "/", LANDLOCK_ABI3_ALL);
    EXPECT(open(fixture->protected, O_WRONLY), -1, EACCES);
    EXPECT(syscall(SYS_landlock_restrict_self, ruleset, 0), 0, 0);
  }
  close(ruleset);
  return failed;
}

static int confined_checks(const struct fixture* fixture, int retained) {
  CHECK(!filter_checks());
  int pair[2];
  EXPECT(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0, 0);
  EXPECT(write(pair[0], "s", 1), 1, 0);
  char byte;
  EXPECT(read(pair[1], &byte, 1), 1, 0);
  CHECK(byte == 's');
  EXPECT(close(pair[0]), 0, 0);
  EXPECT(close(pair[1]), 0, 0);

  EXPECT(open(fixture->protected, O_WRONLY), -1, EACCES);
  EXPECT(open(fixture->protected, O_WRONLY | O_TRUNC), -1, EACCES);
  EXPECT(open(fixture->created, O_CREAT | O_EXCL | O_WRONLY, 0600), -1, EACCES);
  EXPECT(truncate(fixture->protected, 0), -1, EACCES);
  EXPECT(rename(fixture->protected, fixture->renamed), -1, EACCES);
  EXPECT(open(fixture->inside_link, O_WRONLY), -1, EACCES);
  EXPECT(open(fixture->outside_alias, O_WRONLY), -1, EACCES);

  int fd = open(fixture->protected, O_RDONLY);
  CHECK(fd >= 0);
  char content[7];
  EXPECT(read(fd, content, sizeof(content)), sizeof(content), 0);
  CHECK(!memcmp(content, "outside", sizeof(content)));
  EXPECT(close(fd), 0, 0);
  fd = open(fixture->allowed, O_CREAT | O_TRUNC | O_RDWR, 0600);
  CHECK(fd >= 0);
  EXPECT(write(fd, "allowed", 7), 7, 0);
  EXPECT(ftruncate(fd, 4), 0, 0);
  EXPECT(close(fd), 0, 0);
  EXPECT(truncate(fixture->allowed, 2), 0, 0);
  fd = open(fixture->outside_link, O_WRONLY);
  CHECK(fd >= 0);
  EXPECT(write(fd, "a", 1), 1, 0);
  EXPECT(close(fd), 0, 0);

  struct stat before, after;
  EXPECT(fstat(retained, &before), 0, 0);
  EXPECT(ftruncate(retained, before.st_size), 0, 0);
  EXPECT(lseek(retained, 0, SEEK_END), before.st_size, 0);
  EXPECT(write(retained, "+", 1), 1, 0);
  EXPECT(fstat(retained, &after), 0, 0);
  CHECK(after.st_size == before.st_size + 1);
  return 0;
}

static int reap(pid_t child) {
  CHECK(child > 0);
  int status;
  pid_t result;
  do {
    result = waitpid(child, &status, 0);
  } while (result < 0 && errno == EINTR);
  CHECK(result == child);
  if (!WIFEXITED(status) || WEXITSTATUS(status)) {
    fprintf(stderr, "SANDBOX-CONTRACT: child=%ld status=%d\n", (long)child, status);
    return 1;
  }
  return 0;
}

static int restricted_child(const struct fixture* fixture, const char* executable) {
  alarm(60);
  int retained = open(fixture->protected, O_RDWR);
  CHECK(retained >= 0);
  CHECK(!no_new_privileges());
  CHECK(!install_filter());
  CHECK(!install_domain(fixture));
  CHECK(!confined_checks(fixture, retained));
  pid_t child = fork();
  if (!child) {
    alarm(30);
    if (confined_checks(fixture, retained)) {
      _exit(1);
    }
    char descriptor[32];
    snprintf(descriptor, sizeof(descriptor), "%d", retained);
    execl(executable, executable, "--child", fixture->root, descriptor, (char*)NULL);
    fprintf(stderr, "SANDBOX-CONTRACT: exec %s errno=%d\n", executable, errno);
    _exit(1);
  }
  int failed = reap(child);
  close(retained);
  return failed;
}

static int parent_checks(const struct fixture* fixture, long privileges, long mode) {
  EXPECT(syscall(SYS_prctl, PR_GET_NO_NEW_PRIVS, 0UL, 0UL, 0UL, 0UL), privileges, 0);
  EXPECT(syscall(SYS_prctl, PR_GET_SECCOMP, 0UL, 0UL, 0UL, 0UL), mode, 0);
  int fd = syscall(SYS_socket, AF_INET, SOCK_STREAM, 0);
  CHECK(fd >= 0);
  EXPECT(close(fd), 0, 0);
  struct stat info;
  EXPECT(stat(fixture->protected, &info), 0, 0);
  CHECK(info.st_size == 10);
  fd = open(fixture->created, O_CREAT | O_EXCL | O_WRONLY, 0600);
  CHECK(fd >= 0);
  EXPECT(write(fd, "parent", 6), 6, 0);
  EXPECT(close(fd), 0, 0);
  EXPECT(truncate(fixture->protected, 7), 0, 0);
  EXPECT(rename(fixture->protected, fixture->renamed), 0, 0);
  EXPECT(rename(fixture->renamed, fixture->protected), 0, 0);
  return 0;
}

#define PROBE(name, call)                                                   \
  do {                                                                      \
    errno = 0;                                                              \
    long result = (call);                                                   \
    printf("SANDBOX-PROBE: %s result=%ld errno=%d\n", name, result, errno); \
  } while (0)

static int probe(void) {
  PROBE("get-no-new-privs", syscall(SYS_prctl, PR_GET_NO_NEW_PRIVS, 0UL, 0UL, 0UL, 0UL));
  PROBE("get-seccomp", syscall(SYS_prctl, PR_GET_SECCOMP, 0UL, 0UL, 0UL, 0UL));
  uint32_t action = SECCOMP_RET_ALLOW;
  PROBE("seccomp-action-allow", syscall(SYS_seccomp, SECCOMP_GET_ACTION_AVAIL, 0, &action));
  PROBE("landlock-abi",
        syscall(SYS_landlock_create_ruleset, NULL, 0, LANDLOCK_CREATE_RULESET_VERSION));
  PROBE("set-no-new-privs", syscall(SYS_prctl, PR_SET_NO_NEW_PRIVS, 1UL, 0UL, 0UL, 0UL));
  const struct filter_instruction allow[] = {{0x06, 0, 0, SECCOMP_RET_ALLOW}};
  const struct filter_program program = {1, allow};
  PROBE("seccomp-install", syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &program));
  const struct ruleset_attr attr = {LANDLOCK_READ};
  errno = 0;
  int ruleset = syscall(SYS_landlock_create_ruleset, &attr, sizeof(attr), 0);
  printf("SANDBOX-PROBE: landlock-create result=%d errno=%d\n", ruleset, errno);
  int root = open("/", O_PATH | O_CLOEXEC);
  const struct path_beneath_attr rule = {LANDLOCK_READ, root};
  PROBE("landlock-add",
        syscall(SYS_landlock_add_rule, ruleset, LANDLOCK_RULE_PATH_BENEATH, &rule, 0));
  PROBE("landlock-restrict", syscall(SYS_landlock_restrict_self, ruleset, 0));
  if (root >= 0) {
    close(root);
  }
  if (ruleset >= 0) {
    close(ruleset);
  }
  puts("SANDBOX-PROBE: DONE");
  return 0;
}

static int command_denied(long result, int error, const char* operation) {
  if (result == -1 && (error == EACCES || error == EPERM || error == EROFS)) {
    return 0;
  }
  fprintf(stderr, "CODEX-SANDBOX-COMMAND: FAIL %s result=%ld errno=%d\n", operation, result, error);
  return 1;
}

static int command_check(const char* directory, const char* protected, int writable) {
  EXPECT(syscall(SYS_socket, AF_INET, SOCK_STREAM, 0), -1, EPERM);
  struct stat info;
  EXPECT(stat(protected, &info), 0, 0);
  CHECK(S_ISREG(info.st_mode));
  int fd = open(protected, O_RDONLY);
  CHECK(fd >= 0);
  char byte;
  EXPECT(read(fd, &byte, 1), 1, 0);
  EXPECT(close(fd), 0, 0);
  errno = 0;
  fd = open(protected, O_WRONLY);
  int error = errno;
  if (fd >= 0) {
    close(fd);
  }
  CHECK(!command_denied(fd, error, "open protected file"));
  errno = 0;
  long result = truncate(protected, 0);
  CHECK(!command_denied(result, errno, "truncate protected file"));

  char path[4096];
  int length =
      snprintf(path, sizeof(path), "%s/codex-sandbox-command-%ld", directory, (long)getpid());
  CHECK(length > 0 && (size_t)length < sizeof(path));
  errno = 0;
  fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
  if (!writable) {
    CHECK(!command_denied(fd, errno, "create file"));
    puts("CODEX-SANDBOX-COMMAND: PASS");
    return 0;
  }
  CHECK(fd >= 0);
  errno = 0;
  result = write(fd, "codex", 5);
  error = errno;
  int failed = result != 5;
  if (failed) {
    fprintf(stderr, "CODEX-SANDBOX-COMMAND: FAIL write result=%ld errno=%d\n", result, error);
  }
  failed |= close(fd) != 0;
  failed |= unlink(path) != 0;
  CHECK(!failed);
  puts("CODEX-SANDBOX-COMMAND: PASS");
  return 0;
}

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  alarm(90);
  if (argc == 2 && !strcmp(argv[1], "--probe")) {
    return probe();
  }
  if (argc == 4 && !strcmp(argv[1], "--command-check")) {
    return command_check(argv[2], argv[3], 1);
  }
  if (argc == 3 && !strcmp(argv[1], "--read-only-check")) {
    return command_check("/tmp", argv[2], 0);
  }
  struct fixture fixture;
  if (argc == 4 && !strcmp(argv[1], "--child")) {
    CHECK(!paths(&fixture, argv[2]));
    char* end;
    long descriptor = strtol(argv[3], &end, 10);
    CHECK(!*end && descriptor >= 0 && descriptor <= INT32_MAX);
    return confined_checks(&fixture, descriptor);
  }
  CHECK(argc == 1);
  char executable[4096];
  if (!realpath(argv[0], executable)) {
    ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    CHECK(length > 0 && (size_t)length < sizeof(executable) - 1);
    executable[length] = 0;
  }
  long privileges = syscall(SYS_prctl, PR_GET_NO_NEW_PRIVS, 0UL, 0UL, 0UL, 0UL);
  long mode = syscall(SYS_prctl, PR_GET_SECCOMP, 0UL, 0UL, 0UL, 0UL);
  CHECK(privileges >= 0 && mode >= 0);
  CHECK(!thread_isolation(privileges, mode));
  char directory[256];
  snprintf(directory, sizeof(directory), "/tmp/sandbox-contract-%ld", (long)getpid());
  CHECK(!paths(&fixture, directory));
  CHECK(mkdir(fixture.root, 0700) == 0);
  int failed = prepare(&fixture);
  if (!failed) {
    pid_t child = fork();
    if (!child) {
      _exit(restricted_child(&fixture, executable));
    }
    failed = reap(child);
    if (!failed) {
      failed = parent_checks(&fixture, privileges, mode);
    }
  }
  failed |= cleanup(&fixture);
  puts(failed ? "SANDBOX-CONTRACT: FAIL" : "SANDBOX-CONTRACT: PASS");
  return failed;
}
