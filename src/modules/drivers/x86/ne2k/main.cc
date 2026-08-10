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
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/new"
#include "pedigree/kernel/utilities/utility.h"

#include "Ne2k.h"
#include "modules/Module.h"

class Network;

static bool bFound = false;
static List<Ne2k*> g_Cards;

static void probeDevice(Device* pDev) {
  NOTICE("NE2K found");

  // Create a new NE2K node
  Ne2k* pNe2k = new Ne2k(reinterpret_cast<Network*>(pDev));

  // Replace pDev with pNe2k.
  pNe2k->setParent(pDev->getParent());
  pDev->getParent()->replaceChild(pDev, pNe2k);

  g_Cards.pushBack(pNe2k);
  bFound = true;

  // Device(pDev) transfers the live I/O mappings into this object. Keep the
  // inert, source-masked device owned by the module if IRQ admission failed;
  // deleting it here would leave the original tree node without mappings.
  if (!pNe2k->isValid()) {
    ERROR("NE2K: device initialisation failed; device left disabled");
  }
}

static bool entry() {
  Device::searchByVendorIdAndDeviceId(NE2K_VENDOR_ID, NE2K_DEVICE_ID, probeDevice);

  return bFound;
}

static void exit() {
  auto removeCard = [](Device* device, Device* target) {
    return device == target ? nullptr : device;
  };
  auto callback = pedigree_std::make_callable(removeCard);
  while (g_Cards.count()) {
    Device* card = g_Cards.popFront();
    Device::foreach (callback, 0, card);
  }
  bFound = false;
}

MODULE_NAME("ne2k");
MODULE_ENTRY(&entry);
MODULE_EXIT(&exit);
MODULE_DEPENDS("network-stack");
