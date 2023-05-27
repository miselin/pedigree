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

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Serial.h"
#include "pedigree/kernel/utilities/Cord.h"

class SerialLogger : public Log::LogCallback
{
  public:
    SerialLogger();
    virtual ~SerialLogger();

    virtual void callback(const LogCord &cord);

  private:
    Serial *m_pSerial;
    bool m_bInitialised;
    Spinlock m_Lock;
};

static SerialLogger g_SerialCallback;

void installSerialLogger()
{
    Log::instance().installCallback(&g_SerialCallback, false);
}

SerialLogger::SerialLogger()
    : m_pSerial(nullptr), m_bInitialised(false), m_Lock(false)
{
}
SerialLogger::~SerialLogger() = default;

void SerialLogger::callback(const LogCord &cord)
{
    if (!m_bInitialised)
    {
        m_bInitialised = Machine::instance().isInitialised();
        if (!m_bInitialised)
        {
            return;
        }
        else
        {
            m_pSerial = Machine::instance().getSerial(0);
        }
    }

    m_Lock.acquire();
    for (auto it : cord)
    {
        m_pSerial->write(it);
    }
#if !SERIAL_IS_FILE
    // Handle carriage return if we're writing to a real terminal
    // Technically this will create a \n\r, but it will do the same
    // thing. This may also be redundant, but better to be safe than
    // sorry imho.
    m_pSerial->write('\r');
#endif
    m_Lock.release();
}
