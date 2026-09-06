#define _GNU_SOURCE
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/stat.h>

static int open_flags(void) {
  int failed = 0, writer = -1, reader = -1;
  struct fh_file file = {.fd = -1};
  char bytes[20];
  struct stat state;
  CHECK(!fh_create(&file));
  writer = open_by_handle_at(file.fd, (struct file_handle*)&file.handle,
                             O_WRONLY | O_APPEND | O_CLOEXEC | O_NONBLOCK | O_LARGEFILE);
  CHECK(writer >= 0);
  CHECK(fcntl(writer, F_GETFD) & FD_CLOEXEC);
  CHECK((fcntl(writer, F_GETFL) & (O_ACCMODE | O_APPEND | O_NONBLOCK)) ==
        (O_WRONLY | O_APPEND | O_NONBLOCK));
  CHECK(write(writer, "Z", 1) == 1);
  CHECK(!fstat(file.fd, &state) && state.st_size == 17);
  CHECK(pread(file.fd, bytes, 1, 16) == 1 && bytes[0] == 'Z');
  reader = open_by_handle_at(file.fd, (struct file_handle*)&file.handle, O_RDONLY);
  CHECK(reader >= 0);
  errno = 0;
  CHECK(write(reader, "x", 1) == -1 && errno == EBADF);
  close(writer);
  writer = open_by_handle_at(file.fd, (struct file_handle*)&file.handle, O_RDWR | O_TRUNC);
  CHECK(writer >= 0 && !fstat(file.fd, &state) && state.st_size == 0);
  CHECK(write(writer, "new", 3) == 3);
  CHECK(pread(reader, bytes, 3, 0) == 3 && !memcmp(bytes, "new", 3));
out:
  if (reader >= 0)
    close(reader);
  if (writer >= 0)
    close(writer);
  fh_close(&file);
  return failed;
}

int fh_handles(void) {
  int failed = 0, root = -1, first = -1, second = -1, duplicate = -1, alias_fd = -1, path_fd = -1;
  struct fh_file file = {.fd = -1};
  struct fh_handle query, exact, changed;
  char alias[224] = {0}, moved[224] = {0}, bytes[8];
  struct stat state, reopened;
  int mount_id = -1;
  CHECK(!fh_create(&file));
  CHECK(file.mount_id > 0 && file.handle.handle_bytes && file.handle.handle_bytes <= 128);
  memset(&query, 0xa5, sizeof(query));
  query.handle_bytes = 0;
  errno = 0;
  CHECK(name_to_handle_at(AT_FDCWD, file.path, (struct file_handle*)&query, &mount_id, 0) == -1 &&
        errno == EOVERFLOW);
  CHECK(query.handle_bytes == file.handle.handle_bytes && mount_id == file.mount_id);
  for (size_t n = 0; n < sizeof(query.bytes); ++n)
    CHECK(query.bytes[n] == 0xa5);
  query.handle_bytes = file.handle.handle_bytes - 1;
  errno = 0;
  CHECK(name_to_handle_at(AT_FDCWD, file.path, (struct file_handle*)&query, &mount_id, 0) == -1 &&
        errno == EOVERFLOW && query.handle_bytes == file.handle.handle_bytes);
  query.handle_bytes = sizeof(query.bytes);
  CHECK(!name_to_handle_at(AT_FDCWD, file.path, (struct file_handle*)&query, &mount_id, 0));
  CHECK(fh_equal(&query, &file.handle));
  root = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  CHECK(root >= 0);
  exact.handle_bytes = sizeof(exact.bytes);
  CHECK(!name_to_handle_at(root, file.path + 1, (struct file_handle*)&exact, &mount_id, 0));
  CHECK(fh_equal(&exact, &file.handle) && mount_id == file.mount_id);
  path_fd = open(file.path, O_PATH | O_CLOEXEC);
  CHECK(path_fd >= 0 && !fh_export(path_fd, &exact, &mount_id));
  CHECK(fh_equal(&exact, &file.handle));
  CHECK(lseek(file.fd, 7, SEEK_SET) == 7);
  first = open_by_handle_at(root, (struct file_handle*)&file.handle, O_RDONLY);
  second = open_by_handle_at(file.fd, (struct file_handle*)&file.handle, O_RDONLY);
  CHECK(first >= 0 && second >= 0 && lseek(first, 0, SEEK_CUR) == 0);
  CHECK(read(first, bytes, 2) == 2 && !memcmp(bytes, "01", 2));
  CHECK(lseek(second, 0, SEEK_CUR) == 0 && lseek(file.fd, 0, SEEK_CUR) == 7);
  duplicate = dup(first);
  CHECK(duplicate >= 0 && read(duplicate, bytes, 1) == 1 && bytes[0] == '2');
  CHECK(lseek(first, 0, SEEK_CUR) == 3);
  CHECK(!fstat(file.fd, &state) && !fstat(first, &reopened) && state.st_ino == reopened.st_ino);
  snprintf(alias, sizeof(alias), "%s.alias", file.path);
  snprintf(moved, sizeof(moved), "%s.moved", file.path);
  CHECK(!link(file.path, alias) && !rename(file.path, moved));
  snprintf(file.path, sizeof(file.path), "%s", moved);
  moved[0] = 0;
  alias_fd = open(alias, O_RDONLY | O_CLOEXEC);
  CHECK(alias_fd >= 0 && !fh_export(alias_fd, &exact, &mount_id));
  CHECK(fh_equal(&exact, &file.handle));
  CHECK(!unlink(file.path));
  file.path[0] = 0;
  close(second);
  second = open_by_handle_at(root, (struct file_handle*)&file.handle, O_RDONLY);
  CHECK(second >= 0 && pread(second, bytes, 4, 0) == 4 && !memcmp(bytes, "0123", 4));
  changed = file.handle;
  changed.handle_type ^= 0x01000000;
  errno = 0;
  CHECK(open_by_handle_at(root, (struct file_handle*)&changed, O_RDONLY) == -1 && errno == ESTALE);
  changed = file.handle;
  changed.bytes[0] ^= 0x80;
  errno = 0;
  CHECK(open_by_handle_at(root, (struct file_handle*)&changed, O_RDONLY) == -1 && errno == ESTALE);
  CHECK(!unlink(alias));
  alias[0] = 0;
  errno = 0;
  CHECK(open_by_handle_at(root, (struct file_handle*)&file.handle, O_RDONLY) == -1 &&
        errno == ESTALE);
  CHECK(pread(first, bytes, 4, 0) == 4 && !memcmp(bytes, "0123", 4));
  CHECK(!open_flags());
out:
  if (path_fd >= 0)
    close(path_fd);
  if (alias_fd >= 0)
    close(alias_fd);
  if (duplicate >= 0)
    close(duplicate);
  if (second >= 0)
    close(second);
  if (first >= 0)
    close(first);
  if (root >= 0)
    close(root);
  if (alias[0])
    unlink(alias);
  if (moved[0])
    unlink(moved);
  fh_close(&file);
  return failed;
}
