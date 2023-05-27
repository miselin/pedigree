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

#if APIC

#include "Apic.h"

class Device;
class IrqHandler;

irq_id_t Apic::registerIsaIrqHandler(uint8_t, IrqHandler *handler, bool bEdge)
{
    // TODO
    return 0;
}
irq_id_t Apic::registerPciIrqHandler(IrqHandler *handler, Device *pDevice)
{
    // TODO
    return 0;
}
void Apic::acknowledgeIrq(irq_id_t Id)
{
    // TODO
}
void Apic::unregisterHandler(irq_id_t Id, IrqHandler *handler)
{
    // TODO
}
void Apic::enable(irq_id_t Id, bool bEnable)
{
    // TODO
}
bool Apic::initialise()
{
    // TODO
    return false;
    /*
      if (m_LocalApic.initialise(localApic) == false)
      {
        ERROR("LocalApic::initialise(): failed");
        return false;
      }

      return true;*/
}

#endif
