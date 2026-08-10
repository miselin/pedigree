/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef USB_HCD_CALLBACK_DELIVERY_H
#define USB_HCD_CALLBACK_DELIVERY_H

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/WaitQueue.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/utilities/List.h"

namespace UsbHcd {
/**
 * Publishes captured callbacks before an HCD releases its completion lock.
 *
 * A cancellation can steal a pending callback and run it inline. This is what
 * makes callback A synchronously cancelling later callback B safe: waiting for
 * B on the same delivery thread would deadlock. Running callbacks owned by a
 * different thread are drained through a per-record wait queue instead.
 */
class CallbackDeliveryQueue {
 public:
  struct Key {
    uintptr_t transaction;
    size_t generation;

    bool operator==(const Key& other) const {
      return transaction == other.transaction && generation == other.generation;
    }
  };

  using Callback = void (*)(uintptr_t, ssize_t);
  using AfterDelivery = void (*)(void*);
  using OnDestroy = void (*)(void*);

  class Record {
   private:
    friend class CallbackDeliveryQueue;

    enum class State {
      Pending,
      Running,
      Complete,
    };

    Record(const Key& key, Callback callback, uintptr_t parameter, ssize_t result,
           AfterDelivery afterDelivery, void* afterDeliveryContext, OnDestroy onDestroy,
           void* destroyContext)
        : m_Key(key),
          m_Callback(callback),
          m_Parameter(parameter),
          m_Result(result),
          m_AfterDelivery(afterDelivery),
          m_AfterDeliveryContext(afterDeliveryContext),
          m_OnDestroy(onDestroy),
          m_DestroyContext(destroyContext),
          m_References(1),
          m_State(State::Pending),
          m_Runner(nullptr),
          m_CompletionWaiters(),
          m_Completed(false) {}

    ~Record() {
      if (m_OnDestroy)
        m_OnDestroy(m_DestroyContext);
    }

    void retain() {
      m_References += 1;
    }

    void release() {
      if ((m_References -= 1) == 0)
        delete this;
    }

    void waitForCompletion() {
      while (true) {
        auto guard = m_CompletionWaiters.acquire();
        if (m_Completed)
          return;
        const WaitQueue::WakeReason wakeReason = guard.waitForCompletion();
        (void)wakeReason;
      }
    }

    void complete() {
      auto guard = m_CompletionWaiters.acquire();
      m_Completed = true;
      guard.wakeAll();
    }

    Key m_Key;
    Callback m_Callback;
    uintptr_t m_Parameter;
    ssize_t m_Result;
    AfterDelivery m_AfterDelivery;
    void* m_AfterDeliveryContext;
    OnDestroy m_OnDestroy;
    void* m_DestroyContext;
    Atomic<size_t> m_References;

    /** Protected by the queue mutex. */
    State m_State;
    void* m_Runner;

    WaitQueue m_CompletionWaiters;
    /** Protected by m_CompletionWaiters. */
    bool m_Completed;
  };

  CallbackDeliveryQueue() : m_Lock(), m_Records(), m_NextGeneration(0) {}

  ~CallbackDeliveryQueue() {
    assert(empty());
  }

  MUST_USE_RESULT Record* create(const Key& key, Callback callback, uintptr_t parameter,
                                 ssize_t result, AfterDelivery afterDelivery = nullptr,
                                 void* afterDeliveryContext = nullptr,
                                 OnDestroy onDestroy = nullptr, void* destroyContext = nullptr) {
    return new Record(key, callback, parameter, result, afterDelivery, afterDeliveryContext,
                      onDestroy, destroyContext);
  }

  size_t nextGeneration() {
    size_t generation = m_NextGeneration += 1;
    if (!generation)
      generation = m_NextGeneration += 1;
    return generation;
  }

  /** Publishes every record while the caller still owns its HCD lock. */
  void publish(List<Record*>& records) {
    LockGuard<Mutex> guard(m_Lock);
    for (List<Record*>::Iterator it = records.begin(); it != records.end(); ++it) {
      assert(findLocked((*it)->m_Key) == nullptr);
      m_Records.pushBack(*it);
    }
  }

  /** Delivers one batch-owned record and drops the batch reference. */
  void deliver(Record* record) {
    bool run = false;
    bool wait = false;
    {
      LockGuard<Mutex> guard(m_Lock);
      if (record->m_State == Record::State::Pending) {
        record->m_State = Record::State::Running;
        record->m_Runner = currentRunner();
        run = true;
      } else if (record->m_State == Record::State::Running) {
        wait = record->m_Runner != currentRunner();
      }
    }

    if (run)
      runRecord(record);
    else if (wait)
      record->waitForCompletion();
    record->release();
  }

  /**
   * Drains the exact transaction generation.
   *
   * Pending records are stolen and delivered inline. A callback draining
   * itself is already inside the required ownership boundary and returns
   * immediately. Only another thread's running callback requires a wait.
   */
  bool drain(const Key& key) {
    Record* record = nullptr;
    bool run = false;
    bool self = false;
    {
      LockGuard<Mutex> guard(m_Lock);
      record = findLocked(key);
      if (!record)
        return false;

      record->retain();
      if (record->m_State == Record::State::Pending) {
        record->m_State = Record::State::Running;
        record->m_Runner = currentRunner();
        run = true;
      } else if (record->m_State == Record::State::Running) {
        self = record->m_Runner == currentRunner();
      }
    }

    if (run)
      runRecord(record);
    else if (!self)
      record->waitForCompletion();

    record->release();
    return true;
  }

  /**
   * Drains every callback after all producers have been quiesced.
   *
   * A callback may drain another pending record, so the queue is searched
   * again after every delivery. A record running on this thread is already
   * inside the required lifetime boundary and is skipped.
   */
  size_t drainAll() {
    size_t drained = 0;
    while (true) {
      Key key = {0, 0};
      bool found = false;
      {
        LockGuard<Mutex> guard(m_Lock);
        for (List<Record*>::Iterator it = m_Records.begin(); it != m_Records.end(); ++it) {
          Record* record = *it;
          if (record->m_State == Record::State::Running && record->m_Runner == currentRunner()) {
            continue;
          }

          key = record->m_Key;
          found = true;
          break;
        }
      }

      if (!found)
        return drained;
      if (drain(key))
        ++drained;
    }
  }

  bool contains(const Key& key) {
    LockGuard<Mutex> guard(m_Lock);
    return findLocked(key) != nullptr;
  }

  size_t activeCount() {
    LockGuard<Mutex> guard(m_Lock);
    return m_Records.count();
  }

  bool empty() {
    return activeCount() == 0;
  }

 private:
  static void* currentRunner() {
    return Processor::information().getCurrentThread();
  }

  Record* findLocked(const Key& key) {
    for (List<Record*>::Iterator it = m_Records.begin(); it != m_Records.end(); ++it) {
      if ((*it)->m_Key == key)
        return *it;
    }
    return nullptr;
  }

  void finishRecord(Record* record) {
    {
      LockGuard<Mutex> guard(m_Lock);
      bool removed = false;
      for (List<Record*>::Iterator it = m_Records.begin(); it != m_Records.end(); ++it) {
        if (*it == record) {
          m_Records.erase(it);
          removed = true;
          break;
        }
      }
      assert(removed);
      record->m_State = Record::State::Complete;
      record->m_Runner = nullptr;
    }
    record->complete();
  }

  void runRecord(Record* record) {
    TerminationDeferral deliveryLifetime;
    assert(Processor::getInterrupts());
    if (record->m_Callback)
      record->m_Callback(record->m_Parameter, record->m_Result);
    if (record->m_AfterDelivery)
      record->m_AfterDelivery(record->m_AfterDeliveryContext);
    finishRecord(record);
  }

  Mutex m_Lock;
  List<Record*> m_Records;
  Atomic<size_t> m_NextGeneration;

  NOT_COPYABLE_OR_ASSIGNABLE(CallbackDeliveryQueue);
};
}  // namespace UsbHcd

#endif
