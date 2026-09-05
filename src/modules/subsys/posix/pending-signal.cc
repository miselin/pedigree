/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/SignalEvent.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/utilities/lib.h"

#include "PosixSubsystem.h"
#include "queued-signal.h"

void PendingSignalContext::attach(Process* process) {
  LockGuard<Mutex> guard(lock);
  if (!m_Closed && (!m_Process || m_Process == process))
    m_Process = process;
}
void PendingSignalContext::close() {
  if (__atomic_load_n(&m_Closed, __ATOMIC_ACQUIRE))
    return;
  {
    LockGuard<Mutex> guard(lock);
    __atomic_store_n(&m_Closed, true, __ATOMIC_RELEASE);
    m_Process = nullptr;
    for (auto& binding : m_Bindings)
      binding->alive = false;
    m_Bindings.clear();
    recordChange();
  }
  closeReadiness();
}
void PendingSignalContext::retireThread(Thread* thread) {
  {
    LockGuard<Mutex> guard(lock);
    for (auto it = m_Bindings.begin(); it != m_Bindings.end();) {
      if ((*it)->thread == thread) {
        (*it)->alive = false;
        it = m_Bindings.erase(it);
      } else
        ++it;
    }
    recordChange();
  }
  publish();
}
SharedPointer<PendingSignalBinding> PendingSignalContext::bind(Thread* thread) {
  LockGuard<Mutex> guard(lock);
  if (m_Closed || !thread || thread->getParent() != m_Process || !thread->acceptingEvents())
    return SharedPointer<PendingSignalBinding>();
  for (auto& binding : m_Bindings)
    if (binding->thread == thread && binding->alive)
      return binding;
  SharedPointer<PendingSignalBinding> binding(new PendingSignalBinding);
  binding->thread = thread;
  m_Bindings.pushBack(binding);
  return binding;
}
ReadyMask PendingSignalContext::query(const SharedPointer<PendingSignalBinding>& binding,
                                      uint64_t mask, ReadinessGenerations* generations) {
  LockGuard<Mutex> guard(lock);
  if (m_Closed || !binding || !binding->alive)
    return ReadyInvalid | ReadyHangup;
  Scheduler::ProcessLease process;
  Process::ThreadLease thread;
  if (!Scheduler::instance().acquireProcess(process, m_Process) ||
      !process->acquireThread(thread, binding->thread) || !thread->acceptingEvents())
    return ReadyInvalid | ReadyHangup;
  if (generations) {
    for (size_t i = 0; i < 64; ++i)
      if (mask & (uint64_t(1) << i))
        generations->read += m_ProcessGenerations[i] + binding->generations[i];
  }
  return posix_matching_pending(thread.get(), mask) ? ReadyRead : ReadyNone;
}
void PendingSignalContext::recordChange(size_t signal, Thread* target, bool processDirected) {
  if (signal && signal <= 64) {
    if (processDirected)
      ++m_ProcessGenerations[signal - 1];
    else
      for (auto& binding : m_Bindings)
        if (binding->alive && binding->thread == target)
          ++binding->generations[signal - 1];
  }
  __atomic_add_fetch(&m_Version, uint64_t(1), __ATOMIC_RELEASE);
  changed.broadcast();
}
void PendingSignalContext::publish() {
  notifyReadiness(ReadyRead | ReadyHangup);
}
void PendingSignalContext::wake() {
  LockGuard<Mutex> guard(lock);
  changed.broadcast();
}

bool posix_matching_pending(Thread* caller, uint64_t mask) {
  if (caller->pendingSignalMask() & mask)
    return true;
  Process* process = caller->getParent();
  for (size_t i = process->getNumThreads(); i > 0; --i) {
    Process::ThreadLease thread;
    if (process->acquireThread(thread, i - 1) && thread.get() != caller &&
        (thread->pendingSignalMask(true) & mask))
      return true;
  }
  return false;
}

PendingSignalReservation::~PendingSignalReservation() {
  rollback();
}
bool PendingSignalReservation::reserve(Thread* caller, uint64_t mask) {
  Process* process = caller->getParent();
  while (true) {
    Process::ThreadLease selected;
    uint64_t available = 0, firstSequence = ~uint64_t(0);
    for (size_t i = process->getNumThreads(); i > 0; --i) {
      Process::ThreadLease thread;
      if (!process->acquireThread(thread, i - 1))
        continue;
      const uint64_t candidate = thread->pendingSignalMask(thread.get() != caller) & mask;
      const uint64_t sequence =
          candidate
              ? thread->pendingSignalOrder(__builtin_ctzll(candidate) + 1, thread.get() != caller)
              : ~uint64_t(0);
      if (candidate && (!available || __builtin_ctzll(candidate) < __builtin_ctzll(available) ||
                        (__builtin_ctzll(candidate) == __builtin_ctzll(available) &&
                         sequence < firstSequence))) {
        available = candidate & (~candidate + 1);
        firstSequence = sequence;
        selected = pedigree_std::move(thread);
      }
    }
    if (!selected)
      return false;
    Event::Delivery delivery =
        selected->reservePendingSignal(available, selected.get() != caller, firstSequence);
    if (!delivery)
      continue;
    m_Caller = caller;
    m_Selected = pedigree_std::move(selected);
    m_Delivery = pedigree_std::move(delivery);
    return true;
  }
}
PendingSignalRecord PendingSignalReservation::record() const {
  auto* signal = static_cast<SignalEvent*>(m_Delivery.get());
  PendingSignalRecord result;
  result.number = signal->getNumber();
  result.code = signal->getSignalCode();
  result.pid = signal->getSenderProcess();
  result.uid = signal->getSenderUser();
  result.value = signal->getSignalValue();
  signal->timerInfo(result.timerId, result.overrun);
  result.status = signal->childStatus();
  result.userTime = signal->childUserTime();
  result.systemTime = signal->childSystemTime();
  return result;
}
void PendingSignalReservation::commit(int32_t overrun) {
  if (!m_Delivery)
    return;
  static_cast<SignalEvent*>(m_Delivery.get())->completeSignalDelivery(overrun);
  m_Delivery.reset();
  static_cast<PosixSubsystem*>(m_Caller->getParent()->getSubsystem())
      ->pendingSignalContext()
      ->recordChange();
  m_Selected.reset();
}
void PendingSignalReservation::rollback() {
  if (!m_Delivery)
    return;
  auto* signal = static_cast<SignalEvent*>(m_Delivery.get());
  if (!m_Selected->restorePendingSignal(m_Delivery) && signal->isProcessDirected())
    m_Caller->sendEvent(signal);
  m_Delivery.reset();
  static_cast<PosixSubsystem*>(m_Caller->getParent()->getSubsystem())
      ->pendingSignalContext()
      ->recordChange();
  m_Selected.reset();
}

void posix_signal_record_siginfo(const PendingSignalRecord& signal, LinuxQueuedSiginfo& info) {
  ByteSet(&info, 0, sizeof(info));
  auto put32 = [&](size_t offset, int32_t value) { MemoryCopy(info.bytes + offset, &value, 4); };
  auto put64 = [&](size_t offset, uint64_t value) { MemoryCopy(info.bytes + offset, &value, 8); };
  put32(0, signal.number);
  put32(8, signal.code);
  put32(16, signal.pid);
  put32(20, signal.uid);
  put64(24, signal.value);
  if (signal.code == -2) {
    put32(16, signal.timerId);
    put32(20, signal.overrun);
  } else if (signal.number == 17 && signal.code > 0 && signal.code <= 6) {
    put32(24, signal.status);
    put64(32, signal.userTime);
    put64(40, signal.systemTime);
  }
}
