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

#include "pedigree/kernel/machine/InputManager.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/Event.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/process/eventNumbers.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/utilities/Iterator.h"
#include "pedigree/kernel/utilities/utility.h"

// Incoming relative mouse movements are divided by this
#define MOUSE_REDUCE_FACTOR 1

class InputEvent : public Event
{
  public:
    InputEvent(
        InputManager::InputNotification *pNote, uintptr_t param,
        uintptr_t handlerAddress);
    virtual ~InputEvent();

    virtual size_t serialize(uint8_t *pBuffer);

    static bool unserialize(uint8_t *pBuffer, InputEvent &event);

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

InputManager::InputManager()
    : m_InputQueue(), m_QueueLock(), m_Callbacks()
#if THREADS
      ,
      m_InputQueueSize(0), m_pThread(0)
#endif
      ,
      m_bActive(false)
{
}

InputManager::~InputManager()
{
}

void InputManager::initialise()
{
    m_bActive = true;

// Start the worker thread.
#if THREADS
    m_pThread = new Thread(
        Processor::information().getCurrentThread()->getParent(), &trampoline,
        reinterpret_cast<void *>(this));
    m_pThread->setName("InputManager worker thread");
#else
    WARNING("InputManager: No thread support, no worker thread will be active");
#endif
}

void InputManager::shutdown()
{
    m_bActive = false;

#if THREADS
    m_InputQueueSize.release();
    m_pThread->join();
#endif

    // Clean up lists, in case anything came in while we were canceling.
    m_Callbacks.clear();
    m_InputQueue.clear();
}

void InputManager::keyPressed(uint64_t key)
{
    InputNotification *note = new InputNotification;
    note->type = Key;
    note->data.key.key = key;

    putNotification(note);
}

void InputManager::rawKeyUpdate(uint8_t scancode, bool bKeyUp)
{
    InputNotification *note = new InputNotification;
    note->type = RawKey;
    note->data.rawkey.scancode = scancode;
    note->data.rawkey.keyUp = bKeyUp;

    putNotification(note);
}

void InputManager::machineKeyUpdate(uint8_t scancode, bool bKeyUp)
{
    InputNotification *note = new InputNotification;
    note->type = MachineKey;
    note->data.machinekey.scancode = scancode;
    note->data.machinekey.keyUp = bKeyUp;

    putNotification(note);
}

void InputManager::mouseUpdate(
    ssize_t relX, ssize_t relY, ssize_t relZ, uint32_t buttonBitmap)
{
    // Smooth input out
    relX /= MOUSE_REDUCE_FACTOR;
    relY /= MOUSE_REDUCE_FACTOR;
    relZ /= MOUSE_REDUCE_FACTOR;

    InputNotification *note = new InputNotification;
    note->type = Mouse;
    note->data.pointy.relx = relX;
    note->data.pointy.rely = relY;
    note->data.pointy.relz = relZ;
    for (size_t i = 0; i < 64; i++)
        note->data.pointy.buttons[i] = buttonBitmap & (1 << i);

    putNotification(note);
}

void InputManager::joystickUpdate(
    ssize_t relX, ssize_t relY, ssize_t relZ, uint32_t buttonBitmap)
{
    InputNotification *note = new InputNotification;
    note->type = Joystick;
    note->data.pointy.relx = relX;
    note->data.pointy.rely = relY;
    note->data.pointy.relz = relZ;
    for (size_t i = 0; i < 64; i++)
        note->data.pointy.buttons[i] = buttonBitmap & (1 << i);

    putNotification(note);
}

void InputManager::putNotification(InputNotification *note)
{
    // Early short-circuit - don't push onto the queue if no callbacks present.
    if (m_Callbacks.count() == 0)
    {
        WARNING("InputManager dropping input - no callbacks to send to!");
        delete note;
        return;
    }

    LockGuard<Spinlock> guard(m_QueueLock);

    // Can we mitigate this notification?
    if (note->type == Mouse)
    {
        for (List<InputNotification *>::Iterator it = m_InputQueue.begin();
             it != m_InputQueue.end(); it++)
        {
            if ((*it)->type == Mouse)
            {
                (*it)->data.pointy.relx += note->data.pointy.relx;
                (*it)->data.pointy.rely += note->data.pointy.rely;
                (*it)->data.pointy.relz += note->data.pointy.relz;

                for (int i = 0; i < 64; i++)
                {
                    if (note->data.pointy.buttons[i])
                        (*it)->data.pointy.buttons[i] = true;
                }

                // Merged, this precise logic means only one mouse event is ever
                // in the queue, so it's safe to just return here.
                return;
            }
        }
    }

#if THREADS
    m_InputQueue.pushBack(note);
    m_InputQueueSize.release();
#else
    // No need for locking, as no threads exist
    for (List<CallbackItem *>::Iterator it = m_Callbacks.begin();
         it != m_Callbacks.end(); it++)
    {
        if (*it)
        {
            callback_t func = (*it)->func;
            note->meta = (*it)->meta;
            func(*note);
        }
    }

    delete note;
#endif
}

void InputManager::installCallback(
    CallbackType filter, callback_t callback, void *meta, Thread *pThread,
    uintptr_t param)
{
    LockGuard<Spinlock> guard(m_QueueLock);
    CallbackItem *item = new CallbackItem;
    item->func = callback;
#if THREADS
    item->pThread = pThread;
#endif
    item->nParam = param;
    item->filter = filter;
    item->meta = meta;
    m_Callbacks.pushBack(item);
}

void InputManager::removeCallback(
    callback_t callback, void *meta, Thread *pThread)
{
    LockGuard<Spinlock> guard(m_QueueLock);
    for (List<CallbackItem *>::Iterator it = m_Callbacks.begin();
         it != m_Callbacks.end();)
    {
        if (*it)
        {
            if (
#if THREADS
                (pThread == (*it)->pThread) &&
#endif
                (callback == (*it)->func) && (meta == (*it)->meta))
            {
                delete *it;
                it = m_Callbacks.erase(it);
                continue;
            }
        }

        ++it;
    }
}

bool InputManager::removeCallbackByThread(Thread *pThread)
{
#if THREADS
    LockGuard<Spinlock> guard(m_QueueLock);
    for (List<CallbackItem *>::Iterator it = m_Callbacks.begin();
         it != m_Callbacks.end();)
    {
        if (*it)
        {
            if (pThread == (*it)->pThread)
            {
                delete *it;
                it = m_Callbacks.erase(it);
                continue;
            }
        }

        ++it;
    }

    return false;
#endif
}

int InputManager::trampoline(void *ptr)
{
    InputManager *p = reinterpret_cast<InputManager *>(ptr);
    p->mainThread();
    return 0;
}

void InputManager::mainThread()
{
#if THREADS
    while (isActive())
    {
        m_InputQueueSize.acquire();
        if (!m_InputQueue.count())
            continue;  /// \todo Handle exit condition

        m_QueueLock.acquire();
        InputNotification *pNote = m_InputQueue.popFront();
        m_QueueLock.release();

        if (m_Callbacks.count() == 0)
        {
            // Drop the input on the floor - no callbacks to read it in!
            WARNING("InputManager dropping input - no callbacks to send to!");
            delete pNote;
            continue;
        }

        // Don't send the key to applications if it was zero
        if (!pNote)
            continue;

        for (List<CallbackItem *>::Iterator it = m_Callbacks.begin();
             it != m_Callbacks.end(); it++)
        {
            if (*it)
            {
                if ((*it)->filter & pNote->type)
                {
                    Thread *pThread = (*it)->pThread;
                    callback_t func = (*it)->func;
                    if (!pThread)
                    {
                        /// \todo Verify that the callback is in fact in the
                        /// kernel
                        pNote->meta = (*it)->meta;
                        func(*pNote);
                        continue;
                    }

                    InputEvent *pEvent = new InputEvent(
                        pNote, (*it)->nParam,
                        reinterpret_cast<uintptr_t>(func));
                    NOTICE("InputManager: sending event " << pEvent << "!");
                    if (!pThread->sendEvent(pEvent))
                    {
                        WARNING("InputManager - Thread::sendEvent failed, "
                                "skipping this callback");
                        delete pEvent;
                    }
                }
            }
        }

        // Yield to run the events we just transmitted.
        Scheduler::instance().yield();

        delete pNote;
    }
#endif
}

InputEvent::InputEvent(
    InputManager::InputNotification *pNote, uintptr_t param,
    uintptr_t handlerAddress)
    : Event(handlerAddress, true, 0), m_Notification(), m_nParam(param)
{
    m_Notification = *pNote;
}

InputEvent::~InputEvent()
{
}

size_t InputEvent::serialize(uint8_t *pBuffer)
{
    void *alignedBuffer = ASSUME_ALIGNMENT(pBuffer, sizeof(uintptr_t));
    uintptr_t *buf = reinterpret_cast<uintptr_t *>(alignedBuffer);
    buf[0] = EventNumbers::InputEvent;
    buf[1] = m_nParam;
    MemoryCopy(
        &buf[2], &m_Notification, sizeof(InputManager::InputNotification));
    return sizeof(InputManager::InputNotification) + (sizeof(uintptr_t) * 2);
}

bool InputEvent::unserialize(uint8_t *pBuffer, InputEvent &event)
{
    void *alignedBuffer = ASSUME_ALIGNMENT(pBuffer, sizeof(uintptr_t));
    uintptr_t *buf = reinterpret_cast<uintptr_t *>(alignedBuffer);
    if (*buf != EventNumbers::InputEvent)
        return false;

    MemoryCopy(
        &event.m_Notification, &buf[2],
        sizeof(InputManager::InputNotification));
    return true;
}

size_t InputEvent::getNumber()
{
    return EventNumbers::InputEvent;
}

InputManager::CallbackType InputEvent::getType()
{
    return m_Notification.type;
}

uint64_t InputEvent::getKey()
{
    return m_Notification.data.key.key;
}

ssize_t InputEvent::getRelX()
{
    return m_Notification.data.pointy.relx;
}

ssize_t InputEvent::getRelY()
{
    return m_Notification.data.pointy.rely;
}

ssize_t InputEvent::getRelZ()
{
    return m_Notification.data.pointy.relz;
}

void InputEvent::getButtonStates(bool states[64], size_t maxDesired)
{
    for (size_t i = 0; i < maxDesired; i++)
        states[i] = m_Notification.data.pointy.buttons[i];
}
