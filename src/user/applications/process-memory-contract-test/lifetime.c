#define _GNU_SOURCE
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>

struct thread_arguments {
  int command, report;
};
struct thread_address {
  pid_t tid;
  uintptr_t address;
};

static void* target_thread(void* opaque) {
  struct thread_arguments* args = opaque;
  volatile unsigned char bytes[16] = {0x43};
  struct thread_address address = {gettid(), (uintptr_t)bytes};
  int failed = pm_write(args->report, &address, sizeof(address)) ||
               pm_receive(args->command, 't') || bytes[0] != 0x54;
  return (void*)(uintptr_t)failed;
}
static int thread_target(int command, int report, void* ignored) {
  (void)ignored;
  int failed = 0, live = 0;
  pthread_t thread;
  void* result;
  volatile unsigned char main_byte = 0x65;
  uintptr_t main_address = (uintptr_t)&main_byte;
  struct thread_arguments args = {command, report};
  CHECK(!pm_dumpable(1) && !pm_write(report, &main_address, sizeof(main_address)));
  CHECK(!pthread_create(&thread, NULL, target_thread, &args));
  live = 1;
  CHECK(!pthread_join(thread, &result));
  live = 0;
  CHECK(result == NULL && !pm_send(report, 'd'));
  CHECK(!pm_receive(command, 'q'));
out:
  if (live) {
    /* Failure ends the isolated process, including the cooperating thread. */
    return 1;
  }
  return failed;
}
static int nonleader(void) {
  int failed = 0;
  struct pm_peer peer = PM_PEER_INITIALIZER;
  struct thread_address address;
  uintptr_t main_address;
  unsigned char byte;
  CHECK(!pm_spawn(&peer, thread_target, NULL));
  CHECK(!pm_read(peer.report, &main_address, sizeof(main_address)));
  CHECK(!pm_read(peer.report, &address, sizeof(address)));
  CHECK(address.tid > 0 && address.tid != peer.pid);
  CHECK(pm_copy(address.tid, &byte, (void*)address.address, 1, 0) == 1 && byte == 0x43);
  byte = 0x54;
  CHECK(pm_copy(address.tid, &byte, (void*)address.address, 1, 1) == 1);
  CHECK(!pm_send(peer.command, 't') && !pm_receive(peer.report, 'd'));
  errno = 0;
  CHECK(pm_copy(address.tid, &byte, (void*)address.address, 1, 0) == -1 && errno == ESRCH);
  CHECK(pm_copy(peer.pid, &byte, (void*)main_address, 1, 0) == 1 && byte == 0x65);
  CHECK(!pm_send(peer.command, 'q') && !pm_join(&peer));
out:
  pm_cleanup(&peer);
  return failed;
}
static int image_target(int command, int report, void* ignored) {
  (void)ignored;
  int failed = 0;
  unsigned char* bytes =
      mmap(NULL, 3 * pm_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(bytes != MAP_FAILED);
  memset(bytes, 0x71, pm_page);
  memset(bytes + pm_page, 0x82, pm_page);
  memset(bytes + 2 * pm_page, 0x93, pm_page);
  uintptr_t address = (uintptr_t)bytes;
  CHECK(!pm_dumpable(1) && !pm_write(report, &address, sizeof(address)));
  CHECK(!pm_receive(command, 'u') && !munmap(bytes + pm_page, pm_page));
  CHECK(!pm_send(report, 'u') && !pm_receive(command, 'x'));
  char input[24], output[24];
  snprintf(input, sizeof(input), "%d", command);
  snprintf(output, sizeof(output), "%d", report);
  execl(PM_APP, PM_APP, "memory-exec", input, output, (char*)NULL);
  CHECK(0);
out:
  if (bytes != MAP_FAILED)
    munmap(bytes, 3 * pm_page);
  return failed;
}
int pm_exec(int argc, char** argv) {
  if (argc != 4)
    return 2;
  int failed = 0, command = atoi(argv[2]), report = atoi(argv[3]);
  unsigned char* bytes =
      mmap(NULL, pm_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(bytes != MAP_FAILED);
  memset(bytes, 0xe1, pm_page);
  uintptr_t address = (uintptr_t)bytes;
  CHECK(!pm_write(report, &address, sizeof(address)) && !pm_receive(command, 'q'));
  CHECK(bytes[0] == 0xe2 && bytes[1] == 0xe1);
out:
  if (bytes != MAP_FAILED)
    munmap(bytes, pm_page);
  return failed;
}
static int images(void) {
  int failed = 0;
  struct pm_peer peer = PM_PEER_INITIALIZER;
  uintptr_t address, replacement;
  unsigned char* local =
      mmap(NULL, 3 * pm_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(local != MAP_FAILED && !pm_spawn(&peer, image_target, NULL));
  CHECK(!pm_read(peer.report, &address, sizeof(address)));
  CHECK(pm_copy(peer.pid, local, (void*)address, 3 * pm_page, 0) == (ssize_t)(3 * pm_page));
  CHECK(local[0] == 0x71 && local[pm_page] == 0x82 && local[2 * pm_page] == 0x93);
  CHECK(!pm_send(peer.command, 'u') && !pm_receive(peer.report, 'u'));
  memset(local, 0xa4, 3 * pm_page);
  CHECK(pm_copy(peer.pid, local, (void*)address, 3 * pm_page, 0) == (ssize_t)pm_page);
  CHECK(local[0] == 0x71 && local[pm_page] == 0xa4 && local[2 * pm_page] == 0xa4);
  errno = 0;
  CHECK(pm_copy(peer.pid, local, (void*)(address + pm_page), 1, 1) == -1 && errno == EFAULT);
  CHECK(pm_copy(peer.pid, local, (void*)(address + 2 * pm_page), 1, 0) == 1 && local[0] == 0x93);
  CHECK(!pm_send(peer.command, 'x') && !pm_read(peer.report, &replacement, sizeof(replacement)));
  CHECK(pm_copy(peer.pid, local, (void*)replacement, pm_page, 0) == (ssize_t)pm_page);
  for (size_t n = 0; n < pm_page; ++n)
    CHECK(local[n] == 0xe1);
  local[0] = 0xe2;
  CHECK(pm_copy(peer.pid, local, (void*)replacement, 1, 1) == 1);
  pid_t departed = peer.pid;
  CHECK(!pm_send(peer.command, 'q') && !pm_join(&peer));
  errno = 0;
  CHECK(pm_copy(departed, local, (void*)replacement, 1, 0) == -1 && errno == ESRCH);
out:
  pm_cleanup(&peer);
  if (local != MAP_FAILED)
    munmap(local, 3 * pm_page);
  return failed;
}
int pm_lifetime(void) {
  return nonleader() || images();
}
