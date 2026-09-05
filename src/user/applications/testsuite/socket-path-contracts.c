/* Copyright (c) 2026, Pedigree Developers. See LICENSE for licensing details. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

extern void fail(void) __attribute__((noreturn));

static void require(int condition, const char* operation) {
  if (!condition) {
    printf("SOCKET-PATH-CONTRACT: FAIL %s errno=%d\n", operation, errno);
    fail();
  }
}

static void rename_socket_paths(const char* directory) {
  struct sockaddr_un source_path = {.sun_family = AF_UNIX};
  struct sockaddr_un destination = {.sun_family = AF_UNIX};
  char regular_path[108];
  snprintf(source_path.sun_path, sizeof(source_path.sun_path), "%s/socket-source-%ld", directory,
           (long)getpid());
  snprintf(destination.sun_path, sizeof(destination.sun_path), "%s/socket-destination-%ld",
           directory, (long)getpid());
  snprintf(regular_path, sizeof(regular_path), "%s/socket-regular-%ld", directory, (long)getpid());
  const socklen_t source_length =
      offsetof(struct sockaddr_un, sun_path) + strlen(source_path.sun_path) + 1;
  const socklen_t destination_length =
      offsetof(struct sockaddr_un, sun_path) + strlen(destination.sun_path) + 1;
  int source = socket(AF_UNIX, SOCK_DGRAM, 0);
  int victim = socket(AF_UNIX, SOCK_DGRAM, 0);
  int source_peer = socket(AF_UNIX, SOCK_DGRAM, 0);
  int victim_peer = socket(AF_UNIX, SOCK_DGRAM, 0);
  require(source >= 0 && victim >= 0 && source_peer >= 0 && victim_peer >= 0 &&
              bind(source, (struct sockaddr*)&source_path, source_length) == 0 &&
              bind(victim, (struct sockaddr*)&destination, destination_length) == 0 &&
              connect(source_peer, (struct sockaddr*)&source_path, source_length) == 0 &&
              connect(victim_peer, (struct sockaddr*)&destination, destination_length) == 0,
          "prepare socket rename endpoints");
  require(rename(source_path.sun_path, destination.sun_path) == 0,
          "rename replaces an open socket pathname");
  struct stat metadata;
  errno = 0;
  require(lstat(source_path.sun_path, &metadata) == -1 && errno == ENOENT,
          "rename removes old socket name");
  char byte = 0;
  require(send(source_peer, "s", 1, 0) == 1 && recv(source, &byte, 1, 0) == 1 && byte == 's' &&
              send(victim_peer, "v", 1, 0) == 1 && recv(victim, &byte, 1, 0) == 1 && byte == 'v',
          "rename preserves both existing endpoint references");
  require(connect(victim_peer, (struct sockaddr*)&destination, destination_length) == 0 &&
              send(victim_peer, "n", 1, 0) == 1 && recv(source, &byte, 1, 0) == 1 && byte == 'n' &&
              close(victim) == 0,
          "new connection follows renamed endpoint");

  int regular = open(regular_path, O_CREAT | O_EXCL | O_RDWR, 0600);
  require(regular >= 0 && write(regular, "r", 1) == 1 &&
              rename(destination.sun_path, regular_path) == 0 && pread(regular, &byte, 1, 0) == 1 &&
              byte == 'r' && lstat(regular_path, &metadata) == 0 && S_ISSOCK(metadata.st_mode),
          "socket replaces backing file while old descriptor survives");
  int backing = open(source_path.sun_path, O_CREAT | O_EXCL | O_RDWR, 0600);
  require(backing >= 0 && write(backing, "f", 1) == 1 &&
              rename(source_path.sun_path, regular_path) == 0 &&
              stat(regular_path, &metadata) == 0 && S_ISREG(metadata.st_mode) &&
              send(source_peer, "u", 1, 0) == 1 && recv(source, &byte, 1, 0) == 1 && byte == 'u',
          "backing file replaces pathname without closing socket endpoint");
  require(close(source) == 0 && stat(regular_path, &metadata) == 0 && S_ISREG(metadata.st_mode) &&
              close(source_peer) == 0 && close(victim_peer) == 0 && close(regular) == 0 &&
              close(backing) == 0 && unlink(regular_path) == 0,
          "close preserves the renamed backing replacement");
}

void test_socket_path_contracts(void) {
  const int types[] = {SOCK_STREAM, SOCK_DGRAM};
  for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    snprintf(address.sun_path, sizeof(address.sun_path), "/tmp/socket-life-%ld-%d", (long)getpid(),
             types[i]);
    const socklen_t length = offsetof(struct sockaddr_un, sun_path) + strlen(address.sun_path) + 1;
    int bound = socket(AF_UNIX, types[i], 0);
    require(bound >= 0 && bind(bound, (struct sockaddr*)&address, length) == 0,
            "bind socket lifetime fixture");
    if (types[i] == SOCK_STREAM)
      require(listen(bound, 1) == 0, "listen lifetime fixture");
    int duplicate = dup(bound);
    require(duplicate >= 0 && close(bound) == 0, "close duplicated bound descriptor");
    struct stat metadata;
    require(lstat(address.sun_path, &metadata) == 0 && S_ISSOCK(metadata.st_mode),
            "duplicated socket retains pathname");
    int replacement = socket(AF_UNIX, types[i], 0);
    require(replacement >= 0, "replacement socket");
    errno = 0;
    require(bind(replacement, (struct sockaddr*)&address, length) == -1 && errno == EADDRINUSE,
            "bound name cannot be replaced by bind");
    require(close(duplicate) == 0 && lstat(address.sun_path, &metadata) == 0 &&
                S_ISSOCK(metadata.st_mode),
            "final close preserves socket pathname");
    errno = 0;
    require(connect(replacement, (struct sockaddr*)&address, length) == -1 && errno == ECONNREFUSED,
            "closed pathname refuses connections");
    require(unlink(address.sun_path) == 0 &&
                bind(replacement, (struct sockaddr*)&address, length) == 0 &&
                close(replacement) == 0 && unlink(address.sun_path) == 0,
            "explicit unlink permits rebinding");
  }

  struct sockaddr_un address = {.sun_family = AF_UNIX};
  snprintf(address.sun_path, sizeof(address.sun_path), "/tmp/socket-rebind-%ld", (long)getpid());
  const socklen_t length = offsetof(struct sockaddr_un, sun_path) + strlen(address.sun_path) + 1;
  int original = socket(AF_UNIX, SOCK_DGRAM, 0);
  int sender = socket(AF_UNIX, SOCK_DGRAM, 0);
  require(original >= 0 && sender >= 0 && bind(original, (struct sockaddr*)&address, length) == 0 &&
              connect(sender, (struct sockaddr*)&address, length) == 0 &&
              unlink(address.sun_path) == 0,
          "unlink a connected datagram endpoint");
  int replacement = socket(AF_UNIX, SOCK_DGRAM, 0);
  require(replacement >= 0 && bind(replacement, (struct sockaddr*)&address, length) == 0,
          "reuse unlinked pathname for a new endpoint");
  char byte = 0;
  require(sendto(sender, "n", 1, 0, (struct sockaddr*)&address, length) == 1 &&
              recv(replacement, &byte, 1, 0) == 1 && byte == 'n' && send(sender, "o", 1, 0) == 1,
          "explicit datagram destination preserves the default peer");
  require(recv(original, &byte, 1, 0) == 1 && byte == 'o' && close(original) == 0,
          "unlinked endpoint remains usable until close");
  errno = 0;
  require(send(sender, "x", 1, 0) == -1 && errno == ECONNREFUSED,
          "old connection cannot reach replacement endpoint");
  require(connect(sender, (struct sockaddr*)&address, length) == 0 &&
              send(sender, "n", 1, 0) == 1 && recv(replacement, &byte, 1, 0) == 1 && byte == 'n',
          "closing old endpoint preserves replacement pathname");
  require(close(sender) == 0 && close(replacement) == 0 && unlink(address.sun_path) == 0,
          "remove replacement endpoint");
  puts("SOCKET-PATH-CONTRACT: PASS close-unlink-rebind");
  rename_socket_paths("/tmp");
  puts("SOCKET-PATH-CONTRACT: PASS ramfs-rename-replacement");
  rename_socket_paths("");
  puts("SOCKET-PATH-CONTRACT: PASS ext2-rename-replacement");
}
