#define _GNU_SOURCE
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"

size_t rp_page;

static int run(const char* name, int (*function)(void)) {
  printf("REMAP-FILE-PAGES-CONTRACT: BEGIN %s\n", name);
  fflush(stdout);
  pid_t child = fork();
  if (child < 0)
    return -1;
  if (!child) {
    alarm(40);
    int result = function();
    fflush(stdout);
    fflush(stderr);
    _exit(result ? 1 : 0);
  }
  int result = rp_reap(child, 45000);
  printf("REMAP-FILE-PAGES-CONTRACT: %s %s status=%d\n", result ? "FAIL" : "PASS", name, result);
  fflush(stdout);
  return result;
}

int main(int argc, char** argv) {
  rp_page = (size_t)sysconf(_SC_PAGESIZE);
  if (rp_page < 512 || rp_page > 65536 || (rp_page & (rp_page - 1)))
    return 2;
  signal(SIGPIPE, SIG_IGN);
  static const struct {
    const char* name;
    int (*function)(void);
  } suites[] = {{"offsets", rp_offsets},
                {"admission", rp_admission},
                {"lifetime", rp_lifetime},
                {"locks", rp_locks},
                {"resize", rp_resize}};
  int selected = 0;
  for (size_t n = 0; n < sizeof(suites) / sizeof(suites[0]); ++n) {
    if (argc > 1 && strcmp(argv[1], suites[n].name))
      continue;
    selected = 1;
    if (run(suites[n].name, suites[n].function))
      return 1;
  }
  if (!selected)
    return 2;
  puts("REMAP-FILE-PAGES-CONTRACT: END PASS");
  return 0;
}
