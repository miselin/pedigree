/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#include "pedigree/kernel/ActivityDiagnostics.h"

#if PEDIGREE_ACTIVITY_DIAGNOSTICS

#include "pedigree/kernel/time/Time.h"

namespace ActivityDiagnostics {
namespace {

uint64_t g_InterruptCount = 0;
uint64_t g_ExceptionCount = 0;
uint64_t g_HardwareInterruptCount = 0;
uint64_t g_OtherInterruptCount = 0;
uint64_t g_InterruptVectorCounts[InterruptVectorCount] = {};
uint64_t g_InterruptDurationBuckets[DurationBucketCount] = {};
uint64_t g_PageFaultDurationBuckets[DurationBucketCount] = {};
uint64_t g_SchedulerTimerDurationBuckets[DurationBucketCount] = {};
uint64_t g_HardDispatchCount = 0;
uint64_t g_HardDurationBuckets[DurationBucketCount] = {};
uint64_t g_ThreadedDispatchCount = 0;
uint64_t g_ThreadedDurationBuckets[DurationBucketCount] = {};
uint64_t g_SchedulerTimerTicks = 0;
uint64_t g_ScheduleCalls = 0;
uint64_t g_SameThreadSelections = 0;
uint64_t g_ContextSwitches = 0;
uint64_t g_IdleSelections = 0;
uint64_t g_SchedulerIdleFallbacks = 0;
uint64_t g_SchedulerIdleFallbackCurrentReady = 0;
uint64_t g_SchedulerIdleFallbackCurrentPending = 0;
uint64_t g_SchedulerNoEligibleSelections = 0;
uint64_t g_ReadyQueueScanEntries = 0;
uint64_t g_ReadyQueueCandidateVisits = 0;
uint64_t g_ReadyQueuePredicateRejects = 0;
uint64_t g_ReadyQueueSelectionSampleCounter = 0;
uint64_t g_ReadyQueueSelectionSamples = 0;
uint64_t g_ReadyQueueSelectionDurationBuckets[DurationBucketCount] = {};
uint64_t g_TimeAccountingSampleCounter = 0;
uint64_t g_TimeAccountingSamples = 0;
uint64_t g_TimeAccountingDurationBuckets[DurationBucketCount] = {};
uint64_t g_IdleHaltEntries = 0;
uint64_t g_FramebufferFlips = 0;
uint64_t g_FramebufferCells = 0;
uint64_t g_FramebufferDurationBuckets[DurationBucketCount] = {};
uint64_t g_UserReturnStageSamples[UserReturnStageCount] = {};
uint64_t g_UserReturnStageTotalNanoseconds[UserReturnStageCount] = {};
uint64_t g_UserReturnStageDurationBuckets[UserReturnStageCount][DurationBucketCount] = {};
uint64_t g_UserReturnFaultHandledSamples = 0;
uint64_t g_UserReturnFaultFallbackSamples = 0;
uint64_t g_UserReturnInterruptAffinityWaitedSamples = 0;
uint64_t g_UserReturnSyscallAffinityWaitedSamples = 0;
uint64_t g_UserEntryCaptureSamples = 0;
uint64_t g_UserEntryRestoreSamples = 0;
uint64_t g_UserEntryCaptureTscTotal = 0;
uint64_t g_UserEntryRestoreTscTotal = 0;
uint64_t g_UserEntryEmptyTscSamples = 0;
uint64_t g_UserEntryEmptyTscTotal = 0;
uint64_t g_UserEntryCaptureTscBuckets[DurationBucketCount] = {};
uint64_t g_UserEntryRestoreTscBuckets[DurationBucketCount] = {};
uint64_t g_UserEntryEmptyTscBuckets[DurationBucketCount] = {};

size_t durationBucket(uint64_t duration) {
  size_t bucket = 0;
  uint64_t limit = 1000;
  while (bucket + 1 < DurationBucketCount && duration >= limit) {
    ++bucket;
    limit <<= 1;
  }
  return bucket;
}

void recordDuration(uint64_t* buckets, uint64_t duration) {
  __atomic_fetch_add(&buckets[durationBucket(duration)], static_cast<uint64_t>(1),
                     __ATOMIC_RELAXED);
}

size_t tscDurationBucket(uint64_t duration) {
  size_t bucket = 0;
  uint64_t limit = 64;
  while (bucket + 1 < DurationBucketCount && duration >= limit) {
    ++bucket;
    limit <<= 1;
  }
  return bucket;
}

void recordTscDuration(uint64_t* buckets, uint64_t duration) {
  __atomic_fetch_add(&buckets[tscDurationBucket(duration)], static_cast<uint64_t>(1),
                     __ATOMIC_RELAXED);
}

uint64_t load(const uint64_t& value) {
  return __atomic_load_n(&value, __ATOMIC_ACQUIRE);
}

}  // namespace

#if PEDIGREE_X64_USER_ENTRY_DIAGNOSTICS
extern "C" uint64_t pedigree_user_entry_capture_calls;
extern "C" uint64_t pedigree_user_entry_restore_calls;
#endif

uint64_t timestamp() {
  return Time::getTicks();
}

void snapshot(Snapshot& result) {
  result.interruptCount = load(g_InterruptCount);
  result.exceptionCount = load(g_ExceptionCount);
  result.hardwareInterruptCount = load(g_HardwareInterruptCount);
  result.otherInterruptCount = load(g_OtherInterruptCount);
  for (size_t i = 0; i < InterruptVectorCount; ++i)
    result.interruptVectorCounts[i] = load(g_InterruptVectorCounts[i]);
  for (size_t i = 0; i < DurationBucketCount; ++i)
    result.interruptDurationBuckets[i] = load(g_InterruptDurationBuckets[i]);
  for (size_t i = 0; i < DurationBucketCount; ++i)
    result.pageFaultDurationBuckets[i] = load(g_PageFaultDurationBuckets[i]);
  for (size_t i = 0; i < DurationBucketCount; ++i)
    result.schedulerTimerDurationBuckets[i] = load(g_SchedulerTimerDurationBuckets[i]);
  result.hardDispatchCount = load(g_HardDispatchCount);
  for (size_t i = 0; i < DurationBucketCount; ++i)
    result.hardDurationBuckets[i] = load(g_HardDurationBuckets[i]);
  result.threadedDispatchCount = load(g_ThreadedDispatchCount);
  for (size_t i = 0; i < DurationBucketCount; ++i)
    result.threadedDurationBuckets[i] = load(g_ThreadedDurationBuckets[i]);
  result.schedulerTimerTicks = load(g_SchedulerTimerTicks);
  result.scheduleCalls = load(g_ScheduleCalls);
  result.sameThreadSelections = load(g_SameThreadSelections);
  result.contextSwitches = load(g_ContextSwitches);
  result.idleSelections = load(g_IdleSelections);
  result.schedulerIdleFallbacks = load(g_SchedulerIdleFallbacks);
  result.schedulerIdleFallbackCurrentReady = load(g_SchedulerIdleFallbackCurrentReady);
  result.schedulerIdleFallbackCurrentPending = load(g_SchedulerIdleFallbackCurrentPending);
  result.schedulerNoEligibleSelections = load(g_SchedulerNoEligibleSelections);
  result.readyQueueScanEntries = load(g_ReadyQueueScanEntries);
  result.readyQueueCandidateVisits = load(g_ReadyQueueCandidateVisits);
  result.readyQueuePredicateRejects = load(g_ReadyQueuePredicateRejects);
  result.readyQueueSelectionSamples = load(g_ReadyQueueSelectionSamples);
  for (size_t i = 0; i < DurationBucketCount; ++i)
    result.readyQueueSelectionDurationBuckets[i] = load(g_ReadyQueueSelectionDurationBuckets[i]);
  result.timeAccountingSamples = load(g_TimeAccountingSamples);
  for (size_t i = 0; i < DurationBucketCount; ++i)
    result.timeAccountingDurationBuckets[i] = load(g_TimeAccountingDurationBuckets[i]);
  result.idleHaltEntries = load(g_IdleHaltEntries);
  result.framebufferFlips = load(g_FramebufferFlips);
  result.framebufferCells = load(g_FramebufferCells);
  for (size_t i = 0; i < DurationBucketCount; ++i)
    result.framebufferDurationBuckets[i] = load(g_FramebufferDurationBuckets[i]);
  for (size_t stage = 0; stage < UserReturnStageCount; ++stage) {
    result.userReturnStageSamples[stage] = load(g_UserReturnStageSamples[stage]);
    result.userReturnStageTotalNanoseconds[stage] = load(g_UserReturnStageTotalNanoseconds[stage]);
    for (size_t i = 0; i < DurationBucketCount; ++i) {
      result.userReturnStageDurationBuckets[stage][i] =
          load(g_UserReturnStageDurationBuckets[stage][i]);
    }
  }
  result.userReturnFaultHandledSamples = load(g_UserReturnFaultHandledSamples);
  result.userReturnFaultFallbackSamples = load(g_UserReturnFaultFallbackSamples);
  result.userReturnInterruptAffinityWaitedSamples =
      load(g_UserReturnInterruptAffinityWaitedSamples);
  result.userReturnSyscallAffinityWaitedSamples = load(g_UserReturnSyscallAffinityWaitedSamples);
#if PEDIGREE_X64_USER_ENTRY_DIAGNOSTICS
  result.userEntryCaptureCalls = pedigree_user_entry_capture_calls;
  result.userEntryRestoreCalls = pedigree_user_entry_restore_calls;
#else
  result.userEntryCaptureCalls = 0;
  result.userEntryRestoreCalls = 0;
#endif
  result.userEntryCaptureSamples = load(g_UserEntryCaptureSamples);
  result.userEntryRestoreSamples = load(g_UserEntryRestoreSamples);
  result.userEntryCaptureTscTotal = load(g_UserEntryCaptureTscTotal);
  result.userEntryRestoreTscTotal = load(g_UserEntryRestoreTscTotal);
  result.userEntryEmptyTscSamples = load(g_UserEntryEmptyTscSamples);
  result.userEntryEmptyTscTotal = load(g_UserEntryEmptyTscTotal);
  for (size_t i = 0; i < DurationBucketCount; ++i) {
    result.userEntryCaptureTscBuckets[i] = load(g_UserEntryCaptureTscBuckets[i]);
    result.userEntryRestoreTscBuckets[i] = load(g_UserEntryRestoreTscBuckets[i]);
    result.userEntryEmptyTscBuckets[i] = load(g_UserEntryEmptyTscBuckets[i]);
  }
}

void recordInterruptEntry(size_t vector) {
  if (vector >= InterruptVectorCount)
    return;
  __atomic_fetch_add(&g_InterruptCount, static_cast<uint64_t>(1), __ATOMIC_RELAXED);
  __atomic_fetch_add(&g_InterruptVectorCounts[vector], static_cast<uint64_t>(1), __ATOMIC_RELAXED);
  if (vector < 32) {
    __atomic_fetch_add(&g_ExceptionCount, static_cast<uint64_t>(1), __ATOMIC_RELAXED);
  } else if (vector < 48) {
    __atomic_fetch_add(&g_HardwareInterruptCount, static_cast<uint64_t>(1), __ATOMIC_RELAXED);
  } else {
    __atomic_fetch_add(&g_OtherInterruptCount, static_cast<uint64_t>(1), __ATOMIC_RELAXED);
  }
}

void recordInterruptDuration(size_t vector, uint64_t duration) {
  if (vector >= InterruptVectorCount)
    return;
  recordDuration(g_InterruptDurationBuckets, duration);
  if (vector == 14)
    recordDuration(g_PageFaultDurationBuckets, duration);
  else if (vector == 0xfe)
    recordDuration(g_SchedulerTimerDurationBuckets, duration);
}

void recordHardDispatch(uint8_t irq, uint64_t duration) {
  (void)irq;
  __atomic_fetch_add(&g_HardDispatchCount, static_cast<uint64_t>(1), __ATOMIC_RELAXED);
  recordDuration(g_HardDurationBuckets, duration);
}

void recordThreadedDispatch(uint8_t irq, uint64_t duration) {
  (void)irq;
  __atomic_fetch_add(&g_ThreadedDispatchCount, static_cast<uint64_t>(1), __ATOMIC_RELAXED);
  recordDuration(g_ThreadedDurationBuckets, duration);
}

void recordSchedulerTimer() {
  __atomic_fetch_add(&g_SchedulerTimerTicks, static_cast<uint64_t>(1), __ATOMIC_RELAXED);
}

void recordScheduleCall() {
  __atomic_fetch_add(&g_ScheduleCalls, static_cast<uint64_t>(1), __ATOMIC_RELAXED);
}

void recordSameThreadSelection() {
  __atomic_fetch_add(&g_SameThreadSelections, static_cast<uint64_t>(1), __ATOMIC_RELAXED);
}

void recordContextSwitch() {
  __atomic_fetch_add(&g_ContextSwitches, static_cast<uint64_t>(1), __ATOMIC_RELAXED);
}

void recordIdleSelection() {
  __atomic_fetch_add(&g_IdleSelections, static_cast<uint64_t>(1), __ATOMIC_RELAXED);
}

void recordSchedulerIdleFallback(bool currentReady, bool currentPending) {
  __atomic_fetch_add(&g_SchedulerIdleFallbacks, static_cast<uint64_t>(1), __ATOMIC_RELAXED);
  if (currentReady)
    __atomic_fetch_add(&g_SchedulerIdleFallbackCurrentReady, static_cast<uint64_t>(1),
                       __ATOMIC_RELAXED);
  if (currentPending)
    __atomic_fetch_add(&g_SchedulerIdleFallbackCurrentPending, static_cast<uint64_t>(1),
                       __ATOMIC_RELAXED);
}

void recordSchedulerNoEligibleSelection() {
  __atomic_fetch_add(&g_SchedulerNoEligibleSelections, static_cast<uint64_t>(1), __ATOMIC_RELAXED);
}

void recordReadyQueueScanEntry() {
  __atomic_fetch_add(&g_ReadyQueueScanEntries, static_cast<uint64_t>(1), __ATOMIC_RELAXED);
}

void recordReadyQueueCandidateVisit() {
  __atomic_fetch_add(&g_ReadyQueueCandidateVisits, static_cast<uint64_t>(1), __ATOMIC_RELAXED);
}

void recordReadyQueuePredicateReject() {
  __atomic_fetch_add(&g_ReadyQueuePredicateRejects, static_cast<uint64_t>(1), __ATOMIC_RELAXED);
}

bool shouldSampleReadyQueueSelection() {
  return (__atomic_fetch_add(&g_ReadyQueueSelectionSampleCounter, static_cast<uint64_t>(1),
                             __ATOMIC_RELAXED) &
          63) == 0;
}

void recordReadyQueueSelection(uint64_t duration) {
  __atomic_fetch_add(&g_ReadyQueueSelectionSamples, static_cast<uint64_t>(1), __ATOMIC_RELAXED);
  recordDuration(g_ReadyQueueSelectionDurationBuckets, duration);
}

bool shouldSampleTimeAccounting() {
  return (__atomic_fetch_add(&g_TimeAccountingSampleCounter, static_cast<uint64_t>(1),
                             __ATOMIC_RELAXED) &
          63) == 0;
}

void recordTimeAccounting(uint64_t duration) {
  __atomic_fetch_add(&g_TimeAccountingSamples, static_cast<uint64_t>(1), __ATOMIC_RELAXED);
  recordDuration(g_TimeAccountingDurationBuckets, duration);
}

void recordIdleHalt() {
  __atomic_fetch_add(&g_IdleHaltEntries, static_cast<uint64_t>(1), __ATOMIC_RELAXED);
}

void recordFramebufferFlip(size_t cells, uint64_t duration) {
  __atomic_fetch_add(&g_FramebufferFlips, static_cast<uint64_t>(1), __ATOMIC_RELAXED);
  __atomic_fetch_add(&g_FramebufferCells, static_cast<uint64_t>(cells), __ATOMIC_RELAXED);
  recordDuration(g_FramebufferDurationBuckets, duration);
}

void recordUserReturnStage(UserReturnStage stage, uint64_t duration) {
  const size_t index = static_cast<size_t>(stage);
  if (index >= UserReturnStageCount)
    return;
  __atomic_fetch_add(&g_UserReturnStageSamples[index], static_cast<uint64_t>(1), __ATOMIC_RELAXED);
  __atomic_fetch_add(&g_UserReturnStageTotalNanoseconds[index], duration, __ATOMIC_RELAXED);
  recordDuration(g_UserReturnStageDurationBuckets[index], duration);
}

void recordUserReturnFaultOutcome(bool handled) {
  uint64_t* counter =
      handled ? &g_UserReturnFaultHandledSamples : &g_UserReturnFaultFallbackSamples;
  __atomic_fetch_add(counter, static_cast<uint64_t>(1), __ATOMIC_RELAXED);
}

void recordUserReturnAffinityWait(bool syscall) {
  uint64_t* counter = syscall ? &g_UserReturnSyscallAffinityWaitedSamples
                              : &g_UserReturnInterruptAffinityWaitedSamples;
  __atomic_fetch_add(counter, static_cast<uint64_t>(1), __ATOMIC_RELAXED);
}

#if PEDIGREE_X64_USER_ENTRY_DIAGNOSTICS
extern "C" void pedigree_record_user_entry_capture(uint64_t duration, uint64_t emptyDuration) {
  __atomic_fetch_add(&g_UserEntryCaptureSamples, static_cast<uint64_t>(1), __ATOMIC_RELAXED);
  __atomic_fetch_add(&g_UserEntryCaptureTscTotal, duration, __ATOMIC_RELAXED);
  recordTscDuration(g_UserEntryCaptureTscBuckets, duration);
  __atomic_fetch_add(&g_UserEntryEmptyTscSamples, static_cast<uint64_t>(1), __ATOMIC_RELAXED);
  __atomic_fetch_add(&g_UserEntryEmptyTscTotal, emptyDuration, __ATOMIC_RELAXED);
  recordTscDuration(g_UserEntryEmptyTscBuckets, emptyDuration);
}

extern "C" void pedigree_record_user_entry_restore(uint64_t duration, uint64_t emptyDuration) {
  __atomic_fetch_add(&g_UserEntryRestoreSamples, static_cast<uint64_t>(1), __ATOMIC_RELAXED);
  __atomic_fetch_add(&g_UserEntryRestoreTscTotal, duration, __ATOMIC_RELAXED);
  recordTscDuration(g_UserEntryRestoreTscBuckets, duration);
  __atomic_fetch_add(&g_UserEntryEmptyTscSamples, static_cast<uint64_t>(1), __ATOMIC_RELAXED);
  __atomic_fetch_add(&g_UserEntryEmptyTscTotal, emptyDuration, __ATOMIC_RELAXED);
  recordTscDuration(g_UserEntryEmptyTscBuckets, emptyDuration);
}
#endif

}  // namespace ActivityDiagnostics

#endif
