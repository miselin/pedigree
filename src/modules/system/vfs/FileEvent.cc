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
#include "pedigree/kernel/utilities/assert.h"
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

  bool admit() {
    return m_Notifications.tryEnter();
  }

  void notifyAdmitted(const FileEvent& event) {
    m_pObserver->fileEvent(event);
    m_Notifications.leave();
  }

  void closeAdmission() {
    m_Notifications.close();
  }

  void retire() {
    m_Notifications.closeAndWait();
  }

  size_t sequence = 0;

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
    if (m_NextSequence == ~size_t(0) || !m_Targets.tryPushBack(target))
      return false;
    target->sequence = m_NextSequence++;
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
    OperationBarrier::Lease publication;
    if (!m_Publications.tryAcquire(publication))
      return;
    size_t boundary;
    {
      LockGuard<Mutex> guard(m_Lock);
      if (!m_Open)
        return;
      boundary = m_NextSequence - 1;
    }
    size_t after = 0;
    while (true) {
      SharedPointer<FileEventTarget> selected;
      {
        LockGuard<Mutex> guard(m_Lock);
        if (!m_Open)
          return;
        for (const auto& target : m_Targets) {
          if (target->sequence > after && target->sequence <= boundary &&
              target->interestedIn(event.mask) && target->admit()) {
            selected = target;
            after = target->sequence;
            break;
          }
        }
      }
      if (!selected)
        return;
      selected->notifyAdmitted(event);
    }
  }

  void beginClose(const FileEvent* finalEvent = nullptr) {
    size_t boundary;
    {
      LockGuard<Mutex> guard(m_Lock);
      if (!m_Open)
        return;
      m_Open = false;
      // The closing publisher is separately counted so a concurrent drain
      // cannot return before the final callbacks have been admitted.
      const bool admitted = m_ClosingPublication.tryEnter();
      assert(admitted);
      m_ClosingPublication.close();
      m_Publications.close();
      boundary = m_NextSequence - 1;
    }
    size_t after = 0;
    while (true) {
      SharedPointer<FileEventTarget> selected;
      bool deliver = false;
      {
        LockGuard<Mutex> guard(m_Lock);
        for (const auto& target : m_Targets) {
          if (target->sequence > after && target->sequence <= boundary) {
            selected = target;
            after = target->sequence;
            deliver = finalEvent && target->interestedIn(finalEvent->mask) && target->admit();
            target->closeAdmission();
            break;
          }
        }
      }
      if (!selected)
        break;
      if (deliver)
        selected->notifyAdmitted(*finalEvent);
    }
    m_ClosingPublication.leave();
  }

  void drain() {
    {
      LockGuard<Mutex> guard(m_Lock);
      if (m_Open)
        return;
    }
    m_ClosingPublication.wait();
    m_Publications.wait();
    size_t after = 0;
    while (true) {
      SharedPointer<FileEventTarget> selected;
      {
        LockGuard<Mutex> guard(m_Lock);
        for (const auto& target : m_Targets) {
          if (target->sequence > after) {
            selected = target;
            after = target->sequence;
            break;
          }
        }
      }
      if (!selected)
        return;
      selected->retire();
    }
  }

 private:
  Mutex m_Lock;
  List<SharedPointer<FileEventTarget>> m_Targets;
  bool m_Open;
  size_t m_NextSequence = 1;
  OperationBarrier m_Publications;
  OperationBarrier m_ClosingPublication;
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
  if (!target || !m_FileEventState)
    return false;
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
  if (event.mask && m_FileEventState) {
    SharedPointer<FileEventState> state = m_FileEventState;
    state->notify(event);
  }
}

void FileEventSource::notifyFinalFileEvent(const FileEvent& event) {
  if (event.mask && m_FileEventState) {
    SharedPointer<FileEventState> state = m_FileEventState;
    state->beginClose(&event);
    state->drain();
  }
}

void FileEventSource::closeFileEvents() {
  SharedPointer<FileEventState> state = m_FileEventState;
  if (!state)
    return;
  state->beginClose();
  state->drain();
}

void FileEventSource::beginFinalFileEvent(const FileEvent& event) {
  if (m_FileEventState)
    m_FileEventState->beginClose(&event);
}

void FileEventSource::drainFileEvents() {
  if (m_FileEventState)
    m_FileEventState->drain();
}

void InodeEventSource::publish(const FileEvent& event) {
  notifyFileEvent(event);
}

void InodeEventSource::beginRetirement() {
  beginFinalFileEvent(FileEvent(FileEvents::SourceRetired, StringView(), false));
}

void InodeEventSource::finishRetirement() {
  drainFileEvents();
}
