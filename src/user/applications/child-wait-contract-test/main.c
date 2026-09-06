#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc > 2)
    return 2;
  cw_page = (size_t)sysconf(_SC_PAGESIZE);
  if (!cw_page || cw_page > 65536)
    return 2;
  const struct {
    const char* name;
    int (*run)(void);
  } families[] = {{"selectors", cw_selectors}, {"events", cw_events},
                  {"lifetime", cw_lifetime},   {"copyout", cw_copyout},
                  {"errors", cw_errors},       {"interruption", cw_interruption}};
  int selected = 0;
  for (size_t i = 0; i < sizeof(families) / sizeof(families[0]); ++i) {
    if (argc == 2 && strcmp(argv[1], "all") && strcmp(argv[1], families[i].name))
      continue;
    ++selected;
    printf("CHILD-WAIT-CONTRACT: BEGIN %s\n", families[i].name);
    pid_t pid = fork();
    if (!pid) {
      alarm(40);
      int result = families[i].run();
      fflush(stderr);
      _exit(result ? 1 : 0);
    }
    int status = pid < 0 ? -1 : cw_reap(pid, 45000);
    printf("CHILD-WAIT-CONTRACT: %s %s status=%d\n", status ? "FAIL" : "PASS", families[i].name,
           status);
    if (status)
      return 1;
  }
  if (!selected)
    return 2;
  puts("CHILD-WAIT-CONTRACT: END PASS");
  return 0;
}
