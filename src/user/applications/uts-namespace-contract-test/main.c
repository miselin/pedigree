#define _GNU_SOURCE
#include <sched.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/utsname.h>

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  ns_page = (size_t)sysconf(_SC_PAGESIZE);
  if (argc > 1 && !strcmp(argv[1], "uts-exec"))
    return ns_exec(argc, argv);
  struct {
    const char* name;
    int (*run)(void);
  } families[] = {{"names", ns_names},
                  {"membership", ns_membership},
                  {"proc", ns_proc},
                  {"lifetime", ns_lifetime},
                  {"admission", ns_admission}};
  int selected = 0;
  if (argc > 2)
    return 2;
  for (size_t i = 0; i < sizeof(families) / sizeof(families[0]); ++i) {
    if (argc == 2 && strcmp(argv[1], "all") && strcmp(argv[1], families[i].name))
      continue;
    ++selected;
    struct utsname before, after;
    struct ns_identity original, final;
    if (uname(&before) || ns_path_identity("/proc/thread-self/ns/uts", &original))
      return 1;
    printf("UTS-NAMESPACE-CONTRACT: BEGIN %s\n", families[i].name);
    pid_t child = fork();
    if (!child) {
      alarm(40);
      int result;
      if (unshare(CLONE_NEWUTS)) {
        perror("family unshare");
        result = 1;
      } else {
        result = families[i].run();
      }
      fflush(stderr);
      _exit(result ? 1 : 0);
    }
    int status = child < 0 ? -1 : ns_reap(child, 45000);
    if (uname(&after) || memcmp(&before, &after, sizeof(before)) ||
        ns_path_identity("/proc/thread-self/ns/uts", &final) || !ns_same(original, final)) {
      fprintf(stderr, "supervisor namespace changed after %s\n", families[i].name);
      status = 1;
    }
    printf("UTS-NAMESPACE-CONTRACT: %s %s status=%d\n", status ? "FAIL" : "PASS", families[i].name,
           status);
    if (status)
      return 1;
  }
  if (!selected)
    return 2;
  puts("UTS-NAMESPACE-CONTRACT: END PASS");
  return 0;
}
