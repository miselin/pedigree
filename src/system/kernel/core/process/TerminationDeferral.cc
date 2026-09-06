/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"

namespace {
#if PEDIGREE_AFFINITY_TESTS
void reportIdentityFailure(Thread* expected, Thread* observed, Thread* stable, const void* scope,
                           const char* site, const void* caller, bool interrupts) {
  ERROR_NOLOCK("AFFINITY-IDENTITY: expected=" << Hex << expected << " observed=" << observed);
  ERROR_NOLOCK("AFFINITY-IDENTITY: stable=" << Hex << stable << " scope=" << scope);
  ERROR_NOLOCK("AFFINITY-IDENTITY: site=" << site << " caller=" << Hex << caller);
  ERROR_NOLOCK("AFFINITY-IDENTITY: cpu=" << Dec << Processor::index() << " irq=" << interrupts);
}
#endif

#if PEDIGREE_AFFINITY_TESTS
__attribute__((noinline))
#endif
void assertCurrentThread(Thread* thread, const void* scope, const char* site) {
  Thread* observed = thread ? Processor::information().getCurrentThread() : nullptr;
  if (thread && observed != thread) {
#if PEDIGREE_AFFINITY_TESTS
    const bool interrupts = Processor::getInterrupts();
    Processor::setInterrupts(false);
    Thread* stable = Processor::information().getCurrentThread();
    reportIdentityFailure(thread, observed, stable, scope, site, __builtin_return_address(0),
                          interrupts);
#else
    (void)scope;
    (void)site;
#endif
    FATAL("TerminationDeferral moved or released on a different Thread.");
  }
}
}  // namespace

TerminationDeferral::TerminationDeferral(bool active)
    : m_pThread(active ? Processor::information().getCurrentThread() : nullptr), m_Record() {
#if PEDIGREE_AFFINITY_TESTS
  if (active) {
    const bool interrupts = Processor::getInterrupts();
    Processor::setInterrupts(false);
    Thread* stable = Processor::information().getCurrentThread();
    if (m_pThread != stable) {
      reportIdentityFailure(m_pThread, m_pThread, stable, this, "construct",
                            __builtin_return_address(0), interrupts);
      FATAL("TerminationDeferral captured a different Thread.");
    }
    Processor::setInterrupts(interrupts);
  }
#endif
  if (m_pThread) {
    m_pThread->registerDeferredScope(m_Record, true, false);
  }
}

TerminationDeferral::TerminationDeferral(TerminationDeferral&& other) noexcept
    : m_pThread(other.m_pThread), m_Record() {
  assertCurrentThread(m_pThread, this, "move-construct");
  if (m_pThread) {
    m_pThread->moveTerminationDeferral(other.m_Record, m_Record);
  }
  other.m_pThread = nullptr;
}

TerminationDeferral::~TerminationDeferral() {
  if (m_pThread) {
    assertCurrentThread(m_pThread, this, "destroy");
    m_pThread->unregisterTerminationDeferral(m_Record);
  }
}

TerminationDeferral& TerminationDeferral::operator=(TerminationDeferral&& other) noexcept {
  if (this != &other) {
    if (m_pThread && other.m_pThread) {
      assertCurrentThread(m_pThread, this, "replace-destination");
      assertCurrentThread(other.m_pThread, &other, "replace-source");
      if (m_pThread != other.m_pThread) {
        FATAL("TerminationDeferral replaced from a different Thread.");
      }

      // Both objects already protect this Thread. Keep the destination's
      // lexical record and retire the source's pure termination record. Pure
      // deferrals can be reset before a newer lexical scope, unlike cleanup
      // and event records whose release remains strictly LIFO.
      m_pThread->unregisterTerminationDeferral(other.m_Record);
      other.m_pThread = nullptr;
      return *this;
    }

    if (m_pThread) {
      assertCurrentThread(m_pThread, this, "reset-destination");
      m_pThread->unregisterTerminationDeferral(m_Record);
    }
    assertCurrentThread(other.m_pThread, &other, "adopt-source");
    m_pThread = other.m_pThread;
    if (m_pThread) {
      m_pThread->moveTerminationDeferral(other.m_Record, m_Record);
    }
    other.m_pThread = nullptr;
  }
  return *this;
}
