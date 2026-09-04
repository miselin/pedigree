/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/OperationBarrier.h"
#include "pedigree/kernel/process/Readiness.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/utility.h"

namespace {
constexpr ReadyMask AlwaysReported = ReadyError | ReadyHangup | ReadyInvalid;
}

class ReadinessTarget {
 public:
  ReadinessTarget(ReadyMask interest, ReadinessObserver* observer)
      : m_Interest(interest), m_pObserver(observer), m_Notifications() {}

  ~ReadinessTarget() {
    if (!m_Notifications.isClosedAndDrained()) {
      m_Notifications.closeAndWait();
    }
  }

  bool interestedIn(ReadyMask mask) const {
    return (mask & (m_Interest | AlwaysReported)) != 0;
  }

  void notify(ReadyMask mask) {
    OperationBarrier::Lease notification;
    if (!m_Notifications.tryAcquire(notification)) {
      return;
    }

    m_pObserver->readinessChanged(mask);
  }

  void retire() {
    m_Notifications.closeAndWait();
  }

 private:
  ReadyMask m_Interest;
  ReadinessObserver* m_pObserver;
  OperationBarrier m_Notifications;
};

class ReadinessState {
 public:
  ReadinessState() : m_Lock(), m_Targets(), m_bOpen(true) {}

  ~ReadinessState() {
    LockGuard<Mutex> guard(m_Lock);
    if (m_Targets.count()) {
      FATAL("ReadinessState destroyed with live subscriptions.");
    }
  }

  bool add(const SharedPointer<ReadinessTarget>& target) {
    LockGuard<Mutex> guard(m_Lock);
    if (!m_bOpen) {
      return false;
    }

    m_Targets.pushBack(target);
    return true;
  }

  void remove(const SharedPointer<ReadinessTarget>& target) {
    {
      LockGuard<Mutex> guard(m_Lock);
      for (List<SharedPointer<ReadinessTarget>>::Iterator it = m_Targets.begin();
           it != m_Targets.end(); ++it) {
        if (*it == target) {
          m_Targets.erase(it);
          break;
        }
      }
    }

    target->retire();
  }

  void notify(ReadyMask mask) {
    Vector<SharedPointer<ReadinessTarget>> targets;
    {
      LockGuard<Mutex> guard(m_Lock);
      for (const auto& target : m_Targets) {
        if (target->interestedIn(mask)) {
          targets.pushBack(target);
        }
      }
    }

    for (auto& target : targets) {
      target->notify(mask);
    }
  }

  void close(ReadyMask mask) {
    Vector<SharedPointer<ReadinessTarget>> targets;
    {
      LockGuard<Mutex> guard(m_Lock);
      if (!m_bOpen) {
        return;
      }

      m_bOpen = false;
      for (const auto& target : m_Targets) {
        if (target->interestedIn(mask)) {
          targets.pushBack(target);
        }
      }
    }

    for (auto& target : targets) {
      target->notify(mask);
    }
  }

 private:
  Mutex m_Lock;
  List<SharedPointer<ReadinessTarget>> m_Targets;
  bool m_bOpen;
};

ReadinessSubscription::ReadinessSubscription() : m_State(), m_Target(), m_Observer() {}

ReadinessSubscription::ReadinessSubscription(ReadinessSubscription&& other) noexcept
    : m_State(pedigree_std::move(other.m_State)),
      m_Target(pedigree_std::move(other.m_Target)),
      m_Observer(pedigree_std::move(other.m_Observer)) {}

ReadinessSubscription::~ReadinessSubscription() {
  reset();
}

ReadinessSubscription& ReadinessSubscription::operator=(ReadinessSubscription&& other) noexcept {
  if (this != &other) {
    reset();
    m_State = pedigree_std::move(other.m_State);
    m_Target = pedigree_std::move(other.m_Target);
    m_Observer = pedigree_std::move(other.m_Observer);
  }
  return *this;
}

ReadinessSubscription::operator bool() const {
  return static_cast<bool>(m_State) && static_cast<bool>(m_Target);
}

void ReadinessSubscription::reset() {
  SharedPointer<ReadinessState> state = m_State;
  SharedPointer<ReadinessTarget> target = m_Target;
  if (state && target) {
    state->remove(target);
  }

  m_Target.reset();
  m_Observer.reset();
  m_State.reset();
}

ReadinessSource::ReadinessSource() : m_ReadinessState(new ReadinessState) {}

ReadinessSource::~ReadinessSource() {
  closeReadiness();
}

ReadinessGenerations ReadinessSource::readinessGenerations() {
  return ReadinessGenerations();
}

bool ReadinessSource::subscribeReadiness(ReadyMask interest,
                                         const SharedPointer<ReadinessObserver>& observer,
                                         ReadinessSubscription& subscription) {
  subscription.reset();
  if (!observer || !interest) {
    return false;
  }

  SharedPointer<ReadinessTarget> target(new ReadinessTarget(interest, observer.get()));
  subscription.m_State = m_ReadinessState;
  subscription.m_Target = target;
  subscription.m_Observer = observer;
  if (!m_ReadinessState->add(target)) {
    subscription.reset();
    return false;
  }

  return true;
}

void ReadinessSource::notifyReadiness(ReadyMask mask) {
  if (mask) {
    SharedPointer<ReadinessState> state = m_ReadinessState;
    state->notify(mask);
  }
}

void ReadinessSource::closeReadiness(ReadyMask mask) {
  SharedPointer<ReadinessState> state = m_ReadinessState;
  state->close(mask);
}
