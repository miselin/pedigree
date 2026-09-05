/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/syscallError.h"

#include <fcntl.h>

#include "FileDescriptor.h"
#include "PosixSubsystem.h"
#include "signalfd-syscalls.h"

namespace {
constexpr uint64_t Unblockable = (uint64_t(1) << 8) | (uint64_t(1) << 18);
constexpr int Nonblock = 0x800, CloseOnExec = 0x80000;
struct SignalFdRecord {
  uint32_t number;
  int32_t error, code;
  uint32_t pid, uid;
  int32_t fd;
  uint32_t timerId, band, overrun, trap;
  int32_t status, integer;
  uint64_t pointer, userTime, systemTime, address;
  uint16_t addressLowBit, padding;
  int32_t syscall;
  uint64_t callAddress;
  uint32_t architecture;
  uint8_t reserved[28];
};
static_assert(sizeof(SignalFdRecord) == 128, "Linux signalfd_siginfo layout");
static_assert(__builtin_offsetof(SignalFdRecord, pointer) == 48, "signalfd pointer offset");

SignalFdRecord encode(const PendingSignalRecord& source) {
  SignalFdRecord result = {};
  result.number = source.number;
  result.code = source.code;
  result.pid = source.pid;
  result.uid = source.uid;
  if (source.code == -2) {
    result.pid = 0;
    result.uid = 0;
    result.timerId = source.timerId;
    result.overrun = source.overrun;
  }
  result.integer = source.value;
  result.pointer = source.value;
  result.status = source.status;
  result.userTime = source.userTime;
  result.systemTime = source.systemTime;
  return result;
}
struct ScalarCopy {
  uint8_t* destination;
};
bool scalarCopy(void* opaque, const void* data, size_t size) {
  auto& cursor = *static_cast<ScalarCopy*>(opaque);
  if (!PosixSubsystem::copyToUser(cursor.destination, data, size))
    return false;
  cursor.destination += size;
  return true;
}
}  // namespace

class SignalFdState final : public ReadinessSource {
 public:
  explicit SignalFdState(uint64_t mask) : m_Mask(mask & ~Unblockable) {}
  uint64_t mask() const {
    return __atomic_load_n(&m_Mask, __ATOMIC_ACQUIRE);
  }
  void snapshot(uint64_t& mask, uint64_t& generation) {
    LockGuard<Mutex> guard(m_Lock);
    mask = m_Mask;
    generation = m_Generation;
  }
  bool closed() const {
    return __atomic_load_n(&m_Closed, __ATOMIC_ACQUIRE);
  }
  bool setMask(uint64_t mask) {
    {
      LockGuard<Mutex> guard(m_Lock);
      if (m_Closed) {
        SYSCALL_ERROR(BadFileDescriptor);
        return false;
      }
      __atomic_store_n(&m_Mask, mask & ~Unblockable, __ATOMIC_RELEASE);
      __atomic_add_fetch(&m_Generation, uint64_t(1), __ATOMIC_RELEASE);
    }
    notifyReadiness(ReadyRead);
    return true;
  }
  bool addOwner() {
    LockGuard<Mutex> guard(m_Lock);
    if (m_Closed)
      return false;
    ++m_Owners;
    return true;
  }
  void removeOwner() {
    bool last = false;
    {
      LockGuard<Mutex> guard(m_Lock);
      assert(m_Owners);
      last = !--m_Owners;
      if (last)
        __atomic_store_n(&m_Closed, true, __ATOMIC_RELEASE);
    }
    if (last)
      closeReadiness();
  }

 private:
  Mutex m_Lock;
  uint64_t m_Mask;
  uint64_t m_Generation = 0;
  size_t m_Owners = 0;
  bool m_Closed = false;
};

class SignalFdObserver final : public ReadinessObserver {
 public:
  SignalFdObserver(SignalFdView* view, bool maskChanged)
      : m_View(view), m_MaskChanged(maskChanged) {}
  void readinessChanged(ReadyMask) override {
    m_View->changed(m_MaskChanged);
  }

 private:
  SignalFdView* m_View;
  bool m_MaskChanged;
};

SignalFdView::SignalFdView(const SharedPointer<SignalFdState>& state,
                           const SharedPointer<PendingSignalContext>& context,
                           const SharedPointer<PendingSignalBinding>& binding)
    : m_State(state), m_Context(context), m_Binding(binding) {}
SignalFdView::~SignalFdView() {
  // Observer shims borrow this pointer only while their subscription admits a
  // callback; drain them before releasing any view state.
  m_MaskSubscription.reset();
  m_PendingSubscription.reset();
  closeReadiness();
}
bool SignalFdView::subscribe() {
  return m_Context->subscribeReadiness(
             ReadyRead | ReadyHangup,
             SharedPointer<ReadinessObserver>(new SignalFdObserver(this, false)),
             m_PendingSubscription) &&
         m_State->subscribeReadiness(
             ReadyRead | ReadyHangup,
             SharedPointer<ReadinessObserver>(new SignalFdObserver(this, true)),
             m_MaskSubscription);
}
void SignalFdView::changed(bool maskChanged) {
  if (maskChanged)
    m_Context->wake();
  notifyReadiness(ReadyRead | ReadyHangup);
}
ReadyMask SignalFdView::queryReady() {
  if (m_State->closed())
    return ReadyInvalid | ReadyHangup;
  return m_Context->query(m_Binding, m_State->mask());
}
ReadinessGenerations SignalFdView::readinessGenerations() {
  return generationsFor(m_Binding);
}
ReadyMask SignalFdView::queryCallerReady() {
  auto binding = m_Context->bind(Processor::information().getCurrentThread());
  if (!binding)
    return ReadyNone;
  if (m_State->closed())
    return ReadyInvalid | ReadyHangup;
  return m_Context->query(binding, m_State->mask());
}
ReadinessGenerations SignalFdView::callerReadinessGenerations() {
  auto binding = m_Context->bind(Processor::information().getCurrentThread());
  return binding ? generationsFor(binding) : ReadinessGenerations{};
}
ReadinessGenerations SignalFdView::generationsFor(
    const SharedPointer<PendingSignalBinding>& binding) {
  LockGuard<Mutex> guard(m_GenerationLock);
  uint64_t mask, maskGeneration;
  m_State->snapshot(mask, maskGeneration);
  ReadinessGenerations result;
  m_Context->query(binding, mask, &result);
  // Switching masks can lower the selected pending count, so keep the two
  // versions and caller identity separate instead of hiding a readiness edge.
  if (m_GenerationBinding.get() != binding.get() || m_MaskGeneration != maskGeneration ||
      m_PendingGeneration != result.read) {
    m_GenerationBinding = binding;
    m_MaskGeneration = maskGeneration;
    m_PendingGeneration = result.read;
    ++m_ReadGeneration;
  }
  result.read = m_ReadGeneration;
  return result;
}

SignalFd::SignalFd(uint64_t mask) : m_State(new SignalFdState(mask)) {}
SignalFd::~SignalFd() = default;
bool SignalFd::setMask(uint64_t mask) {
  return m_State->setMask(mask);
}
bool SignalFd::addDescriptorOwner() {
  return m_State->addOwner();
}
void SignalFd::removeDescriptorOwner() {
  m_State->removeOwner();
}
SharedPointer<SignalFdView> SignalFd::bindCaller() {
  Thread* thread = Processor::information().getCurrentThread();
  auto* owner = static_cast<PosixSubsystem*>(thread->getParent()->getSubsystem());
  auto context = owner->pendingSignalContext();
  auto binding = context->bind(thread);
  if (!binding || m_State->closed())
    return SharedPointer<SignalFdView>();
  SharedPointer<SignalFdView> view(new SignalFdView(m_State, context, binding));
  if (!view->subscribe())
    return SharedPointer<SignalFdView>();
  return view;
}
ssize_t SignalFd::readToUser(void* destination, size_t count, bool canBlock) {
  ScalarCopy cursor{static_cast<uint8_t*>(destination)};
  return readWithCopy(count, canBlock, scalarCopy, &cursor);
}
ssize_t SignalFd::readWithCopy(size_t count, bool canBlock, PosixDescriptorReadCopy copy,
                               void* opaque) {
  TerminationDeferral termination;
  if (count < sizeof(SignalFdRecord)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  auto view = bindCaller();
  if (!view) {
    SYSCALL_ERROR(BadFileDescriptor);
    return -1;
  }
  Thread* caller = Processor::information().getCurrentThread();
  auto context = view->m_Context;
  PendingSignalNotification notification(context);
  LockGuard<Mutex> guard(context->lock);
  size_t copied = 0;
  while (count - copied >= sizeof(SignalFdRecord)) {
    if (m_State->closed()) {
      if (copied) {
        caller->setErrno(0);
        return copied;
      }
      SYSCALL_ERROR(BadFileDescriptor);
      return -1;
    }
    PendingSignalReservation reservation;
    if (reservation.reserve(caller, m_State->mask())) {
      const PendingSignalRecord record = reservation.record();
      const SignalFdRecord result = encode(record);
      if (!copy(opaque, &result, sizeof(result))) {
        if (copied) {
          caller->setErrno(0);
          return copied;
        }
        SYSCALL_ERROR(BadAddress);
        return -1;
      }
      reservation.commit(record.overrun);
      copied += sizeof(result);
      continue;
    }
    if (copied) {
      caller->setErrno(0);
      return copied;
    }
    if (!canBlock) {
      SYSCALL_ERROR(NoMoreProcesses);
      return -1;
    }
    if (caller->hasEvents()) {
      SYSCALL_ERROR(Interrupted);
      return -1;
    }
    ConditionVariable::Error error = ConditionVariable::NoError;
    if (!context->changed.wait(context->lock, error)) {
      if (!ConditionVariable::mutexAcquired(error))
        guard.disown();
      SYSCALL_ERROR(Interrupted);
      return -1;
    }
  }
  caller->setErrno(0);
  return copied;
}

int posix_signalfd(int fd, const uint64_t* mask, size_t size) {
  return posix_signalfd4(fd, mask, size, 0);
}
int posix_signalfd4(int fd, const uint64_t* mask, size_t size, int flags) {
  if (size != sizeof(uint64_t) || (flags & ~(Nonblock | CloseOnExec))) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  uint64_t interest = 0;
  if (!PosixSubsystem::copyFromUser(&interest, mask, sizeof(interest))) {
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  if (fd != -1) {
    DescriptorLease descriptor;
    if (!acquireDescriptor(fd, descriptor)) {
      SYSCALL_ERROR(BadFileDescriptor);
      return -1;
    }
    auto signalFd = descriptor->getSignalFdImpl();
    if (!signalFd) {
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }
    if (!signalFd->setMask(interest))
      return -1;
    Processor::information().getCurrentThread()->setErrno(0);
    return fd;
  }
  const size_t number = getAvailableDescriptor();
  FileDescriptor* descriptor =
      new FileDescriptor(nullptr, 0, number, flags & CloseOnExec ? FD_CLOEXEC : 0,
                         O_RDWR | (flags & Nonblock ? O_NONBLOCK : 0));
  descriptor->setSignalFdImpl(SharedPointer<SignalFd>(new SignalFd(interest)));
  addDescriptor(number, descriptor);
  Processor::information().getCurrentThread()->setErrno(0);
  return number;
}
