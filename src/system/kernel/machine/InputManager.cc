/*
 * Copyright (c) 2008-2014, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/InputManager.h"
#include "pedigree/kernel/process/Event.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/process/eventNumbers.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/utilities/Iterator.h"
#include "pedigree/kernel/utilities/utility.h"

// Incoming relative mouse movements are divided by this
#define MOUSE_REDUCE_FACTOR 1

class InputEvent : public Event {
 public:
  InputEvent(InputManager::InputNotification* pNote, uintptr_t param, uintptr_t handlerAddress);
  virtual ~InputEvent();

  virtual size_t serialize(uint8_t* pBuffer);

  static bool unserialize(uint8_t* pBuffer, InputEvent& event);

  virtual size_t getNumber();

  InputManager::CallbackType getType();

  uint64_t getKey();
  ssize_t getRelX();
  ssize_t getRelY();
  ssize_t getRelZ();

  void getButtonStates(bool states[64], size_t maxDesired = 64);

 private:
  InputManager::InputNotification m_Notification;

  uintptr_t m_nParam;
};

InputManager InputManager::m_Instance;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
InputManager::CallbackPinHook InputManager::m_CallbackPinHook = nullptr;
#endif

InputManager::InputManager()
    : m_InputQueue(),
      m_QueueLock(),
      m_Callbacks()
#if THREADS
      ,
      m_InputQueueSize(0),
      m_pThread(0),
      m_pCallbackDispatchThread(nullptr)
#endif
      ,
      m_bActive(false) {
}

InputManager::~InputManager() {}

void InputManager::initialise() {
  m_bActive = true;

// Start the worker thread.
#if THREADS
  m_pThread = new Thread(Processor::information().getCurrentThread()->getParent(), &trampoline,
                         reinterpret_cast<void*>(this));
  m_pThread->setName("InputManager worker thread");
#else
  WARNING("InputManager: No thread support, no worker thread will be active");
#endif
}

void InputManager::shutdown() {
  m_bActive = false;

#if THREADS
  m_InputQueueSize.release();
  if (m_pThread) {
    m_pThread->joinForCompletion();
    m_pThread = nullptr;
  }
#endif

  // Clean up lists, in case anything came in while we were canceling.
  Vector<CallbackItem*> callbacks;
  Vector<InputNotification*> notifications;
  m_QueueLock.acquire();
  while (m_Callbacks.count()) {
    CallbackItem* item = m_Callbacks.popFront();
#if THREADS
    if (item->inFlight) {
      FATAL("InputManager shut down with a pinned callback.");
    }
#endif
    callbacks.pushBack(item);
  }
  while (m_InputQueue.count()) {
    notifications.pushBack(m_InputQueue.popFront());
  }
  m_QueueLock.release();

  for (auto item : callbacks) {
    delete item;
  }
  for (auto notification : notifications) {
    delete notification;
  }
}

void InputManager::keyPressed(uint64_t key) {
  InputNotification* note = new InputNotification;
  note->type = Key;
  note->data.key.key = key;

  putNotification(note);
}

void InputManager::rawKeyUpdate(uint8_t scancode, bool bKeyUp) {
  InputNotification* note = new InputNotification;
  note->type = RawKey;
  note->data.rawkey.scancode = scancode;
  note->data.rawkey.keyUp = bKeyUp;

  putNotification(note);
}

void InputManager::machineKeyUpdate(uint8_t scancode, bool bKeyUp) {
  InputNotification* note = new InputNotification;
  note->type = MachineKey;
  note->data.machinekey.scancode = scancode;
  note->data.machinekey.keyUp = bKeyUp;

  putNotification(note);
}

void InputManager::mouseUpdate(ssize_t relX, ssize_t relY, ssize_t relZ, uint32_t buttonBitmap) {
  // Smooth input out
  relX /= MOUSE_REDUCE_FACTOR;
  relY /= MOUSE_REDUCE_FACTOR;
  relZ /= MOUSE_REDUCE_FACTOR;

  InputNotification* note = new InputNotification;
  note->type = Mouse;
  note->data.pointy.relx = relX;
  note->data.pointy.rely = relY;
  note->data.pointy.relz = relZ;
  for (size_t i = 0; i < 64; i++)
    note->data.pointy.buttons[i] = buttonBitmap & (1 << i);

  putNotification(note);
}

void InputManager::joystickUpdate(ssize_t relX, ssize_t relY, ssize_t relZ, uint32_t buttonBitmap) {
  InputNotification* note = new InputNotification;
  note->type = Joystick;
  note->data.pointy.relx = relX;
  note->data.pointy.rely = relY;
  note->data.pointy.relz = relZ;
  for (size_t i = 0; i < 64; i++)
    note->data.pointy.buttons[i] = buttonBitmap & (1 << i);

  putNotification(note);
}

void InputManager::putNotification(InputNotification* note) {
#if THREADS
  bool merged = false;
  bool accepted = false;
  m_QueueLock.acquire();
  if (m_bActive && m_Callbacks.count()) {
    // Mitigation keeps at most one queued relative-mouse notification.
    if (note->type == Mouse) {
      for (auto queued : m_InputQueue) {
        if (queued->type != Mouse) {
          continue;
        }

        queued->data.pointy.relx += note->data.pointy.relx;
        queued->data.pointy.rely += note->data.pointy.rely;
        queued->data.pointy.relz += note->data.pointy.relz;
        for (size_t i = 0; i < 64; ++i) {
          if (note->data.pointy.buttons[i]) {
            queued->data.pointy.buttons[i] = true;
          }
        }
        merged = true;
        break;
      }
    }

    if (!merged) {
      m_InputQueue.pushBack(note);
      accepted = true;
    }
  }
  m_QueueLock.release();

  if (merged) {
    delete note;
    return;
  }
  if (!accepted) {
    WARNING("InputManager dropping input - no callbacks to send to!");
    delete note;
    return;
  }

  m_InputQueueSize.release();
#else
  struct SynchronousCallback {
    callback_t func;
    void* meta;
  };
  Vector<SynchronousCallback> callbacks;

  m_QueueLock.acquire();
  for (auto item : m_Callbacks) {
    if (item && (item->filter & note->type)) {
      callbacks.pushBack({item->func, item->meta});
    }
  }
  m_QueueLock.release();

  for (auto callback : callbacks) {
    note->meta = callback.meta;
    callback.func(*note);
  }
  delete note;
#endif
}

void InputManager::installCallback(CallbackType filter, callback_t callback, void* meta,
                                   Thread* pThread, uintptr_t param) {
  if (!callback) {
    return;
  }

  CallbackItem* item = new CallbackItem;
  item->func = callback;
#if THREADS
  item->pThread = pThread;
#endif
  item->nParam = param;
  item->filter = filter;
  item->meta = meta;
#if THREADS
  item->inFlight = 0;
  item->enabled = true;
  item->draining = false;
  item->removers = 0;
  item->deferredRemoval = false;
#endif

  LockGuard<Spinlock> guard(m_QueueLock);
  m_Callbacks.pushBack(item);
}

void InputManager::removeCallback(callback_t callback, void* meta, Thread* pThread) {
#if THREADS
  removeCallbacks(callback, meta, pThread, false);
#else
  LockGuard<Spinlock> guard(m_QueueLock);
  for (List<CallbackItem*>::Iterator it = m_Callbacks.begin(); it != m_Callbacks.end();) {
    if (*it) {
      if (
#if THREADS
          (pThread == (*it)->pThread) &&
#endif
          (callback == (*it)->func) && (meta == (*it)->meta)) {
        delete *it;
        it = m_Callbacks.erase(it);
        continue;
      }
    }

    ++it;
  }
#endif
}

bool InputManager::removeCallbackByThread(Thread* pThread) {
#if THREADS
  return removeCallbacks(nullptr, nullptr, pThread, true);
#else
  return false;
#endif
}

#if THREADS
bool InputManager::removeCallbacks(callback_t callback, void* meta, Thread* pThread,
                                   bool byThread) {
  if (byThread && !pThread) {
    return false;
  }

  TerminationDeferral terminationDeferral;
  Vector<CallbackItem*> drain;
  Vector<CallbackItem*> deleteNow;
  bool removed = false;

  Thread* current = Processor::information().getCurrentThread();
  m_QueueLock.acquire();
  const bool callbackContext = current && current == m_pCallbackDispatchThread;

  for (List<CallbackItem*>::Iterator it = m_Callbacks.begin(); it != m_Callbacks.end();) {
    CallbackItem* item = *it;
    const bool matches =
        byThread ? item->pThread == pThread
                 : item->pThread == pThread && item->func == callback && item->meta == meta;
    if (!matches) {
      ++it;
      continue;
    }

    removed = true;
    item->enabled = false;

    if (callbackContext) {
      if (!item->inFlight) {
        it = m_Callbacks.erase(it);
        deleteNow.pushBack(item);
      } else {
        // A callback cannot drain its own snapshot pin. Keep the
        // disabled item discoverable so an external remover can take
        // over the synchronous drain before the callback returns.
        if (!item->draining) {
          item->deferredRemoval = true;
        }
        ++it;
      }
    } else {
      item->deferredRemoval = false;
      item->draining = true;
      ++item->removers;
      drain.pushBack(item);
      ++it;
    }
  }
  m_QueueLock.release();

  for (auto item : deleteNow) {
    delete item;
  }
  for (auto item : drain) {
    drainCallback(item);
  }

  return removed;
}

void InputManager::drainCallback(CallbackItem* item) {
  while (true) {
    bool complete = false;
    {
      auto waitGuard = item->drainWaiters.acquire();
      m_QueueLock.acquire();
      if (!item->inFlight) {
        complete = true;
        m_QueueLock.release();
      } else {
        m_QueueLock.release();
        const WaitQueue::WakeReason reason =
            waitGuard.waitForCompletion(WaitQueue::Channel(item), Thread::CallbackDrain,
                                        reinterpret_cast<uintptr_t>(item->func));
        (void)reason;
      }
    }

    if (complete) {
      break;
    }
  }

  bool deleteItem = false;
  m_QueueLock.acquire();
  if (!item->removers) {
    FATAL("InputManager callback remover underflow.");
  }
  --item->removers;
  if (!item->removers) {
    for (List<CallbackItem*>::Iterator it = m_Callbacks.begin(); it != m_Callbacks.end(); ++it) {
      if (*it == item) {
        m_Callbacks.erase(it);
        deleteItem = true;
        break;
      }
    }
  }
  m_QueueLock.release();

  if (deleteItem) {
    delete item;
  }
}
#endif

int InputManager::trampoline(void* ptr) {
  InputManager* p = reinterpret_cast<InputManager*>(ptr);
  p->mainThread();
  return 0;
}

void InputManager::mainThread() {
#if THREADS
  while (isActive()) {
    if (!m_InputQueueSize.acquire()) {
      continue;
    }

    Vector<CallbackItem*> callbacks;
    InputNotification* note = nullptr;

    m_QueueLock.acquire();
    if (m_InputQueue.count()) {
      note = m_InputQueue.popFront();
    }
    if (note) {
      for (auto item : m_Callbacks) {
        if (item && item->enabled && (item->filter & note->type)) {
          ++item->inFlight;
          callbacks.pushBack(item);
        }
      }
    }
    m_QueueLock.release();

    if (!note) {
      continue;
    }

    if (!callbacks.count()) {
      WARNING("InputManager dropping input - no callbacks to send to!");
      delete note;
      continue;
    }

    TerminationDeferral dispatchDeferral;
    Thread* current = Processor::information().getCurrentThread();
    for (auto item : callbacks) {
      callback_t func = nullptr;
      Thread* target = nullptr;
      uintptr_t param = 0;
      void* meta = nullptr;

      m_QueueLock.acquire();
      if (item->enabled) {
        func = item->func;
        target = item->pThread;
        param = item->nParam;
        meta = item->meta;
        m_pCallbackDispatchThread = current;
      }
      m_QueueLock.release();

      if (func) {
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
        CallbackPinHook hook = __atomic_load_n(&m_CallbackPinHook, __ATOMIC_ACQUIRE);
        if (hook) {
          hook(func, meta);
        }
#endif

        if (!target) {
          note->meta = meta;
          func(*note);
        } else {
          InputEvent* event = new InputEvent(note, param, reinterpret_cast<uintptr_t>(func));
          NOTICE("InputManager: sending event " << event << "!");
          if (!target->sendEvent(event)) {
            WARNING(
                "InputManager - Thread::sendEvent failed, "
                "skipping this callback");
            delete event;
          }
        }
      }

      bool deleteDeferred = false;
      {
        auto completionGuard = item->drainWaiters.acquire();
        bool wakeDrainer = false;
        m_QueueLock.acquire();
        if (func) {
          m_pCallbackDispatchThread = nullptr;
        }
        if (!item->inFlight) {
          FATAL("InputManager callback pin underflow.");
        }
        --item->inFlight;
        if (!item->inFlight) {
          wakeDrainer = item->draining;
          deleteDeferred = item->deferredRemoval;
          if (deleteDeferred) {
            for (List<CallbackItem*>::Iterator it = m_Callbacks.begin(); it != m_Callbacks.end();
                 ++it) {
              if (*it == item) {
                m_Callbacks.erase(it);
                break;
              }
            }
          }
        }
        m_QueueLock.release();

        if (wakeDrainer) {
          completionGuard.wakeAll(WaitQueue::WakeReason::Signalled, WaitQueue::Channel(item));
        }
      }
      if (deleteDeferred) {
        delete item;
      }
    }

    // Yield to run the events we just transmitted.
    Scheduler::instance().yield();

    delete note;
  }
#endif
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
void InputManager::setCallbackPinHook(CallbackPinHook hook) {
  __atomic_store_n(&m_CallbackPinHook, hook, __ATOMIC_RELEASE);
}
#endif

InputEvent::InputEvent(InputManager::InputNotification* pNote, uintptr_t param,
                       uintptr_t handlerAddress)
    : Event(handlerAddress, true, 0), m_Notification(), m_nParam(param) {
  m_Notification = *pNote;
}

InputEvent::~InputEvent() {}

size_t InputEvent::serialize(uint8_t* pBuffer) {
  void* alignedBuffer = ASSUME_ALIGNMENT(pBuffer, sizeof(uintptr_t));
  uintptr_t* buf = reinterpret_cast<uintptr_t*>(alignedBuffer);
  buf[0] = EventNumbers::InputEvent;
  buf[1] = m_nParam;
  MemoryCopy(&buf[2], &m_Notification, sizeof(InputManager::InputNotification));
  return sizeof(InputManager::InputNotification) + (sizeof(uintptr_t) * 2);
}

bool InputEvent::unserialize(uint8_t* pBuffer, InputEvent& event) {
  void* alignedBuffer = ASSUME_ALIGNMENT(pBuffer, sizeof(uintptr_t));
  uintptr_t* buf = reinterpret_cast<uintptr_t*>(alignedBuffer);
  if (*buf != EventNumbers::InputEvent)
    return false;

  MemoryCopy(&event.m_Notification, &buf[2], sizeof(InputManager::InputNotification));
  return true;
}

size_t InputEvent::getNumber() {
  return EventNumbers::InputEvent;
}

InputManager::CallbackType InputEvent::getType() {
  return m_Notification.type;
}

uint64_t InputEvent::getKey() {
  return m_Notification.data.key.key;
}

ssize_t InputEvent::getRelX() {
  return m_Notification.data.pointy.relx;
}

ssize_t InputEvent::getRelY() {
  return m_Notification.data.pointy.rely;
}

ssize_t InputEvent::getRelZ() {
  return m_Notification.data.pointy.relz;
}

void InputEvent::getButtonStates(bool states[64], size_t maxDesired) {
  for (size_t i = 0; i < maxDesired; i++)
    states[i] = m_Notification.data.pointy.buttons[i];
}
