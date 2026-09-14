#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>

#define CHECK(expression)                                                        \
  do {                                                                           \
    if (!(expression)) {                                                         \
      fprintf(stderr, "METADATA-CONTRACT: line=%d errno=%d\n", __LINE__, errno); \
      return 0;                                                                  \
    }                                                                            \
  } while (0)
#define ERROR(expression, expected)                   \
  do {                                                \
    errno = 0;                                        \
    CHECK((expression) == -1 && errno == (expected)); \
  } while (0)

_Static_assert(SYS_truncate == 76 && SYS_lchown == 94 && SYS_mknodat == 259 &&
                   SYS_fchmodat == 268 && SYS_utimensat == 280 && SYS_statx == 332 &&
                   SYS_fchmodat2 == 452,
               "Linux amd64 metadata routes");
_Static_assert(sizeof(struct statx) == 256, "statx output ABI");

static int make_file(int directory, const char* name, mode_t mode) {
  int fd = openat(directory, name, O_CREAT | O_EXCL | O_RDWR, mode);
  if (fd < 0)
    return 0;
  return close(fd) == 0;
}

static int truncate_cases(int directory, const char* absolute) {
  unsigned char written[8192], readback[8192];
  struct stat st;
  int fd = openat(directory, "data", O_RDWR);
  CHECK(fd >= 0);
  memset(written, 0xa7, sizeof(written));
  CHECK(write(fd, written, sizeof(written)) == sizeof(written));
  CHECK(lseek(fd, 5000, SEEK_SET) == 5000);
  CHECK(truncate(absolute, 4111) == 0);
  CHECK(fstat(fd, &st) == 0 && st.st_size == 4111 && lseek(fd, 0, SEEK_CUR) == 5000);
  CHECK(syscall(SYS_truncate, absolute, (off_t)sizeof(written)) == 0);
  CHECK(pread(fd, readback, sizeof(readback), 0) == sizeof(readback));
  for (size_t i = 0; i < sizeof(readback); ++i)
    CHECK(readback[i] == (i < 4111 ? 0xa7 : 0));
  CHECK(lseek(fd, 0, SEEK_CUR) == 5000);
  ERROR(truncate(absolute, -1), EINVAL);
  ERROR(syscall(SYS_truncate, (const char*)1, (off_t)1), EFAULT);
  char absent[PATH_MAX];
  snprintf(absent, sizeof(absent), "%s-missing", absolute);
  ERROR(truncate(absent, 0), ENOENT);
  CHECK(close(fd) == 0);
  return 1;
}

static int mode_cases(int directory, int object, int link, const char* absolute) {
  struct stat st;
  CHECK(syscall(SYS_fchmodat, directory, "data", 0620, 0x40000000) == 0);
  CHECK(fstat(object, &st) == 0 && (st.st_mode & 07777) == 0620);
  CHECK(syscall(SYS_fchmodat2, directory, "data", 0640, 0) == 0);
  CHECK(fchmodat(directory, "data", 0604, AT_SYMLINK_NOFOLLOW) == 0);
  CHECK(fstat(object, &st) == 0 && (st.st_mode & 07777) == 0604);
  CHECK(syscall(SYS_fchmodat2, object, "", 0600, AT_EMPTY_PATH) == 0);
  CHECK(syscall(SYS_fchmodat2, directory, "link", 0660, 0) == 0);
  CHECK(fstat(object, &st) == 0 && (st.st_mode & 07777) == 0660);
  CHECK(syscall(SYS_fchmodat2, -123, absolute, 0640, 0) == 0);
  ERROR(syscall(SYS_fchmodat2, directory, "data", 0700, 0x40000000), EINVAL);
  ERROR(syscall(SYS_fchmodat2, -123, "data", 0700, 0), EBADF);
  ERROR(syscall(SYS_fchmodat2, object, "data", 0700, 0), ENOTDIR);
  ERROR(syscall(SYS_fchmodat2, object, "", 0700, 0), ENOENT);
  ERROR(syscall(SYS_fchmodat2, object, (const char*)1, 0700, 0), EFAULT);
  ERROR(fchmodat(directory, "link", 0600, AT_SYMLINK_NOFOLLOW), EOPNOTSUPP);
  ERROR(syscall(SYS_fchmodat2, link, "", 0600, AT_EMPTY_PATH), EOPNOTSUPP);
  ERROR(syscall(SYS_fchmodat2, object, "", S_ISUID | 0700, AT_EMPTY_PATH), EOPNOTSUPP);
  ERROR(syscall(SYS_fchmodat2, object, "", S_ISGID | 0700, AT_EMPTY_PATH), EOPNOTSUPP);
  CHECK(fstat(object, &st) == 0 && (st.st_mode & 07777) == 0640);
  return 1;
}

static int timestamp_cases(int directory, int object, int link, const char* absolute) {
  struct timespec times[2] = {{1700000000, 123456789}, {1700000300, 987654321}};
  struct stat st, originalLink;
  CHECK(utimensat(directory, "data", times, 0) == 0);
  CHECK(fstat(object, &st) == 0 && st.st_atime == times[0].tv_sec &&
        st.st_mtime == times[1].tv_sec && st.st_atim.tv_nsec == 0 && st.st_mtim.tv_nsec == 0);
  times[0] = (struct timespec){-1, UTIME_OMIT};
  times[1] = (struct timespec){1700000400, 0};
  CHECK(syscall(SYS_utimensat, object, "", times, AT_EMPTY_PATH) == 0);
  CHECK(fstat(object, &st) == 0 && st.st_atime == 1700000000 && st.st_mtime == 1700000400);
  times[0].tv_nsec = 1000000000;
  ERROR(utimensat(directory, "data", times, 0), EINVAL);
  times[0].tv_nsec = -1;
  ERROR(syscall(SYS_utimensat, directory, "data", times, 0), EINVAL);
  times[0] = (struct timespec){-1, 0};
  ERROR(utimensat(directory, "data", times, 0), EOVERFLOW);
  CHECK(fstat(object, &st) == 0 && st.st_atime == 1700000000 && st.st_mtime == 1700000400);
  times[0].tv_nsec = times[1].tv_nsec = UTIME_OMIT;
  CHECK(syscall(SYS_utimensat, -123, (const char*)1, times, 0x40000000) == 0);
  times[0] = (struct timespec){1700000500, 0};
  times[1] = (struct timespec){1700000600, 0};
  CHECK(utimensat(-123, absolute, times, 0) == 0);
  ERROR(futimens(object, times), EBADF);
  int ordinary = openat(directory, "data", O_RDONLY);
  CHECK(ordinary >= 0 && futimens(ordinary, times) == 0 && close(ordinary) == 0);
  CHECK(fstatat(directory, "link", &originalLink, AT_SYMLINK_NOFOLLOW) == 0);
  times[0].tv_nsec = UTIME_OMIT;
  times[1].tv_sec = 1700000700;
  CHECK(utimensat(directory, "link", times, AT_SYMLINK_NOFOLLOW) == 0);
  CHECK(fstat(link, &st) == 0 && st.st_mtime == 1700000700 && st.st_atime == originalLink.st_atime);
  CHECK(fstat(object, &st) == 0 && st.st_mtime == 1700000600);
  times[0] = (struct timespec){1700000800, 0};
  times[1].tv_nsec = UTIME_OMIT;
  CHECK(syscall(SYS_utimensat, link, "", times, AT_EMPTY_PATH) == 0);
  CHECK(fstat(link, &st) == 0 && st.st_atime == 1700000800 && st.st_mtime == 1700000700);
  times[0].tv_nsec = times[1].tv_nsec = UTIME_NOW;
  const time_t before = time(NULL);
  CHECK(utimensat(directory, "data", times, 0) == 0);
  const time_t after = time(NULL);
  CHECK(fstat(object, &st) == 0 && st.st_atime >= before && st.st_atime <= after &&
        st.st_mtime >= before && st.st_mtime <= after);
  ERROR(syscall(SYS_utimensat, directory, "data", (const void*)1, 0), EFAULT);
  ERROR(syscall(SYS_utimensat, directory, "data", NULL, 0x40000000), EINVAL);
  return 1;
}

static int ownership_cases(int directory, int object, const char* linkPath) {
  struct stat st;
  CHECK(lchown(linkPath, 34567, 34568) == 0);
  CHECK(fstatat(directory, "link", &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(st.st_mode) &&
        st.st_uid == 34567 && st.st_gid == 34568);
  CHECK(fstat(object, &st) == 0 && st.st_uid == 0 && st.st_gid == 0);
  CHECK(syscall(SYS_lchown, linkPath, (uid_t)-1, (gid_t)0) == 0);
  CHECK(fstatat(directory, "link", &st, AT_SYMLINK_NOFOLLOW) == 0 && st.st_uid == 34567 &&
        st.st_gid == 0);
  const struct stat original = st;
  for (unsigned attempt = 0; time(NULL) <= original.st_ctime && attempt < 4; ++attempt) {
    const struct timespec delay = {1, 0};
    CHECK(nanosleep(&delay, NULL) == 0);
  }
  CHECK(time(NULL) > original.st_ctime);
  CHECK(lchown(linkPath, (uid_t)-1, (gid_t)-1) == 0);
  CHECK(fstatat(directory, "link", &st, AT_SYMLINK_NOFOLLOW) == 0 &&
        st.st_ctime > original.st_ctime && st.st_uid == original.st_uid &&
        st.st_gid == original.st_gid && st.st_atime == original.st_atime &&
        st.st_mtime == original.st_mtime);
  ERROR(syscall(SYS_lchown, (const char*)1, (uid_t)0, (gid_t)0), EFAULT);
  return 1;
}

static int statx_cases(int directory, int object, int link, const char* absolute) {
  struct {
    struct statx value;
    unsigned char tail[16];
  } output;
  struct stat st;
  CHECK(fstat(object, &st) == 0);
  memset(&output, 0xa5, sizeof(output));
  CHECK(syscall(SYS_statx, directory, "data", 0, STATX_ALL | STATX_MNT_ID, &output.value) == 0);
  struct statx* sx = &output.value;
  CHECK((sx->stx_mask & STATX_BASIC_STATS) == STATX_BASIC_STATS);
  CHECK(!(sx->stx_mask & (STATX_BTIME | STATX_MNT_ID_UNIQUE | STATX_DIOALIGN | STATX_SUBVOL |
                          STATX_WRITE_ATOMIC)));
  CHECK(sx->stx_mode == st.st_mode && sx->stx_ino == st.st_ino && sx->stx_size == st.st_size &&
        sx->stx_blocks == st.st_blocks && sx->stx_nlink == st.st_nlink &&
        sx->stx_uid == st.st_uid && sx->stx_gid == st.st_gid);
  CHECK(sx->stx_dev_major == major(st.st_dev) && sx->stx_dev_minor == minor(st.st_dev) &&
        sx->stx_rdev_major == 0 && sx->stx_rdev_minor == 0);
  CHECK(sx->stx_attributes == 0 && sx->stx_attributes_mask == 0);
  CHECK(sx->stx_btime.tv_sec == 0 && sx->stx_btime.tv_nsec == 0);
  CHECK(sx->stx_atime.tv_sec == st.st_atime && sx->stx_mtime.tv_sec == st.st_mtime &&
        sx->stx_ctime.tv_sec == st.st_ctime);
  CHECK(sx->stx_atime.tv_nsec == 0 && sx->stx_mtime.tv_nsec == 0 && sx->stx_ctime.tv_nsec == 0);
  for (size_t i = 152; i < sizeof(*sx); ++i)
    CHECK(((const unsigned char*)sx)[i] == 0);
  for (size_t i = 0; i < sizeof(output.tail); ++i)
    CHECK(output.tail[i] == 0xa5);
  CHECK(statx(-123, absolute, AT_STATX_DONT_SYNC, 0, sx) == 0 && S_ISREG(sx->stx_mode));
  CHECK(statx(object, "", AT_EMPTY_PATH, STATX_BASIC_STATS, sx) == 0 && sx->stx_ino == st.st_ino);
  CHECK(syscall(SYS_statx, object, NULL, AT_EMPTY_PATH, STATX_BASIC_STATS, sx) == 0 &&
        sx->stx_ino == st.st_ino);
  CHECK(syscall(SYS_statx, AT_FDCWD, NULL, AT_EMPTY_PATH, STATX_BASIC_STATS, sx) == 0 &&
        S_ISDIR(sx->stx_mode));
  CHECK(statx(link, "", AT_EMPTY_PATH, STATX_TYPE, sx) == 0 && S_ISLNK(sx->stx_mode));
  CHECK(statx(directory, "link", 0, STATX_TYPE, sx) == 0 && S_ISREG(sx->stx_mode));
  CHECK(statx(directory, "link", AT_SYMLINK_NOFOLLOW, STATX_TYPE, sx) == 0 &&
        S_ISLNK(sx->stx_mode));
  CHECK(statx(directory, "null", 0, STATX_TYPE, sx) == 0 && S_ISREG(sx->stx_mode));
  ERROR(syscall(SYS_statx, directory, "data", 0, 0x80000000U, sx), EINVAL);
  ERROR(syscall(SYS_statx, directory, "data", 0x6000, 0, sx), EINVAL);
  ERROR(syscall(SYS_statx, directory, "data", 0x40000000, 0, sx), EINVAL);
  ERROR(syscall(SYS_statx, -123, "data", 0, 0, sx), EBADF);
  ERROR(syscall(SYS_statx, object, "data", 0, 0, sx), ENOTDIR);
  ERROR(syscall(SYS_statx, object, "", 0, 0, sx), ENOENT);
  ERROR(syscall(SYS_statx, directory, NULL, 0, 0, sx), EFAULT);
  ERROR(syscall(SYS_statx, directory, "data", 0, 0, (void*)1), EFAULT);
  const long page = sysconf(_SC_PAGESIZE);
  void* boundary = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(boundary != MAP_FAILED && page >= 256);
  CHECK(mprotect((char*)boundary + page, page, PROT_NONE) == 0);
  CHECK(syscall(SYS_statx, directory, "data", 0, 0, (char*)boundary + page - 256) == 0);
  ERROR(syscall(SYS_statx, directory, "data", 0, 0, (char*)boundary + page - 255), EFAULT);
  CHECK(munmap(boundary, 2 * page) == 0);
  return 1;
}

static int mknod_cases(int directory, int object, const char* absolute) {
  struct stat st;
  mode_t previous = umask(0027);
  CHECK(mknodat(directory, "regular", S_IFREG | 0666, makedev(1, 3)) == 0);
  CHECK(syscall(SYS_mknodat, directory, "plain", 0666, 0) == 0);
  CHECK(mkfifoat(directory, "fifo", 0666) == 0);
  CHECK(syscall(SYS_mknodat, directory, "raw-fifo", S_IFIFO | 0666, 0) == 0);
  umask(previous);
  CHECK(fstatat(directory, "regular", &st, 0) == 0 && S_ISREG(st.st_mode) &&
        (st.st_mode & 0777) == 0640 && st.st_rdev == 0 && st.st_size == 0);
  CHECK(fstatat(directory, "fifo", &st, 0) == 0 && S_ISFIFO(st.st_mode) &&
        (st.st_mode & 0777) == 0640);
  int fd = openat(directory, "fifo", O_RDWR | O_NONBLOCK);
  char value = 0;
  CHECK(fd >= 0 && write(fd, "q", 1) == 1 && read(fd, &value, 1) == 1 && value == 'q');
  CHECK(close(fd) == 0);
  CHECK(mknodat(-123, absolute, S_IFREG | 0600, 0) == 0);
  CHECK(mkdirat(directory, "subdir", 0700) == 0);
  int retained = openat(directory, "subdir", O_PATH | O_DIRECTORY);
  CHECK(retained >= 0);
  CHECK(renameat(directory, "subdir", directory, "moved") == 0);
  CHECK(mknodat(retained, "inside", S_IFREG | 0600, 0) == 0);
  CHECK(fstatat(directory, "moved/inside", &st, 0) == 0 && S_ISREG(st.st_mode));
  CHECK(unlinkat(retained, "inside", 0) == 0 && close(retained) == 0);
  CHECK(unlinkat(directory, "moved", AT_REMOVEDIR) == 0);
  ERROR(mknodat(directory, "regular", S_IFREG | 0600, 0), EEXIST);
  ERROR(mknodat(directory, "link", S_IFREG | 0600, 0), EEXIST);
  ERROR(mknodat(-123, "new", S_IFREG | 0600, 0), EBADF);
  ERROR(mknodat(object, "new", S_IFREG | 0600, 0), ENOTDIR);
  ERROR(mknodat(directory, "", S_IFREG | 0600, 0), ENOENT);
  ERROR(mknodat(directory, ".", S_IFIFO | 0600, 0), EEXIST);
  ERROR(mknodat(directory, "trailing/", S_IFREG | 0600, 0), ENOENT);
  ERROR(mknodat(directory, "bad-directory", S_IFDIR | 0700, 0), EPERM);
  ERROR(mknodat(directory, "device", S_IFCHR | 0600, makedev(1, 3)), EOPNOTSUPP);
  ERROR(mknodat(directory, "setid", S_IFREG | S_ISUID | 0700, 0), EOPNOTSUPP);
  char longName[257];
  memset(longName, 'x', 256);
  longName[256] = 0;
  ERROR(mknodat(directory, longName, S_IFIFO | 0600, 0), ENAMETOOLONG);
  ERROR(syscall(SYS_mknodat, directory, (const char*)1, S_IFREG | 0600, 0), EFAULT);
  return 1;
}

static int denied_cases(int directory, const char* absolute, const char* linkPath) {
  struct timespec explicit[2] = {{1700000000, 0}, {1700000000, 0}};
  CHECK(setgroups(0, NULL) == 0 && setgid(42424) == 0 && setuid(42424) == 0);
  ERROR(truncate(absolute, 0), EACCES);
  ERROR(syscall(SYS_fchmodat2, directory, "data", 0777, 0), EPERM);
  ERROR(lchown(linkPath, 42424, (gid_t)-1), EPERM);
  ERROR(utimensat(directory, "data", explicit, 0), EPERM);
  ERROR(utimensat(directory, "data", NULL, 0), EACCES);
  CHECK(utimensat(directory, "writable", NULL, 0) == 0);
  ERROR(utimensat(directory, "writable", explicit, 0), EPERM);
  ERROR(mknodat(directory, "denied", S_IFIFO | 0600, 0), EACCES);
  struct statx st;
  CHECK(statx(directory, "data", 0, STATX_BASIC_STATS, &st) == 0);
  return 1;
}

int main(int argc, char** argv) {
  const char* base = argc > 1 ? argv[1] : "/tmp";
  char directoryPath[PATH_MAX], absolute[PATH_MAX], linkPath[PATH_MAX], created[PATH_MAX];
  snprintf(directoryPath, sizeof(directoryPath), "%s/metadata-contract-%ld", base, (long)getpid());
  snprintf(absolute, sizeof(absolute), "%s/data", directoryPath);
  snprintf(linkPath, sizeof(linkPath), "%s/link", directoryPath);
  snprintf(created, sizeof(created), "%s/absolute", directoryPath);
  int directory = -1, object = -1, link = -1, success = 0, createdDirectory = 0;
  const mode_t originalMask = umask(0);
  if (geteuid() != 0 || mkdir(directoryPath, 0755))
    goto done;
  createdDirectory = 1;
  directory = open(directoryPath, O_PATH | O_DIRECTORY);
  if (directory < 0 || !make_file(directory, "data", 0600) || !make_file(directory, "null", 0600) ||
      !make_file(directory, "writable", 0666) || symlinkat("data", directory, "link"))
    goto done;
  object = openat(directory, "data", O_PATH);
  link = openat(directory, "link", O_PATH | O_NOFOLLOW);
  if (object < 0 || link < 0 || !truncate_cases(directory, absolute) ||
      !mode_cases(directory, object, link, absolute) ||
      !timestamp_cases(directory, object, link, absolute) ||
      !ownership_cases(directory, object, linkPath) ||
      !statx_cases(directory, object, link, absolute) || !mknod_cases(directory, object, created))
    goto done;
  if (syscall(SYS_fchmodat2, object, "", 0600, AT_EMPTY_PATH))
    goto done;
  pid_t child = fork();
  if (!child)
    _exit(denied_cases(directory, absolute, linkPath) ? 0 : 1);
  int status = 0;
  if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status))
    goto done;
  success = 1;
done:
  if (!success)
    fprintf(stderr, "METADATA-CONTRACT: setup or case failure errno=%d\n", errno);
  umask(originalMask);
  if (link >= 0)
    close(link);
  if (object >= 0)
    close(object);
  if (directory >= 0) {
    const char* names[] = {"data",     "link",     "null",   "writable", "regular", "plain", "fifo",
                           "raw-fifo", "absolute", "denied", "trailing", "device",  "setid"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
      unlinkat(directory, names[i], 0);
    unlinkat(directory, "subdir/inside", 0);
    unlinkat(directory, "moved/inside", 0);
    unlinkat(directory, "subdir", AT_REMOVEDIR);
    unlinkat(directory, "moved", AT_REMOVEDIR);
    close(directory);
  }
  if (createdDirectory)
    rmdir(directoryPath);
  printf("METADATA-CONTRACT: %s base=%s\n", success ? "PASS" : "FAIL", base);
  return success ? 0 : 1;
}
