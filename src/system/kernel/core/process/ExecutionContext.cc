/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/ExecutionContext.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"

ExecutionContextGuard::ExecutionContextGuard(ExecutionContext context)
    : m_Thread(Processor::information().getCurrentThread()),
      m_StateLevel(0),
      m_Previous(ExecutionContext::AtomicThread),
      m_Cleanup(),
      m_Active(false) {
  if (!m_Thread) {
    return;
  }

  const bool interruptsWereEnabled = Processor::getInterrupts();
  Processor::setInterrupts(false);

  m_StateLevel = __atomic_load_n(&m_Thread->m_nStateLevel, __ATOMIC_ACQUIRE);
  m_Previous = m_Thread->m_StateLevels[m_StateLevel].m_ExecutionContext.current();

  // Publish abandonment cleanup before changing the classification. A
  // nested fault can therefore never leave a reused Thread state labelled
  // as an IRQ context.
  m_Thread->armAtomicStateCleanup(m_Cleanup, abandon, this);
  m_Active = true;
  m_Thread->m_StateLevels[m_StateLevel].m_ExecutionContext.enter(context);

  Processor::setInterrupts(interruptsWereEnabled);
}

ExecutionContextGuard::~ExecutionContextGuard() {
  if (!m_Active) {
    return;
  }

  const bool interruptsWereEnabled = Processor::getInterrupts();
  Processor::setInterrupts(false);
  restore();
  m_Thread->disarmAtomicStateCleanup(m_Cleanup);
  Processor::setInterrupts(interruptsWereEnabled);
}

void ExecutionContextGuard::abandon(void* context) {
  ExecutionContextGuard* guard = reinterpret_cast<ExecutionContextGuard*>(context);
  if (guard) {
    guard->restore();
  }
}

void ExecutionContextGuard::restore() {
  if (!m_Active) {
    return;
  }

  if (m_StateLevel >= MAX_NESTED_EVENTS) {
    FATAL_NOLOCK("Execution-context cleanup recorded an invalid state level");
    return;
  }

  m_Thread->m_StateLevels[m_StateLevel].m_ExecutionContext.restore(m_Previous);
  m_Active = false;
}
