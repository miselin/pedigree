/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#include "FileEvent.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/OperationBarrier.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/utility.h"

class FileEventTarget {
 public:
  FileEventTarget(FileEventMask interest, FileEventObserver* observer)
      : m_Interest(interest), m_pObserver(observer), m_Notifications() {}

  ~FileEventTarget() {
    if (!m_Notifications.isClosedAndDrained()) {
      m_Notifications.closeAndWait();
    }
  }

  bool interestedIn(FileEventMask mask) const {
    return (mask & m_Interest) != 0;
  }

  void notify(const FileEvent& event) {
    OperationBarrier::Lease notification;
    if (m_Notifications.tryAcquire(notification)) {
      m_pObserver->fileEvent(event);
    }
  }

  bool admit() {
    return m_Notifications.tryEnter();
  }

  void notifyAdmitted(const FileEvent& event) {
    m_pObserver->fileEvent(event);
    m_Notifications.leave();
  }

  void retire() {
    m_Notifications.closeAndWait();
  }

 private:
  FileEventMask m_Interest;
  FileEventObserver* m_pObserver;
  OperationBarrier m_Notifications;
};

class FileEventState {
 public:
  FileEventState() : m_Lock(), m_Targets(), m_Open(true) {}

  ~FileEventState() {
    LockGuard<Mutex> guard(m_Lock);
    if (m_Targets.count()) {
      FATAL("FileEventState destroyed with live subscriptions.");
    }
  }

  bool add(const SharedPointer<FileEventTarget>& target) {
    LockGuard<Mutex> guard(m_Lock);
    if (!m_Open) {
      return false;
    }
    m_Targets.pushBack(target);
    return true;
  }

  void remove(const SharedPointer<FileEventTarget>& target) {
    {
      LockGuard<Mutex> guard(m_Lock);
      for (auto it = m_Targets.begin(); it != m_Targets.end(); ++it) {
        if (*it == target) {
          m_Targets.erase(it);
          break;
        }
      }
    }
    target->retire();
  }

  void notify(const FileEvent& event) {
    Vector<SharedPointer<FileEventTarget>> targets;
    {
      LockGuard<Mutex> guard(m_Lock);
      if (!m_Open) {
        return;
      }
      for (const auto& target : m_Targets) {
        if (target->interestedIn(event.mask)) {
          targets.pushBack(target);
        }
      }
    }
    for (auto& target : targets) {
      target->notify(event);
    }
  }

  void close(const FileEvent* finalEvent = nullptr) {
    Vector<SharedPointer<FileEventTarget>> targets;
    Vector<SharedPointer<FileEventTarget>> finalTargets;
    bool deliverFinal = false;
    {
      LockGuard<Mutex> guard(m_Lock);
      if (m_Open) {
        m_Open = false;
        deliverFinal = finalEvent != nullptr;
      }
      for (const auto& target : m_Targets) {
        targets.pushBack(target);
        if (deliverFinal && target->interestedIn(finalEvent->mask) && target->admit()) {
          finalTargets.pushBack(target);
        }
      }
    }
    for (auto& target : finalTargets) {
      target->notifyAdmitted(*finalEvent);
    }
    for (auto& target : targets) {
      target->retire();
    }
  }

 private:
  Mutex m_Lock;
  List<SharedPointer<FileEventTarget>> m_Targets;
  bool m_Open;
};

FileEventSubscription::FileEventSubscription() : m_State(), m_Target(), m_Observer() {}

FileEventSubscription::FileEventSubscription(FileEventSubscription&& other) noexcept
    : m_State(pedigree_std::move(other.m_State)),
      m_Target(pedigree_std::move(other.m_Target)),
      m_Observer(pedigree_std::move(other.m_Observer)) {}

FileEventSubscription::~FileEventSubscription() {
  reset();
}

FileEventSubscription& FileEventSubscription::operator=(FileEventSubscription&& other) noexcept {
  if (this != &other) {
    reset();
    m_State = pedigree_std::move(other.m_State);
    m_Target = pedigree_std::move(other.m_Target);
    m_Observer = pedigree_std::move(other.m_Observer);
  }
  return *this;
}

FileEventSubscription::operator bool() const {
  return static_cast<bool>(m_State) && static_cast<bool>(m_Target);
}

void FileEventSubscription::reset() {
  SharedPointer<FileEventState> state = m_State;
  SharedPointer<FileEventTarget> target = m_Target;
  if (state && target) {
    state->remove(target);
  }
  m_Target.reset();
  m_Observer.reset();
  m_State.reset();
}

FileEventSource::FileEventSource() : m_FileEventState(new FileEventState) {}

FileEventSource::~FileEventSource() {
  closeFileEvents();
}

bool FileEventSource::subscribeFileEvents(FileEventMask interest,
                                          const SharedPointer<FileEventObserver>& observer,
                                          FileEventSubscription& subscription) {
  subscription.reset();
  if (!interest || !observer) {
    return false;
  }

  SharedPointer<FileEventTarget> target(new FileEventTarget(interest, observer.get()));
  subscription.m_State = m_FileEventState;
  subscription.m_Target = target;
  subscription.m_Observer = observer;
  if (!m_FileEventState->add(target)) {
    subscription.reset();
    return false;
  }
  return true;
}

void FileEventSource::notifyFileEvent(const FileEvent& event) {
  if (event.mask) {
    SharedPointer<FileEventState> state = m_FileEventState;
    state->notify(event);
  }
}

void FileEventSource::notifyFinalFileEvent(const FileEvent& event) {
  if (event.mask) {
    SharedPointer<FileEventState> state = m_FileEventState;
    state->close(&event);
  }
}

void FileEventSource::closeFileEvents() {
  SharedPointer<FileEventState> state = m_FileEventState;
  state->close();
}
