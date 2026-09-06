#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/personality.h>
#include <sys/utsname.h>
#include <sys/wait.h>

#define CHECK(condition)                                                            \
  do {                                                                              \
    if (!(condition)) {                                                             \
      fprintf(stderr, "PERSONALITY-CONTRACT: line=%d errno=%d\n", __LINE__, errno); \
      goto fail;                                                                    \
    }                                                                               \
  } while (0)

static char native_machine[65];

static int selected(int expected, const char* machine) {
  struct utsname name;
  return personality(0xffffffffUL) == expected && !uname(&name) && !strcmp(name.machine, machine);
}

static void* thread_test(void* unused) {
  (void)unused;
  if (!selected(PER_LINUX32, "i686"))
    return (void*)1;
  if (personality(PER_LINUX) != PER_LINUX32 || !selected(PER_LINUX, native_machine))
    return (void*)2;
  return NULL;
}

int main(int argc, char** argv) {
  if (argc == 2 && !strcmp(argv[1], "--exec-child"))
    return selected(PER_LINUX32, "i686") ? 0 : 3;
  struct utsname name;
  int result = 1;
  const int original = personality(0xffffffffUL);
  CHECK(original >= 0);
  CHECK(personality(PER_LINUX) == original);
  CHECK(uname(&name) == 0);
  memcpy(native_machine, name.machine, sizeof(native_machine));
  CHECK(personality(PER_LINUX32) == PER_LINUX);
  CHECK(selected(PER_LINUX32, "i686"));
  const unsigned long unsupported[] = {PER_BSD,
                                       PER_LINUX32 | ADDR_LIMIT_3GB,
                                       PER_LINUX | ADDR_NO_RANDOMIZE,
                                       PER_LINUX | READ_IMPLIES_EXEC,
                                       PER_LINUX | UNAME26,
                                       PER_LINUX_FDPIC};
  for (size_t i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); ++i) {
    errno = 0;
    CHECK(personality(unsupported[i]) == -1 && errno == EINVAL);
    CHECK(selected(PER_LINUX32, "i686"));
  }
  pthread_t thread;
  void* returned = (void*)9;
  CHECK(pthread_create(&thread, NULL, thread_test, NULL) == 0);
  CHECK(pthread_join(thread, &returned) == 0 && returned == NULL);
  CHECK(selected(PER_LINUX32, "i686"));
  pid_t child = fork();
  CHECK(child >= 0);
  if (!child) {
    if (!selected(PER_LINUX32, "i686"))
      _exit(4);
    execlp(argv[0], argv[0], "--exec-child", (char*)NULL);
    _exit(5);
  }
  int status = 0;
  CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  CHECK(personality(PER_LINUX) == PER_LINUX32);
  CHECK(selected(PER_LINUX, native_machine));
  result = 0;
fail:
  if (original >= 0)
    personality((unsigned long)original);
  puts(result ? "PERSONALITY-CONTRACT: FAIL" : "PERSONALITY-CONTRACT: PASS");
  return result;
}
