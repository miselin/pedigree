#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <time.h>
#include <unistd.h>

#define RAMROOT "/tmp/compile-matrix-root"
#define PATHS "/root/compile-bench/ramroot-paths"
#define BYTE_LIMIT (512ULL * 1024 * 1024)
#define ENTRY_LIMIT 100000
#define FNV_START 14695981039346656037ULL

struct entry {
  char* path;
  char* link;
  mode_t mode;
  uint64_t bytes, hash;
};
static struct entry* entries;
static size_t entry_count, entry_capacity;
static uint64_t total_bytes, rounded_bytes, manifest_hash, total_files;
static int root_fd;

static void fail(const char* operation, const char* path) {
  dprintf(1, "COMPILEBENCH FAIL operation=%s path=%s errno=%d\n", operation, path, errno);
  exit(1);
}

static uint64_t hash_bytes(uint64_t hash, const void* data, size_t size) {
  const unsigned char* bytes = data;
  for (size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= 1099511628211ULL;
  }
  return hash;
}

static uint64_t hash_number(uint64_t hash, uint64_t number) {
  for (unsigned i = 0; i < 8; ++i) {
    unsigned char byte = number >> (8 * i);
    hash = hash_bytes(hash, &byte, 1);
  }
  return hash;
}

static int compare_names(const void* a, const void* b) {
  return strcmp(*(char* const*)a, *(char* const*)b);
}

static int compare_entries(const void* a, const void* b) {
  return strcmp(((const struct entry*)a)->path, ((const struct entry*)b)->path);
}

static void validate_path(const char* path) {
  if (path[0] != '/' || !path[1] || strlen(path) >= PATH_MAX)
    goto invalid;
  for (const char* p = path + 1; *p;) {
    const char* end = strchr(p, '/');
    size_t n = end ? (size_t)(end - p) : strlen(p);
    if (!n || n > NAME_MAX || (n == 1 && p[0] == '.') ||
        (n == 2 && p[0] == '.' && p[1] == '.'))
      goto invalid;
    p += n;
    if (*p)
      ++p;
  }
  if (path[strlen(path) - 1] == '/' || !strcmp(path, "/dev") ||
      !strncmp(path, "/dev/", 5) || !strcmp(path, "/proc") ||
      !strncmp(path, "/proc/", 6))
    goto invalid;
  return;
invalid:
  errno = EINVAL;
  fail("copy-path", path);
}

static void remember(struct entry entry) {
  if (entry_count == ENTRY_LIMIT) {
    errno = EFBIG;
    fail("entry-budget", entry.path);
  }
  if (entry_count == entry_capacity) {
    size_t capacity = entry_capacity ? entry_capacity * 2 : 128;
    void* next = realloc(entries, capacity * sizeof(*entries));
    if (!next)
      fail("entry-allocation", entry.path);
    entries = next;
    entry_capacity = capacity;
  }
  entries[entry_count++] = entry;
}

static uint64_t read_hash(int fd, uint64_t* bytes, int destination, const char* path) {
  unsigned char buffer[32768];
  uint64_t hash = FNV_START;
  *bytes = 0;
  for (;;) {
    ssize_t n = read(fd, buffer, sizeof(buffer));
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0)
      fail("read", path);
    if (!n)
      break;
    if (*bytes > BYTE_LIMIT - (uint64_t)n) {
      errno = EFBIG;
      fail("file-budget", path);
    }
    *bytes += n;
    hash = hash_bytes(hash, buffer, n);
    if (destination >= 0) {
      for (ssize_t done = 0; done < n;) {
        ssize_t wrote = write(destination, buffer + done, n - done);
        if (wrote < 0 && errno == EINTR)
          continue;
        if (wrote <= 0)
          fail("write", path);
        done += wrote;
      }
    }
  }
  return hash;
}

static void copy_entry(const char* path, int parent, const char* name, unsigned depth) {
  if (depth > 128) {
    errno = ELOOP;
    fail("tree-depth", path);
  }
  struct stat st;
  if (lstat(path, &st))
    fail("source-stat", path);
  struct entry entry = {strdup(path), NULL, st.st_mode, 0, 0};
  if (!entry.path)
    fail("path-allocation", path);
  if (S_ISDIR(st.st_mode)) {
    if (mkdirat(parent, name, 0700) && errno != EEXIST)
      fail("mkdir", path);
    int target = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    int source = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    DIR* directory = source < 0 ? NULL : fdopendir(source);
    if (target < 0 || !directory)
      fail("open-directory", path);
    char** names = NULL;
    size_t count = 0;
    struct dirent* item;
    errno = 0;
    while ((item = readdir(directory))) {
      if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, ".."))
        continue;
      if (count == ENTRY_LIMIT) {
        errno = EFBIG;
        fail("directory-budget", path);
      }
      char** next = realloc(names, (count + 1) * sizeof(*names));
      if (!next || !(next[count] = strdup(item->d_name)))
        fail("directory-allocation", path);
      names = next;
      ++count;
      errno = 0;
    }
    if (errno || closedir(directory))
      fail("read-directory", path);
    qsort(names, count, sizeof(*names), compare_names);
    remember(entry);
    for (size_t i = 0; i < count; ++i) {
      char child[PATH_MAX];
      if (snprintf(child, sizeof(child), "%s/%s", path, names[i]) >= (int)sizeof(child))
        fail("child-path", path);
      copy_entry(child, target, names[i], depth + 1);
      free(names[i]);
    }
    free(names);
    if (fchmod(target, st.st_mode & 07777) || close(target))
      fail("directory-mode", path);
    return;
  }
  if (S_ISREG(st.st_mode)) {
    if (st.st_size < 0 || (uint64_t)st.st_size > BYTE_LIMIT ||
        rounded_bytes > BYTE_LIMIT - (((uint64_t)st.st_size + 4095) & ~4095ULL)) {
      errno = EFBIG;
      fail("byte-budget", path);
    }
    rounded_bytes += ((uint64_t)st.st_size + 4095) & ~4095ULL;
    int source = open(path, O_RDONLY | O_NOFOLLOW);
    int target = openat(parent, name, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (source < 0 || target < 0)
      fail("open-file", path);
    entry.hash = read_hash(source, &entry.bytes, target, path);
    if (entry.bytes != (uint64_t)st.st_size || close(source) || lseek(target, 0, SEEK_SET) < 0)
      fail("source-size", path);
    uint64_t bytes;
    uint64_t hash = read_hash(target, &bytes, -1, path);
    if (bytes != entry.bytes || hash != entry.hash)
      fail("copy-content", path);
    if (fchmod(target, st.st_mode & 07777) || close(target))
      fail("file-mode", path);
    total_bytes += entry.bytes;
  } else if (S_ISLNK(st.st_mode)) {
    char text[PATH_MAX];
    ssize_t size = readlink(path, text, sizeof(text) - 1);
    if (size <= 0 || size == (ssize_t)sizeof(text) - 1)
      fail("readlink", path);
    text[size] = 0;
    entry.link = strdup(text);
    if (!entry.link || symlinkat(text, parent, name))
      fail("symlink", path);
    entry.bytes = size;
    entry.hash = hash_bytes(FNV_START, text, size);
  } else {
    errno = EOPNOTSUPP;
    fail("file-type", path);
  }
  remember(entry);
}

static void copy_path(const char* path) {
  validate_path(path);
  char components[PATH_MAX];
  strcpy(components, path + 1);
  char* rest = components;
  int parent = dup(root_fd);
  if (parent < 0)
    fail("root-dup", path);
  char* component;
  while ((component = strsep(&rest, "/"))) {
    if (!rest) {
      copy_entry(path, parent, component, 0);
      break;
    }
    if (mkdirat(parent, component, 0755) && errno != EEXIST)
      fail("parent-mkdir", path);
    int next = openat(parent, component, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (next < 0 || close(parent))
      fail("parent-open", path);
    parent = next;
  }
  if (close(parent))
    fail("parent-close", path);
}

static void audit(void) {
  struct stat root;
  if (stat("/", &root))
    fail("root-stat", "/");
  uint64_t manifest = FNV_START, files = 0, links = 0, directories = 0;
  qsort(entries, entry_count, sizeof(*entries), compare_entries);
  for (size_t i = 0; i < entry_count; ++i) {
    struct entry* entry = &entries[i];
    struct stat st, target;
    if (lstat(entry->path, &st) || (st.st_mode & S_IFMT) != (entry->mode & S_IFMT) ||
        (st.st_mode & 07777) != (entry->mode & 07777))
      fail("audit-mode", entry->path);
    if (stat(entry->path, &target) || target.st_dev != root.st_dev)
      fail("audit-root", entry->path);
    if (S_ISREG(entry->mode)) {
      int fd = open(entry->path, O_RDONLY | O_NOFOLLOW);
      uint64_t bytes;
      if (fd < 0)
        fail("audit-open", entry->path);
      uint64_t hash = read_hash(fd, &bytes, -1, entry->path);
      if (close(fd) || bytes != entry->bytes || hash != entry->hash)
        fail("audit-content", entry->path);
      ++files;
    } else if (entry->link) {
      char text[PATH_MAX];
      ssize_t size = readlink(entry->path, text, sizeof(text));
      if (size != (ssize_t)entry->bytes || memcmp(text, entry->link, entry->bytes))
        fail("audit-link", entry->path);
      ++links;
    } else
      ++directories;
    manifest = hash_number(manifest, strlen(entry->path));
    manifest = hash_bytes(manifest, entry->path, strlen(entry->path));
    manifest = hash_number(manifest, entry->mode);
    manifest = hash_number(manifest, entry->bytes);
    manifest = hash_number(manifest, entry->hash);
  }
  printf("RAMROOT manifest entries=%zu files=%llu directories=%llu symlinks=%llu "
         "bytes=%llu rounded_bytes=%llu fnv1a64=%016llx\n", entry_count,
         (unsigned long long)files, (unsigned long long)directories, (unsigned long long)links,
         (unsigned long long)total_bytes, (unsigned long long)rounded_bytes,
         (unsigned long long)manifest);
  total_files = files;
  manifest_hash = manifest;
}

int main(int argc, char** argv) {
  if (argc > 2 || (argc == 2 && strcmp(argv[1], "--stdio")))
    fail("arguments", "compile-matrix-ramroot");
  if (argc == 1) {
    int serial = open("/dev/ttyS0", O_RDWR);
    if (serial < 0 || dup2(serial, 0) < 0 || dup2(serial, 1) < 0 || dup2(serial, 2) < 0)
      fail("serial", "/dev/ttyS0");
    if (serial > 2)
      close(serial);
  }
  setvbuf(stdout, NULL, _IONBF, 0);
  umask(0);
  int dev = open("/dev", O_RDONLY | O_DIRECTORY);
  if (dev < 0 || (dev != 3 && dup2(dev, 3) < 0) || fcntl(3, F_SETFD, 0))
    fail("dev-directory", "/dev");
  if (dev != 3)
    close(dev);
  if ((mkdir(RAMROOT, 0700) && errno != EEXIST) || mount("none", RAMROOT, "ramfs", 0, NULL))
    fail("mount", RAMROOT);
  struct statfs fs;
  if (statfs(RAMROOT, &fs))
    fail("statfs", RAMROOT);
  printf("RAMROOT mount path=%s requested=ramfs reported_type=%lx\n", RAMROOT,
         (unsigned long)fs.f_type);
  root_fd = open(RAMROOT, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  FILE* list = fopen(PATHS, "r");
  if (root_fd < 0 || !list)
    fail("setup", PATHS);
  char** paths = NULL;
  char line[PATH_MAX + 2];
  size_t count = 0;
  while (fgets(line, sizeof(line), list)) {
    size_t length = strcspn(line, "\r\n");
    line[length] = 0;
    validate_path(line);
    if (count == ENTRY_LIMIT)
      fail("path-budget", PATHS);
    char** next = realloc(paths, (count + 1) * sizeof(*paths));
    if (!next || !(next[count] = strdup(line)))
      fail("path-allocation", PATHS);
    paths = next;
    ++count;
  }
  if (ferror(list) || fclose(list) || !count)
    fail("path-list", PATHS);
  qsort(paths, count, sizeof(*paths), compare_names);
  for (size_t i = 0; i < count; ++i) {
    copy_path(paths[i]);
    free(paths[i]);
  }
  free(paths);
  if (mkdirat(root_fd, "tmp", 01777) || mkdirat(root_fd, "proc", 0755) ||
      mount("none", RAMROOT "/proc", "proc", 0, NULL) ||
      symlinkat("/proc/self/fd/3", root_fd, "dev") || close(root_fd))
    fail("special-paths", RAMROOT);
  sync();
  struct timespec delay = {2, 0};
  while (nanosleep(&delay, &delay))
    if (errno != EINTR)
      fail("settle", RAMROOT);
  if (chroot(RAMROOT) || chdir("/"))
    fail("chroot", RAMROOT);
  audit();
  int null = open("/dev/null", O_RDWR);
  char byte;
  if (null < 0 || read(null, &byte, 1) != 0 || write(null, "X", 1) != 1 || close(null) ||
      access("/usr/bin/gcc", X_OK) || access("/root/compile-bench/compile-matrix", X_OK) ||
      access("/lib/ld-musl-x86_64.so.1", R_OK) || access("/usr/lib/libc.so", R_OK) ||
      access("/usr/lib/gcc/x86_64-pedigree/15.3.0/libstdc++.a", R_OK))
    fail("key-paths", "/");
  DIR* fds = opendir("/proc/self/fd");
  if (!fds)
    fail("descriptors", "/proc/self/fd");
  struct dirent* fd;
  while ((fd = readdir(fds))) {
    char* end;
    long number = strtol(fd->d_name, &end, 10);
    if (*fd->d_name && !*end && number > 3 && number != dirfd(fds))
      if (close((int)number))
        fail("descriptor-close", fd->d_name);
  }
  if (closedir(fds))
    fail("descriptors-close", "/proc/self/fd");
  printf("COMPILEBENCH RAMROOT READY files=%llu bytes=%llu fnv1a64=%016llx\n",
         (unsigned long long)total_files, (unsigned long long)total_bytes,
         (unsigned long long)manifest_hash);
  // Pedigree's driver opens its polling serial device; Linux retains its stdio gate.
  char* args[] = {"/root/compile-bench/compile-matrix", "--run", argc == 2 ? "--stdio" : NULL,
                  NULL};
  execv(args[0], args);
  fail("exec", args[0]);
}
