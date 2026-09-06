#define _GNU_SOURCE
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"

size_t fh_page;
static const char *persist_base, *persist_token;
static int persist_write;

static int run(const char* name, int (*test)(void)) {
  printf("FANOTIFY-HANDLE-CONTRACT: BEGIN %s\n", name);
  fflush(stdout);
  pid_t child = fork();
  if (!child) {
    alarm(40);
    int result = test();
    fflush(stdout);
    fflush(stderr);
    _exit(result ? 1 : 0);
  }
  int result = child > 0 ? fh_reap(child, 45000) : -1;
  printf("FANOTIFY-HANDLE-CONTRACT: %s %s status=%d\n", result ? "FAIL" : "PASS", name, result);
  fflush(stdout);
  return result;
}
static int persistence(void) {
  return fh_persistence(persist_base, persist_token, persist_write);
}
int main(int argc, char** argv) {
  fh_page = (size_t)sysconf(_SC_PAGESIZE);
  if (fh_page < 512 || fh_page > 65536 || (fh_page & (fh_page - 1)))
    return 2;
  signal(SIGPIPE, SIG_IGN);
  if (argc > 1 && !strcmp(argv[1], "fanotify-exec"))
    return fh_exec(argc, argv);
  if (argc == 4 && (!strcmp(argv[1], "persist-write") || !strcmp(argv[1], "persist-read"))) {
    persist_base = argv[2];
    persist_token = argv[3];
    persist_write = !strcmp(argv[1], "persist-write");
    if (run(argv[1], persistence))
      return 1;
    printf("FANOTIFY-HANDLE-PERSIST: END %s PASS token=%s\n", persist_write ? "WRITE" : "READ",
           persist_token);
    return 0;
  }
  if (argc > 2)
    return 2;
  static const struct {
    const char* name;
    int (*test)(void);
  } suites[] = {{"handles", fh_handles}, {"admission", fh_admission}, {"events", fh_events},
                {"queue", fh_queue},     {"lifetime", fh_lifetime},   {"waits", fh_waits}};
  int found = 0;
  for (size_t n = 0; n < sizeof(suites) / sizeof(suites[0]); ++n) {
    if (argc == 2 && strcmp(argv[1], "all") && strcmp(argv[1], suites[n].name))
      continue;
    found = 1;
    if (run(suites[n].name, suites[n].test))
      return 1;
  }
  if (!found)
    return 2;
  puts("FANOTIFY-HANDLE-CONTRACT: END PASS");
  return 0;
}
