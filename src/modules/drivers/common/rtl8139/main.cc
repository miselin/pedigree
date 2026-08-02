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

#include "Rtl8139.h"
#include "modules/Module.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/Network.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/new"
#include "pedigree/kernel/utilities/utility.h"

static bool bFound = false;
static List<Rtl8139 *> g_Cards;

static void probeDevice(Device *pDev)
{
    NOTICE("RTL8139 found");

    // Create a new RTL8139 node
    Rtl8139 *pRtl8139 = new Rtl8139(reinterpret_cast<Network *>(pDev));
    if (!pRtl8139->isInitialised())
    {
        delete pRtl8139;
        return;
    }

    // Replace pDev with pRtl8139.
    pRtl8139->setParent(pDev->getParent());
    pDev->getParent()->replaceChild(pDev, pRtl8139);
    g_Cards.pushBack(pRtl8139);
    bFound = true;
}

static bool entry()
{
    Device::searchByVendorIdAndDeviceId(
        RTL8139_VENDOR_ID, RTL8139_DEVICE_ID, probeDevice);

    return bFound;
}

static void exit()
{
    auto removeCard = [](Device *device, Device *target) {
        return device == target ? nullptr : device;
    };
    auto callback = pedigree_std::make_callable(removeCard);
    while (g_Cards.count())
    {
        Device *card = g_Cards.popFront();
        Device::foreach (callback, 0, card);
    }
    bFound = false;
}

MODULE_INFO("rtl8139", &entry, &exit, "network-stack");
