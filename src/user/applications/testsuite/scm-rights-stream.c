/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>

extern void fail(void) __attribute__((noreturn));

union stream_control_buffer {
  struct cmsghdr alignment;
  unsigned char bytes[CMSG_SPACE(2 * sizeof(int))];
};

static ssize_t send_stream_rights(int socketFd, const int* descriptors, size_t descriptorCount,
                                  const void* payload, size_t payloadLength) {
  union stream_control_buffer control = {0};
  struct iovec vector = {(void*)payload, payloadLength};
  struct msghdr message = {
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
  return sendmsg(socketFd, &message, MSG_NOSIGNAL);
}

static ssize_t receive_stream_rights(int socketFd, int flags, void* payload, size_t payloadCapacity,
                                     size_t controlCapacity, int expectedCount, int* received,
                                     int* outputFlags) {
  union stream_control_buffer control = {0};
  struct iovec vector = {payload, payloadCapacity};
  struct msghdr message = {
      .msg_iov = &vector,
      .msg_iovlen = 1,
      .msg_control = control.bytes,
      .msg_controllen = controlCapacity,
  };
  const ssize_t result = recvmsg(socketFd, &message, flags);
  if (result < 0)
    return result;

  *outputFlags = message.msg_flags;
  if (!expectedCount) {
    if (message.msg_controllen)
      fail();
    return result;
  }

  struct cmsghdr* header = CMSG_FIRSTHDR(&message);
  if (!header || header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
      header->cmsg_len != CMSG_LEN((size_t)expectedCount * sizeof(int)))
    fail();
  memcpy(received, CMSG_DATA(header), (size_t)expectedCount * sizeof(int));
  return result;
}

static void expect_broken_pipe(int descriptor) {
  void (*previousSigpipe)(int) = signal(SIGPIPE, SIG_IGN);
  if (previousSigpipe == SIG_ERR)
    fail();
  errno = 0;
  const ssize_t result = write(descriptor, "x", 1);
  const int error = errno;
  if (signal(SIGPIPE, previousSigpipe) == SIG_ERR || result != -1 || error != EPIPE)
    fail();
}

static void test_ordering_and_sender_close(void) {
  int sockets[2];
  int channel[2];
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sockets) || pipe(channel))
    fail();

  char empty = 0;
  int received[2] = {-1, -1};
  int outputFlags = 0;
  errno = 0;
  if (receive_stream_rights(sockets[1], 0, &empty, 1, sizeof(union stream_control_buffer), 0,
                            received, &outputFlags) != -1 ||
      errno != EAGAIN)
    fail();

  if (write(sockets[0], "ab", 2) != 2 ||
      send_stream_rights(sockets[0], &channel[0], 1, "cd", 2) != 2 ||
      write(sockets[0], "ef", 2) != 2 || close(channel[0]) || close(sockets[0]))
    fail();

  char byte = 0;
  if (read(sockets[1], &byte, 1) != 1 || byte != 'a')
    fail();

  char marked[8] = {0};
  const ssize_t markedLength =
      receive_stream_rights(sockets[1], MSG_CMSG_CLOEXEC, marked, sizeof(marked),
                            CMSG_SPACE(sizeof(int)), 1, received, &outputFlags);
  if (markedLength != 2 || memcmp(marked, "bc", 2) || outputFlags || received[0] < 0 ||
      fcntl(received[0], F_GETFD) != FD_CLOEXEC || write(channel[1], "R", 1) != 1 ||
      read(received[0], &byte, 1) != 1 || byte != 'R' || close(received[0]) || close(channel[1]))
    fail();

  char tail[4] = {0};
  if (read(sockets[1], tail, sizeof(tail)) != 3 || memcmp(tail, "def", 3) ||
      read(sockets[1], tail, sizeof(tail)) != 0 || close(sockets[1]))
    fail();
}

static void test_split_iovec_marker(void) {
  int sockets[2];
  int channel[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) || pipe(channel) ||
      write(sockets[0], "a", 1) != 1)
    fail();

  union stream_control_buffer sendControl = {0};
  struct iovec sendVectors[2] = {
      {(void*)"", 0},
      {(void*)"bc", 2},
  };
  struct msghdr sendMessage = {
      .msg_iov = sendVectors,
      .msg_iovlen = 2,
      .msg_control = sendControl.bytes,
      .msg_controllen = CMSG_SPACE(sizeof(int)),
  };
  struct cmsghdr* sendHeader = CMSG_FIRSTHDR(&sendMessage);
  sendHeader->cmsg_len = CMSG_LEN(sizeof(int));
  sendHeader->cmsg_level = SOL_SOCKET;
  sendHeader->cmsg_type = SCM_RIGHTS;
  memcpy(CMSG_DATA(sendHeader), &channel[0], sizeof(channel[0]));
  if (sendmsg(sockets[0], &sendMessage, MSG_NOSIGNAL) != 2 || close(channel[0]))
    fail();

  char payload[4] = {0};
  union stream_control_buffer receiveControl = {0};
  struct iovec receiveVectors[2] = {
      {payload, 1},
      {payload + 1, sizeof(payload) - 1},
  };
  struct msghdr receiveMessage = {
      .msg_iov = receiveVectors,
      .msg_iovlen = 2,
      .msg_control = receiveControl.bytes,
      .msg_controllen = CMSG_SPACE(sizeof(int)),
  };
  if (recvmsg(sockets[1], &receiveMessage, 0) != 2 || memcmp(payload, "ab", 2) ||
      receiveMessage.msg_flags)
    fail();
  struct cmsghdr* receiveHeader = CMSG_FIRSTHDR(&receiveMessage);
  if (!receiveHeader || receiveHeader->cmsg_level != SOL_SOCKET ||
      receiveHeader->cmsg_type != SCM_RIGHTS || receiveHeader->cmsg_len != CMSG_LEN(sizeof(int)))
    fail();
  int received = -1;
  memcpy(&received, CMSG_DATA(receiveHeader), sizeof(received));
  char byte = 0;
  if (received < 0 || write(channel[1], "I", 1) != 1 || read(received, &byte, 1) != 1 ||
      byte != 'I' || close(received) || read(sockets[1], &byte, 1) != 1 || byte != 'c' ||
      close(channel[1]) || close(sockets[0]) || close(sockets[1]))
    fail();
}

static void test_plain_read_discards_rights(void) {
  int sockets[2];
  int channel[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) || pipe(channel) ||
      send_stream_rights(sockets[0], &channel[0], 1, "p", 1) != 1 || close(channel[0]))
    fail();

  char payload = 0;
  if (read(sockets[1], &payload, 1) != 1 || payload != 'p')
    fail();
  expect_broken_pipe(channel[1]);
  if (close(channel[1]) || close(sockets[0]) || close(sockets[1]))
    fail();
}

static void test_truncation(void) {
  int sockets[2];
  int channels[2][2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) || pipe(channels[0]) || pipe(channels[1]))
    fail();

  int sent[2] = {channels[0][0], channels[1][0]};
  if (send_stream_rights(sockets[0], sent, 2, "t", 1) != 1 || close(sent[0]) || close(sent[1]))
    fail();

  char payload = 0;
  int received[2] = {-1, -1};
  int outputFlags = 0;
  if (receive_stream_rights(sockets[1], 0, &payload, 1, CMSG_LEN(sizeof(int)), 1, received,
                            &outputFlags) != 1 ||
      payload != 't' || !(outputFlags & MSG_CTRUNC) || write(channels[0][1], "T", 1) != 1)
    fail();
  if (read(received[0], &payload, 1) != 1 || payload != 'T' || close(received[0]) ||
      close(channels[0][1]))
    fail();
  expect_broken_pipe(channels[1][1]);
  if (close(channels[1][1]) || close(sockets[0]) || close(sockets[1]))
    fail();
}

static void test_multiple_controls_and_fault_retry(void) {
  int sockets[2];
  int channels[2][2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) || pipe(channels[0]) || pipe(channels[1]))
    fail();

  if (send_stream_rights(sockets[0], &channels[0][0], 1, "12", 2) != 2 ||
      send_stream_rights(sockets[0], &channels[1][0], 1, "34", 2) != 2 || close(channels[0][0]) ||
      close(channels[1][0]))
    fail();

  char payload[8] = {0};
  int received[2] = {-1, -1};
  int outputFlags = 0;
  if (receive_stream_rights(sockets[1], 0, payload, sizeof(payload), CMSG_SPACE(sizeof(int)), 1,
                            received, &outputFlags) != 1 ||
      payload[0] != '1' || write(channels[0][1], "A", 1) != 1 ||
      read(received[0], payload, 1) != 1 || payload[0] != 'A' || close(received[0]) ||
      close(channels[0][1]))
    fail();

  memset(payload, 0, sizeof(payload));
  if (receive_stream_rights(sockets[1], 0, payload, sizeof(payload), CMSG_SPACE(sizeof(int)), 1,
                            received, &outputFlags) != 2 ||
      memcmp(payload, "23", 2) || write(channels[1][1], "B", 1) != 1 ||
      read(received[0], payload, 1) != 1 || payload[0] != 'B' || close(received[0]) ||
      close(channels[1][1]) || read(sockets[1], payload, 1) != 1 || payload[0] != '4')
    fail();

  int faultChannel[2];
  if (pipe(faultChannel) || send_stream_rights(sockets[0], &faultChannel[0], 1, "f", 1) != 1 ||
      close(faultChannel[0]))
    fail();

  char faultPayload = 0;
  struct iovec faultVector = {&faultPayload, sizeof(faultPayload)};
  struct msghdr faultMessage = {
      .msg_iov = &faultVector,
      .msg_iovlen = 1,
      .msg_control = (void*)-1,
      .msg_controllen = CMSG_SPACE(sizeof(int)),
  };
  errno = 0;
  if (recvmsg(sockets[1], &faultMessage, 0) != -1 || errno != EFAULT ||
      receive_stream_rights(sockets[1], 0, &faultPayload, 1, CMSG_SPACE(sizeof(int)), 1, received,
                            &outputFlags) != 1 ||
      faultPayload != 'f' || close(received[0]) || close(faultChannel[1]) || close(sockets[0]) ||
      close(sockets[1]))
    fail();
}

static void test_empty_payload_and_close_drain(void) {
  int sockets[2];
  int channel[2];
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sockets) || pipe(channel))
    fail();

  struct iovec emptyVector = {(void*)"", 0};
  struct msghdr emptyMessage = {
      .msg_iov = &emptyVector,
      .msg_iovlen = 1,
  };
  errno = 0;
  if (send_stream_rights(sockets[0], &channel[0], 1, "", 0) != -1 || errno != EINVAL ||
      sendmsg(sockets[0], &emptyMessage, MSG_NOSIGNAL) != 0 || close(channel[0]))
    fail();
  expect_broken_pipe(channel[1]);
  char payload = 0;
  errno = 0;
  if (recv(sockets[1], &payload, 1, 0) != -1 || errno != EAGAIN || close(channel[1]) ||
      close(sockets[0]) || close(sockets[1]))
    fail();

  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) || pipe(channel) ||
      send_stream_rights(sockets[0], &channel[0], 1, "c", 1) != 1 || close(channel[0]) ||
      close(sockets[1]))
    fail();
  expect_broken_pipe(channel[1]);
  if (close(channel[1]) || close(sockets[0]))
    fail();
}

void test_scm_rights_stream(void) {
  puts("Testing AF_UNIX stream descriptor passing... ");
  fflush(stdout);
  test_ordering_and_sender_close();
  test_split_iovec_marker();
  test_plain_read_discards_rights();
  test_truncation();
  test_multiple_controls_and_fault_retry();
  test_empty_payload_and_close_drain();
  puts("OK\n");
  fflush(stdout);
}
