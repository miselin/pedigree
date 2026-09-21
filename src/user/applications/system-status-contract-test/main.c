/* Copyright (c) 2026, Pedigree Developers. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pedigree/log.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#define CHECK(expression)                                                                  \
  do {                                                                                     \
    if (!(expression)) {                                                                   \
      fprintf(stderr, "SYSTEM-STATUS-CONTRACT: FAIL line=%d errno=%d\n", __LINE__, errno); \
      return 1;                                                                            \
    }                                                                                      \
  } while (0)

static int memory_status(void) {
  int fd = open("/proc/meminfo", O_RDONLY);
  CHECK(fd >= 0);
  unsigned char guarded[16];
  memset(guarded, 0xa5, sizeof(guarded));
  CHECK(pread(fd, guarded + 1, 1, 0) == 1 && guarded[1] == 'M');
  CHECK(guarded[0] == 0xa5 && guarded[2] == 0xa5);
  CHECK(pread(fd, guarded + 1, 5, 4) == 5 && !memcmp(guarded + 1, "otal:", 5));
  CHECK(guarded[0] == 0xa5 && guarded[6] == 0xa5);
  CHECK(pread(fd, guarded, sizeof(guarded), LLONG_MAX - sizeof(guarded)) == 0);

  char contents[512];
  ssize_t bytes = pread(fd, contents, sizeof(contents) - 1, 0);
  CHECK(bytes > 0);
  contents[bytes] = 0;
  unsigned long long total = 0, freeKb = 0;
  CHECK(sscanf(contents, "MemTotal: %llu kB\nMemFree: %llu kB", &total, &freeKb) == 2);
  CHECK(total > 0 && freeKb <= total);
  CHECK(close(fd) == 0);
  puts("SYSTEM-STATUS-CONTRACT: MEMORY PASS");
  return 0;
}

static ssize_t read_status_file(const char* path, char* contents, size_t capacity) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  ssize_t bytes = read(fd, contents, capacity - 1);
  int saved = errno;
  close(fd);
  errno = saved;
  if (bytes >= 0)
    contents[bytes] = 0;
  return bytes;
}

static int process_status(void) {
  char contents[2048];
  unsigned long long uptime = 0, uptime_hundredths = 0, idle = 0, idle_hundredths = 0;
  CHECK(read_status_file("/proc/uptime", contents, sizeof(contents)) > 0);
  CHECK(sscanf(contents, "%llu.%llu %llu.%llu", &uptime, &uptime_hundredths, &idle,
               &idle_hundredths) == 4);
  CHECK(uptime_hundredths < 100 && idle_hundredths < 100);

  CHECK(read_status_file("/proc/stat", contents, sizeof(contents)) > 0);
  unsigned long long user = 0, kernel = 0, idle_ticks = 0;
  CHECK(sscanf(contents, "cpu %llu %*u %llu %llu", &user, &kernel, &idle_ticks) == 3);
  CHECK(strstr(contents, "\nbtime ") && strstr(contents, "\nprocs_running "));

  CHECK(read_status_file("/proc/loadavg", contents, sizeof(contents)) > 0);
  unsigned long long running = 0, tasks = 0, last_pid = 0;
  CHECK(sscanf(contents, "%*u.%*u %*u.%*u %*u.%*u %llu/%llu %llu", &running, &tasks,
               &last_pid) == 3);
  CHECK(tasks > 0 && running <= tasks && last_pid >= (unsigned long long)getpid());

  CHECK(read_status_file("/proc/cpuinfo", contents, sizeof(contents)) > 0);
  CHECK(strstr(contents, "processor\t: 0") && strstr(contents, "vendor_id\t: Pedigree"));
  CHECK(read_status_file("/proc/partitions", contents, sizeof(contents)) > 0);
  CHECK(strstr(contents, "major minor") && strstr(contents, "disk"));

  CHECK(read_status_file("/proc/self/status", contents, sizeof(contents)) > 0);
  char pid_line[64];
  snprintf(pid_line, sizeof(pid_line), "Pid:\t%ld\n", (long)getpid());
  CHECK(strstr(contents, "Name:\t") && strstr(contents, pid_line) && strstr(contents, "VmRSS:\t"));

  CHECK(read_status_file("/proc/self/stat", contents, sizeof(contents)) > 0);
  unsigned long long stat_pid = 0;
  CHECK(sscanf(contents, "%llu (", &stat_pid) == 1 && stat_pid == (unsigned long long)getpid());
  char* stat_field = strrchr(contents, ')');
  CHECK(stat_field);
  size_t stat_fields = 2;
  while (*++stat_field) {
    while (*stat_field == ' ' || *stat_field == '\n')
      ++stat_field;
    if (!*stat_field)
      break;
    ++stat_fields;
    while (*stat_field && *stat_field != ' ' && *stat_field != '\n')
      ++stat_field;
    --stat_field;
  }
  CHECK(stat_fields == 52);

  CHECK(read_status_file("/proc/self/statm", contents, sizeof(contents)) > 0);
  unsigned long long size = 0, resident = 0, shared = 0;
  CHECK(sscanf(contents, "%llu %llu %llu", &size, &resident, &shared) == 3);
  CHECK(resident <= size && shared <= resident);

  ssize_t command_bytes = read_status_file("/proc/self/cmdline", contents, sizeof(contents));
  CHECK(command_bytes > 1 && contents[command_bytes - 1] == 0);
  CHECK(strstr(contents, "system-status-contract-test"));

  struct stat expected, proc_path;
  CHECK(!stat(".", &expected) && !stat("/proc/self/cwd", &proc_path));
  CHECK(expected.st_dev == proc_path.st_dev && expected.st_ino == proc_path.st_ino);
  CHECK(!stat("/", &expected) && !stat("/proc/self/root", &proc_path));
  CHECK(expected.st_dev == proc_path.st_dev && expected.st_ino == proc_path.st_ino);
  char executable[PATH_MAX] = {};
  ssize_t executable_length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
  CHECK(executable_length > 0 && strstr(executable, "system-status-contract-test"));
  puts("SYSTEM-STATUS-CONTRACT: PROCFS PASS");
  return 0;
}

static int first_entry(const char* path, char* name, size_t capacity) {
  DIR* directory = opendir(path);
  if (!directory)
    return -1;
  struct dirent* entry;
  int found = 0;
  while ((entry = readdir(directory))) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
      continue;
    snprintf(name, capacity, "%s", entry->d_name);
    found = 1;
    break;
  }
  closedir(directory);
  return found;
}

static int sysfs_status(void) {
  struct stat status;
  CHECK(!stat("/sys/class", &status) && S_ISDIR(status.st_mode));
  CHECK(!stat("/sys/bus/pci/devices", &status) && S_ISDIR(status.st_mode));
  CHECK(!stat("/sys/firmware/efi", &status) && S_ISDIR(status.st_mode));

  char entry[128], path[256], contents[512];
  CHECK(first_entry("/sys/bus/pci/devices", entry, sizeof(entry)) == 1);
  snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/vendor", entry);
  CHECK(read_status_file(path, contents, sizeof(contents)) > 0 && !strncmp(contents, "0x", 2));
  snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/device", entry);
  CHECK(read_status_file(path, contents, sizeof(contents)) > 0 && !strncmp(contents, "0x", 2));

  int block = first_entry("/sys/class/block", entry, sizeof(entry));
  CHECK(block >= 0);
  if (block) {
    snprintf(path, sizeof(path), "/sys/class/block/%s/size", entry);
    CHECK(read_status_file(path, contents, sizeof(contents)) > 0);
  }

  int network = first_entry("/sys/class/net", entry, sizeof(entry));
  CHECK(network >= 0);
  if (network) {
    snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", entry);
    CHECK(read_status_file(path, contents, sizeof(contents)) > 0);
  }
  puts("SYSTEM-STATUS-CONTRACT: SYSFS PASS");
  return 0;
}

static int device_status(void) {
  struct stat status;
  CHECK(!stat("/dev/disk/by-uuid", &status) && S_ISDIR(status.st_mode));
  CHECK(!stat("/dev/disk/by-label", &status) && S_ISDIR(status.st_mode));
  CHECK(!stat("/dev/disk/by-partuuid", &status) && S_ISDIR(status.st_mode));
  CHECK(!stat("/dev/disk/by-partlabel", &status) && S_ISDIR(status.st_mode));

  char entry[128], path[256];
  CHECK(first_entry("/dev/disk/by-uuid", entry, sizeof(entry)) == 1);
  snprintf(path, sizeof(path), "/dev/disk/by-uuid/%s", entry);
  char target[64] = {};
  ssize_t length = readlink(path, target, sizeof(target) - 1);
  CHECK(length > 0 && !strncmp(target, "/dev/block/", 11));
  CHECK(!stat(path, &status) && S_ISBLK(status.st_mode));
  CHECK(!stat("/dev/disk/by-label/pedigree", &status) && S_ISBLK(status.st_mode));

  unsigned char byte = 0xa5;
  int fd = open("/dev/full", O_RDWR);
  CHECK(fd >= 0);
  CHECK(read(fd, &byte, 1) == 1 && byte == 0);
  errno = 0;
  CHECK(write(fd, "x", 1) == -1 && errno == ENOSPC);
  CHECK(close(fd) == 0);

  memset(target, 0, sizeof(target));
  length = readlink("/dev/stdin", target, sizeof(target) - 1);
  CHECK(length > 0 && !strcmp(target, "/proc/self/fd/0"));
  length = readlink("/dev/stdout", target, sizeof(target) - 1);
  CHECK(length > 0 && !strcmp(target, "/proc/self/fd/1"));
  length = readlink("/dev/stderr", target, sizeof(target) - 1);
  CHECK(length > 0 && !strcmp(target, "/proc/self/fd/2"));

  fd = open("/dev/shm/system-status-contract", O_CREAT | O_EXCL | O_RDWR, 0600);
  CHECK(fd >= 0 && write(fd, "shm", 3) == 3 && close(fd) == 0);
  CHECK(unlink("/dev/shm/system-status-contract") == 0);
  fd = open("/run/lock/system-status-contract", O_CREAT | O_EXCL | O_RDWR, 0600);
  CHECK(fd >= 0 && close(fd) == 0);
  CHECK(unlink("/run/lock/system-status-contract") == 0);
  puts("SYSTEM-STATUS-CONTRACT: DEVFS PASS");
  return 0;
}

static int network_status(void) {
  int fd = open("/proc/net/interfaces", O_RDONLY);
  CHECK(fd >= 0);
  char contents[8192];
  ssize_t bytes = read(fd, contents, sizeof(contents) - 1);
  CHECK(bytes > 0);
  contents[bytes] = 0;
  CHECK(strstr(contents, "  Device:") || !strcmp(contents, "No network devices.\n"));
  if (strstr(contents, "  Device:")) {
    CHECK(strstr(contents, "  MAC:") && strstr(contents, "  IPv4:") &&
          strstr(contents, "  Netmask:") && strstr(contents, "  Gateway:"));
  }
  CHECK(close(fd) == 0);
  fd = open("/proc/net/dev", O_RDONLY);
  CHECK(fd >= 0);
  bytes = read(fd, contents, sizeof(contents) - 1);
  CHECK(bytes > 0);
  contents[bytes] = 0;
  CHECK(strstr(contents, "Inter-") && strstr(contents, "Transmit"));
  CHECK(close(fd) == 0);
  fd = open("/proc/resolv.conf", O_RDONLY);
  CHECK(fd >= 0);
  bytes = read(fd, contents, sizeof(contents) - 1);
  CHECK(bytes >= 0);
  contents[bytes] = 0;
  CHECK(!bytes || strstr(contents, "nameserver "));
  CHECK(close(fd) == 0);
  puts("SYSTEM-STATUS-CONTRACT: NETWORK PASS");
  return 0;
}

static int kernel_log(void) {
  CHECK(syscall(SYS_syslog, 0, NULL, 0) == 0);
  CHECK(syscall(SYS_syslog, 1, NULL, 0) == 0);
  long capacity = syscall(SYS_syslog, 10, NULL, 0);
  CHECK(capacity > 0 && capacity < INT_MAX);
  CHECK(syscall(SYS_syslog, 3, NULL, 0) == 0);
  errno = 0;
  CHECK(syscall(SYS_syslog, 3, NULL, -1) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(syscall(SYS_syslog, 3, (void*)(uintptr_t)1, 1) == -1 && errno == EFAULT);
  errno = 0;
  CHECK(syscall(SYS_syslog, 42, NULL, 0) == -1 && errno == EINVAL);
  void* readonly = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(readonly != MAP_FAILED);
  errno = 0;
  CHECK(syscall(SYS_syslog, 3, readonly, 1) == -1 && errno == EFAULT);
  CHECK(munmap(readonly, 4096) == 0);

  char marker[80];
  snprintf(marker, sizeof(marker), "SYSTEM-STATUS-KLOG-%ld", (long)getpid());
  CHECK(pedigree_log(LOG_NOTICE, "%s", marker) == 0);
  char* buffer = malloc((size_t)capacity + 2);
  CHECK(buffer);
  for (int pass = 0; pass < 3; ++pass) {
    memset(buffer, 0xa5, (size_t)capacity + 2);
    long count = syscall(SYS_syslog, 3, buffer + 1, capacity);
    CHECK(count > 0 && count <= capacity);
    CHECK((unsigned char)buffer[0] == 0xa5 && (unsigned char)buffer[capacity + 1] == 0xa5);
    buffer[count + 1] = 0;
    CHECK(strstr(buffer + 1, marker));
    if (pass == 1) {
      errno = 0;
      CHECK(syscall(SYS_syslog, 4, buffer + 1, capacity) == -1 && errno == ENOSYS);
      errno = 0;
      CHECK(syscall(SYS_syslog, 5, NULL, 0) == -1 && errno == ENOSYS);
    }
  }
  unsigned char tail[3] = {0xa5, 0xa5, 0xa5};
  CHECK(syscall(SYS_syslog, 3, tail + 1, 1) == 1 && tail[1] == '\n');
  CHECK(tail[0] == 0xa5 && tail[2] == 0xa5);
  free(buffer);
  puts("SYSTEM-STATUS-CONTRACT: KERNEL-LOG PASS");
  return 0;
}

int main(void) {
  if (memory_status() || process_status() || sysfs_status() || device_status() ||
      network_status() || kernel_log())
    return 1;
  puts("SYSTEM-STATUS-CONTRACT: PASS");
  return 0;
}
