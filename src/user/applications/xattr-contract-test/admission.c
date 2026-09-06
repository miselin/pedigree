#define _GNU_SOURCE
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/xattr.h>

static int names_and_faults(void) {
  int failed = 0, path_fd = -1, anonymous = -1;
  struct xa_file file = {.fd = -1};
  void* bad = MAP_FAILED;
  char name[257], missing[224], nondirectory[224];
  CHECK(!xa_create(&file, XA_RAMFS, 0));
  CHECK(!fsetxattr(file.fd, "user.keep", "k", 1, 0));
  bad = mmap(NULL, xa_page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(bad != MAP_FAILED);
  CHECK(fsetxattr(file.fd, "", "v", 1, 0) == -1 && errno == ERANGE);
  CHECK(fgetxattr(file.fd, "", NULL, 0) == -1 && errno == ERANGE);
  CHECK(fremovexattr(file.fd, "") == -1 && errno == ERANGE);
  CHECK(fsetxattr(file.fd, "user.", "v", 1, 0) == -1 && errno == EINVAL);
  memcpy(name, "user.", 5);
  memset(name + 5, 'n', 250);
  name[255] = 0;
  CHECK(!fsetxattr(file.fd, name, "v", 1, 0));
  CHECK(!xa_value(&file, XA_FD, name, "v", 1));
  CHECK(!fremovexattr(file.fd, name));
  name[255] = 'n';
  name[256] = 0;
  CHECK(fsetxattr(file.fd, name, "v", 1, 0) == -1 && errno == ERANGE);
  CHECK(fgetxattr(file.fd, name, NULL, 0) == -1 && errno == ERANGE);
  CHECK(fremovexattr(file.fd, name) == -1 && errno == ERANGE);
  const char high_name[] = {'u', 's', 'e', 'r', '.', (char)0x80, (char)0xff, 0};
  CHECK(!fsetxattr(file.fd, high_name, "b", 1, 0));
  CHECK(!xa_value(&file, XA_FD, high_name, "b", 1));
  CHECK(!fremovexattr(file.fd, high_name));
  CHECK(!fsetxattr(file.fd, "user.zero\0ignored", "z", 1, 0));
  CHECK(!xa_value(&file, XA_FD, "user.zero", "z", 1));
  CHECK(fgetxattr(file.fd, "user.Zero", NULL, 0) == -1 && errno == ENODATA);
  CHECK(!fremovexattr(file.fd, "user.zero"));
  const char* unsupported[] = {"trusted.test", "security.test", "system.posix_acl_access",
                               "unknown.test", "User.test"};
  for (size_t n = 0; n < sizeof(unsupported) / sizeof(unsupported[0]); ++n) {
    CHECK(fsetxattr(file.fd, unsupported[n], "v", 1, 0) == -1 && errno == EOPNOTSUPP);
    CHECK(fgetxattr(file.fd, unsupported[n], NULL, 0) == -1 && errno == EOPNOTSUPP);
    CHECK(fremovexattr(file.fd, unsupported[n]) == -1 && errno == EOPNOTSUPP);
  }
  const char* keep[] = {"user.keep"};
  CHECK(!xa_names(&file, XA_FD, keep, 1));
  CHECK(fsetxattr(-1, bad, bad, 1, 4) == -1 && errno == EBADF);
  CHECK(fgetxattr(-1, bad, bad, 1) == -1 && errno == EBADF);
  CHECK(flistxattr(-1, bad, 1) == -1 && errno == EBADF);
  CHECK(fremovexattr(-1, bad) == -1 && errno == EBADF);
  CHECK(fsetxattr(file.fd, bad, "v", 1, 0) == -1 && errno == EFAULT);
  CHECK(fgetxattr(file.fd, bad, NULL, 0) == -1 && errno == EFAULT);
  CHECK(fremovexattr(file.fd, bad) == -1 && errno == EFAULT);
  snprintf(missing, sizeof(missing), "%s.missing", file.path);
  CHECK(setxattr(missing, bad, bad, 1, 4) == -1 && errno == EINVAL);
  CHECK(setxattr(missing, "user.x", bad, 1, 0) == -1 && errno == EFAULT);
  CHECK(getxattr(missing, bad, NULL, 0) == -1 && errno == ENOENT);
  CHECK(removexattr(missing, "") == -1 && errno == ERANGE);
  CHECK(removexattr(missing, "user.x") == -1 && errno == ENOENT);
  snprintf(nondirectory, sizeof(nondirectory), "%s/child", file.path);
  CHECK(getxattr(nondirectory, "user.x", NULL, 0) == -1 && errno == ENOTDIR);
  snprintf(nondirectory, sizeof(nondirectory), "%s/", file.path);
  CHECK(lgetxattr(nondirectory, "user.keep", NULL, 0) == -1 && errno == ENOTDIR);
  CHECK(lsetxattr(nondirectory, "user.keep", "x", 1, 0) == -1 && errno == ENOTDIR);
  CHECK((path_fd = open(file.path, O_PATH | O_CLOEXEC)) >= 0);
  CHECK(fsetxattr(path_fd, "user.x", "x", 1, 0) == -1 && errno == EBADF);
  CHECK(fgetxattr(path_fd, "user.keep", NULL, 0) == -1 && errno == EBADF);
  CHECK(flistxattr(path_fd, NULL, 0) == -1 && errno == EBADF);
  CHECK(fremovexattr(path_fd, "user.keep") == -1 && errno == EBADF);
  CHECK((anonymous = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) >= 0);
  CHECK(fsetxattr(anonymous, "user.x", "x", 1, 0) == -1 && errno == EOPNOTSUPP);
  CHECK(fgetxattr(anonymous, "user.x", NULL, 0) == -1 && errno == EOPNOTSUPP);
  CHECK(flistxattr(anonymous, NULL, 0) == -1 && errno == EOPNOTSUPP);
  CHECK(fremovexattr(anonymous, "user.x") == -1 && errno == EOPNOTSUPP);
  CHECK(!xa_value(&file, XA_PATH, "user.keep", "k", 1));
out:
  if (anonymous >= 0)
    close(anonymous);
  if (path_fd >= 0)
    close(path_fd);
  if (bad != MAP_FAILED)
    munmap(bad, xa_page);
  xa_close(&file);
  return failed;
}

static int link_resolution(void) {
  int failed = 0;
  struct xa_file file = {.fd = -1}, directory = {.fd = -1};
  char link_path[224] = {0}, loop_path[224] = {0}, intermediate[448], parent_link[224] = {0};
  char directory_link[224] = {0}, trailing[256];
  CHECK(!xa_create(&file, XA_EXT2, 0));
  CHECK(!fsetxattr(file.fd, "user.key", "a", 1, 0));
  snprintf(link_path, sizeof(link_path), "%s.link", file.path);
  snprintf(loop_path, sizeof(loop_path), "%s.loop", file.path);
  snprintf(parent_link, sizeof(parent_link), "%s.parent", file.path);
  CHECK(!symlink(file.path + 1, link_path));
  CHECK(!symlink(loop_path + 1, loop_path));
  CHECK(!symlink(".", parent_link));
  CHECK(!chdir("/"));
  char value = 0;
  CHECK(getxattr(link_path, "user.key", &value, 1) == 1 && value == 'a');
  CHECK(!setxattr(link_path, "user.key", "b", 1, XATTR_REPLACE));
  CHECK(!xa_value(&file, XA_FD, "user.key", "b", 1));
  CHECK(lgetxattr(link_path, "user.key", NULL, 0) == -1 && errno == ENODATA);
  CHECK(lsetxattr(link_path, "user.key", "x", 1, 0) == -1 && errno == EPERM);
  CHECK(lremovexattr(link_path, "user.key") == -1 && errno == EPERM);
  CHECK(llistxattr(link_path, NULL, 0) == 0);
  CHECK(listxattr(link_path, NULL, 0) == (ssize_t)sizeof("user.key"));
  snprintf(intermediate, sizeof(intermediate), "%s%s", parent_link, file.path);
  CHECK(!lsetxattr(intermediate, "user.key", "c", 1, 0));
  CHECK(lgetxattr(file.path + 1, "user.key", &value, 1) == 1 && value == 'c');
  CHECK(!xa_create(&directory, XA_EXT2, 1));
  snprintf(directory_link, sizeof(directory_link), "%s.link", directory.path);
  CHECK(!symlink(directory.path + 1, directory_link));
  snprintf(trailing, sizeof(trailing), "%s/", directory_link);
  CHECK(!lsetxattr(trailing, "user.directory", "d", 1, 0));
  CHECK(lgetxattr(trailing, "user.directory", &value, 1) == 1 && value == 'd');
  CHECK(llistxattr(trailing, NULL, 0) == (ssize_t)sizeof("user.directory"));
  CHECK(!lremovexattr(trailing, "user.directory"));
  CHECK(!removexattr(link_path, "user.key"));
  CHECK(fgetxattr(file.fd, "user.key", NULL, 0) == -1 && errno == ENODATA);
  CHECK(getxattr(loop_path, "user.key", NULL, 0) == -1 && errno == ELOOP);
  CHECK(!unlink(file.path));
  file.path[0] = 0;
  CHECK(getxattr(link_path, "user.key", NULL, 0) == -1 && errno == ENOENT);
  CHECK(lgetxattr(link_path, "user.key", NULL, 0) == -1 && errno == ENODATA);
out:
  if (directory_link[0])
    unlink(directory_link);
  xa_close(&directory);
  if (parent_link[0])
    unlink(parent_link);
  if (loop_path[0])
    unlink(loop_path);
  if (link_path[0])
    unlink(link_path);
  xa_close(&file);
  return failed;
}

int xa_admission(void) {
  return names_and_faults() || link_resolution();
}
