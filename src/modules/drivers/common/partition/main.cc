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

#include "PartitionService.h"
#include "apple.h"
#include "modules/Module.h"
#include "msdos.h"
#include "pedigree/kernel/Service.h"
#include "pedigree/kernel/ServiceFeatures.h"
#include "pedigree/kernel/ServiceManager.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/utility.h"

static Service *pService = 0;
static ServiceFeatures *pFeatures = 0;

static bool probeDevice(Disk *pDev)
{
    // Does the disk have an MS-DOS partition table?
    if (msdosProbeDisk(pDev))
        return true;

    // No? how about an Apple_Map?
    if (appleProbeDisk(pDev))
        return true;

    // Oh well, better luck next time.
    return false;
}

static Device *checkNode(Device *pDev)
{
    bool hasPartitions = false;
    if (pDev->getType() == Device::Disk)
    {
        // Check that none of its children are Partitions
        // (in which case we've probed this before!)
        for (unsigned int i = 0; i < pDev->getNumChildren(); i++)
        {
            String name;
            pDev->getChild(i)->getName(name);
            if (!StringCompare(name, "msdos-partition") ||
                !StringCompare(name, "apple-partition"))
            {
                hasPartitions = true;
                break;
            }
        }

        if (!hasPartitions)
            probeDevice(static_cast<Disk *>(pDev));
    }

    return pDev;
}

bool PartitionService::serve(
    ServiceFeatures::Type type, void *pData, size_t dataLen)
{
    // Correct type?
    if (pFeatures->provides(type))
    {
        // We only provide Touch services
        if (type & ServiceFeatures::touch)
        {
            Disk *pDisk = static_cast<Disk *>(pData);
            return probeDevice(pDisk);
        }
    }

    // Not provided by us, fail!
    return false;
}

static bool entry()
{
    // Install the Partition Service
    pService = new PartitionService;
    pFeatures = new ServiceFeatures;
    pFeatures->add(ServiceFeatures::touch);
    ServiceManager::instance().addService(
        String("partition"), pService, pFeatures);

    // Walk the device tree looking for disks that don't have "partition"
    // children.
    Device::foreach (checkNode);

    // Never fail, even if no partitions found. The partition service is still
    // critical to the system.
    return true;
}

static void exit()
{
    ServiceManager::instance().removeService(String("partition"));
    delete pService;
    delete pFeatures;
}

#if ARM_COMMON  // No ATA controller
MODULE_INFO("partition", &entry, &exit);
#elif HOSTED
MODULE_INFO("partition", &entry, &exit, "diskimage");
#else
MODULE_INFO("partition", &entry, &exit, "ata");
#endif
