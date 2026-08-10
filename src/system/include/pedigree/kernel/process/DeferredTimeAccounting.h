/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_PROCESS_DEFERREDTIMEACCOUNTING_H
#define PEDIGREE_KERNEL_PROCESS_DEFERREDTIMEACCOUNTING_H

#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/time/Time.h"

static_assert(__atomic_always_lock_free(sizeof(Time::Timestamp), nullptr),
              "interrupt time accounting requires lock-free timestamps");
static_assert(__atomic_always_lock_free(sizeof(size_t), nullptr),
              "interrupt time accounting requires lock-free work generations");
static_assert(__atomic_always_lock_free(sizeof(bool), nullptr),
              "interrupt time accounting requires lock-free admission flags");

enum class CpuTimeMode {
  Kernel,
  User,
};

/** The mode transitions made by a syscall or interrupt kernel scope. */
class KernelTimeTransition {
 public:
  static constexpr CpuTimeMode interrupted(bool fromUserspace) {
    return fromUserspace ? CpuTimeMode::User : CpuTimeMode::Kernel;
  }

  static constexpr CpuTimeMode handler() {
    return CpuTimeMode::Kernel;
  }

  static constexpr CpuTimeMode resumed(bool fromUserspace) {
    return interrupted(fromUserspace);
  }
};

/** Independent monotonic CPU-time baselines owned by one schedulable Thread. */
class ThreadTimeAccounting {
 public:
  ThreadTimeAccounting() : m_Kernel(), m_User() {}

  void record(CpuTimeMode mode, Time::Timestamp now, size_t processor = 0) {
    Entry* state = entry(mode);
    if (installProcessorBaseline(state, processor, now)) {
      return;
    }

    Time::Timestamp* baseline = &state->timestamp;
    Time::Timestamp previous = __atomic_load_n(baseline, __ATOMIC_ACQUIRE);
    if (now <= previous) {
      return;
    }

    // A failed handoff means a nested scope installed a newer sample.
    // Retrying is unnecessary and would make the raw path unbounded.
    __atomic_compare_exchange_n(baseline, &previous, now, false, __ATOMIC_ACQ_REL,
                                __ATOMIC_ACQUIRE);
  }

  Time::Timestamp elapsed(CpuTimeMode mode, Time::Timestamp now, size_t processor = 0) {
    Entry* state = entry(mode);
    if (installProcessorBaseline(state, processor, now)) {
      return 0;
    }

    Time::Timestamp* baseline = &state->timestamp;
    Time::Timestamp previous = __atomic_load_n(baseline, __ATOMIC_ACQUIRE);
    if (now < previous) {
      return 0;
    }

    // One failed handoff means a nested accounting scope advanced this
    // exact Thread's baseline and already owns the overlapping interval.
    // Do not spin in raw interrupt context or charge that interval twice.
    const Time::Timestamp elapsed = now - previous;
    return __atomic_compare_exchange_n(baseline, &previous, now, false, __ATOMIC_ACQ_REL,
                                       __ATOMIC_ACQUIRE)
               ? elapsed
               : 0;
  }

 private:
  struct Entry {
    Entry() : timestamp(0), processor(~static_cast<size_t>(0)) {}

    Time::Timestamp timestamp;
    size_t processor;
  };

  static bool installProcessorBaseline(Entry* state, size_t processor, Time::Timestamp now) {
    if (__atomic_load_n(&state->processor, __ATOMIC_ACQUIRE) == processor) {
      return false;
    }

    // A schedulable Thread cannot execute concurrently on its old and new
    // CPUs. Publish the destination timestamp before its CPU identifier so
    // nested scopes which observe the new identifier also see its sample.
    __atomic_store_n(&state->timestamp, now, __ATOMIC_RELEASE);
    __atomic_store_n(&state->processor, processor, __ATOMIC_RELEASE);
    return true;
  }

  Entry* entry(CpuTimeMode mode) {
    return mode == CpuTimeMode::User ? &m_User : &m_Kernel;
  }

  Entry m_Kernel;
  Entry m_User;

  ThreadTimeAccounting(const ThreadTimeAccounting&) = delete;
  ThreadTimeAccounting& operator=(const ThreadTimeAccounting&) = delete;
};

/** Coalesces process CPU-time updates into one ordinary-worker edge. */
class DeferredTimeAccounting {
 public:
  DeferredTimeAccounting() : m_Pending(0) {}

  /** Returns true only for the 0-to-1 edge which must ring the worker. */
  bool publish(Time::Timestamp elapsed) {
    if (!elapsed) {
      return false;
    }

    return __atomic_exchange_n(&m_Pending, static_cast<size_t>(1), __ATOMIC_ACQ_REL) == 0;
  }

  /** Claims the edge; a racing later publication re-arms it. */
  bool take() {
    return __atomic_exchange_n(&m_Pending, static_cast<size_t>(0), __ATOMIC_ACQ_REL) != 0;
  }

  bool pending() const {
    return __atomic_load_n(&m_Pending, __ATOMIC_ACQUIRE) != 0;
  }

 private:
  size_t m_Pending;

  DeferredTimeAccounting(const DeferredTimeAccounting&) = delete;
  DeferredTimeAccounting& operator=(const DeferredTimeAccounting&) = delete;
};

/** Lost-wakeup-free readiness state for one accounting worker. */
class DeferredTimeAccountingWorkerState {
 public:
  DeferredTimeAccountingWorkerState() : m_Published(0), m_Completed(0), m_Active(0) {}

  /** Records one publication before the scheduler doorbell is rung. */
  void publish() {
    __atomic_add_fetch(&m_Published, static_cast<size_t>(1), __ATOMIC_RELEASE);
  }

  /** Keeps the worker eligible and snapshots the generation it will drain. */
  size_t beginBatch() {
    __atomic_store_n(&m_Active, static_cast<size_t>(1), __ATOMIC_RELEASE);
    return __atomic_load_n(&m_Published, __ATOMIC_ACQUIRE);
  }

  /** Completes only the snapshotted generation, preserving racing work. */
  void finishBatch(size_t generation) {
    __atomic_store_n(&m_Completed, generation, __ATOMIC_RELEASE);
    __atomic_store_n(&m_Active, static_cast<size_t>(0), __ATOMIC_RELEASE);
  }

  bool ready(bool stopping = false) const {
    return stopping || __atomic_load_n(&m_Active, __ATOMIC_ACQUIRE) || !caughtUp();
  }

  bool caughtUp() const {
    return __atomic_load_n(&m_Published, __ATOMIC_ACQUIRE) ==
           __atomic_load_n(&m_Completed, __ATOMIC_ACQUIRE);
  }

 private:
  size_t m_Published;
  size_t m_Completed;
  size_t m_Active;

  DeferredTimeAccountingWorkerState(const DeferredTimeAccountingWorkerState&) = delete;
  DeferredTimeAccountingWorkerState& operator=(const DeferredTimeAccountingWorkerState&) = delete;
};

#endif
