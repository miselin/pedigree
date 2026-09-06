#define _GNU_SOURCE
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"

size_t xa_page;
size_t xa_block;
static const char *persist_base, *persist_token;
static int persist_write;

static int block_size(const char* text) {
  char* end;
  errno = 0;
  unsigned long value = strtoul(text, &end, 10);
  if (errno || !text[0] || *end || value < 1024 || value > 65536 || (value & (value - 1)))
    return -1;
  xa_block = value;
  return 0;
}

static int run(const char* name, int (*function)(void)) {
  printf("XATTR-CONTRACT: BEGIN %s\n", name);
  fflush(stdout);
  pid_t child = fork();
  int result = -1;
  if (!child) {
    alarm(40);
    result = function();
    fflush(stdout);
    fflush(stderr);
    _exit(result ? 1 : 0);
  }
  if (child > 0)
    result = xa_reap(child, 45000);
  printf("XATTR-CONTRACT: %s %s status=%d\n", result ? "FAIL" : "PASS", name, result);
  fflush(stdout);
  return result;
}

static int persistence(void) {
  return xa_persistence(persist_base, persist_token, persist_write);
}

int main(int argc, char** argv) {
  xa_page = (size_t)sysconf(_SC_PAGESIZE);
  if (xa_page < 512 || xa_page > 65536 || (xa_page & (xa_page - 1)))
    return 2;
  signal(SIGPIPE, SIG_IGN);
  if (argc == 5 && (!strcmp(argv[1], "persist-write") || !strcmp(argv[1], "persist-read"))) {
    if (block_size(argv[4]))
      return 2;
    persist_base = argv[2];
    persist_token = argv[3];
    persist_write = !strcmp(argv[1], "persist-write");
    if (run(argv[1], persistence))
      return 1;
    printf("XATTR-PERSIST-CONTRACT: END %s PASS token=%s\n", persist_write ? "WRITE" : "READ",
           persist_token);
    return 0;
  }
  const char* selected = NULL;
  for (int n = 1; n < argc; ++n) {
    if (!strcmp(argv[n], "--block-size") && n + 1 < argc) {
      if (block_size(argv[++n]))
        return 2;
    } else if (!selected)
      selected = argv[n];
    else
      return 2;
  }
  if (!xa_block) {
    fputs("XATTR-CONTRACT: verified Ext2 --block-size is required\n", stderr);
    return 2;
  }
  static const struct {
    const char* name;
    int (*function)(void);
  } suites[] = {
      {"values", xa_values},     {"admission", xa_admission},     {"permissions", xa_permissions},
      {"lifetime", xa_lifetime}, {"concurrency", xa_concurrency}, {"events", xa_events},
      {"ext2", xa_ext2}};
  int found = 0;
  for (size_t n = 0; n < sizeof(suites) / sizeof(suites[0]); ++n) {
    if (selected && strcmp(selected, "all") && strcmp(selected, suites[n].name))
      continue;
    found = 1;
    if (run(suites[n].name, suites[n].function))
      return 1;
  }
  if (!found)
    return 2;
  puts("XATTR-CONTRACT: END PASS");
  return 0;
}
