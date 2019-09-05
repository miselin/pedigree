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

#include "modules/Module.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Timer.h"
#include "pedigree/kernel/machine/TimerHandler.h"
#include "pedigree/kernel/processor/IoBase.h"
#include "pedigree/kernel/processor/state_forward.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/utility.h"

enum Ib700TimeEntries
{
    Seconds30 = 0,
    Seconds28 = 1,
    Seconds26 = 2,
    Seconds24 = 3,
    Seconds22 = 4,
    Seconds20 = 5,
    Seconds18 = 6,
    Seconds16 = 7,
    Seconds14 = 8,
    Seconds12 = 9,
    Seconds10 = 10,
    Seconds8 = 11,
    Seconds6 = 12,
    Seconds4 = 13,
    Seconds2 = 14,
    Seconds0 = 15,
};

class Ib700Watchdog : public Device, public TimerHandler
{
  public:
    Ib700Watchdog(Device *pDev);

    virtual ~Ib700Watchdog();

    virtual bool initialise();

    virtual void getName(String &str);

    virtual void timer(uint64_t delta, InterruptState &state);

  private:
    IoBase *m_pBase;
};

static bool entry()
{
    auto f = [](Device *p) {
        if (p->addresses().count() > 0)
        {
            if (p->addresses()[0]->m_Name.compare("ib700-base"))
            {
                Ib700Watchdog *pNewChild = new Ib700Watchdog(p);
                if (pNewChild->initialise())
                {
                    return static_cast<Device *>(pNewChild);
                }
                else
                {
                    ERROR("IB700 initialisation failed!");
                    delete pNewChild;
                }
            }
        }

        return p;
    };

    auto c = pedigree_std::make_callable(f);
    Device::foreach (c, 0);

    return true;
}

static void exit()
{
}

MODULE_INFO("ib700_wdt", &entry, &exit);

Ib700Watchdog::Ib700Watchdog(Device *pDev) : Device(pDev)
{
    setSpecificType(String("watchdog-timer"));
}

Ib700Watchdog::~Ib700Watchdog()
{
    if (m_pBase)
    {
        // Disable any existing timer.
        m_pBase->write16(0, 2);
    }
}

bool Ib700Watchdog::initialise()
{
    m_pBase = addresses()[0]->m_Io;
    if (!m_pBase)
        return false;

    // Disable any existing timer.
    m_pBase->write16(0, 0);

    // Register ourselves with the core timer so we can continually
    // reset the watchdog timer as needed.
    Timer *t = Machine::instance().getTimer();
    if (t)
    {
        t->registerHandler(this);

        // Enable our timer with a 10 second timeout.
        m_pBase->write16(Seconds10, 2);

        return true;
    }

    return false;
}

void Ib700Watchdog::getName(String &str)
{
    str.assign("ib700_wdt", 10);
}

void Ib700Watchdog::timer(uint64_t delta, InterruptState &state)
{
    // Timer fired, push the watchdog back now (watchdog expects to be
    // polled by the system regularly).
    m_pBase->write16(Seconds10, 2);
}
