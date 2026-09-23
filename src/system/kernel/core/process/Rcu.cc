/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/Rcu.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"

RcuReadGuard::RcuReadGuard() : m_State(nullptr), m_Interrupts(Processor::getInterrupts()) {
  Processor::setInterrupts(false);
  if (Processor::m_Initialised != 2) {
    panic("RCU requires completed processor discovery.");
  }
#if HOSTED
  if (!Processor::onHostedExecutionThread()) {
    panic("RCU guard requires the hosted execution thread.");
  }
#endif
  m_State = &Processor::information().rcuState();
  m_State->enter();
}

RcuReadGuard::~RcuReadGuard() {
  if (Processor::getInterrupts() || &Processor::information().rcuState() != m_State) {
    panic("RCU reader changed its execution context.");
  }
  m_State->leave();
  Processor::setInterrupts(m_Interrupts);
}

void Rcu::synchronize() {
  if (Processor::m_Initialised != 2 || Processor::information().rcuState().active()) {
    panic("RCU grace period requires initialized processors and no local reader.");
  }
  // SC publication, reader admission/load, and these snapshots share an order:
  // a reader of an old pointer is either visible here or has already finished.
  for (size_t i = 0; i < Processor::getCount(); ++i) {
    RcuReadState& state = Processor::informationAt(i)->rcuState();
    const auto before = state.snapshot();
    while (!state.passed(before)) {
      Processor::pause();
    }
  }
}

RcuRetireQueue::~RcuRetireQueue() {
  drain();
}

void RcuRetireQueue::retire(void* object, Reclaim reclaim) {
  if (!reclaim || Processor::information().rcuState().active() ||
      !Processor::information().getCurrentThread() || !Processor::getInterrupts() ||
      Processor::inDeviceHardIrq()) {
    panic("RCU retirement requires a callback and ordinary task context.");
  }
  TerminationDeferral lifetime;
  LockGuard<Mutex> lock(m_Lock);
  if (m_Count == Capacity) {
    drainLocked();
  }
  m_Entries[m_Count++] = {object, reclaim};
}

void RcuRetireQueue::drain() {
  if (Processor::information().rcuState().active() || !Processor::getInterrupts() ||
      !Processor::information().getCurrentThread() || Processor::inDeviceHardIrq()) {
    panic("RCU reclamation requires ordinary task context.");
  }
  TerminationDeferral lifetime;
  LockGuard<Mutex> lock(m_Lock);
  drainLocked();
}

void RcuRetireQueue::drainLocked() {
  if (!m_Count) {
    return;
  }
  Rcu::synchronize();
  for (size_t i = 0; i < m_Count; ++i) {
    m_Entries[i].reclaim(m_Entries[i].object);
  }
  m_Count = 0;
}
