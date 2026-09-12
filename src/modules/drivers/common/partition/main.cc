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

#include "pedigree/kernel/Service.h"
#include "pedigree/kernel/ServiceFeatures.h"
#include "pedigree/kernel/ServiceManager.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/OperationBarrier.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/utility.h"

#include "Gpt.h"
#include "PartitionService.h"
#include "apple.h"
#include "modules/Module.h"
#include "msdos.h"

static Service* pService = 0;
static ServiceFeatures* pFeatures = 0;
static OperationBarrier g_ProbeOperations;

static bool probeDevice(Disk* pDev) {
  if (pDev->getSpecificType() == String("partition"))
    return true;
  for (size_t i = 0; i < pDev->getNumChildren(); ++i)
    if (pDev->getChild(i)->getSpecificType() == String("partition"))
      return true;
  if (gptProbeDisk(pDev))
    return true;

  // Does the disk have an MS-DOS partition table?
  if (msdosProbeDisk(pDev))
    return true;

  // No? how about an Apple_Map?
  if (pDev->getNativeBlockSize() == 512 && appleProbeDisk(pDev))
    return true;

  // Oh well, better luck next time.
  return false;
}

static Device* checkNode(Device* pDev) {
  bool hasPartitions = false;
  if (pDev->getType() == Device::Disk && pDev->getSpecificType() != String("partition")) {
    // Check that none of its children are Partitions
    // (in which case we've probed this before!)
    for (unsigned int i = 0; i < pDev->getNumChildren(); i++) {
      String name;
      pDev->getChild(i)->getName(name);
      if (pDev->getChild(i)->getSpecificType() == String("partition")) {
        hasPartitions = true;
        break;
      }
    }

    if (!hasPartitions) {
      probeDevice(static_cast<Disk*>(pDev));
    }
  }

  return pDev;
}

bool PartitionService::serve(ServiceFeatures::Type type, void* pData, size_t dataLen) {
  OperationBarrier::Lease operation;
  if (!g_ProbeOperations.tryAcquire(operation))
    return false;

  // Correct type?
  if (pFeatures->provides(type)) {
    // We only provide Touch services
    if (type & ServiceFeatures::touch) {
      Disk* pDisk = static_cast<Disk*>(pData);
      return probeDevice(pDisk);
    }
  }

  // Not provided by us, fail!
  return false;
}

static bool entry() {
  // Install the Partition Service
  pService = new PartitionService;
  pFeatures = new ServiceFeatures;
  pFeatures->add(ServiceFeatures::touch);
  ServiceManager::instance().addService(String("partition"), pService, pFeatures);

  // Walk the device tree looking for disks that don't have "partition"
  // children.
  Device::foreach (checkNode);

  NOTICE("partition entry() is done");

  // Never fail, even if no partitions found. The partition service is still
  // critical to the system.
  return true;
}

static Device* removePartition(Device* device) {
  if (device->getSpecificType() != String("partition"))
    return device;

#if !HOSTED
  // Mounted filesystems and paging retain the physical endpoint. Closing it
  // before deletion also excludes new users until the backing driver exits.
  Disk* physical = static_cast<Disk*>(device)->physicalDisk();
  if (!physical || !physical->tryCloseEndpoint())
    panic("Partition shutdown blocked by a live disk owner");
#endif

  // Device::foreach removes the child and deletes it under the tree lock.
  return nullptr;
}

static void exit() {
  ServiceManager::instance().removeService(String("partition"));
  g_ProbeOperations.closeAndWait();
  Device::foreach (removePartition);
  delete pService;
  delete pFeatures;
}

#if HOSTED
MODULE_INFO_RUNTIME_PINNED("partition", &entry, &exit, "diskimage");
#else
MODULE_INFO_RUNTIME_PINNED("partition", &entry, &exit);
MODULE_OPTIONAL_DEPENDS("ata", "ahci", "nvme", "usb-mass-storage");
#endif
