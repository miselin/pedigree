/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/un.h>

extern void fail(void) __attribute__((noreturn));

union control_buffer {
  struct cmsghdr alignment;
  unsigned char bytes[CMSG_SPACE(2 * sizeof(int))];
};

static socklen_t unix_address(const char* path, struct sockaddr_un* address) {
  memset(address, 0, sizeof(*address));
  address->sun_family = AF_UNIX;
  if (strlen(path) >= sizeof(address->sun_path))
    fail();
  strcpy(address->sun_path, path);
  return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(path) + 1);
}

static ssize_t send_rights(int socketFd, const struct sockaddr_un* address, socklen_t addressLength,
                           const int* descriptors, size_t descriptorCount, char payload) {
  union control_buffer control = {0};
  struct iovec vector = {&payload, sizeof(payload)};
  struct msghdr message = {
      .msg_name = (void*)address,
      .msg_namelen = addressLength,
      .msg_iov = &vector,
      .msg_iovlen = 1,
      .msg_control = control.bytes,
      .msg_controllen = CMSG_SPACE(descriptorCount * sizeof(int)),
  };
  struct cmsghdr* header = CMSG_FIRSTHDR(&message);
  header->cmsg_len = CMSG_LEN(descriptorCount * sizeof(int));
  header->cmsg_level = SOL_SOCKET;
  header->cmsg_type = SCM_RIGHTS;
  memcpy(CMSG_DATA(header), descriptors, descriptorCount * sizeof(int));
  return sendmsg(socketFd, &message, 0);
}

static int receive_rights(int socketFd, int flags, size_t controlCapacity, int expectedCount,
                          int* received, int* outputFlags, char expectedPayload) {
  union control_buffer control = {0};
  char payload = 0;
  struct iovec vector = {&payload, sizeof(payload)};
  struct msghdr message = {
      .msg_iov = &vector,
      .msg_iovlen = 1,
      .msg_control = control.bytes,
      .msg_controllen = controlCapacity,
  };

  if (recvmsg(socketFd, &message, flags) != 1 || payload != expectedPayload)
    return -1;
  *outputFlags = message.msg_flags;
  if (!expectedCount) {
    return message.msg_controllen == 0 ? 0 : -1;
  }

  struct cmsghdr* header = CMSG_FIRSTHDR(&message);
  if (!header || header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
      header->cmsg_len != CMSG_LEN((size_t)expectedCount * sizeof(int)))
    return -1;
  memcpy(received, CMSG_DATA(header), (size_t)expectedCount * sizeof(int));
  return 0;
}

void test_scm_rights(void) {
  static const char socketPath[] = "/scm-rights.sock";
  struct sockaddr_un address;
  socklen_t addressLength = unix_address(socketPath, &address);
  int receiver = -1;
  int sender = -1;
  int stream = -1;

  puts("Testing AF_UNIX datagram descriptor passing... ");
  fflush(stdout);
  (void)unlink(socketPath);
  receiver = socket(AF_UNIX, SOCK_DGRAM, 0);
  sender = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (receiver < 0 || sender < 0 || bind(receiver, (struct sockaddr*)&address, addressLength))
    fail();

  int firstPipe[2];
  if (pipe(firstPipe) || fcntl(firstPipe[0], F_SETFD, FD_CLOEXEC) ||
      send_rights(sender, &address, addressLength, &firstPipe[0], 1, 'a') != 1 ||
      close(firstPipe[0]))
    fail();

  int received[2] = {-1, -1};
  int outputFlags = 0;
  if (receive_rights(receiver, 0, CMSG_SPACE(sizeof(int)), 1, received, &outputFlags, 'a') ||
      outputFlags || fcntl(received[0], F_GETFD) != 0 || write(firstPipe[1], "A", 1) != 1)
    fail();
  char byte = 0;
  if (read(received[0], &byte, 1) != 1 || byte != 'A' || close(received[0]) || close(firstPipe[1]))
    fail();

  int cloexecPipe[2];
  if (pipe(cloexecPipe) ||
      send_rights(sender, &address, addressLength, &cloexecPipe[0], 1, 'b') != 1 ||
      close(cloexecPipe[0]))
    fail();
  if (receive_rights(receiver, MSG_CMSG_CLOEXEC, CMSG_SPACE(sizeof(int)), 1, received, &outputFlags,
                     'b') ||
      fcntl(received[0], F_GETFD) != FD_CLOEXEC || close(received[0]) || close(cloexecPipe[1]))
    fail();

  int truncated[2][2];
  int sent[2];
  if (pipe(truncated[0]) || pipe(truncated[1]))
    fail();
  sent[0] = truncated[0][0];
  sent[1] = truncated[1][0];
  if (send_rights(sender, &address, addressLength, sent, 2, 'c') != 1 || close(sent[0]) ||
      close(sent[1]))
    fail();
  if (receive_rights(receiver, 0, CMSG_LEN(sizeof(int)), 1, received, &outputFlags, 'c') ||
      !(outputFlags & MSG_CTRUNC) || write(truncated[0][1], "C", 1) != 1)
    fail();
  byte = 0;
  if (read(received[0], &byte, 1) != 1 || byte != 'C' || close(received[0]) ||
      close(truncated[0][1]))
    fail();
  void (*previousSigpipe)(int) = signal(SIGPIPE, SIG_IGN);
  if (previousSigpipe == SIG_ERR)
    fail();
  errno = 0;
  int hiddenWrite = (int)write(truncated[1][1], "x", 1);
  int hiddenWriteError = errno;
  int hiddenClose = close(truncated[1][1]);
  if (signal(SIGPIPE, previousSigpipe) == SIG_ERR || hiddenWrite != -1 ||
      hiddenWriteError != EPIPE || hiddenClose)
    fail();

  int faultPipe[2];
  if (pipe(faultPipe) || send_rights(sender, &address, addressLength, &faultPipe[0], 1, 'd') != 1 ||
      close(faultPipe[0]))
    fail();
  char payload = 0;
  struct iovec vector = {&payload, sizeof(payload)};
  struct msghdr badReceive = {
      .msg_iov = &vector,
      .msg_iovlen = 1,
      .msg_control = (void*)-1,
      .msg_controllen = CMSG_SPACE(sizeof(int)),
  };
  errno = 0;
  if (recvmsg(receiver, &badReceive, 0) != -1 || errno != EFAULT ||
      receive_rights(receiver, 0, CMSG_SPACE(sizeof(int)), 1, received, &outputFlags, 'd') ||
      close(received[0]) || close(faultPipe[1]))
    fail();

  int unsupportedPipe[2];
  if (pipe(unsupportedPipe))
    fail();
  errno = 0;
  if (send_rights(sender, &address, addressLength, &sender, 1, 'e') != -1 || errno != EOPNOTSUPP)
    fail();
  stream = socket(AF_UNIX, SOCK_STREAM, 0);
  errno = 0;
  if (stream < 0 || send_rights(stream, 0, 0, &unsupportedPipe[0], 1, 'f') != -1 ||
      errno != ENOTCONN)
    fail();

  char malformedPayload = 'm';
  struct iovec malformedVector = {&malformedPayload, sizeof(malformedPayload)};
  union control_buffer malformedControl = {0};
  struct msghdr malformedMessage = {
      .msg_name = &address,
      .msg_namelen = addressLength,
      .msg_iov = &malformedVector,
      .msg_iovlen = 1,
      .msg_control = malformedControl.bytes,
      .msg_controllen = CMSG_SPACE(sizeof(int)),
  };
  struct cmsghdr* malformedHeader = CMSG_FIRSTHDR(&malformedMessage);
  malformedHeader->cmsg_len = CMSG_LEN(sizeof(int)) - 1;
  malformedHeader->cmsg_level = SOL_SOCKET;
  malformedHeader->cmsg_type = SCM_RIGHTS;
  memcpy(CMSG_DATA(malformedHeader), &unsupportedPipe[0], sizeof(int));
  errno = 0;
  if (sendmsg(sender, &malformedMessage, 0) != -1 || errno != EINVAL)
    fail();

  malformedHeader->cmsg_len = CMSG_LEN(sizeof(int));
  malformedHeader->cmsg_level = SOL_SOCKET;
  malformedHeader->cmsg_type = 0x7fffffff;
  errno = 0;
  if (sendmsg(sender, &malformedMessage, 0) != -1 || errno != EOPNOTSUPP)
    fail();

  union control_buffer misalignedControl = {0};
  malformedMessage.msg_control = misalignedControl.bytes + 1;
  malformedMessage.msg_controllen = CMSG_LEN(sizeof(int));
  struct cmsghdr alignedHeader = {
      .cmsg_len = CMSG_LEN(sizeof(int)),
      .cmsg_level = SOL_SOCKET,
      .cmsg_type = SCM_RIGHTS,
  };
  memcpy(misalignedControl.bytes + 1, &alignedHeader, sizeof(alignedHeader));
  memcpy(misalignedControl.bytes + 1 + CMSG_LEN(0), &unsupportedPipe[0], sizeof(int));
  errno = 0;
  if (sendmsg(sender, &malformedMessage, 0) != 1 ||
      receive_rights(receiver, 0, CMSG_SPACE(sizeof(int)), 1, received, &outputFlags, 'm') ||
      close(received[0]))
    fail();

  if (send_rights(sender, &address, addressLength, &unsupportedPipe[0], 1, 'n') != 1)
    fail();
  union control_buffer misalignedReceiveControl = {0};
  char misalignedPayload = 0;
  struct iovec misalignedVector = {&misalignedPayload, sizeof(misalignedPayload)};
  struct msghdr misalignedReceive = {
      .msg_iov = &misalignedVector,
      .msg_iovlen = 1,
      .msg_control = misalignedReceiveControl.bytes + 1,
      .msg_controllen = CMSG_LEN(sizeof(int)),
  };
  if (recvmsg(receiver, &misalignedReceive, 0) != 1 || misalignedPayload != 'n' ||
      misalignedReceive.msg_controllen != CMSG_LEN(sizeof(int)))
    fail();
  struct cmsghdr receivedHeader = {0};
  int misalignedReceived = -1;
  memcpy(&receivedHeader, misalignedReceiveControl.bytes + 1, sizeof(receivedHeader));
  memcpy(&misalignedReceived, misalignedReceiveControl.bytes + 1 + CMSG_LEN(0), sizeof(int));
  if (receivedHeader.cmsg_len != CMSG_LEN(sizeof(int)) || receivedHeader.cmsg_level != SOL_SOCKET ||
      receivedHeader.cmsg_type != SCM_RIGHTS || misalignedReceived < 0 || close(misalignedReceived))
    fail();

  errno = 0;
  int invalid = -1;
  if (send_rights(sender, &address, addressLength, &invalid, 1, 'g') != -1 || errno != EBADF)
    fail();

  if (close(unsupportedPipe[0]) || close(unsupportedPipe[1]) || close(stream) || close(sender) ||
      close(receiver))
    fail();
  (void)unlink(socketPath);
  puts("OK\n");
  fflush(stdout);
}
