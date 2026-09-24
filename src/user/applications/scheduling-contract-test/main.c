#define _GNU_SOURCE
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc > 1 && !strcmp(argv[1], "schedule-exec"))
    return sc_exec(argc, argv);
  if (argc > 3 || sc_init(argc == 3 ? atoi(argv[2]) : 0))
    return 2;
  const struct {
    const char* name;
    int (*run)(void);
  } families[] = {{"api-policy", sc_api},
                  {"placement", sc_placement},
                  {"wakeups", sc_wakeups},
                  {"lifecycle", sc_lifecycle},
                  {"permissions", sc_permissions}};
  int selected = 0;
  for (size_t i = 0; i < sizeof(families) / sizeof(families[0]); ++i) {
    if (argc >= 2 && strcmp(argv[1], "all") && strcmp(argv[1], families[i].name))
      continue;
    ++selected;
    printf("SCHEDULING-CONTRACT: BEGIN %s\n", families[i].name);
    pid_t pid = fork();
    if (!pid) {
      alarm(55);
      int result = families[i].run();
      fflush(stderr);
      _exit(result ? 1 : 0);
    }
    int status = pid < 0 ? -1 : sc_reap(pid, 60000);
    cpu_set_t current;
    if (sched_getaffinity(0, sizeof(current), &current) || !CPU_EQUAL(&current, &sc_allowed)) {
      fputs("supervisor affinity changed\n", stderr);
      status = 1;
    }
    printf("SCHEDULING-CONTRACT: %s %s status=%d\n", status ? "FAIL" : "PASS", families[i].name,
           status);
    if (status)
      return 1;
  }
  if (!selected)
    return 2;
  puts("SCHEDULING-CONTRACT: END PASS");
  return 0;
}
