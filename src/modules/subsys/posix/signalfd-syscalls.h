/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_SIGNALFD_SYSCALLS_H
#define POSIX_SIGNALFD_SYSCALLS_H

#include "pedigree/kernel/process/Readiness.h"

#include "descriptor-read.h"
#include "queued-signal.h"

class SignalFdState;
class SignalFdObserver;

class SignalFdView final : public ReadinessSource {
 public:
  ~SignalFdView() override;
  ReadyMask queryReady();
  ReadinessGenerations readinessGenerations() override;
  ReadyMask queryCallerReady();
  ReadinessGenerations callerReadinessGenerations();

 private:
  friend class SignalFd;
  friend class SignalFdObserver;
  SignalFdView(const SharedPointer<SignalFdState>& state,
               const SharedPointer<PendingSignalContext>& context,
               const SharedPointer<PendingSignalBinding>& binding);
  bool subscribe();
  void changed(bool maskChanged);
  ReadinessGenerations generationsFor(const SharedPointer<PendingSignalBinding>& binding);
  SharedPointer<SignalFdState> m_State;
  SharedPointer<PendingSignalContext> m_Context;
  SharedPointer<PendingSignalBinding> m_Binding;
  ReadinessSubscription m_PendingSubscription;
  ReadinessSubscription m_MaskSubscription;
  Mutex m_GenerationLock;
  SharedPointer<PendingSignalBinding> m_GenerationBinding;
  uint64_t m_MaskGeneration = 0;
  uint64_t m_PendingGeneration = 0;
  uint64_t m_ReadGeneration = 0;
};

class SignalFd {
 public:
  explicit SignalFd(uint64_t mask);
  ~SignalFd();
  ssize_t readToUser(void* destination, size_t count, bool canBlock);
  ssize_t readWithCopy(size_t count, bool canBlock, PosixDescriptorReadCopy copy, void* context);
  SharedPointer<SignalFdView> bindCaller();
  bool setMask(uint64_t mask);
  bool addDescriptorOwner();
  void removeDescriptorOwner();

 private:
  SharedPointer<SignalFdState> m_State;
};

int posix_signalfd(int fd, const uint64_t* mask, size_t size);
int posix_signalfd4(int fd, const uint64_t* mask, size_t size, int flags);
#endif
