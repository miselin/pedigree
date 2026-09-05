/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_MQUEUE_NETLINK_H
#define POSIX_MQUEUE_NETLINK_H

#include "pedigree/kernel/process/ConditionVariable.h"

#include "net-syscalls.h"

/** Netlink cookie delivery used by Linux SIGEV_THREAD message queue notifications. */
class MqueueNetlinkSocket final : public NetworkSyscalls {
 public:
  MqueueNetlinkSocket(int type, int protocol);
  ~MqueueNetlinkSocket() override;
  bool create() override;
  bool reserveCookie();
  void deliverCookie(const uint8_t cookie[32], bool removed);
  void lastDescriptorClosed() override;
  bool canPoll() const override;
  ReadyMask queryReady(bool reading, bool writing) override;
  ReadinessGenerations readinessGenerations() override;
  int connect(const struct sockaddr_storage*, socklen_t) override;
  ssize_t sendto_msg(const struct msghdr*, const SharedPointer<SocketRights>&) override;
  ssize_t recvfrom_msg(struct msghdr*, SharedPointer<SocketRights>*) override;
  int listen(int) override;
  int bind(const struct sockaddr_storage*, socklen_t) override;
  int accept(struct sockaddr_storage*, socklen_t*, int, DescriptorLease*) override;
  int shutdown(int) override;
  int getpeername(struct sockaddr_storage*, socklen_t*) override;
  int getsockname(struct sockaddr_storage*, socklen_t*) override;
  int setsockopt(int, int, const void*, socklen_t) override;
  int getsockopt(int, int, void*, socklen_t*) override;

 private:
  Mutex m_Lock;
  ConditionVariable m_Changed;
  uint8_t m_Cookies[64][32];
  size_t m_Head, m_Count, m_Reserved;
  bool m_Closed;
  ReadinessGenerations m_Generations;
};

#endif
