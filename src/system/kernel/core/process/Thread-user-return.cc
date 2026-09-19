/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/UserReturnFrame.h"

Thread::UserReturnFrameScope::UserReturnFrameScope(Thread& owner, UserReturnFrame& frame)
    : m_Owner(&owner), m_StateLevel(0), m_Frame(&frame), m_Previous(nullptr), m_Record() {
  EnsureInterrupts interrupts(false);
  if (Processor::information().getCurrentThread() != &owner || frame.m_Owner != &owner)
    FATAL("User-return frame scope has a foreign owner");
  m_StateLevel = owner.m_nStateLevel;
  m_Previous = owner.m_StateLevels[m_StateLevel].m_UserReturnFrame;
  owner.armStateCleanup(m_Record, &UserReturnFrameScope::restore, this);
  owner.m_StateLevels[m_StateLevel].m_UserReturnFrame = &frame;
}

Thread::UserReturnFrameScope::~UserReturnFrameScope() {
  EnsureInterrupts interrupts(false);
  if (m_Owner) {
    m_Owner->disarmStateCleanup(m_Record);
    restore(this);
  }
}

void Thread::UserReturnFrameScope::restore(void* context) {
  auto& scope = *static_cast<UserReturnFrameScope*>(context);
  EnsureInterrupts interrupts(false);
  if (!scope.m_Owner)
    return;
  if (Processor::information().getCurrentThread() != scope.m_Owner)
    FATAL("User-return frame scope retired on a foreign Thread");
  auto& slot = scope.m_Owner->m_StateLevels[scope.m_StateLevel].m_UserReturnFrame;
  if (slot != scope.m_Frame)
    FATAL("User-return frame scopes retired out of order");
  slot = scope.m_Previous;
  scope.m_Owner = nullptr;
}

UserReturnFrame* Thread::currentUserReturnFrame() const {
  EnsureInterrupts interrupts(false);
  if (Processor::information().getCurrentThread() != this)
    return nullptr;
  return m_StateLevels[m_nStateLevel].m_UserReturnFrame;
}

bool Thread::tryRequireSignalFrames() {
  LockGuard<Spinlock> guard(m_Lock);
  if (m_bShutdown || getUnwindState() != Continue || m_LegacyUserCallbackPins ||
      m_SignalFramesRequired)
    return false;
  __atomic_store_n(&m_SignalFramesRequired, true, __ATOMIC_RELEASE);
  markUserReturnWorkFlag(UserReturnSignalFrames);
  return true;
}

void Thread::clearSignalFrameRequirement() {
  LockGuard<Spinlock> guard(m_Lock);
  __atomic_store_n(&m_SignalFramesRequired, false, __ATOMIC_RELEASE);
  clearUserReturnWorkFlag(UserReturnSignalFrames);
}

bool Thread::eventNeedsUserReturnFrameUnlocked(Event* event) const {
  return event->requiresExactUserReturnState() ||
         (m_SignalFramesRequired && event->isSignalEvent() && event->getNumber() != 9);
}

void Thread::setUserReturnSignalParked(bool parked) {
  LockGuard<Spinlock> guard(m_Lock);
  if (Processor::information().getCurrentThread() != this)
    FATAL("User-return signal park has a foreign owner");
  m_UserReturnSignalParked = parked;
  if (parked)
    markUserReturnWorkPending();
}

bool Thread::canSkipUserReturnWork() {
  Process* process = m_pParent;
  const bool workPending = userReturnWorkPending();
  return process && process->getState() == Process::Active && getUnwindState() == Continue &&
         !workPending && m_OriginalSyscallState == nullptr;
}

bool Thread::clearUserReturnWorkIfIdle() {
  LockGuard<Spinlock> guard(m_Lock);
  const StateLevel& state = m_StateLevels[m_nStateLevel];
  if (!m_pParent || m_pParent->getState() != Process::Active || m_EventQueue.count() ||
      getUnwindState() != Continue || m_EventDeferralDepth || m_TerminationDeferralDepth ||
      state.m_DeferredSignalMaskRestore || m_UserReturnSignalParked || m_SignalFramesRequired ||
      __atomic_load_n(&m_DeferredSubsystemExceptionState, __ATOMIC_ACQUIRE) ||
      m_OriginalSyscallState) {
    return false;
  }
  __atomic_store_n(&m_UserReturnWorkPending, static_cast<size_t>(0), __ATOMIC_RELEASE);
  return true;
}
