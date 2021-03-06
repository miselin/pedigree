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

#include "Decrementer.h"
#include "../core/processor/ppc32/InterruptManager.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/openfirmware/Device.h"
#include "pedigree/kernel/machine/openfirmware/OpenFirmware.h"
#include "pedigree/kernel/processor/InterruptManager.h"
#include "pedigree/kernel/processor/Processor.h"

Decrementer Decrementer::m_Instance;

bool Decrementer::registerHandler(TimerHandler *handler)
{
    if (UNLIKELY(handler == 0 && m_Handler != 0))
        return false;

    m_Handler = handler;
    return true;
}

bool Decrementer::initialise()
{
    // Allocate the Interrupt.
    if (InterruptManager::instance().registerInterruptHandler(8, this) == false)
        return false;

    // Find the frequency of the decrementer.
    OFDevice chosen(OpenFirmware::instance().findDevice("/chosen"));
    OFDevice cpu(chosen.getProperty("cpu"));
    m_Frequency =
        reinterpret_cast<uint32_t>(cpu.getProperty("timebase-frequency"));
    if (static_cast<int32_t>(m_Frequency) <= 0)
    {
        WARNING("Cpu::timebase-frequency property not available!");
        m_Frequency = 0x100000;
    }

    // Set up the decrementer to fire in DECREMENTER_PERIOD milliseconds.
    uint32_t n = (DECREMENTER_PERIOD * m_Frequency) / 1000;
    asm volatile("mtdec %0" : : "r"(n));

    return true;
}
void Decrementer::uninitialise()
{
}

Decrementer::Decrementer() : m_Handler(0), m_Frequency(0)
{
}

uint32_t numFired = 0;

void Decrementer::interrupt(size_t interruptNumber, InterruptState &state)
{
    // Set up the decrementer to fire in DECREMENTER_PERIOD milliseconds.
    uint32_t n = (DECREMENTER_PERIOD * m_Frequency) / 1000;
    asm volatile("mtdec %0" : : "r"(n));

    //   numFired++;
    //   if (numFired == 300)
    //   {
    //     Processor::breakpoint();
    //   }

    // TODO: Delta is wrong
    if (LIKELY(m_Handler != 0))
        m_Handler->timer(DECREMENTER_PERIOD, state);
}
