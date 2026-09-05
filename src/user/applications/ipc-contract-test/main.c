#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/wait.h>

int ipc_test_messages(void);
int ipc_test_semaphores(void);
int ipc_test_mqueues(void);
int ipc_test_shared_memory(void);
int ipc_test_shared_memory_exec(int argc, char** argv);
int ipc_test_mqueue_exec(int argc, char** argv);

static int run(const char* name, int (*test)(void)) {
  printf("IPC-CONTRACT: BEGIN %s\n", name);
  fflush(stdout);
  pid_t child = fork();
  if (child < 0) {
    perror("fork");
    return -1;
  }
  if (!child) {
    alarm(90);
    int result = test();
    fflush(stdout);
    fflush(stderr);
    _exit(result ? 1 : 0);
  }
  struct timespec started, now;
  clock_gettime(CLOCK_MONOTONIC, &started);
  for (;;) {
    int status = 0;
    pid_t result = waitpid(child, &status, WNOHANG);
    if (result == child) {
      if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        printf("IPC-CONTRACT: PASS %s\n", name);
        return 0;
      }
      printf("IPC-CONTRACT: FAIL %s status=%d\n", name, status);
      return -1;
    }
    if (result < 0 && errno != EINTR) {
      perror("waitpid");
      kill(child, SIGKILL);
      return -1;
    }
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec - started.tv_sec > 100) {
      kill(child, SIGKILL);
      while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
      }
      printf("IPC-CONTRACT: FAIL %s timeout\n", name);
      return -1;
    }
    struct timespec pause = {0, 10000000};
    nanosleep(&pause, NULL);
  }
}

int main(int argc, char** argv) {
  if (argc > 1 && !strcmp(argv[1], "shm-exec")) {
    return ipc_test_shared_memory_exec(argc, argv);
  }
  if (argc > 1 && !strcmp(argv[1], "mq-exec")) {
    return ipc_test_mqueue_exec(argc, argv);
  }
  static const struct {
    const char* name;
    int (*test)(void);
  } suites[] = {{"messages", ipc_test_messages},
                {"semaphores", ipc_test_semaphores},
                {"mqueues", ipc_test_mqueues},
                {"shared-memory", ipc_test_shared_memory}};
  int selected = 0;
  for (unsigned i = 0; i < sizeof suites / sizeof suites[0]; ++i) {
    if (argc > 1 && strcmp(argv[1], suites[i].name)) {
      continue;
    }
    selected = 1;
    if (run(suites[i].name, suites[i].test)) {
      return 1;
    }
  }
  if (!selected) {
    fprintf(stderr, "Unknown IPC contract suite\n");
    return 2;
  }
  puts("IPC-CONTRACT: END PASS");
  return 0;
}
