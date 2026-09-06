#define _GNU_SOURCE
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"

static int number(const char* text) {
  char* end;
  errno = 0;
  long value = strtol(text, &end, 10);
  return errno || !*text || *end || value < 0 || value > INT_MAX ? -1 : (int)value;
}

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc == 3 && !strcmp(argv[1], "--exec-child")) {
    int report = number(argv[2]);
    return report < 0 ? 2 : tc_exec_child(report);
  }
  if (argc == 5 && !strcmp(argv[1], "--exec-tracer")) {
    int child = number(argv[2]), command = number(argv[3]), report = number(argv[4]);
    return child <= 0 || command < 0 || report < 0 ? 2 : tc_exec_tracer(child, command, report);
  }
  if (argc > 2)
    return 2;
  tc_page = (size_t)sysconf(_SC_PAGESIZE);
  if (!tc_page || tc_page > 65536)
    return 2;
  const struct {
    const char* name;
    int (*run)(void);
  } families[] = {{"registers", tc_registers}, {"inspect", tc_inspect},     {"signals", tc_signals},
                  {"ownership", tc_ownership}, {"lifecycle", tc_lifecycle}, {"errors", tc_errors}};
  int selected = 0;
  for (size_t i = 0; i < sizeof(families) / sizeof(families[0]); ++i) {
    if (argc == 2 && strcmp(argv[1], "all") && strcmp(argv[1], families[i].name))
      continue;
    ++selected;
    printf("PTRACE-CONTRACT: BEGIN %s\n", families[i].name);
    pid_t family = fork();
    if (!family) {
      if (setpgid(0, 0))
        _exit(120);
      alarm(50);
      int failed = families[i].run();
      fflush(stderr);
      _exit(failed ? 1 : 0);
    }
    if (family < 0)
      return 1;
    int status = 0;
    int result = tc_wait(family, &status, 55000);
    if (result || !WIFEXITED(status) || WEXITSTATUS(status)) {
      // The family group includes its tracees even when a tracer times out.
      kill(-family, SIGKILL);
      if (result || (!WIFEXITED(status) && !WIFSIGNALED(status)))
        tc_cleanup(&family);
      printf("PTRACE-CONTRACT: FAIL %s result=%d status=%#x errno=%d\n", families[i].name, result,
             status, errno);
      return 1;
    }
    printf("PTRACE-CONTRACT: PASS %s\n", families[i].name);
  }
  if (!selected)
    return 2;
  puts("PTRACE-CONTRACT: END PASS");
  return 0;
}
