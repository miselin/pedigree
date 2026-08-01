/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_MACHINE_MACH_PC_RTCALARMQUEUE_H
#define PEDIGREE_KERNEL_MACHINE_MACH_PC_RTCALARMQUEUE_H

#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/assert.h"

/**
 * Allocation-free ownership state for RTC alarms.
 *
 * The caller serialises every operation. Claimed records remain visible until
 * completeDispatch(), so a remote remover can distinguish a committed
 * delivery from an alarm which is still safe to cancel. A same-context
 * remover cannot drain its own stack and is therefore recorded as deferred.
 */
class RtcAlarmQueue
{
  public:
    class Record
    {
      public:
        Record()
            : m_pEvent(nullptr), m_Deadline(0), m_pTarget(nullptr),
              m_pNext(nullptr), m_pDispatchOwner(nullptr),
              m_bDispatching(false), m_bDeferredRemoval(false)
        {
        }

        void prepare(void *event, uint64_t deadline, void *target)
        {
            assert(!m_pNext);
            assert(!m_bDispatching);
            m_pEvent = event;
            m_Deadline = deadline;
            m_pTarget = target;
            m_pDispatchOwner = nullptr;
            m_bDeferredRemoval = false;
        }

        void *event() const
        {
            return m_pEvent;
        }

        uint64_t deadline() const
        {
            return m_Deadline;
        }

        void *target() const
        {
            return m_pTarget;
        }

        Record *next() const
        {
            return m_pNext;
        }

        bool dispatching() const
        {
            return m_bDispatching;
        }

        bool deferredRemoval() const
        {
            return m_bDeferredRemoval;
        }

      private:
        friend class RtcAlarmQueue;

        void clear()
        {
            m_pEvent = nullptr;
            m_Deadline = 0;
            m_pTarget = nullptr;
            m_pDispatchOwner = nullptr;
            m_bDispatching = false;
            m_bDeferredRemoval = false;
        }

        void *m_pEvent;
        uint64_t m_Deadline;
        void *m_pTarget;
        Record *m_pNext;
        void *m_pDispatchOwner;
        bool m_bDispatching;
        bool m_bDeferredRemoval;
    };

    enum class RemovalDisposition
    {
        NotFound,
        Removed,
        SelfDeferred,
        RemoteInFlight,
    };

    struct Removal
    {
        RemovalDisposition disposition;
        Record *record;
        uint64_t deadline;
    };

    RtcAlarmQueue() : m_pFirst(nullptr), m_pLast(nullptr), m_pFree(nullptr)
    {
    }

    Record *takeReusable()
    {
        Record *record = m_pFree;
        if (record)
        {
            m_pFree = record->m_pNext;
            record->m_pNext = nullptr;
        }
        return record;
    }

    void add(Record *record)
    {
        assert(record);
        assert(record->m_pEvent);
        assert(record->m_pTarget);
        assert(!record->m_pNext);
        assert(!record->m_bDispatching);
        if (m_pLast)
        {
            m_pLast->m_pNext = record;
        }
        else
        {
            m_pFirst = record;
        }
        m_pLast = record;
    }

    Record *claimDue(uint64_t now, void *owner)
    {
        assert(owner);
        for (Record *record = m_pFirst; record; record = record->m_pNext)
        {
            if (!record->m_bDispatching && record->m_Deadline <= now)
            {
                record->m_bDispatching = true;
                record->m_pDispatchOwner = owner;
                return record;
            }
        }
        return nullptr;
    }

    Removal removeFirst(void *event, void *owner)
    {
        Record *previous = nullptr;
        for (Record *record = m_pFirst; record; record = record->m_pNext)
        {
            if (record->m_pEvent != event)
            {
                previous = record;
                continue;
            }
            if (record->m_bDispatching)
            {
                if (record->m_pDispatchOwner == owner)
                {
                    record->m_bDeferredRemoval = true;
                    return {
                        RemovalDisposition::SelfDeferred, nullptr,
                        record->m_Deadline};
                }
                return {
                    RemovalDisposition::RemoteInFlight, nullptr,
                    record->m_Deadline};
            }

            const uint64_t deadline = record->m_Deadline;
            unlink(previous, record);
            return {RemovalDisposition::Removed, record, deadline};
        }
        return {RemovalDisposition::NotFound, nullptr, 0};
    }

    Record *removeAllQueued(
        void *event, void *owner, bool &remoteInFlight, bool &selfDeferred)
    {
        Record *removed = nullptr;
        Record *removedTail = nullptr;
        Record *previous = nullptr;
        Record *record = m_pFirst;
        while (record)
        {
            Record *next = record->m_pNext;
            if (record->m_pEvent != event)
            {
                previous = record;
                record = next;
                continue;
            }
            if (record->m_bDispatching)
            {
                if (record->m_pDispatchOwner == owner)
                {
                    record->m_bDeferredRemoval = true;
                    selfDeferred = true;
                }
                else
                {
                    remoteInFlight = true;
                }
                previous = record;
                record = next;
                continue;
            }

            unlink(previous, record);
            record->m_pNext = nullptr;
            if (removedTail)
            {
                removedTail->m_pNext = record;
            }
            else
            {
                removed = record;
            }
            removedTail = record;
            record = next;
        }
        return removed;
    }

    bool hasRemoteInFlight(void *event, void *owner) const
    {
        for (Record *record = m_pFirst; record; record = record->m_pNext)
        {
            if (record->m_pEvent == event && record->m_bDispatching &&
                record->m_pDispatchOwner != owner)
            {
                return true;
            }
        }
        return false;
    }

    void completeDispatch(Record *record)
    {
        assert(record);
        assert(record->m_bDispatching);
        Record *previous = nullptr;
        Record *candidate = m_pFirst;
        while (candidate && candidate != record)
        {
            previous = candidate;
            candidate = candidate->m_pNext;
        }
        assert(candidate == record);
        unlink(previous, record);
        recycle(record);
    }

    void recycleList(Record *records)
    {
        while (records)
        {
            Record *next = records->m_pNext;
            records->m_pNext = nullptr;
            recycle(records);
            records = next;
        }
    }

    Record *detachActive()
    {
        Record *records = m_pFirst;
        m_pFirst = nullptr;
        m_pLast = nullptr;
        return records;
    }

    Record *detachFree()
    {
        Record *records = m_pFree;
        m_pFree = nullptr;
        return records;
    }

    size_t activeCount() const
    {
        size_t count = 0;
        for (Record *record = m_pFirst; record; record = record->m_pNext)
        {
            ++count;
        }
        return count;
    }

    size_t freeCount() const
    {
        size_t count = 0;
        for (Record *record = m_pFree; record; record = record->m_pNext)
        {
            ++count;
        }
        return count;
    }

  private:
    void unlink(Record *previous, Record *record)
    {
        if (previous)
        {
            previous->m_pNext = record->m_pNext;
        }
        else
        {
            m_pFirst = record->m_pNext;
        }
        if (m_pLast == record)
        {
            m_pLast = previous;
        }
        record->m_pNext = nullptr;
    }

    void recycle(Record *record)
    {
        record->clear();
        record->m_pNext = m_pFree;
        m_pFree = record;
    }

    Record *m_pFirst;
    Record *m_pLast;
    Record *m_pFree;
};

#endif
