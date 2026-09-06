#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>
#include <sys/resource.h>

struct shared_wait {
  int command;
  int report;
  void* page;
};

static void* change_shared_limit(void* argument) {
  struct shared_wait* context = argument;
  int failed = 0;
  struct rlimit limit;
  CHECK(ml_receive(context->command, 'G') == 0);
  CHECK(getrlimit(RLIMIT_MEMLOCK, &limit) == 0 && limit.rlim_cur == 2 * ml_page);
  CHECK(mlock(context->page, ml_page) == 0);
  CHECK(ml_limit(ml_page) == 0 && ml_send(context->report, 'C') == 0);
  CHECK(ml_receive(context->command, 'R') == 0);
  CHECK(munlock(context->page, ml_page) == 0);
out:
  if (failed)
    ml_send(context->report, 'E');
  return (void*)(uintptr_t)failed;
}

static int shared_threads(void) {
  int failed = 0;
  int command[2] = {-1, -1};
  int report[2] = {-1, -1};
  pthread_t thread;
  int thread_live = 0;
  unsigned char* pages = MAP_FAILED;
  struct shared_wait context;
  void* result = NULL;
  struct rlimit limit;
  CHECK(ml_limit(2 * ml_page) == 0 && ml_unprivileged() == 0);
  CHECK(pipe(command) == 0 && pipe(report) == 0);
  pages = mmap(NULL, 3 * ml_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(pages != MAP_FAILED);
  context = (struct shared_wait){command[0], report[1], pages + ml_page};
  CHECK(pthread_create(&thread, NULL, change_shared_limit, &context) == 0);
  thread_live = 1;
  CHECK(mlock(pages, ml_page) == 0 && ml_send(command[1], 'G') == 0);
  CHECK(ml_receive(report[0], 'C') == 0);
  CHECK(getrlimit(RLIMIT_MEMLOCK, &limit) == 0 && limit.rlim_cur == ml_page);
  errno = 0;
  CHECK(madvise(pages, 2 * ml_page, MADV_DONTNEED) == -1 && errno == EINVAL);
  CHECK(munlock(pages, ml_page) == 0);
  errno = 0;
  CHECK(mlock(pages + 2 * ml_page, ml_page) == -1 && errno == ENOMEM);
  CHECK(ml_send(command[1], 'R') == 0);
  CHECK(pthread_join(thread, &result) == 0);
  thread_live = 0;
  CHECK(result == NULL && mlock(pages + 2 * ml_page, ml_page) == 0);
out:
  if (thread_live) {
    ml_send(command[1], 'R');
    pthread_join(thread, NULL);
  }
  munlockall();
  for (int n = 0; n < 2; ++n) {
    if (command[n] >= 0)
      close(command[n]);
    if (report[n] >= 0)
      close(report[n]);
  }
  if (pages != MAP_FAILED)
    munmap(pages, 3 * ml_page);
  return failed;
}

static void* leave_locked_stack(void* argument) {
  (void)argument;
  volatile unsigned char storage[2 * ml_page];
  void* aligned = (void*)(((uintptr_t)storage + ml_page - 1) & ~(uintptr_t)(ml_page - 1));
  storage[0] = 0x65;
  return (void*)(uintptr_t)(mlock(aligned, ml_page) != 0);
}

static int thread_retirement(void) {
  int failed = 0;
  int live = 0;
  pthread_t thread;
  void* result = NULL;
  void* page = MAP_FAILED;
  CHECK(ml_limit(ml_page) == 0);
  page = mmap(NULL, ml_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(page != MAP_FAILED && pthread_create(&thread, NULL, leave_locked_stack, NULL) == 0);
  live = 1;
  CHECK(pthread_join(thread, &result) == 0);
  live = 0;
  CHECK(result == NULL && mlock(page, ml_page) == 0);
out:
  if (live)
    pthread_join(thread, NULL);
  munlockall();
  if (page != MAP_FAILED)
    munmap(page, ml_page);
  return failed;
}

static int fork_child(unsigned char* inherited) {
  int failed = 0;
  struct rlimit limit;
  void* fresh = MAP_FAILED;
  CHECK(getrlimit(RLIMIT_MEMLOCK, &limit) == 0 && limit.rlim_cur == 2 * ml_page);
  CHECK(madvise(inherited, 2 * ml_page, MADV_DONTNEED) == 0 && inherited[0] == 0);
  fresh = mmap(NULL, 3 * ml_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(fresh != MAP_FAILED && mlock(fresh, 2 * ml_page) == 0);
  errno = 0;
  CHECK(mlock((unsigned char*)fresh + 2 * ml_page, ml_page) == -1 && errno == ENOMEM);
  inherited[0] = 0x77;
out:
  munlockall();
  if (fresh != MAP_FAILED)
    munmap(fresh, 3 * ml_page);
  return failed;
}

static int fork_policy(void) {
  int failed = 0;
  pid_t child = -1;
  unsigned char* pages = MAP_FAILED;
  void* extra = MAP_FAILED;
  CHECK(ml_limit(2 * ml_page) == 0);
  pages = mmap(NULL, 2 * ml_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  extra = mmap(NULL, ml_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(pages != MAP_FAILED && extra != MAP_FAILED);
  pages[0] = 0x31;
  CHECK(mlock(pages, 2 * ml_page) == 0 && mlockall(MCL_FUTURE | MCL_ONFAULT) == 0);
  child = fork();
  CHECK(child >= 0);
  if (!child) {
    alarm(8);
    _exit(fork_child(pages));
  }
  int status = ml_reap(child, 10000);
  child = -1;
  CHECK(status == 0 && pages[0] == 0x31);
  errno = 0;
  CHECK(madvise(pages, 2 * ml_page, MADV_DONTNEED) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(mlock(extra, ml_page) == -1 && errno == ENOMEM);
out:
  if (child > 0) {
    kill(child, SIGKILL);
    ml_reap(child, 2000);
  }
  munlockall();
  if (extra != MAP_FAILED)
    munmap(extra, ml_page);
  if (pages != MAP_FAILED)
    munmap(pages, 2 * ml_page);
  return failed;
}

int ml_exec(int argc, char** argv) {
  (void)argv;
  int failed = 0;
  struct rlimit limit;
  void* pages = MAP_FAILED;
  alarm(8);
  CHECK(argc == 2 && geteuid() != 0);
  CHECK(getrlimit(RLIMIT_MEMLOCK, &limit) == 0 && limit.rlim_cur == 2 * ml_page &&
        limit.rlim_max == 16 * 1024 * 1024);
  pages = mmap(NULL, 3 * ml_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(pages != MAP_FAILED && mlock(pages, 2 * ml_page) == 0);
  errno = 0;
  CHECK(mlock((unsigned char*)pages + 2 * ml_page, ml_page) == -1 && errno == ENOMEM);
out:
  munlockall();
  if (pages != MAP_FAILED)
    munmap(pages, 3 * ml_page);
  printf("MEMORY-LOCK-CONTRACT: EXEC %s\n", failed ? "FAIL" : "PASS");
  return failed;
}

static int exec_policy(void) {
  pid_t child = fork();
  if (child < 0)
    return 1;
  if (!child) {
    alarm(8);
    void* pages =
        mmap(NULL, 2 * ml_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pages == MAP_FAILED || ml_limit(2 * ml_page) || mlock(pages, 2 * ml_page) ||
        mlockall(MCL_FUTURE | MCL_ONFAULT))
      _exit(2);
    execl("/applications/memory-lock-contract-test", "memory-lock-contract-test",
          "memory-lock-exec", NULL);
    _exit(3);
  }
  int result = ml_reap(child, 10000);
  if (result)
    fprintf(stderr, "MEMORY-LOCK-CONTRACT: exec child status=%d\n", result);
  return result != 0;
}

int ml_lifetime(void) {
  return shared_threads() || thread_retirement() || fork_policy() || exec_policy();
}
