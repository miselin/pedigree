#define _GNU_SOURCE
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>
#include <sys/socket.h>

static int fork_seals(void) {
  int failed = 0, fd = -1, alias = -1, pair[2] = {-1, -1};
  pid_t child = -1;
  CHECK((fd = mf_make(32)) >= 0 && (alias = dup(fd)) >= 0);
  CHECK(pwrite(fd, "a", 1, 0) == 1);
  CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
  CHECK((child = fork()) >= 0);
  if (!child) {
    alarm(8);
    close(pair[0]);
    close(fd);
    if (write(pair[1], "r", 1) != 1 || mf_byte(pair[1], 'g'))
      _exit(10);
    if (fcntl(alias, F_GET_SEALS) != F_SEAL_GROW || fcntl(alias, F_ADD_SEALS, F_SEAL_SHRINK) ||
        pwrite(alias, "b", 1, 0) != 1)
      _exit(11);
    _exit(write(pair[1], "s", 1) == 1 ? 0 : 12);
  }
  close(pair[1]);
  pair[1] = -1;
  CHECK(!mf_byte(pair[0], 'r'));
  CHECK(!fcntl(fd, F_ADD_SEALS, F_SEAL_GROW));
  CHECK(write(pair[0], "g", 1) == 1 && !mf_byte(pair[0], 's'));
  CHECK(fcntl(fd, F_GET_SEALS) == (F_SEAL_GROW | F_SEAL_SHRINK));
  CHECK(fcntl(alias, F_GET_SEALS) == (F_SEAL_GROW | F_SEAL_SHRINK));
  CHECK(!mf_contents(fd, 0, "b", 1));
  const int status = mf_reap(child, 5000);
  child = -1;
  CHECK(!status);
out:
  if (child > 0) {
    kill(child, SIGKILL);
    mf_reap(child, 1000);
  }
  if (pair[0] >= 0)
    close(pair[0]);
  if (pair[1] >= 0)
    close(pair[1]);
  if (alias >= 0)
    close(alias);
  if (fd >= 0)
    close(fd);
  return failed;
}
static int queued_rights(void) {
  int failed = 0, fd = -1, received = -1, pair[2] = {-1, -1}, gate[2] = {-1, -1};
  pid_t child = -1;
  CHECK((fd = mf_make(32)) >= 0 && pwrite(fd, "queue", 5, 0) == 5);
  CHECK(!fcntl(fd, F_ADD_SEALS, F_SEAL_GROW));
  CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair) && !socketpair(AF_UNIX, SOCK_STREAM, 0, gate));
  CHECK((child = fork()) >= 0);
  if (!child) {
    alarm(8);
    close(pair[0]);
    close(gate[0]);
    close(fd);
    if (write(gate[1], "r", 1) != 1 || mf_byte(gate[1], 'g'))
      _exit(20);
    int transferred = mf_receive_fd(pair[1]);
    if (transferred < 0 || fcntl(transferred, F_GETFD) != FD_CLOEXEC ||
        fcntl(transferred, F_GET_SEALS) != F_SEAL_GROW || mf_contents(transferred, 0, "queue", 5) ||
        fcntl(transferred, F_ADD_SEALS, F_SEAL_SHRINK) || mf_send_fd(pair[1], transferred))
      _exit(21);
    close(transferred);
    _exit(write(gate[1], "s", 1) == 1 ? 0 : 22);
  }
  close(pair[1]);
  pair[1] = -1;
  close(gate[1]);
  gate[1] = -1;
  CHECK(!mf_byte(gate[0], 'r') && !mf_send_fd(pair[0], fd));
  CHECK(!close(fd));
  fd = -1;
  /* The child cannot receive until only the queued rights keep the file alive. */
  CHECK(write(gate[0], "g", 1) == 1 && !mf_byte(gate[0], 's'));
  CHECK((received = mf_receive_fd(pair[0])) >= 0);
  CHECK(fcntl(received, F_GET_SEALS) == (F_SEAL_GROW | F_SEAL_SHRINK));
  CHECK(!mf_contents(received, 0, "queue", 5));
  CHECK(ftruncate(received, 31) == -1 && errno == EPERM);
  const int status = mf_reap(child, 5000);
  child = -1;
  CHECK(!status);
out:
  if (child > 0) {
    kill(child, SIGKILL);
    mf_reap(child, 1000);
  }
  for (int n = 0; n < 2; ++n) {
    if (pair[n] >= 0)
      close(pair[n]);
    if (gate[n] >= 0)
      close(gate[n]);
  }
  if (received >= 0)
    close(received);
  if (fd >= 0)
    close(fd);
  return failed;
}
static int child_capability(int explicit_unmap) {
  int failed = 0, fd = -1, pair[2] = {-1, -1};
  const size_t page = sysconf(_SC_PAGESIZE);
  char* mapping = MAP_FAILED;
  pid_t child = -1;
  CHECK((fd = mf_make(page)) >= 0);
  mapping = mmap(NULL, page, PROT_READ, MAP_SHARED, fd, 0);
  CHECK(mapping != MAP_FAILED && !socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
  CHECK((child = fork()) >= 0);
  if (!child) {
    alarm(8);
    close(pair[0]);
    close(fd);
    if (write(pair[1], "r", 1) != 1 || mf_byte(pair[1], 'g') ||
        mprotect(mapping, page, PROT_READ | PROT_WRITE))
      _exit(30);
    mapping[0] = 'c';
    if (write(pair[1], "w", 1) != 1 || mf_byte(pair[1], 'u'))
      _exit(31);
    if (explicit_unmap &&
        (munmap(mapping, page) || write(pair[1], "u", 1) != 1 || mf_byte(pair[1], 'q')))
      _exit(32);
    _exit(0);
  }
  close(pair[1]);
  pair[1] = -1;
  CHECK(!mf_byte(pair[0], 'r'));
  CHECK(!munmap(mapping, page));
  mapping = MAP_FAILED;
  CHECK(fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_SHRINK) == -1 && errno == EBUSY);
  CHECK(fcntl(fd, F_GET_SEALS) == 0 && !fcntl(fd, F_ADD_SEALS, F_SEAL_FUTURE_WRITE));
  CHECK(write(pair[0], "g", 1) == 1 && !mf_byte(pair[0], 'w'));
  CHECK(!mf_contents(fd, 0, "c", 1));
  CHECK(fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE) == -1 && errno == EBUSY);
  CHECK(write(pair[0], "u", 1) == 1);
  if (explicit_unmap) {
    CHECK(!mf_byte(pair[0], 'u'));
    CHECK(!fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE));
    CHECK(write(pair[0], "q", 1) == 1);
  }
  const int status = mf_reap(child, 5000);
  child = -1;
  CHECK(!status);
  CHECK(!fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE));
  CHECK(fcntl(fd, F_GET_SEALS) == (F_SEAL_WRITE | F_SEAL_FUTURE_WRITE));
out:
  if (child > 0) {
    kill(child, SIGKILL);
    mf_reap(child, 1000);
  }
  if (pair[0] >= 0)
    close(pair[0]);
  if (pair[1] >= 0)
    close(pair[1]);
  if (mapping != MAP_FAILED)
    munmap(mapping, page);
  if (fd >= 0)
    close(fd);
  return failed;
}
static int mapped_last_close(void) {
  int failed = 0, fd = -1;
  const size_t page = sysconf(_SC_PAGESIZE);
  char *mapping = MAP_FAILED, *alias = MAP_FAILED, *target = MAP_FAILED, *private = MAP_FAILED;
  size_t length = 2 * page;
  pid_t child = -1;
  CHECK((fd = mf_make(3 * page)) >= 0 && pwrite(fd, "last", 4, 0) == 4);
  mapping = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  alias = mmap(NULL, page, PROT_READ, MAP_SHARED, fd, 0);
  CHECK(mapping != MAP_FAILED && alias != MAP_FAILED);
  CHECK(!fcntl(fd, F_ADD_SEALS, F_SEAL_FUTURE_WRITE));
  private = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  CHECK(private != MAP_FAILED);
  private[0] = 'p';
  CHECK(!close(fd));
  fd = -1;
  CHECK(!memcmp(alias, "last", 4));
  target = mmap(NULL, 3 * page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(target != MAP_FAILED);
  char* moved = mremap(mapping, length, 3 * page, MREMAP_MAYMOVE | MREMAP_FIXED, target);
  CHECK(moved == target);
  mapping = moved;
  length = 3 * page;
  target = MAP_FAILED;
  CHECK(!madvise(mapping, length, MADV_DONTNEED) && !memcmp(mapping, "last", 4));
  mapping[0] = 'L';
  mapping[2 * page] = 'g';
  CHECK(alias[0] == 'L');
  CHECK(!madvise(mapping, length, MADV_DONTNEED));
  CHECK(mapping[0] == 'L' && mapping[2 * page] == 'g');
  CHECK((child = fork()) >= 0);
  if (!child) {
    alarm(8);
    if (private[0] != 'p' || mapping[0] != 'L' || mapping[2 * page] != 'g')
      _exit(35);
    private[0] = 'c';
    mapping[0] = 'F';
    _exit(private[0] == 'c' && alias[0] == 'F' ? 0 : 36);
  }
  const int status = mf_reap(child, 5000);
  child = -1;
  CHECK(!status && private[0] == 'p' && alias[0] == 'F' && mapping[0] == 'F');
out:
  if (child > 0) {
    kill(child, SIGKILL);
    mf_reap(child, 1000);
  }
  if (private != MAP_FAILED)
    munmap(private, page);
  if (target != MAP_FAILED)
    munmap(target, 3 * page);
  if (alias != MAP_FAILED)
    munmap(alias, page);
  if (mapping != MAP_FAILED)
    munmap(mapping, length);
  if (fd >= 0)
    close(fd);
  return failed;
}
int memfd_exec(int argc, char** argv) {
  if (argc != 4)
    return 40;
  const int closed = atoi(argv[2]), retained = atoi(argv[3]);
  if (fcntl(closed, F_GETFD) != -1 || errno != EBADF || fcntl(retained, F_GETFD) != 0 ||
      fcntl(retained, F_GET_SEALS) != (F_SEAL_GROW | F_SEAL_SHRINK) ||
      mf_contents(retained, 0, "exec", 4))
    return 41;
  if (fcntl(retained, F_ADD_SEALS, F_SEAL_WRITE))
    return 42;
  return close(retained) ? 43 : 0;
}
static int exec_lifetime(void) {
  int failed = 0, fd = -1, alias = -1;
  pid_t child = -1;
  char closed[24], retained[24];
  CHECK((fd = memfd_create("exec", MFD_ALLOW_SEALING | MFD_CLOEXEC)) >= 0);
  CHECK(write(fd, "exec", 4) == 4 && (alias = dup(fd)) >= 0);
  CHECK(!fcntl(fd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK));
  snprintf(closed, sizeof(closed), "%d", fd);
  snprintf(retained, sizeof(retained), "%d", alias);
  CHECK((child = fork()) >= 0);
  if (!child) {
    alarm(8);
    execl(MEMFD_APP, MEMFD_APP, "memfd-exec", closed, retained, (char*)NULL);
    _exit(44);
  }
  const int status = mf_reap(child, 6000);
  child = -1;
  CHECK(!status);
  CHECK(fcntl(fd, F_GET_SEALS) == (F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_WRITE));
out:
  if (child > 0) {
    kill(child, SIGKILL);
    mf_reap(child, 1000);
  }
  if (alias >= 0)
    close(alias);
  if (fd >= 0)
    close(fd);
  return failed;
}
int memfd_lifetime(void) {
  return fork_seals() || queued_rights() || child_capability(1) || child_capability(0) ||
         mapped_last_close() || exec_lifetime();
}
