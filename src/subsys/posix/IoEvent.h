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

#include "pedigree/kernel/process/Event.h"
#include "pedigree/kernel/process/eventNumbers.h"

class PosixSubsystem;

/** Event class for passing to File::monitor. */
class IoEvent : public Event
{
  public:
    /**
     * Pass in a subsystem which is used to forward on the event.
     * This allows the correct event handler in userspace to change without
     * having to re-create the IoEvent.
     */
    IoEvent();
    IoEvent(PosixSubsystem *subsystem, File *file);
    virtual ~IoEvent();

    void fire();

    PosixSubsystem *getSubsystem()
    {
        return m_pSubsystem;
    }

    //
    // Event interface
    //
    virtual size_t serialize(uint8_t *pBuffer);
    static bool unserialize(uint8_t *pBuffer, IoEvent &event);
    virtual size_t getNumber()
    {
        return EventNumbers::IoEvent;
    }

  private:
    PosixSubsystem *m_pSubsystem;
    File *m_pFile;
    IoEvent *m_pRetriggerInstance;
};
