#define _GNU_SOURCE
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>

struct map_arguments {
  int fd;
  unsigned char* cow;
};
struct map_addresses {
  uintptr_t anonymous, shared, private, cow, stack, heap;
};
static unsigned char pattern(size_t n) {
  return (unsigned char)(n % 251 + 1);
}

static int mapping_target(int command, int report, void* argument) {
  struct map_arguments* args = argument;
  int failed = 0;
  unsigned char *anonymous = MAP_FAILED, *shared = MAP_FAILED, *private = MAP_FAILED;
  volatile unsigned char stack[32] = {0x27};
  anonymous = mmap(NULL, 3 * pm_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  shared = mmap(NULL, 3 * pm_page, PROT_READ | PROT_WRITE, MAP_SHARED, args->fd, 0);
  private = mmap(NULL, 3 * pm_page, PROT_READ | PROT_WRITE, MAP_PRIVATE, args->fd, 0);
  CHECK(anonymous != MAP_FAILED && shared != MAP_FAILED && private != MAP_FAILED);
  uintptr_t original = (uintptr_t)syscall(SYS_brk, 0);
  CHECK(original && original != UINTPTR_MAX && original < UINTPTR_MAX - 2 * pm_page);
  uintptr_t heap = (original + pm_page - 1) & ~(uintptr_t)(pm_page - 1);
  CHECK((uintptr_t)syscall(SYS_brk, heap + pm_page) == heap + pm_page);
  *(volatile unsigned char*)heap = 0x38;
  struct map_addresses addresses = {(uintptr_t)anonymous, (uintptr_t)shared, (uintptr_t)private,
                                    (uintptr_t)args->cow, (uintptr_t)stack,  heap};
  CHECK(!pm_dumpable(1) && !pm_write(report, &addresses, sizeof(addresses)));
  CHECK(!pm_receive(command, 'w'));
  CHECK(anonymous[0] == 0x31 && stack[0] == 0x42 && *(volatile unsigned char*)heap == 0x53);
  CHECK(args->cow[0] == 0x64 && private[7] == 0x75 && shared[11] == 0x86);
  CHECK(private[11] == pattern(11) && shared[7] == pattern(7));
  CHECK(!msync(shared, 3 * pm_page, MS_SYNC) && !fsync(args->fd));
  CHECK(!pm_send(report, 's'));
  CHECK(!pm_receive(command, 'p'));
  CHECK(!mprotect(anonymous + pm_page, pm_page, PROT_NONE));
  CHECK(!mprotect(private, 3 * pm_page, PROT_READ));
  CHECK(!pm_send(report, 'p'));
  CHECK(!pm_receive(command, 'e'));
  CHECK(!ftruncate(args->fd, pm_page + 137));
  CHECK(!pm_send(report, 'e') && !pm_receive(command, 'q'));
out:
  if (private != MAP_FAILED)
    munmap(private, 3 * pm_page);
  if (shared != MAP_FAILED)
    munmap(shared, 3 * pm_page);
  if (anonymous != MAP_FAILED)
    munmap(anonymous, 3 * pm_page);
  /* The bounded raw heap belongs to this child and retires with its image. */
  return failed;
}
static int backend(int disk) {
  int failed = 0, fd = -1;
  char path[128] = {0};
  struct pm_peer peer = PM_PEER_INITIALIZER;
  struct map_addresses addresses;
  unsigned char *buffer = MAP_FAILED, *cow = MAP_FAILED;
  buffer = mmap(NULL, 3 * pm_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  cow = mmap(NULL, pm_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(buffer != MAP_FAILED && cow != MAP_FAILED);
  memset(cow, 0x19, pm_page);
  if (disk) {
    snprintf(path, sizeof(path), "/process-memory-%d", getpid());
    fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
  } else
    fd = memfd_create("process-memory", MFD_CLOEXEC);
  CHECK(fd >= 0);
  for (size_t n = 0; n < 3 * pm_page; ++n)
    buffer[n] = pattern(n);
  CHECK(!pm_write(fd, buffer, 3 * pm_page));
  struct map_arguments arguments = {fd, cow};
  CHECK(!pm_spawn(&peer, mapping_target, &arguments));
  CHECK(!pm_read(peer.report, &addresses, sizeof(addresses)));
  memset(buffer, 0xff, 3 * pm_page);
  CHECK(pm_copy(peer.pid, buffer, (void*)addresses.anonymous, 3 * pm_page, 0) ==
        (ssize_t)(3 * pm_page));
  for (size_t n = 0; n < 3 * pm_page; ++n)
    CHECK(buffer[n] == 0);
  CHECK(pm_copy(peer.pid, buffer, (void*)addresses.private, pm_page, 0) == (ssize_t)pm_page);
  for (size_t n = 0; n < pm_page; ++n)
    CHECK(buffer[n] == pattern(n));
  unsigned char byte = 0;
  CHECK(pm_copy(peer.pid, &byte, (void*)addresses.stack, 1, 0) == 1 && byte == 0x27);
  CHECK(pm_copy(peer.pid, &byte, (void*)addresses.heap, 1, 0) == 1 && byte == 0x38);
  CHECK(pm_copy(peer.pid, &byte, (void*)addresses.cow, 1, 0) == 1 && byte == 0x19);
  byte = 0x31;
  CHECK(pm_copy(peer.pid, &byte, (void*)addresses.anonymous, 1, 1) == 1);
  byte = 0x42;
  CHECK(pm_copy(peer.pid, &byte, (void*)addresses.stack, 1, 1) == 1);
  byte = 0x53;
  CHECK(pm_copy(peer.pid, &byte, (void*)addresses.heap, 1, 1) == 1);
  byte = 0x64;
  CHECK(pm_copy(peer.pid, &byte, (void*)addresses.cow, 1, 1) == 1);
  byte = 0x75;
  CHECK(pm_copy(peer.pid, &byte, (void*)(addresses.private + 7), 1, 1) == 1);
  byte = 0x86;
  CHECK(pm_copy(peer.pid, &byte, (void*)(addresses.shared + 11), 1, 1) == 1);
  CHECK(cow[0] == 0x19 && cow[pm_page - 1] == 0x19);
  CHECK(!pm_send(peer.command, 'w') && !pm_receive(peer.report, 's'));
  CHECK(pread(fd, &byte, 1, 7) == 1 && byte == pattern(7));
  CHECK(pread(fd, &byte, 1, 11) == 1 && byte == 0x86);
  CHECK(!pm_send(peer.command, 'p') && !pm_receive(peer.report, 'p'));
  errno = 0;
  CHECK(pm_copy(peer.pid, &byte, (void*)addresses.private, 1, 1) == -1 && errno == EFAULT);
  CHECK(pm_copy(peer.pid, &byte, (void*)(addresses.private + 7), 1, 0) == 1 && byte == 0x75);
  memset(buffer, 0x91, 3 * pm_page);
  CHECK(pm_copy(peer.pid, buffer, (void*)addresses.anonymous, 3 * pm_page, 0) == (ssize_t)pm_page);
  CHECK(buffer[0] == 0x31 && buffer[pm_page] == 0x91 && buffer[2 * pm_page] == 0x91);
  memset(buffer, 0xa2, 3 * pm_page);
  CHECK(pm_copy(peer.pid, buffer, (void*)addresses.anonymous, 3 * pm_page, 1) == (ssize_t)pm_page);
  CHECK(pm_copy(peer.pid, &byte, (void*)(addresses.anonymous + 2 * pm_page), 1, 0) == 1 &&
        byte == 0);
  CHECK(!pm_send(peer.command, 'e') && !pm_receive(peer.report, 'e'));
  memset(buffer, 0xb3, 3 * pm_page);
  CHECK(pm_copy(peer.pid, buffer, (void*)addresses.shared, 3 * pm_page, 0) ==
        (ssize_t)(2 * pm_page));
  CHECK(buffer[11] == 0x86 && buffer[pm_page + 136] == pattern(pm_page + 136));
  for (size_t n = pm_page + 137; n < 2 * pm_page; ++n)
    CHECK(buffer[n] == 0);
  CHECK(buffer[2 * pm_page] == 0xb3);
  byte = 0xc4;
  CHECK(pm_copy(peer.pid, &byte, (void*)(addresses.shared + pm_page + 200), 1, 1) == 1);
  CHECK(pm_copy(peer.pid, &byte, (void*)(addresses.shared + pm_page + 200), 1, 0) == 1 &&
        byte == 0xc4);
  errno = 0;
  CHECK(pm_copy(peer.pid, &byte, (void*)(addresses.shared + 2 * pm_page), 1, 1) == -1 &&
        errno == EFAULT);
  struct stat st;
  CHECK(!fstat(fd, &st) && st.st_size == (off_t)(pm_page + 137));
  CHECK(!pm_send(peer.command, 'q') && !pm_join(&peer));
out:
  pm_cleanup(&peer);
  if (fd >= 0)
    close(fd);
  if (path[0])
    unlink(path);
  if (cow != MAP_FAILED)
    munmap(cow, pm_page);
  if (buffer != MAP_FAILED)
    munmap(buffer, 3 * pm_page);
  return failed;
}
int pm_mappings(void) {
  return backend(0) || backend(1);
}
