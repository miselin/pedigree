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

#include <compiler.h>
#include <machine/Machine.h>
#include "Pit.h"

/** Hundred hertz frequency. */
#define PIT_FREQUENCY 100

Pit Pit::m_Instance;

bool Pit::registerHandler(TimerHandler *handler)
{
  if (UNLIKELY(handler == 0 && m_Handler != 0))
    return false;

  m_Handler = handler;
  return true;
}

bool Pit::initialise()
{
  // Allocate the PIT I/O range
  if (m_IoPort.allocate(0x40, 4) == false)
    return false;

  // Allocate the IRQ
  IrqManager &irqManager = *Machine::instance().getIrqManager();
  m_IrqId = irqManager.registerIsaIrqHandler(0, this, true);
  if (m_IrqId == 0)
    return false;

  // Set the PIT frequency
  // The value we send to the PIT is the value to divide it's input clock
  // (1193180 Hz) by, to get our required frequency. Important to note is
  // that the divisor must be small enough to fit into 16-bits.
  size_t divisor = 1193180 / PIT_FREQUENCY;

  // Send the command byte.
  m_IoPort.write8(0x36, 3);

  // Divisor has to be sent byte-wise, so split here into upper/lower bytes.
  uint8_t l = divisor & 0xFF;
  uint8_t h = (divisor>>8) & 0xFF;

  // Send the frequency divisor.
  m_IoPort.write8(l, 0);
  m_IoPort.write8(h, 0);

  return true;
}
void Pit::uninitialise()
{
  // TODO: Reset the PIT frequency

  // Free the IRQ
  if (m_IrqId != 0)
  {
    IrqManager &irqManager = *Machine::instance().getIrqManager();
    irqManager.unregisterHandler(m_IrqId, this);
  }

  // Free the PIT I/O range
  m_IoPort.free();
}

Pit::Pit()
   : m_IoPort("PIT"), m_IrqId(0), m_Handler(0)
{
}

bool Pit::irq(irq_id_t number, InterruptState &state)
{
  // TODO: Delta is wrong
  if (LIKELY(m_Handler != 0))
    m_Handler->timer(0, state);

  // Processor::information().getScheduler().checkEventState(state.getStackPointer());

  return true;
}
