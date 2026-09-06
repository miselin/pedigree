#define _GNU_SOURCE
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/stat.h>

struct manifest {
  char magic[16];
  uint32_t version, payload_size;
  uint64_t inode;
  int32_t writer_mount;
  char token[64];
  unsigned char payload[64];
  struct fh_handle linked, stale;
};
static const char magic[] = "FH-PERSIST-1";
static const char payload[] = "persistent fanotify handle\n";

static int valid_token(const char* token) {
  size_t count = strlen(token);
  if (!count || count >= sizeof(((struct manifest*)0)->token))
    return 0;
  for (size_t n = 0; n < count; ++n) {
    char ch = token[n];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
          ch == '-' || ch == '_'))
      return 0;
  }
  return 1;
}
static int save(int directory, const char* name, const void* bytes, size_t size) {
  int fd = openat(directory, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0)
    return -1;
  int failed = fh_write_all(fd, bytes, size) || fsync(fd);
  if (close(fd))
    failed = 1;
  return failed ? -1 : 0;
}
static int load(int directory, const char* name, void* bytes, size_t size) {
  int fd = openat(directory, name, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return -1;
  char extra;
  int failed = fh_read_all(fd, bytes, size) || read(fd, &extra, 1) != 0;
  if (close(fd))
    failed = 1;
  return failed ? -1 : 0;
}

int fh_persistence(const char* base, const char* token, int write_stage) {
  int failed = 0, directory = -1, parent = -1, target = -1, stale = -1, decoded = -1, alias = -1;
  struct manifest manifest = {0};
  struct stat state, alias_state;
  char completion[128], actual_completion[128], parent_path[192];
  char original_path[224], renamed_path[224], alias_path[224], stale_path[224];
  CHECK(base[0] == '/' && strlen(base) < sizeof(parent_path) - 1 && valid_token(token));
  CHECK(base[strlen(base) - 1] != '/');
  const char* slash = strrchr(base, '/');
  size_t parent_length = slash == base ? 1 : (size_t)(slash - base);
  memcpy(parent_path, base, parent_length);
  parent_path[parent_length] = 0;
  snprintf(original_path, sizeof(original_path), "%s/target", base);
  snprintf(renamed_path, sizeof(renamed_path), "%s/renamed", base);
  snprintf(alias_path, sizeof(alias_path), "%s/alias", base);
  snprintf(stale_path, sizeof(stale_path), "%s/stale", base);
  int completion_length =
      snprintf(completion, sizeof(completion), "FH-HANDLE-COMPLETE 1 %s\n", token);
  CHECK(completion_length > 0 && (size_t)completion_length < sizeof(completion));
  if (write_stage) {
    CHECK(!mkdir(base, 0700));
    parent = open(parent_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(parent >= 0);
  }
  directory = open(base, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  CHECK(directory >= 0);
  if (write_stage) {
    memcpy(manifest.magic, magic, sizeof(magic));
    manifest.version = 1;
    manifest.payload_size = sizeof(payload) - 1;
    memcpy(manifest.payload, payload, manifest.payload_size);
    memcpy(manifest.token, token, strlen(token) + 1);
    target = open(original_path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    CHECK(target >= 0 && !fh_write_all(target, manifest.payload, manifest.payload_size));
    CHECK(!fh_export(target, &manifest.linked, &manifest.writer_mount));
    CHECK(!fstat(target, &state));
    manifest.inode = state.st_ino;
    CHECK(!link(original_path, alias_path) && !rename(original_path, renamed_path));
    CHECK(!fsync(target) && !close(target));
    target = -1;
    stale = open(stale_path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    int stale_mount;
    CHECK(stale >= 0 && !fh_export(stale, &manifest.stale, &stale_mount));
    CHECK(stale_mount == manifest.writer_mount && !fsync(stale));
    CHECK(!unlink(stale_path) && !close(stale));
    stale = -1;
    CHECK(!save(directory, "manifest", &manifest, sizeof(manifest)));
    CHECK(!fsync(directory) && !fsync(parent));
    CHECK(!save(directory, "complete", completion, (size_t)completion_length));
    CHECK(!fsync(directory) && !fsync(parent));
  } else {
    CHECK(!load(directory, "complete", actual_completion, (size_t)completion_length));
    CHECK(!memcmp(actual_completion, completion, (size_t)completion_length));
    CHECK(!load(directory, "manifest", &manifest, sizeof(manifest)));
    CHECK(!memcmp(manifest.magic, magic, sizeof(magic)) && manifest.version == 1);
    CHECK(!memcmp(manifest.token, token, strlen(token) + 1));
    CHECK(manifest.payload_size == sizeof(payload) - 1 &&
          !memcmp(manifest.payload, payload, sizeof(payload) - 1));
    CHECK(manifest.linked.handle_bytes && manifest.linked.handle_bytes <= 128 &&
          manifest.stale.handle_bytes && manifest.stale.handle_bytes <= 128);
    decoded = open_by_handle_at(directory, (struct file_handle*)&manifest.linked, O_RDONLY);
    CHECK(decoded >= 0 && !fstat(decoded, &state) && (uint64_t)state.st_ino == manifest.inode);
    unsigned char actual[sizeof(manifest.payload)];
    CHECK(!fh_read_all(decoded, actual, manifest.payload_size));
    CHECK(!memcmp(actual, manifest.payload, manifest.payload_size));
    CHECK(read(decoded, actual, 1) == 0);
    target = open(renamed_path, O_RDONLY | O_CLOEXEC);
    alias = open(alias_path, O_RDONLY | O_CLOEXEC);
    CHECK(target >= 0 && alias >= 0 && !fstat(alias, &alias_state));
    CHECK(alias_state.st_ino == state.st_ino && alias_state.st_nlink == 2);
    struct fh_handle fresh;
    int mount_id;
    CHECK(!fh_export(target, &fresh, &mount_id) && fh_equal(&fresh, &manifest.linked));
    CHECK(!fh_export(alias, &fresh, &mount_id) && fh_equal(&fresh, &manifest.linked));
    errno = 0;
    CHECK(open_by_handle_at(directory, (struct file_handle*)&manifest.stale, O_RDONLY) == -1 &&
          errno == ESTALE);
    printf("FANOTIFY-HANDLE-PERSIST: verified inode=%llu writer_mount=%d reader_mount=%d\n",
           (unsigned long long)manifest.inode, manifest.writer_mount, mount_id);
  }
out:
  if (alias >= 0 && close(alias))
    failed = 1;
  if (decoded >= 0 && close(decoded))
    failed = 1;
  if (stale >= 0 && close(stale))
    failed = 1;
  if (target >= 0 && close(target))
    failed = 1;
  if (directory >= 0 && close(directory))
    failed = 1;
  if (parent >= 0 && close(parent))
    failed = 1;
  return failed;
}
