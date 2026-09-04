/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_PROCESS_READINESS_H
#define PEDIGREE_KERNEL_PROCESS_READINESS_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/SharedPointer.h"

using ReadyMask = uint32_t;

enum ReadyFlag : ReadyMask {
  ReadyNone = 0,
  ReadyRead = 1U << 0,
  ReadyPriority = 1U << 1,
  ReadyWrite = 1U << 2,
  ReadyError = 1U << 3,
  ReadyHangup = 1U << 4,
  ReadyReadHangup = 1U << 5,
  ReadyInvalid = 1U << 6,
  ReadyAll = ReadyRead | ReadyPriority | ReadyWrite | ReadyError | ReadyHangup | ReadyReadHangup |
             ReadyInvalid,
};

class ReadinessState;
class ReadinessTarget;

/** Receives a hint that one or more readiness predicates may have changed. */
class EXPORTED_PUBLIC ReadinessObserver {
 public:
  virtual ~ReadinessObserver() = default;

  /**
   * Called without the source's registry lock held. Consumers must query the
   * source again before reporting readiness; the mask is only a wakeup hint.
   */
  virtual void readinessChanged(ReadyMask mask) = 0;
};

/**
 * Owns one exact registration with a ReadinessSource.
 *
 * reset() removes only this registration and drains a notification which was
 * already admitted. It must not be called recursively from readinessChanged().
 */
class EXPORTED_PUBLIC ReadinessSubscription {
 public:
  ReadinessSubscription();
  ReadinessSubscription(ReadinessSubscription&& other) noexcept;
  ~ReadinessSubscription();

  ReadinessSubscription& operator=(ReadinessSubscription&& other) noexcept;

  explicit operator bool() const;
  void reset();

 private:
  friend class ReadinessSource;

  ReadinessSubscription(const ReadinessSubscription&) = delete;
  ReadinessSubscription& operator=(const ReadinessSubscription&) = delete;

  SharedPointer<ReadinessState> m_State;
  SharedPointer<ReadinessTarget> m_Target;
  SharedPointer<ReadinessObserver> m_Observer;
};

/** A lifetime-safe registry for mask-filtered readiness notifications. */
class EXPORTED_PUBLIC ReadinessSource {
 public:
  ReadinessSource();
  virtual ~ReadinessSource();

  /**
   * Registers an observer. Callers must query the source after this returns to
   * close the snapshot-versus-registration race; duplicate wakeups are valid.
   */
  MUST_USE_RESULT bool subscribeReadiness(ReadyMask interest,
                                          const SharedPointer<ReadinessObserver>& observer,
                                          ReadinessSubscription& subscription);

 protected:
  /** Notify interested observers that the supplied predicates may have changed. */
  void notifyReadiness(ReadyMask mask);

  /** Reject future subscriptions and issue one final terminal notification. */
  void closeReadiness(ReadyMask mask = ReadyInvalid | ReadyHangup);

 private:
  ReadinessSource(const ReadinessSource&) = delete;
  ReadinessSource& operator=(const ReadinessSource&) = delete;

  SharedPointer<ReadinessState> m_ReadinessState;
};

#endif
