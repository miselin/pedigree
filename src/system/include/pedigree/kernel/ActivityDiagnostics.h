/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#ifndef PEDIGREE_KERNEL_ACTIVITYDIAGNOSTICS_H
#define PEDIGREE_KERNEL_ACTIVITYDIAGNOSTICS_H

#include "pedigree/kernel/compiler.h"

#include <config.h>
#include <stddef.h>
#include <stdint.h>

namespace ActivityDiagnostics {

static constexpr size_t DurationBucketCount = 16;
static constexpr size_t InterruptVectorCount = 256;
static constexpr size_t UserReturnSamplePeriod = 64;
static constexpr size_t UserEntrySamplePeriod = 256;

enum class UserReturnStage : size_t {
  InterruptTail,
  SyscallTail,
  InterruptWork,
  SyscallWork,
  Checkpoint,
  ProcessStop,
  DeferredFault,
  Event,
  InterruptAffinity,
  SyscallAffinity,
  InterruptAccounting,
  SyscallAccounting,
  Count,
};

static constexpr size_t UserReturnStageCount = static_cast<size_t>(UserReturnStage::Count);

/** A detached, monotonically increasing view of kernel activity counters. */
struct Snapshot {
  uint64_t interruptCount;
  uint64_t exceptionCount;
  uint64_t hardwareInterruptCount;
  uint64_t otherInterruptCount;
  uint64_t interruptVectorCounts[InterruptVectorCount];
  uint64_t interruptDurationBuckets[DurationBucketCount];
  uint64_t pageFaultDurationBuckets[DurationBucketCount];
  uint64_t schedulerTimerDurationBuckets[DurationBucketCount];
  uint64_t hardDispatchCount;
  uint64_t hardDurationBuckets[DurationBucketCount];
  uint64_t threadedDispatchCount;
  uint64_t threadedDurationBuckets[DurationBucketCount];
  uint64_t schedulerTimerTicks;
  uint64_t scheduleCalls;
  uint64_t sameThreadSelections;
  uint64_t contextSwitches;
  uint64_t idleSelections;
  uint64_t schedulerIdleFallbacks;
  uint64_t schedulerIdleFallbackCurrentReady;
  uint64_t schedulerIdleFallbackCurrentPending;
  uint64_t schedulerNoEligibleSelections;
  uint64_t readyQueueScanEntries;
  uint64_t readyQueueCandidateVisits;
  uint64_t readyQueuePredicateRejects;
  uint64_t readyQueueSelectionSamples;
  uint64_t readyQueueSelectionDurationBuckets[DurationBucketCount];
  uint64_t timeAccountingSamples;
  uint64_t timeAccountingDurationBuckets[DurationBucketCount];
  uint64_t idleHaltEntries;
  uint64_t framebufferFlips;
  uint64_t framebufferCells;
  uint64_t framebufferDurationBuckets[DurationBucketCount];
  uint64_t userReturnStageSamples[UserReturnStageCount];
  uint64_t userReturnStageTotalNanoseconds[UserReturnStageCount];
  uint64_t userReturnStageDurationBuckets[UserReturnStageCount][DurationBucketCount];
  uint64_t userReturnFaultHandledSamples;
  uint64_t userReturnFaultFallbackSamples;
  uint64_t userReturnInterruptAffinityWaitedSamples;
  uint64_t userReturnSyscallAffinityWaitedSamples;
  uint64_t userReturnInterruptAblationEligible;
  uint64_t userReturnInterruptAblationFast;
  uint64_t userReturnInterruptAblationFallback;
  uint64_t userReturnSyscallAblationEligible;
  uint64_t userReturnSyscallAblationFast;
  uint64_t userReturnSyscallAblationFallback;
  uint64_t userEntryCaptureCalls;
  uint64_t userEntryRestoreCalls;
  uint64_t userEntryCaptureSamples;
  uint64_t userEntryRestoreSamples;
  uint64_t userEntryCaptureTscTotal;
  uint64_t userEntryRestoreTscTotal;
  uint64_t userEntryEmptyTscSamples;
  uint64_t userEntryEmptyTscTotal;
  uint64_t userEntryCaptureTscBuckets[DurationBucketCount];
  uint64_t userEntryRestoreTscBuckets[DurationBucketCount];
  uint64_t userEntryEmptyTscBuckets[DurationBucketCount];
};

#if PEDIGREE_ACTIVITY_DIAGNOSTICS

EXPORTED_PUBLIC uint64_t timestamp();
EXPORTED_PUBLIC void snapshot(Snapshot& result);

void recordInterruptEntry(size_t vector);
void recordInterruptDuration(size_t vector, uint64_t duration);
void recordHardDispatch(uint8_t irq, uint64_t duration);
void recordThreadedDispatch(uint8_t irq, uint64_t duration);
void recordSchedulerTimer();
void recordScheduleCall();
void recordSameThreadSelection();
void recordContextSwitch();
void recordIdleSelection();
void recordSchedulerIdleFallback(bool currentReady, bool currentPending);
void recordSchedulerNoEligibleSelection();
void recordReadyQueueScanEntry();
void recordReadyQueueCandidateVisit();
void recordReadyQueuePredicateReject();
bool shouldSampleReadyQueueSelection();
void recordReadyQueueSelection(uint64_t duration);
bool shouldSampleTimeAccounting();
void recordTimeAccounting(uint64_t duration);
void recordIdleHalt();
EXPORTED_PUBLIC void recordFramebufferFlip(size_t cells, uint64_t duration);
void recordUserReturnStage(UserReturnStage stage, uint64_t duration);
void recordUserReturnFaultOutcome(bool handled);
void recordUserReturnAffinityWait(bool syscall);
void recordUserReturnAblation(bool syscall, bool fast);

class InterruptScope {
 public:
  explicit InterruptScope(size_t vector) : m_Vector(vector), m_Start(timestamp()) {
    recordInterruptEntry(vector);
  }

  ~InterruptScope() {
    recordInterruptDuration(m_Vector, timestamp() - m_Start);
  }

 private:
  size_t m_Vector;
  uint64_t m_Start;
};

class HardDispatchScope {
 public:
  explicit HardDispatchScope(uint8_t irq) : m_Irq(irq), m_Start(timestamp()) {}

  ~HardDispatchScope() {
    recordHardDispatch(m_Irq, timestamp() - m_Start);
  }

 private:
  uint8_t m_Irq;
  uint64_t m_Start;
};

class ReadyQueueSelectionScope {
 public:
  ReadyQueueSelectionScope()
      : m_Active(shouldSampleReadyQueueSelection()), m_Start(m_Active ? timestamp() : 0) {}

  ~ReadyQueueSelectionScope() {
    if (m_Active)
      recordReadyQueueSelection(timestamp() - m_Start);
  }

 private:
  bool m_Active;
  uint64_t m_Start;
};

class TimeAccountingScope {
 public:
  TimeAccountingScope()
      : m_Active(shouldSampleTimeAccounting()), m_Start(m_Active ? timestamp() : 0) {}

  ~TimeAccountingScope() {
    if (m_Active)
      recordTimeAccounting(timestamp() - m_Start);
  }

 private:
  bool m_Active;
  uint64_t m_Start;
};

#else

inline uint64_t timestamp() {
  return 0;
}

inline void snapshot(Snapshot& result) {
  result = {};
}

inline void recordInterruptEntry(size_t) {}
inline void recordInterruptDuration(size_t, uint64_t) {}
inline void recordHardDispatch(uint8_t, uint64_t) {}
inline void recordThreadedDispatch(uint8_t, uint64_t) {}
inline void recordSchedulerTimer() {}
inline void recordScheduleCall() {}
inline void recordSameThreadSelection() {}
inline void recordContextSwitch() {}
inline void recordIdleSelection() {}
inline void recordSchedulerIdleFallback(bool, bool) {}
inline void recordSchedulerNoEligibleSelection() {}
inline void recordReadyQueueScanEntry() {}
inline void recordReadyQueueCandidateVisit() {}
inline void recordReadyQueuePredicateReject() {}
inline bool shouldSampleReadyQueueSelection() {
  return false;
}
inline void recordReadyQueueSelection(uint64_t) {}
inline bool shouldSampleTimeAccounting() {
  return false;
}
inline void recordTimeAccounting(uint64_t) {}
inline void recordIdleHalt() {}
inline void recordFramebufferFlip(size_t, uint64_t) {}
inline void recordUserReturnStage(UserReturnStage, uint64_t) {}
inline void recordUserReturnFaultOutcome(bool) {}
inline void recordUserReturnAffinityWait(bool) {}
inline void recordUserReturnAblation(bool, bool) {}

class InterruptScope {
 public:
  explicit InterruptScope(size_t) {}
};

class HardDispatchScope {
 public:
  explicit HardDispatchScope(uint8_t) {}
};

class ReadyQueueSelectionScope {
 public:
  ReadyQueueSelectionScope() {}
};

class TimeAccountingScope {
 public:
  TimeAccountingScope() {}
};

#endif

}  // namespace ActivityDiagnostics

#endif
