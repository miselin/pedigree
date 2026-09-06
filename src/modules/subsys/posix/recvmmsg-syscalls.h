/* Copyright (c) 2026, Pedigree Developers. */
#ifndef RECVMMSG_SYSCALLS_H
#define RECVMMSG_SYSCALLS_H

#include <stddef.h>

#include "linux-wait-abi.h"
#include <sys/socket.h>

class DescriptorLease;

struct LinuxMmsghdr {
  struct msghdr msg_hdr;
  unsigned int msg_len;
};

#if UINTPTR_MAX == UINT64_MAX
static_assert(sizeof(LinuxMmsghdr) == 64 && offsetof(LinuxMmsghdr, msg_len) == 56,
              "Linux amd64 mmsghdr layout must match musl.");
#endif

ssize_t posix_recvmsg_user_descriptor(const DescriptorLease& descriptor, struct msghdr* message,
                                      int flags, unsigned int* receivedLength = nullptr);
int posix_recvmmsg(int fd, LinuxMmsghdr* messages, unsigned int count, unsigned int flags,
                   LinuxKernelTimespec* timeout);

#endif
