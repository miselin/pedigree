/* Copyright (c) 2026, Pedigree Developers. See LICENSE for licensing details. */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <utime.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/wait.h>

extern void fail(void) __attribute__((noreturn));
extern void test_socket_path_contracts(void);

static void require(int condition, const char* operation) {
  if (!condition) {
    printf("USERCOPY-CONTRACT: FAIL %s errno=%d\n", operation, errno);
    fail();
  }
}

static void socket_buffers(void* inaccessible, size_t page) {
  int sockets[2];
  require(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "stream socketpair");
  int enabled = 1;
  require(setsockopt(sockets[0], SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) == 0,
          "snapshot socket option input");
  errno = 0;
  require(setsockopt(sockets[0], SOL_SOCKET, SO_REUSEADDR, inaccessible, sizeof(int)) == -1 &&
              errno == EFAULT,
          "socket option input protection");
  unsigned char option[sizeof(int) + 2];
  memset(option, 0xA7, sizeof(option));
  socklen_t option_length = 1;
  require(getsockopt(sockets[0], SOL_SOCKET, SO_TYPE, option + 1, &option_length) == 0 &&
              option_length == 1 && option[0] == 0xA7 && option[2] == 0xA7 &&
              option[1] == SOCK_STREAM,
          "truncated socket option output");
  option_length = 0;
  require(
      getsockopt(sockets[0], SOL_SOCKET, SO_TYPE, NULL, &option_length) == 0 && option_length == 0,
      "zero capacity socket option");
  errno = 0;
  require(
      getsockopt(sockets[0], SOL_SOCKET, SO_TYPE, option, inaccessible) == -1 && errno == EFAULT,
      "socket option length protection");

  char first[] = "ab", second[] = "cd";
  struct iovec send_vectors[] = {{NULL, 0}, {first, 2}, {NULL, 0}, {second, 2}};
  struct msghdr message = {.msg_iov = send_vectors, .msg_iovlen = 4};
  require(sendmsg(sockets[0], &message, 0) == 4, "gather stream payload");
  char* output = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  require(output != MAP_FAILED, "demand receive buffer");
  struct iovec receive_vectors[] = {{output, 1}, {NULL, 0}, {output + 1, 3}};
  message = (struct msghdr){.msg_iov = receive_vectors, .msg_iovlen = 3};
  require(recvmsg(sockets[1], &message, 0) == 4 && !memcmp(output, "abcd", 4),
          "scatter into demand pages");
  errno = 0;
  require(send(sockets[0], inaccessible, 4, 0) == -1 && errno == EFAULT, "stream send protection");
  require(send(sockets[0], "x", 1, 0) == 1, "queue protected receive");
  errno = 0;
  require(recv(sockets[1], inaccessible, 1, 0) == -1 && errno == EFAULT,
          "stream receive protection");
  require(munmap(output, page) == 0, "release receive buffer");
  close(sockets[0]);
  close(sockets[1]);

  struct sockaddr_un destination = {.sun_family = AF_UNIX};
  snprintf(destination.sun_path, sizeof(destination.sun_path), "/tmp/usercopy-dgram-%ld.sock",
           (long)getpid());
  const socklen_t destination_length =
      offsetof(struct sockaddr_un, sun_path) + strlen(destination.sun_path) + 1;
  unlink(destination.sun_path);
  sockets[0] = socket(AF_UNIX, SOCK_DGRAM, 0);
  require(sockets[0] >= 0, "create datagram sender");
  sockets[1] = socket(AF_UNIX, SOCK_DGRAM, 0);
  require(sockets[1] >= 0, "create datagram receiver");
  require(bind(sockets[1], (struct sockaddr*)&destination, destination_length) == 0,
          "bind datagram pathname");
  require(connect(sockets[0], (struct sockaddr*)&destination, destination_length) == 0,
          "connect datagram pathname");
  require(send(sockets[0], "packet", 6, 0) == 6, "datagram send");
  char truncated[4] = {0, 0, 0, 0x6A};
  require(recv(sockets[1], truncated, 3, MSG_TRUNC) == 6 && !memcmp(truncated, "pac", 3) &&
              truncated[3] == 0x6A,
          "datagram truncation copy extent");
  require(unlink(destination.sun_path) == 0, "remove datagram pathname");
  close(sockets[0]);
  close(sockets[1]);
  puts("USERCOPY-CONTRACT: PASS socket-buffers");
}

static void socket_addresses(void* inaccessible) {
  struct sockaddr_un address = {.sun_family = AF_UNIX};
  snprintf(address.sun_path, sizeof(address.sun_path), "/tmp/usercopy-%ld.sock", (long)getpid());
  unlink(address.sun_path);
  const socklen_t length = offsetof(struct sockaddr_un, sun_path) + strlen(address.sun_path) + 1;
  int listener = socket(AF_UNIX, SOCK_STREAM, 0);
  require(listener >= 0, "pathname socket");
  errno = 0;
  require(bind(listener, inaccessible, length) == -1 && errno == EFAULT, "bind address protection");
  require(bind(listener, (struct sockaddr*)&address, length) == 0 && listen(listener, 1) == 0,
          "bind and listen from address snapshot");
  int regular = open("/dev/null", O_RDWR);
  require(regular >= 0, "non-socket descriptor");
  errno = 0;
  require(connect(regular, (struct sockaddr*)&address, length) == -1 && errno == ENOTSOCK,
          "non-socket connect rejected");
  close(regular);

  pid_t child = fork();
  require(child >= 0, "accept peer fork");
  if (!child) {
    close(listener);
    int peer = socket(AF_UNIX, SOCK_STREAM, 0);
    if (peer < 0 || connect(peer, (struct sockaddr*)&address, length) != 0 ||
        send(peer, "k", 1, 0) != 1)
      _exit(11);
    close(peer);
    _exit(0);
  }
  unsigned char peer_address[3] = {0xA5, 0xA5, 0xA5};
  socklen_t peer_length = 1;
  int accepted = accept(listener, (struct sockaddr*)(peer_address + 1), &peer_length);
  require(accepted >= 0 && peer_length >= sizeof(sa_family_t) && peer_address[0] == 0xA5 &&
              peer_address[2] == 0xA5,
          "bounded accepted address output");
  char byte = 0;
  require(recv(accepted, &byte, 1, 0) == 1 && byte == 'k', "accepted stream usable");
  close(accepted);
  require(unlink(address.sun_path) == 0, "remove socket pathname");
  close(listener);
  int status = 0;
  require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "pathname peer completed");
  puts("USERCOPY-CONTRACT: PASS socket-addresses");
}

static void console_buffers(void* inaccessible) {
  int master = posix_openpt(O_RDWR | O_NOCTTY);
  require(master >= 0, "open console pair");
  struct winsize requested = {.ws_row = 31, .ws_col = 93}, actual = {0};
  require(ioctl(master, TIOCSWINSZ, &requested) == 0 && ioctl(master, TIOCGWINSZ, &actual) == 0 &&
              actual.ws_row == 31 && actual.ws_col == 93,
          "console window snapshot roundtrip");
  errno = 0;
  require(ioctl(master, TIOCSWINSZ, inaccessible) == -1 && errno == EFAULT,
          "console input protection");
  errno = 0;
  require(ioctl(master, TIOCGWINSZ, inaccessible) == -1 && errno == EFAULT,
          "console output protection");
  int enabled = 1;
  require(ioctl(master, FIONBIO, &enabled) == 0 && (fcntl(master, F_GETFL) & O_NONBLOCK),
          "nonblocking input snapshot");
  errno = 0;
  require(ioctl(master, FIONBIO, NULL) == -1 && errno == EFAULT &&
              (fcntl(master, F_GETFL) & O_NONBLOCK),
          "invalid nonblocking input preserves flags");
  close(master);
  puts("USERCOPY-CONTRACT: PASS console-buffers");
}

struct directory_record {
  uint64_t inode;
  int64_t offset;
  unsigned short length;
  unsigned char type;
  char name[];
};

static void directory_buffers(void* inaccessible) {
  int directory = open("/tmp", O_RDONLY | O_DIRECTORY);
  require(directory >= 0, "open directory");
  errno = 0;
  require(syscall(SYS_getdents64, directory, inaccessible, 256) == -1 && errno == EFAULT,
          "directory output protection");
  unsigned char records[512];
  ssize_t count = syscall(SYS_getdents64, directory, records, sizeof(records));
  require(count > 0, "directory output snapshot");
  for (size_t offset = 0; offset < (size_t)count;) {
    struct directory_record* record = (struct directory_record*)(records + offset);
    require(record->length >= offsetof(struct directory_record, name) + 1 &&
                record->length % 8 == 0 && record->length <= count - offset &&
                memchr(record->name, 0, record->length - offsetof(struct directory_record, name)),
            "aligned bounded directory record");
    offset += record->length;
  }
  close(directory);
  puts("USERCOPY-CONTRACT: PASS directory-buffers");
}

static void mapped_write_buffers(size_t page) {
  char path[80];
  snprintf(path, sizeof(path), "/tmp/usercopy-mapped-%ld", (long)getpid());
  int file = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
  require(file >= 0 && ftruncate(file, page) == 0 && pwrite(file, "mapped", 6, 0) == 6,
          "prepare file-backed write source");
  for (int operation = 0; operation < 4; ++operation) {
    char* source = mmap(NULL, page, PROT_READ, MAP_PRIVATE, file, 0);
    require(source != MAP_FAILED && lseek(file, 0, SEEK_SET) == 0,
            "cold mapping for same-file write");
    struct iovec vectors[] = {{source, 2}, {source + 2, 4}};
    ssize_t written;
    switch (operation) {
      case 0:
        written = write(file, source, 6);
        break;
      case 1:
        written = pwrite(file, source, 6, 0);
        break;
      case 2:
        written = writev(file, vectors, 2);
        break;
      default:
        written = pwritev(file, vectors, 2, 0);
        break;
    }
    require(written == 6, "write materializes source outside backing mutation lock");
    require(munmap(source, page) == 0, "release cold write source");
  }
  char actual[6];
  require(pread(file, actual, sizeof(actual), 0) == sizeof(actual) &&
              !memcmp(actual, "mapped", sizeof(actual)),
          "mapped source contents preserved");
  close(file);
  require(unlink(path) == 0, "remove mapped write fixture");
  puts("USERCOPY-CONTRACT: PASS mapped-write-buffers");
}

static void metadata_buffers(void* inaccessible) {
  char path[80], link[88];
  // The root Ext2 backend supplies the existing symlink capability.
  snprintf(path, sizeof(path), "/usercopy-metadata-%ld", (long)getpid());
  snprintf(link, sizeof(link), "%s.link", path);
  int file = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
  require(file >= 0 && write(file, "metadata", 8) == 8, "prepare metadata file");
  struct stat by_path, by_descriptor;
  require(stat(path, &by_path) == 0 && fstat(file, &by_descriptor) == 0 &&
              S_ISREG(by_path.st_mode) && by_path.st_size == 8 &&
              by_path.st_ino == by_descriptor.st_ino && by_descriptor.st_size == 8,
          "public stat snapshots agree");
  struct statfs filesystem;
  struct statvfs capacity;
  require(statfs(path, &filesystem) == 0 && filesystem.f_bsize > 0 &&
              fstatfs(file, &filesystem) == 0 && filesystem.f_bsize > 0 &&
              statvfs(path, &capacity) == 0 && capacity.f_bsize > 0 &&
              fstatvfs(file, &capacity) == 0 && capacity.f_bsize > 0,
          "public filesystem metadata snapshots");

  // musl transforms or clears metadata buffers itself, so test the kernel
  // copy boundary through the same syscall entries those wrappers use.
  errno = 0;
  require(syscall(SYS_stat, path, inaccessible) == -1 && errno == EFAULT, "stat output protection");
  errno = 0;
  require(syscall(SYS_fstat, file, inaccessible) == -1 && errno == EFAULT,
          "fstat output protection");
  errno = 0;
  require(syscall(SYS_newfstatat, AT_FDCWD, path, inaccessible, 0) == -1 && errno == EFAULT,
          "fstatat output protection");
  errno = 0;
  require(syscall(SYS_statfs, path, inaccessible) == -1 && errno == EFAULT,
          "statfs output protection");
  errno = 0;
  require(syscall(SYS_fstatfs, file, inaccessible) == -1 && errno == EFAULT,
          "fstatfs output protection");

  require(symlink(path, link) == 0, "prepare readlink target");
  unsigned char target[96];
  memset(target, 0xA7, sizeof(target));
  require(readlink(link, (char*)target + 1, 3) == 3 && !memcmp(target + 1, path, 3) &&
              target[0] == 0xA7 && target[4] == 0xA7,
          "readlink truncates without a terminator");
  memset(target, 0xA7, sizeof(target));
  size_t target_length = strlen(path);
  require(readlinkat(AT_FDCWD, link, (char*)target + 1, target_length) == (ssize_t)target_length &&
              !memcmp(target + 1, path, target_length) && target[0] == 0xA7 &&
              target[target_length + 1] == 0xA7,
          "readlinkat exact target extent");
  errno = 0;
  require(readlink(link, inaccessible, 3) == -1 && errno == EFAULT, "readlink output protection");

  // musl now routes timestamp wrappers through utimensat. These legacy
  // entries remain supported independently and need their own copy checks.
  struct utimbuf seconds = {.actime = 11, .modtime = 22};
  struct timeval fractions[2] = {{33, 100}, {44, 200}};
  require(syscall(SYS_utime, path, &seconds) == 0 && syscall(SYS_utimes, path, fractions) == 0 &&
              syscall(SYS_futimesat, AT_FDCWD, path, fractions) == 0 &&
              fstat(file, &by_descriptor) == 0,
          "legacy timestamp inputs snapshotted");
  errno = 0;
  require(syscall(SYS_utime, path, inaccessible) == -1 && errno == EFAULT,
          "legacy utime input protection");
  errno = 0;
  require(syscall(SYS_utimes, path, inaccessible) == -1 && errno == EFAULT,
          "legacy utimes input protection");
  errno = 0;
  require(syscall(SYS_futimesat, AT_FDCWD, path, inaccessible) == -1 && errno == EFAULT,
          "legacy futimesat input protection");
  require(fstat(file, &by_path) == 0 && by_path.st_atim.tv_sec == by_descriptor.st_atim.tv_sec &&
              by_path.st_atim.tv_nsec == by_descriptor.st_atim.tv_nsec &&
              by_path.st_mtim.tv_sec == by_descriptor.st_mtim.tv_sec &&
              by_path.st_mtim.tv_nsec == by_descriptor.st_mtim.tv_nsec,
          "rejected timestamp inputs preserve metadata");
  close(file);
  require(unlink(link) == 0 && unlink(path) == 0, "remove metadata fixtures");
  puts("USERCOPY-CONTRACT: PASS metadata-buffers");
}

static void working_directory_buffer(void* inaccessible) {
  char path[80];
  snprintf(path, sizeof(path), "/tmp/usercopy-cwd-%ld", (long)getpid());
  int original = open(".", O_RDONLY | O_DIRECTORY);
  require(original >= 0 && mkdir(path, 0700) == 0 && chdir(path) == 0, "prepare working directory");
  char expected[256];
  require(getcwd(expected, sizeof(expected)) == expected, "working directory baseline");
  const size_t length = strlen(expected);
  unsigned char output[sizeof(expected) + 2];
  memset(output, 0xA7, sizeof(output));
  require(getcwd((char*)output + 1, length + 1) == (char*)output + 1 &&
              !memcmp(output + 1, expected, length + 1) && output[0] == 0xA7 &&
              output[length + 2] == 0xA7,
          "getcwd exact capacity includes terminator");
  require(syscall(SYS_getcwd, output + 1, length + 1) == (long)length + 1 &&
              output[length + 1] == 0 && output[length + 2] == 0xA7,
          "getcwd syscall reports terminated extent");
  memset(output, 0xA7, sizeof(output));
  errno = 0;
  require(getcwd((char*)output + 1, length) == NULL && errno == ERANGE && output[1] == 0xA7,
          "getcwd insufficient capacity preserves output");
  errno = 0;
  require(getcwd(inaccessible, length + 1) == NULL && errno == EFAULT, "getcwd output protection");
  require(fchdir(original) == 0, "restore working directory");
  close(original);
  require(rmdir(path) == 0, "remove working directory fixture");
  puts("USERCOPY-CONTRACT: PASS working-directory-buffer");
}

void test_usercopy_contracts(void) {
  const size_t page = (size_t)sysconf(_SC_PAGESIZE);
  void* inaccessible = mmap(NULL, page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  require(inaccessible != MAP_FAILED, "protected user range");
  socket_buffers(inaccessible, page);
  socket_addresses(inaccessible);
  test_socket_path_contracts();
  console_buffers(inaccessible);
  directory_buffers(inaccessible);
  mapped_write_buffers(page);
  metadata_buffers(inaccessible);
  working_directory_buffer(inaccessible);
  require(munmap(inaccessible, page) == 0, "release protected user range");
  puts("USERCOPY-CONTRACT: PASS all");
}
