#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/quota.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/xattr.h>

#define CHECK(condition)                                                      \
  do {                                                                        \
    if (!(condition)) {                                                       \
      fprintf(stderr, "QUOTA-CONTRACT: line=%d errno=%d\n", __LINE__, errno); \
      goto fail;                                                              \
    }                                                                         \
  } while (0)

enum { TestUser = 4100, TestGroup = 4200 };
static char device[512];

static int control(unsigned operation, int type, int id, void* address) {
  return quotactl((int)((operation << 8) | (unsigned)type), device, id, address);
}

static int get(int type, int id, struct dqblk* record) {
  memset(record, 0, sizeof(*record));
  return control(Q_GETQUOTA, type, id, record);
}

static int limits(int type, int id, uint64_t blocks, uint64_t inodes) {
  struct dqblk record = {0};
  record.dqb_valid = QIF_LIMITS;
  record.dqb_bhardlimit = blocks;
  record.dqb_ihardlimit = inodes;
  return control(Q_SETQUOTA, type, id, &record);
}

static int find_device(dev_t mounted) {
  DIR* directory = opendir("/dev/block");
  if (!directory)
    return -1;
  int result = -1;
  struct dirent* entry;
  while ((entry = readdir(directory))) {
    struct stat attributes;
    if (snprintf(device, sizeof(device), "/dev/block/%s", entry->d_name) >= sizeof(device))
      continue;
    if (!stat(device, &attributes) && S_ISBLK(attributes.st_mode) &&
        attributes.st_rdev == mounted) {
      result = 0;
      break;
    }
  }
  closedir(directory);
  return result;
}

static int create_quota(const char* path) {
  const uint32_t old_record[8] = {0};
  int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0)
    return -1;
  const int result = write(fd, old_record, sizeof(old_record)) == sizeof(old_record) ? 0 : -1;
  close(fd);
  return result;
}

static int child_permissions(const char* creation) {
  struct dqblk record;
  if (setgid(TestGroup) || setuid(TestUser))
    return 1;
  if (get(USRQUOTA, TestUser, &record) || get(GRPQUOTA, TestGroup, &record))
    return 2;
  errno = 0;
  if (get(USRQUOTA, TestUser + 1, &record) != -1 || errno != EPERM)
    return 3;
  errno = 0;
  if (limits(USRQUOTA, TestUser, 0, 0) != -1 || errno != EPERM)
    return 4;
  errno = 0;
  int fd = open(creation, O_CREAT | O_EXCL | O_WRONLY, 0600);
  if (fd >= 0) {
    close(fd);
    return 5;
  }
  return errno == EDQUOT ? 0 : 6;
}

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: quota-contract-test EXT2-SCRATCH-DIRECTORY\n");
    return 2;
  }
  char directory[512], users[544], groups[544], first[544], second[544], creation[544];
  int fd = -1, other = -1, quota_fd = -1, result = 1;
  int user_enabled = 0, group_enabled = 0;
  struct stat attributes;
  struct dqblk user_base, group_base, record, destination;
  uint32_t format = 0;
  const long page = sysconf(_SC_PAGESIZE);
  void* denied = MAP_FAILED;
  snprintf(directory, sizeof(directory), "%s/quota-%ld", argv[1], (long)getpid());
  snprintf(users, sizeof(users), "%s/quota.user", directory);
  snprintf(groups, sizeof(groups), "%s/quota.group", directory);
  snprintf(first, sizeof(first), "%s/first", directory);
  snprintf(second, sizeof(second), "%s/second", directory);
  snprintf(creation, sizeof(creation), "%s/child", directory);
  CHECK(geteuid() == 0 && page > 0);
  CHECK(mkdir(directory, 0777) == 0 && chmod(directory, 0777) == 0);
  CHECK(stat(directory, &attributes) == 0 && find_device(attributes.st_dev) == 0);
  errno = 0;
  CHECK(control(Q_GETFMT, USRQUOTA, 0, &format) == -1 && errno == ESRCH);
  errno = 0;
  CHECK(control(Q_GETFMT, GRPQUOTA, 0, &format) == -1 && errno == ESRCH);
  CHECK(create_quota(users) == 0 && create_quota(groups) == 0);
  fd = open(first, O_CREAT | O_EXCL | O_RDWR, 0600);
  other = open(second, O_CREAT | O_EXCL | O_RDWR, 0600);
  CHECK(fd >= 0 && other >= 0);
  CHECK(fchown(fd, TestUser, TestGroup) == 0 && fchown(other, TestUser, TestGroup) == 0);
  CHECK(fstat(fd, &attributes) == 0 && attributes.st_blksize >= 1024);
  const uint64_t block = attributes.st_blksize;
  CHECK(control(Q_QUOTAON, USRQUOTA, QFMT_VFS_OLD, users) == 0);
  user_enabled = 1;
  CHECK(control(Q_QUOTAON, GRPQUOTA, QFMT_VFS_OLD, groups) == 0);
  group_enabled = 1;
  CHECK(control(Q_GETFMT, USRQUOTA, 0, &format) == 0 && format == QFMT_VFS_OLD);
  CHECK(get(USRQUOTA, TestUser, &user_base) == 0 && get(GRPQUOTA, TestGroup, &group_base) == 0);
  CHECK(user_base.dqb_curinodes >= 2 && group_base.dqb_curinodes >= 2);
  CHECK(limits(USRQUOTA, TestUser, user_base.dqb_curspace / 1024 + 14 * block / 1024,
               user_base.dqb_curinodes) == 0);
  CHECK(limits(GRPQUOTA, TestGroup, group_base.dqb_curspace / 1024 + 14 * block / 1024,
               group_base.dqb_curinodes) == 0);
  CHECK(fallocate(fd, FALLOC_FL_KEEP_SIZE, 0, 13 * block) == 0);
  CHECK(fstat(fd, &attributes) == 0 && attributes.st_size == 0 &&
        attributes.st_blocks == 14 * block / 512);
  CHECK(get(USRQUOTA, TestUser, &record) == 0 &&
        record.dqb_curspace == user_base.dqb_curspace + 14 * block);
  errno = 0;
  CHECK(fallocate(other, FALLOC_FL_KEEP_SIZE, 0, block) == -1 && errno == EDQUOT);
  errno = 0;
  CHECK(fsetxattr(other, "user.quota", "x", 1, 0) == -1 && errno == EDQUOT);
  CHECK(ftruncate(fd, 0) == 0);
  CHECK(get(USRQUOTA, TestUser, &record) == 0 && record.dqb_curspace == user_base.dqb_curspace);
  CHECK(fsetxattr(other, "user.quota", "x", 1, 0) == 0);
  CHECK(get(GRPQUOTA, TestGroup, &record) == 0 &&
        record.dqb_curspace == group_base.dqb_curspace + block);
  CHECK(fremovexattr(other, "user.quota") == 0);

  pid_t child = fork();
  CHECK(child >= 0);
  if (!child)
    _exit(child_permissions(creation));
  int status = 0;
  CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);

  quota_fd = open(users, O_RDWR);
  CHECK(quota_fd >= 0);
  errno = 0;
  CHECK(pwrite(quota_fd, "x", 1, 0) == -1 && errno == EPERM);
  errno = 0;
  CHECK(ftruncate(quota_fd, 0) == -1 && errno == EPERM);
  errno = 0;
  CHECK(unlink(users) == -1 && errno == EPERM);
  close(quota_fd);
  quota_fd = -1;
  denied = mmap(NULL, page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(denied != MAP_FAILED);
  errno = 0;
  CHECK(control(Q_SETQUOTA, USRQUOTA, TestUser, denied) == -1 && errno == EFAULT);
  errno = 0;
  CHECK(control(Q_GETQUOTA, USRQUOTA, TestUser, denied) == -1 && errno == EFAULT);
  struct dqblk unsupported = {0};
  unsupported.dqb_valid = QIF_BLIMITS;
  unsupported.dqb_bsoftlimit = 1;
  errno = 0;
  CHECK(control(Q_SETQUOTA, USRQUOTA, TestUser, &unsupported) == -1 && errno == EOPNOTSUPP);
  errno = 0;
  CHECK(quotactl((int)((unsigned)Q_SYNC << 8), first, 0, NULL) == -1 && errno == ENOTBLK);

  CHECK(fallocate(fd, FALLOC_FL_KEEP_SIZE, 0, 2 * block) == 0);
  CHECK(get(USRQUOTA, TestUser + 1, &destination) == 0);
  CHECK(limits(USRQUOTA, TestUser + 1, destination.dqb_curspace / 1024 + block / 1024,
               destination.dqb_curinodes + 1) == 0);
  errno = 0;
  CHECK(fchown(fd, TestUser + 1, -1) == -1 && errno == EDQUOT);
  CHECK(fstat(fd, &attributes) == 0 && attributes.st_uid == TestUser);
  CHECK(unlink(first) == 0);
  CHECK(get(USRQUOTA, TestUser, &record) == 0 &&
        record.dqb_curspace == user_base.dqb_curspace + 2 * block);
  CHECK(close(fd) == 0);
  fd = -1;
  CHECK(get(USRQUOTA, TestUser, &record) == 0 && record.dqb_curspace == user_base.dqb_curspace &&
        record.dqb_curinodes == user_base.dqb_curinodes - 1);
  CHECK(control(Q_SYNC, USRQUOTA, 0, NULL) == 0);
  CHECK(control(Q_QUOTAOFF, GRPQUOTA, 0, NULL) == 0);
  group_enabled = 0;
  CHECK(control(Q_QUOTAOFF, USRQUOTA, 0, NULL) == 0);
  user_enabled = 0;
  CHECK(control(Q_QUOTAON, USRQUOTA, QFMT_VFS_OLD, users) == 0);
  user_enabled = 1;
  CHECK(get(USRQUOTA, TestUser, &record) == 0 &&
        record.dqb_bhardlimit == user_base.dqb_curspace / 1024 + 14 * block / 1024 &&
        record.dqb_curinodes == user_base.dqb_curinodes - 1);
  CHECK(control(Q_QUOTAOFF, USRQUOTA, 0, NULL) == 0);
  user_enabled = 0;
  result = 0;
fail:
  if (denied != MAP_FAILED)
    munmap(denied, page);
  if (group_enabled && control(Q_QUOTAOFF, GRPQUOTA, 0, NULL))
    result = 1;
  if (user_enabled && control(Q_QUOTAOFF, USRQUOTA, 0, NULL))
    result = 1;
  if (quota_fd >= 0)
    close(quota_fd);
  if (fd >= 0)
    close(fd);
  if (other >= 0)
    close(other);
  unlink(first);
  unlink(second);
  unlink(creation);
  unlink(users);
  unlink(groups);
  rmdir(directory);
  puts(result ? "QUOTA-CONTRACT: FAIL" : "QUOTA-CONTRACT: PASS");
  return result;
}
