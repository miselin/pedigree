#define _GNU_SOURCE
#include <grp.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/fsuid.h>
#include <sys/mman.h>
#include <sys/prctl.h>

static int dumpable(void) {
  return prctl(PR_GET_DUMPABLE, 0UL, 0UL, 0UL, 0UL);
}
static int self(void) {
  int failed = 0;
  unsigned char source = 0x31, destination = 0;
  CHECK(!pm_dumpable(0) && dumpable() == 0);
  CHECK(pm_copy(getpid(), &destination, &source, 1, 0) == 1 && destination == 0x31);
  destination = 0x42;
  CHECK(pm_copy(getpid(), &destination, &source, 1, 1) == 1 && source == 0x42);
  errno = 0;
  CHECK(prctl(PR_SET_DUMPABLE, 2UL, 0UL, 0UL, 0UL) == -1 && errno == EINVAL);
  CHECK(dumpable() == 0 && !pm_dumpable(1) && dumpable() == 1);
out:
  return failed;
}
static int policy_target(int command, int report, void* ignored) {
  (void)ignored;
  int failed = 0;
  volatile unsigned char byte;
  uintptr_t address = (uintptr_t)&byte;
  for (int step = 0; step < 8; ++step) {
    CHECK(!setresuid(0, 0, 0) && !setresgid(0, 0, 0));
    switch (step) {
      case 2:
        CHECK(!setresuid(70001, -1, -1));
        break;
      case 3:
        CHECK(!setresuid(-1, 70001, -1));
        break;
      case 4:
        CHECK(!setresuid(-1, -1, 70001));
        break;
      case 5:
        CHECK(!setresgid(70002, -1, -1));
        break;
      case 6:
        CHECK(!setresgid(-1, 70002, -1));
        break;
      case 7:
        CHECK(!setresgid(-1, -1, 70002));
        break;
    }
    CHECK(!pm_dumpable(step != 1));
    byte = 0x4a;
    CHECK(!pm_write(report, &address, sizeof(address)) && !pm_receive(command, 'n'));
    CHECK(byte == (step == 0 ? 0x5b : 0x4a));
  }
out:
  return failed;
}
static int policy(void) {
  int failed = 0;
  struct pm_peer peer = PM_PEER_INITIALIZER;
  uintptr_t address;
  unsigned char byte;
  CHECK(getuid() == 0 && geteuid() == 0 && getgid() == 0 && getegid() == 0);
  CHECK(!pm_spawn(&peer, policy_target, NULL));
  for (int step = 0; step < 8; ++step) {
    CHECK(!pm_read(peer.report, &address, sizeof(address)));
    byte = 0x6c;
    if (!step) {
      CHECK(pm_copy(peer.pid, &byte, (void*)address, 1, 0) == 1 && byte == 0x4a);
      CHECK(setfsuid(70001) == 0 && setfsgid(70002) == 0);
      CHECK(!seteuid(70003) && getuid() == 0 && geteuid() == 70003);
      CHECK(pm_copy(peer.pid, &byte, (void*)address, 1, 0) == 1 && byte == 0x4a);
      CHECK(!seteuid(0) && setfsgid(0) == 70002);
      byte = 0x5b;
      CHECK(pm_copy(peer.pid, &byte, (void*)address, 1, 1) == 1);
    } else {
      errno = 0;
      CHECK(pm_copy(peer.pid, &byte, (void*)address, 1, 0) == -1 && errno == EPERM);
      CHECK(byte == 0x6c);
      errno = 0;
      CHECK(pm_copy(peer.pid, &byte, (void*)address, 1, 1) == -1 && errno == EPERM);
    }
    CHECK(!pm_send(peer.command, 'n'));
  }
  CHECK(!pm_join(&peer));
out:
  seteuid(0);
  setfsuid(0);
  setfsgid(0);
  pm_cleanup(&peer);
  return failed;
}
static int ids(uid_t r, uid_t e, uid_t s, gid_t gr, gid_t ge, gid_t gs) {
  uid_t actual_r, actual_e, actual_s;
  gid_t actual_gr, actual_ge, actual_gs;
  return getresuid(&actual_r, &actual_e, &actual_s) ||
         getresgid(&actual_gr, &actual_ge, &actual_gs) || actual_r != r || actual_e != e ||
         actual_s != s || actual_gr != gr || actual_ge != ge || actual_gs != gs;
}
static int transitions(void) {
  int failed = 0;
  pid_t child = -1;
  CHECK(!setresgid(70001, 70002, 70003) && !setresuid(71001, 71002, 71003));
  CHECK(!ids(71001, 71002, 71003, 70001, 70002, 70003) && dumpable() == 0);
  CHECK(setfsuid(-1) == 71002 && setfsgid(-1) == 70002);
  CHECK(!pm_dumpable(1) && setfsuid(71003) == 71002 && dumpable() == 0);
  errno = 0;
  CHECK(setfsuid(79999) == 71003 && errno == 0 && setfsuid(-1) == 71003);
  CHECK(!setresuid(-1, -1, -1) && setfsuid(-1) == 71003);
  CHECK(!setresuid(-1, 71002, -1) && setfsuid(-1) == 71002);
  CHECK(setfsgid(70003) == 70002);
  errno = 0;
  CHECK(setfsgid(79999) == 70003 && errno == 0 && setfsgid(-1) == 70003);
  CHECK(!setresgid(-1, 70002, -1) && setfsgid(-1) == 70002);
  errno = 0;
  CHECK(setresuid(71002, 79999, -1) == -1 && errno == EPERM);
  CHECK(!ids(71001, 71002, 71003, 70001, 70002, 70003));
  errno = 0;
  CHECK(setresgid(70002, 79999, -1) == -1 && errno == EPERM);
  CHECK(!ids(71001, 71002, 71003, 70001, 70002, 70003));
  CHECK(!pm_dumpable(1));
  execl("/applications/process-memory-missing", "missing", (char*)NULL);
  CHECK(errno == ENOENT && dumpable() == 1 && !ids(71001, 71002, 71003, 70001, 70002, 70003));
  child = fork();
  CHECK(child >= 0);
  if (!child) {
    if (ids(71001, 71002, 71003, 70001, 70002, 70003) || dumpable() != 1)
      _exit(10);
    execl(PM_APP, PM_APP, "fs-exec", "71001", "71002", "70001", "70002", "0", (char*)NULL);
    _exit(11);
  }
  int status = pm_reap(child, 10000);
  child = -1;
  CHECK(status == 0);
  CHECK(!setuid(71001) && !setgid(70001));
  CHECK(!ids(71001, 71001, 71003, 70001, 70001, 70003));
  errno = 0;
  CHECK(setuid(79999) == -1 && errno == EPERM);
  errno = 0;
  CHECK(setgid(79999) == -1 && errno == EPERM);
  CHECK(!ids(71001, 71001, 71003, 70001, 70001, 70003));
out:
  if (child > 0)
    pm_reap(child, 100);
  return failed;
}
static int groups(void) {
  int failed = 0;
  gid_t chosen[] = {70003, 70001, 70003}, actual[3], invalid = (gid_t)-1;
  void* bad = mmap(NULL, pm_page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(bad != MAP_FAILED && !setgroups(3, chosen));
  CHECK(getgroups(0, NULL) == 3 && getgroups(3, actual) == 3);
  CHECK(actual[0] == 70001 && actual[1] == 70003 && actual[2] == 70003);
  errno = 0;
  CHECK(setgroups(33, NULL) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(setgroups(1, bad) == -1 && errno == EFAULT);
  errno = 0;
  CHECK(setgroups(1, &invalid) == -1 && errno == EINVAL);
  CHECK(getgroups(3, actual) == 3 && actual[0] == 70001 && actual[2] == 70003);
  CHECK(!setuid(71001));
  errno = 0;
  CHECK(setgroups(0, NULL) == -1 && errno == EPERM);
  CHECK(getgroups(0, NULL) == 3);
out:
  if (bad != MAP_FAILED)
    munmap(bad, pm_page);
  return failed;
}
int pm_credentials(void) {
  return self() || policy() || pm_isolate(transitions) || pm_isolate(groups);
}
