/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/utilities/lib.h"

#include <signal.h>

#include "PosixSubsystem.h"
#include "queued-signal.h"
#include "trace-state.h"

namespace {
void ignoreTraceSignal(int) {}
bool stopping(int signal) {
  return signal == 19 || signal == 20 || signal == 21 || signal == 22;
}
TraceSignalInfo signalInfo(SignalEvent& event) {
  PendingSignalRecord record;
  record.number = event.getNumber();
  record.code = event.getSignalCode();
  record.pid = event.getSenderProcess();
  record.uid = event.getSenderUser();
  record.value = event.getSignalValue();
  record.status = event.childStatus();
  record.userTime = event.childUserTime();
  record.systemTime = event.childSystemTime();
  event.timerInfo(record.timerId, record.overrun);
  LinuxQueuedSiginfo encoded;
  posix_signal_record_siginfo(record, encoded);
  TraceSignalInfo info;
  MemoryCopy(&info, &encoded, sizeof(info));
  return info;
}
void releaseRelation(void* value) {
  static_cast<TraceRelationRef*>(value)->reset();
}
void complete(SignalEvent& event) {
  int32_t timer = 0, overrun = 0;
  event.timerInfo(timer, overrun);
  event.completeSignalDelivery(overrun);
}
}  // namespace

TraceStatus PosixSubsystem::prepareTraceSignal(Thread& thread, int signal, int32_t pid,
                                               uint32_t uid, bool inheritReservation,
                                               UniquePointer<SignalEvent>& result) {
  UniquePointer<SignalEvent> prepared;
  {
    LockGuard<UnlikelyLock> handlers(m_SignalHandlersLock);
    auto* handler = m_SignalHandlers.lookup(signal);
    if (!handler)
      return TraceStatus::Missing;
    if (handler->pEvent)
      prepared = UniquePointer<SignalEvent>::adopt(
          static_cast<SignalEvent*>(handler->pEvent->cloneForDelivery()));
    else if (handler->type == 2)
      prepared = UniquePointer<SignalEvent>::allocate(
          reinterpret_cast<uintptr_t>(ignoreTraceSignal), signal, ~0UL, uint64_t(0), true, true,
          Event::HandlerPrivilege::Kernel, SignalEvent::DeliveryDisposition::DefaultAction);
  }
  if (!prepared)
    return TraceStatus::NoMemory;
  if (signal >= 35 && !inheritReservation) {
    auto reservation = posix_signal_reserve_queue(thread.getParent());
    if (!reservation)
      return TraceStatus::Full;
    prepared.get()->setDeliveryState(reservation);
  }
  prepared.get()->setSignalOrigin(0, pid, uid);
  prepared.get()->setProcessDirected(false);
  result = pedigree_std::move(prepared);
  return TraceStatus::Success;
}

bool PosixSubsystem::publishTraceSignal(Thread& thread, UniquePointer<SignalEvent>& prepared,
                                        SignalEvent* source) {
  if (!prepared)
    return false;
  PendingSignalNotification notification(m_PendingSignals);
  LockGuard<Mutex> pending(m_PendingSignals->lock);
  SignalEvent* replacement = prepared.get();
  const int signal = replacement->getNumber();
  if (source && source->hasDeliveryState())
    source->transferDeliveryStateTo(*replacement);
  if (signal == 18) {
    constexpr int stops[] = {19, 20, 21, 22};
    for (int stop : stops)
      thread.cullSignalEvent(stop);
    thread.getParent()->resume();
  } else if (stopping(signal)) {
    thread.cullSignalEvent(18);
  }
  replacement->setContinuationEpoch(thread.getParent()->getContinuationEpoch());
  replacement->setTraceBypass(!(thread.getSignalMask() & (uint64_t(1) << (signal - 1))));
  if (!thread.sendEvent(replacement))
    return false;
  prepared.releaseOwnership();
  m_PendingSignals->recordChange(signal, &thread, false);
  return true;
}

Subsystem::UserReturnEventResult PosixSubsystem::userReturnEvent(Thread& thread, Event& event,
                                                                 UserReturnFrame& frame) {
  if (getAbi() != LinuxAbi || !event.isSignalEvent() || event.getNumber() == 9)
    return UserReturnEventResult::Deliver;
  auto& signal = static_cast<SignalEvent&>(event);
  if (signal.consumeTraceBypass())
    return UserReturnEventResult::Deliver;
  TraceRelationRef relation;
  Thread::StackDiscardScope lifetime(releaseRelation, &relation);
  if (!m_TraceContext.acquireIncoming(relation))
    return UserReturnEventResult::Deliver;
  if (!signal.deliveryActive())
    return UserReturnEventResult::Consumed;
  const int number = signal.getNumber();
  auto decision = relation->stop(thread, frame, PosixTraceRelation::StopKind::Signal, number,
                                 signalInfo(signal), 0, signal.hasDeliveryState());
  Thread::StackDiscardScope preparedLifetime(
      [](void* value) { static_cast<UniquePointer<SignalEvent>*>(value)->reset(); },
      &decision.prepared);
  if (decision.terminal) {
    if (thread.getUnwindState() == Thread::Continue)
      thread.deferSignalExit(9);
    return UserReturnEventResult::Terminal;
  }
  if (!decision.signal) {
    complete(signal);
    return UserReturnEventResult::Consumed;
  }
  SignalDisposition disposition;
  if (decision.signal == number && stopping(number) && getSignalDisposition(number, disposition) &&
      disposition.type == 1) {
    const size_t epoch = signal.getContinuationEpoch();
    if (thread.getParent()->getContinuationEpoch() == epoch && !decision.detached) {
      decision = relation->stop(thread, frame, PosixTraceRelation::StopKind::Group, number,
                                TraceSignalInfo{}, epoch);
      if (decision.terminal) {
        if (thread.getUnwindState() == Thread::Continue)
          thread.deferSignalExit(9);
        return UserReturnEventResult::Terminal;
      }
    } else if (decision.detached) {
      thread.getParent()->suspendIfContinuationEpoch(number, epoch);
    }
    complete(signal);
    return UserReturnEventResult::Consumed;
  }
  if (decision.signal == number)
    return UserReturnEventResult::Deliver;
  if (!publishTraceSignal(thread, decision.prepared, &signal))
    return UserReturnEventResult::Terminal;
  return UserReturnEventResult::Consumed;
}

Subsystem::UserReturnResult PosixSubsystem::userReturnCheckpoint(Thread& thread,
                                                                 UserReturnFrame& frame) {
  TraceRelationRef relation;
  Thread::StackDiscardScope lifetime(releaseRelation, &relation);
  if (!m_TraceContext.acquireIncoming(relation) || !relation->takeExecTrap())
    return UserReturnResult::Continue;
  TraceSignalInfo info;
  int32_t number = 5, code = 0;
  MemoryCopy(reinterpret_cast<uint8_t*>(&info), &number, 4);
  MemoryCopy(reinterpret_cast<uint8_t*>(&info) + 8, &code, 4);
  auto decision = relation->stop(thread, frame, PosixTraceRelation::StopKind::Exec, 5, info);
  Thread::StackDiscardScope preparedLifetime(
      [](void* value) { static_cast<UniquePointer<SignalEvent>*>(value)->reset(); },
      &decision.prepared);
  if (decision.terminal) {
    if (thread.getUnwindState() == Thread::Continue)
      thread.deferSignalExit(9);
    return UserReturnResult::Terminal;
  }
  if (decision.signal && !publishTraceSignal(thread, decision.prepared, nullptr))
    return UserReturnResult::Terminal;
  return UserReturnResult::Continue;
}

bool PosixSubsystem::traceException(Thread& thread, int& signal, InterruptState& state,
                                    ExceptionType exception, uintptr_t address,
                                    uintptr_t errorCode) {
  auto* frame = thread.currentUserReturnFrame();
  TraceRelationRef relation;
  Thread::StackDiscardScope lifetime(releaseRelation, &relation);
  if (!frame || !m_TraceContext.acquireIncoming(relation))
    return false;
  SignalEvent source(0, signal);
  int32_t code = 128;
  switch (exception) {
    case PageFault:
      code = errorCode & 1 ? 2 : 1;
      break;
    case InvalidOpcode:
      code = 1;
      break;
    case GeneralProtectionFault:
    case FileMappingFault:
      code = 2;
      break;
    case DivideByZero:
      code = 1;
      break;
    case FpuError:
    case SpecialFpuError:
      code = 7;
      break;
    default:
      break;
  }
  source.setSignalOrigin(code, 0, 0);
  auto info = signalInfo(source);
  const uintptr_t fault = exception == PageFault || exception == FileMappingFault
                              ? address
                              : state.getInstructionPointer();
  ByteSet(reinterpret_cast<uint8_t*>(&info) + 16, 0, sizeof(info) - 16);
  MemoryCopy(reinterpret_cast<uint8_t*>(&info) + 16, &fault, sizeof(fault));
  auto decision =
      relation->stop(thread, *frame, PosixTraceRelation::StopKind::Signal, signal, info);
  Thread::StackDiscardScope preparedLifetime(
      [](void* value) { static_cast<UniquePointer<SignalEvent>*>(value)->reset(); },
      &decision.prepared);
  if (decision.terminal) {
    if (thread.getUnwindState() == Thread::Continue)
      thread.deferSignalExit(9);
    return true;
  }
  if (!decision.signal)
    return true;
  if (decision.signal != signal) {
    if (!publishTraceSignal(thread, decision.prepared, nullptr) &&
        thread.getUnwindState() == Thread::Continue)
      thread.deferSignalExit(9);
    return true;
  }
  SignalDisposition disposition;
  if (getSignalDisposition(signal, disposition) && disposition.type == 0)
    return false;
  // A default synchronous fault terminates at this return boundary without
  // manufacturing a second pending delivery and a second trace stop.
  if (disposition.type == 1)
    thread.deferSignalExit(signal);
  return true;
}
