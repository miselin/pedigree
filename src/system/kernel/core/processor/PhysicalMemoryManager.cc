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

#include <processor/PhysicalMemoryManager.h>
#include <processor/MemoryRegion.h>

void PhysicalMemoryManager::allocateMemoryRegionList(Vector<MemoryRegionInfo*> &MemoryRegions)
{
  for (size_t i = 0;i < m_MemoryRegions.count();i++)
  {
    MemoryRegionInfo *pMemoryRegionInfo = new MemoryRegionInfo(m_MemoryRegions[i]->virtualAddress(),
                                                               m_MemoryRegions[i]->physicalAddress(),
                                                               m_MemoryRegions[i]->size(),
                                                               m_MemoryRegions[i]->name());
    MemoryRegions.pushBack(pMemoryRegionInfo);
  }
}
void PhysicalMemoryManager::freeMemoryRegionList(Vector<MemoryRegionInfo*> &MemoryRegions)
{
  while (MemoryRegions.count() != 0)
  {
    MemoryRegionInfo *pMemoryRegionInfo = MemoryRegions.popBack();
    delete pMemoryRegionInfo;
  }
}
