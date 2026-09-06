#define _GNU_SOURCE
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>

static int names_and_flags(void) {
  int failed = 0, plain = -1, first = -1, second = -1, long_fd = -1;
  const size_t page = sysconf(_SC_PAGESIZE);
  char* denied = MAP_FAILED;
  mode_t mask = umask(0077);
  struct stat st, other;
  char name[251];
  CHECK((plain = memfd_create("", 0)) >= 0);
  CHECK(fcntl(plain, F_GET_SEALS) == F_SEAL_SEAL);
  CHECK(fcntl(plain, F_ADD_SEALS, F_SEAL_GROW) == -1 && errno == EPERM);
  CHECK(fcntl(plain, F_ADD_SEALS, 0) == -1 && errno == EPERM);
  CHECK(fcntl(plain, F_GET_SEALS) == F_SEAL_SEAL);
  CHECK((fcntl(plain, F_GETFL) & O_ACCMODE) == O_RDWR);
  CHECK(fcntl(plain, F_GETFD) == 0);
  CHECK(!fstat(plain, &st) && S_ISREG(st.st_mode) && (st.st_mode & 07777) == 0777 &&
        st.st_nlink == 0 && st.st_size == 0 && st.st_uid == geteuid() && st.st_gid == getegid());
  CHECK(read(plain, name, 1) == 0);
  CHECK(lseek(plain, 17, SEEK_SET) == 17 && lseek(plain, 0, SEEK_END) == 0);
  CHECK((first = memfd_create("same label", MFD_ALLOW_SEALING | MFD_CLOEXEC)) >= 0);
  CHECK((second = memfd_create("same label", MFD_ALLOW_SEALING)) >= 0);
  CHECK(fcntl(first, F_GET_SEALS) == 0 && fcntl(second, F_GET_SEALS) == 0);
  CHECK(fcntl(first, F_GETFD) == FD_CLOEXEC && fcntl(second, F_GETFD) == 0);
  CHECK(!fstat(first, &st) && !fstat(second, &other) &&
        (st.st_ino != other.st_ino || st.st_dev != other.st_dev));
  CHECK(write(first, "a", 1) == 1 && !mf_size(second, 0));
  CHECK(!fcntl(first, F_ADD_SEALS, F_SEAL_GROW) && fcntl(second, F_GET_SEALS) == 0);
  memset(name, 'n', sizeof(name));
  name[249] = 0;
  CHECK((long_fd = memfd_create(name, MFD_ALLOW_SEALING)) >= 0);
  name[249] = 'n';
  name[250] = 0;
  CHECK(memfd_create(name, 0) == -1 && errno == EINVAL);
  CHECK(memfd_create("invalid", 0x80000000U) == -1 && errno == EINVAL);
  CHECK(memfd_create("huge", MFD_HUGETLB) == -1 && errno == EINVAL);
  denied = mmap(NULL, page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(denied != MAP_FAILED);
  CHECK(memfd_create(denied, 0) == -1 && errno == EFAULT);
out:
  umask(mask);
  if (denied != MAP_FAILED)
    munmap(denied, page);
  if (long_fd >= 0)
    close(long_fd);
  if (second >= 0)
    close(second);
  if (first >= 0)
    close(first);
  if (plain >= 0)
    close(plain);
  return failed;
}
static int ordinary_io(void) {
  int failed = 0, fd = -1, alias = -1, regular = -1;
  char bytes[16] = {0};
  char path[128];
  snprintf(path, sizeof(path), "/tmp/memfd-regular-%ld", (long)getpid());
  CHECK((fd = memfd_create("io", MFD_ALLOW_SEALING)) >= 0);
  CHECK(write(fd, "abcd", 4) == 4 && pwrite(fd, "XY", 2, 1) == 2);
  CHECK(lseek(fd, 0, SEEK_CUR) == 4 && !mf_contents(fd, 0, "aXYd", 4));
  CHECK((alias = dup(fd)) >= 0);
  CHECK(lseek(alias, 1, SEEK_SET) == 1 && read(fd, bytes, 2) == 2 && !memcmp(bytes, "XY", 2));
  CHECK(lseek(alias, 0, SEEK_CUR) == 3);
  struct iovec vector[] = {{"12", 2}, {"34", 2}};
  CHECK(pwritev(fd, vector, 2, 4) == 4 && lseek(fd, 0, SEEK_CUR) == 3);
  CHECK(!mf_contents(fd, 0, "aXYd1234", 8));
  CHECK(lseek(fd, 0, SEEK_END) == 8 && writev(fd, vector, 2) == 4);
  CHECK(!ftruncate(fd, 16) && !mf_contents(fd, 12, "\0\0\0\0", 4));
  struct iovec input[] = {{bytes, 3}, {bytes + 3, 5}};
  CHECK(preadv(fd, input, 2, 4) == 8 && !memcmp(bytes, "12341234", 8));
  CHECK(!fcntl(alias, F_SETFL, O_APPEND));
  CHECK((fcntl(fd, F_GETFL) & O_APPEND) && write(fd, "!", 1) == 1 && !mf_size(fd, 17));
  CHECK(!mf_contents(fd, 16, "!", 1));
  CHECK((regular = open(path, O_CREAT | O_EXCL | O_RDWR, 0600)) >= 0);
  CHECK(fcntl(regular, F_GET_SEALS) == -1 && errno == EINVAL);
  CHECK(fcntl(regular, F_ADD_SEALS, F_SEAL_GROW) == -1 && errno == EINVAL);
  CHECK(fcntl(-1, F_GET_SEALS) == -1 && errno == EBADF);
out:
  if (regular >= 0) {
    close(regular);
    unlink(path);
  }
  if (alias >= 0)
    close(alias);
  if (fd >= 0)
    close(fd);
  return failed;
}
int memfd_creation(void) {
  return names_and_flags() || ordinary_io();
}
