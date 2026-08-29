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

#include "apple.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/utility.h"

#include "Partition.h"

class Device;

bool appleProbeDisk(Disk* pDisk) {
  // Read the second sector (512 bytes) of the disk into a buffer.
  BufferView buffer = pDisk->read(512ULL);
  if (!buffer || buffer.size() < sizeof(ApplePartitionMap)) {
    if (buffer) {
      pDisk->unpin(512ULL);
    }
    WARNING("Disk read failure during Apple partition table search.");
    return false;
  }

  ApplePartitionMap map;
  MemoryCopy(&map, buffer.data(), sizeof(map));
  pDisk->unpin(512ULL);

  String diskName;
  pDisk->getName(diskName);

  // Check for the magic signature.
  if (map.pmSig != BIG_TO_HOST16(APPLE_PM_SIG)) {
    NOTICE("Apple partition map not found on disk " << diskName);
    return false;
  }

  NOTICE("Apple partition map found on disk " << diskName);

  // Cache the number of partition table entries.
  size_t nEntries = BIG_TO_HOST32(map.pmMapBlkCnt);
  for (size_t i = 0; i < nEntries; i++) {
    if (i > 0)  // We don't need to load anything in for the first pmap -
                // already done!
    {
      const uint64_t diskLocation = 512ULL + i * 512ULL;
      buffer = pDisk->read(diskLocation);
      if (!buffer || buffer.size() < sizeof(ApplePartitionMap)) {
        if (buffer) {
          pDisk->unpin(diskLocation);
        }
        WARNING("Disk read failure during partition table recognition.");
        return false;
      }
      MemoryCopy(&map, buffer.data(), sizeof(map));
      pDisk->unpin(diskLocation);
    }

    NOTICE("Detected partition '" << map.pmPartName << "', type '" << map.pmParType << "'");

    // Create a partition object.
    Partition* pObj = new Partition(
        String(map.pmParType), static_cast<uint64_t>(BIG_TO_HOST32(map.pmPyPartStart)) * 512ULL,
        static_cast<uint64_t>(BIG_TO_HOST32(map.pmPartBlkCnt)) * 512ULL);
    pObj->setParent(static_cast<Device*>(pDisk));
    pDisk->addChild(static_cast<Device*>(pObj));
  }
  return true;
}
