#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/fsuid.h>
#include <sys/mman.h>
#include <sys/prctl.h>

struct task_view {
  int command[2], report[2];
  int held, result;
};
struct view_report {
  pid_t tid;
  struct ns_identity self, caller;
};
static void* task_view_worker(void* argument) {
  struct task_view* state = argument;
  struct view_report report = {.tid = gettid()};
  state->result = 1;
  if (unshare(CLONE_NEWUTS) || ns_set("proc-worker", "proc-worker") ||
      ns_path_identity("/proc/self/ns/uts", &report.self) ||
      ns_path_identity("/proc/thread-self/ns/uts", &report.caller))
    return NULL;
  state->held = open("/proc/thread-self/ns/uts", O_RDONLY | O_CLOEXEC);
  if (state->held < 0 || ns_write(state->report[1], &report, sizeof(report)) ||
      ns_receive(state->command[0], 'Q'))
    return NULL;
  state->result = 0;
  return NULL;
}

static int proc_views(void) {
  int failed = 0, started = 0, joined = 0, saved = -1, old_view = -1;
  pthread_t worker;
  struct task_view state = {.command = {-1, -1}, .report = {-1, -1}, .held = -1};
  struct view_report report;
  struct ns_identity leader, path, held;
  char worker_path[96], leader_path[96], ns_directory[96], link[80];
  DIR* directory = NULL;
  void* readonly = MAP_FAILED;
  CHECK(ns_set("proc-leader", "proc-leader") == 0);
  saved = open("/proc/thread-self/ns/uts", O_RDONLY | O_CLOEXEC);
  CHECK(saved >= 0 && ns_fd_identity(saved, &leader) == 0);
  snprintf(leader_path, sizeof(leader_path), "/proc/%d/ns/uts", getpid());
  CHECK(ns_path_identity(leader_path, &path) == 0 && ns_same(path, leader));
  snprintf(ns_directory, sizeof(ns_directory), "/proc/%d/ns", getpid());
  directory = opendir(ns_directory);
  CHECK(directory != NULL);
  int found = 0;
  for (struct dirent* entry; (entry = readdir(directory));)
    found += !strcmp(entry->d_name, "uts");
  CHECK(found == 1);
  closedir(directory);
  directory = NULL;
  CHECK(pipe(state.command) == 0 && pipe(state.report) == 0);
  CHECK(pthread_create(&worker, NULL, task_view_worker, &state) == 0);
  started = 1;
  CHECK(ns_read(state.report[0], &report, sizeof(report)) == 0);
  CHECK(ns_same(report.self, leader) && !ns_same(report.caller, leader));
  snprintf(worker_path, sizeof(worker_path), "/proc/%d/task/%d/ns/uts", getpid(), report.tid);
  CHECK(ns_path_identity(worker_path, &path) == 0 && ns_same(path, report.caller));
  snprintf(ns_directory, sizeof(ns_directory), "/proc/%d/task/%d/ns", getpid(), report.tid);
  old_view = open(ns_directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  CHECK(old_view >= 0);
  memset(link, '?', sizeof(link));
  ssize_t count = readlink(worker_path, link, sizeof(link) - 1);
  CHECK(count > 6 && count < (ssize_t)sizeof(link));
  link[count] = 0;
  CHECK(!strncmp(link, "uts:[", 5) && link[count - 1] == ']');
  char short_link[4] = {'?', '?', '?', '?'};
  CHECK(readlink(worker_path, short_link, 3) == 3 && !memcmp(short_link, "uts", 3) &&
        short_link[3] == '?');
  readonly = mmap(NULL, ns_page, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(readonly != MAP_FAILED);
  errno = 0;
  CHECK(readlink(worker_path, readonly, 20) == -1 && errno == EFAULT);
  CHECK(ns_expect("proc-leader", "proc-leader") == 0);
  CHECK(ns_send(state.command[1], 'Q') == 0);
  CHECK(pthread_join(worker, NULL) == 0);
  joined = 1;
  CHECK(state.result == 0);
  CHECK(ns_fd_identity(state.held, &held) == 0 && ns_same(held, report.caller));
  CHECK(setns(state.held, 0) == 0 && ns_expect("proc-worker", "proc-worker") == 0);
  CHECK(ns_path_identity("/proc/thread-self/ns/uts", &path) == 0 && ns_same(path, held));
  CHECK(ns_fd_identity(saved, &path) == 0 && ns_same(path, leader));
  CHECK(setns(saved, CLONE_NEWUTS) == 0 && ns_expect("proc-leader", "proc-leader") == 0);
  int stale = openat(old_view, "uts", O_RDONLY | O_CLOEXEC);
  if (stale >= 0)
    close(stale);
  CHECK(stale < 0);
out:
  if (failed && state.command[1] >= 0) {
    close(state.command[1]);
    state.command[1] = -1;
  }
  if (started && !joined)
    pthread_join(worker, NULL);
  for (int i = 0; i < 2; ++i) {
    if (state.command[i] >= 0)
      close(state.command[i]);
    if (state.report[i] >= 0)
      close(state.report[i]);
  }
  if (state.held >= 0)
    close(state.held);
  if (old_view >= 0)
    close(old_view);
  if (saved >= 0) {
    if (setns(saved, 0))
      failed = 1;
    close(saved);
  }
  if (directory)
    closedir(directory);
  if (readonly != MAP_FAILED)
    munmap(readonly, ns_page);
  return failed;
}

static int credential_target(int command, int report, void* argument) {
  (void)argument;
  if (unshare(CLONE_NEWUTS) || setgroups(0, NULL) || setresgid(1002, 1002, 1002) ||
      setresuid(1001, 1001, 1001) || prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) || ns_send(report, 'R') ||
      ns_receive(command, 'D') || prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) || ns_send(report, 'D') ||
      ns_receive(command, 'Q'))
    return 1;
  return 0;
}

static int proc_permissions(void) {
  int failed = 0, fd = -1;
  struct ns_peer child = NS_PEER_INITIALIZER;
  char path[80], text[80];
  CHECK(ns_spawn(&child, credential_target, NULL) == 0 && ns_receive(child.report, 'R') == 0);
  snprintf(path, sizeof(path), "/proc/%d/ns/uts", child.pid);
  errno = 0;
  CHECK(readlink(path, text, sizeof(text)) == -1 && errno == EACCES);
  setfsuid(1001);
  setfsgid(1002);
  CHECK(setfsuid((uid_t)-1) == 1001 && setfsgid((gid_t)-1) == 1002);
  CHECK(getuid() == 0 && geteuid() == 0);
  CHECK(readlink(path, text, sizeof(text)) > 0);
  fd = open(path, O_RDONLY | O_CLOEXEC);
  CHECK(fd >= 0);
  CHECK(ns_send(child.command, 'D') == 0 && ns_receive(child.report, 'D') == 0);
  errno = 0;
  CHECK(readlink(path, text, sizeof(text)) == -1 && errno == EACCES);
  CHECK(readlink("/proc/thread-self/ns/uts", text, sizeof(text)) > 0);
  CHECK(ns_send(child.command, 'Q') == 0 && ns_join(&child) == 0);
  struct ns_identity retained;
  CHECK(ns_fd_identity(fd, &retained) == 0);
out:
  setfsuid(0);
  setfsgid(0);
  if (setfsuid((uid_t)-1) != 0 || setfsgid((gid_t)-1) != 0)
    failed = 1;
  if (fd >= 0)
    close(fd);
  ns_cleanup(&child);
  return failed;
}

int ns_proc(void) {
  return proc_views() || proc_permissions();
}
