#define _GNU_SOURCE
#include <limits.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>

typedef ssize_t (*copy_fn)(pid_t, const struct iovec*, unsigned long, const struct iovec*,
                           unsigned long, unsigned long);

static int scatter_target(int command, int report, void* argument) {
  (void)argument;
  int failed = 0;
  unsigned char bytes[64];
  for (size_t n = 0; n < sizeof(bytes); ++n)
    bytes[n] = (unsigned char)(0x20 + n);
  uintptr_t address = (uintptr_t)bytes;
  CHECK(!pm_dumpable(1) && !pm_write(report, &address, sizeof(address)));
  CHECK(!pm_receive(command, 'q'));
  for (int n = 0; n < 5; ++n)
    CHECK(bytes[3 + n] == (unsigned char)('A' + n));
  for (int n = 0; n < 15; ++n)
    CHECK(bytes[32 + n] == (unsigned char)('F' + n));
  CHECK(bytes[2] == 0x22 && bytes[8] == 0x28 && bytes[47] == 0x4f);
out:
  return failed;
}
static int scatter(void) {
  int failed = 0;
  struct pm_peer peer = PM_PEER_INITIALIZER;
  uintptr_t address;
  unsigned char bytes[32], payload[20];
  CHECK(!pm_spawn(&peer, scatter_target, NULL));
  CHECK(!pm_read(peer.report, &address, sizeof(address)));
  memset(bytes, 0, sizeof(bytes));
  struct iovec local[] = {{NULL, 0}, {bytes, 7}, {bytes + 7, 13}};
  struct iovec remote[] = {{(void*)(address + 3), 5}, {NULL, 0}, {(void*)(address + 32), 15}};
  CHECK(process_vm_readv(peer.pid, local, 3, remote, 3, 0) == 20);
  for (int n = 0; n < 5; ++n)
    CHECK(bytes[n] == 0x23 + n);
  for (int n = 0; n < 15; ++n)
    CHECK(bytes[5 + n] == 0x40 + n);
  for (int n = 0; n < 20; ++n)
    payload[n] = (unsigned char)('A' + n);
  struct iovec input[] = {{payload, 8}, {NULL, 0}, {payload + 8, 12}};
  CHECK(process_vm_writev(peer.pid, input, 3, remote, 3, 0) == 20);
  CHECK(process_vm_readv(peer.pid, local, 3, remote, 3, 0) == 20 && !memcmp(bytes, payload, 20));
  CHECK(!pm_send(peer.command, 'q') && !pm_join(&peer));
out:
  pm_cleanup(&peer);
  return failed;
}
static int admission(void) {
  int failed = 0;
  unsigned char source[16] = "cooperative", destination[16] = {0};
  struct iovec local = {destination, sizeof(source)}, remote = {source, sizeof(source)};
  struct iovec empty = {NULL, 0}, huge = {source, (size_t)SSIZE_MAX + 1};
  void* bad = mmap(NULL, pm_page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(bad != MAP_FAILED);
  copy_fn functions[] = {process_vm_readv, process_vm_writev};
  for (size_t n = 0; n < 2; ++n) {
    copy_fn copy = functions[n];
    errno = 0;
    CHECK(copy(0, NULL, 0, NULL, 0, 1) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(copy(getpid(), &local, 1025, &remote, 1, 0) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(copy(getpid(), bad, 1, &remote, 1, 0) == -1 && errno == EFAULT);
    errno = 0;
    CHECK(copy(getpid(), &huge, 1, &remote, 1, 0) == -1 && errno == EINVAL);
    CHECK(copy(-1, NULL, 0, bad, ULONG_MAX, 0) == 0);
    CHECK(copy(0, &empty, 1, bad, 1025, 0) == 0);
    errno = 0;
    CHECK(copy(getpid(), &local, 1, &remote, 1025, 0) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(copy(0, &local, 1, bad, 1, 0) == -1 && errno == EFAULT);
    errno = 0;
    CHECK(copy(getpid(), &local, 1, &huge, 1, 0) == -1 && errno == EINVAL);
    CHECK(copy(-1, &local, 1, &empty, 1, 0) == 0);
    errno = 0;
    CHECK(copy(0, &local, 1, &remote, 1, 0) == -1 && errno == ESRCH);
    struct iovec inaccessible = {bad, 1};
    errno = 0;
    CHECK(copy(0, &inaccessible, 1, &remote, 1, 0) == -1 && errno == ESRCH);
  }
  CHECK(process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == sizeof(source));
  CHECK(!memcmp(source, destination, sizeof(source)));
  memset(destination, 0x64, sizeof(destination));
  CHECK(process_vm_writev(getpid(), &local, 1, &remote, 1, 0) == sizeof(source));
  CHECK(!memcmp(source, destination, sizeof(source)));
out:
  if (bad != MAP_FAILED)
    munmap(bad, pm_page);
  return failed;
}
static int partial_pages(void) {
  int failed = 0;
  unsigned char *local = MAP_FAILED, *remote = MAP_FAILED;
  local = mmap(NULL, 2 * pm_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  remote = mmap(NULL, 2 * pm_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(local != MAP_FAILED && remote != MAP_FAILED);
  memset(local, 0x35, 2 * pm_page);
  memset(remote, 0x67, 2 * pm_page);
  CHECK(!mprotect(remote + pm_page, pm_page, PROT_NONE));
  CHECK(pm_copy(getpid(), local, remote, 2 * pm_page, 1) == (ssize_t)pm_page);
  CHECK(remote[0] == 0x35 && remote[pm_page - 1] == 0x35);
  memset(local, 0x89, 2 * pm_page);
  CHECK(pm_copy(getpid(), local, remote, 2 * pm_page, 0) == (ssize_t)pm_page);
  CHECK(local[0] == 0x35 && local[pm_page - 1] == 0x35 && local[pm_page] == 0x89);
  CHECK(!mprotect(remote + pm_page, pm_page, PROT_READ | PROT_WRITE));
  CHECK(remote[pm_page] == 0x67);
  CHECK(!mprotect(local + pm_page, pm_page, PROT_NONE));
  memset(remote, 0xab, pm_page);
  CHECK(pm_copy(getpid(), local, remote, 2 * pm_page, 0) == (ssize_t)pm_page);
  CHECK(local[0] == 0xab && local[pm_page - 1] == 0xab);
  memset(local, 0xcd, pm_page);
  CHECK(pm_copy(getpid(), local, remote, 2 * pm_page, 1) == (ssize_t)pm_page);
  CHECK(remote[0] == 0xcd && remote[pm_page - 1] == 0xcd && remote[pm_page] == 0x67);
  errno = 0;
  CHECK(pm_copy(getpid(), local + pm_page, remote, 1, 1) == -1 && errno == EFAULT);
out:
  if (remote != MAP_FAILED)
    munmap(remote, 2 * pm_page);
  if (local != MAP_FAILED)
    munmap(local, 2 * pm_page);
  return failed;
}
int pm_copies(void) {
  return admission() || scatter() || partial_pages();
}
