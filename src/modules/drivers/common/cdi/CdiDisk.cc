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

#include "CdiDisk.h"
#include "pedigree/kernel/TargetInfo.h"
#include <stddef.h>
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/Service.h"
#include "pedigree/kernel/ServiceFeatures.h"
#include "pedigree/kernel/ServiceManager.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/new"

// Prototypes in the extern "C" block to ensure that they are not mangled
extern "C" {
    void cdi_cpp_disk_register(struct cdi_storage_device* device);

    int cdi_storage_read(struct cdi_storage_device* device, uint64_t pos, size_t size, void* dest);
    int cdi_storage_write(struct cdi_storage_device* device, uint64_t pos, size_t size, void* src);
};

CdiDisk::CdiDisk(Disk* pDev, struct cdi_storage_device* device) :
    Disk(pDev), m_Device(device), m_Cache(), m_CacheMutex(),
    m_nAlignPoints(0)
{
    setSpecificType(String("CDI Disk"));
}

CdiDisk::CdiDisk(struct cdi_storage_device *device) :
    Disk(), m_Device(device), m_Cache(), m_CacheMutex(),
    m_nAlignPoints(0)
{
    setSpecificType(String("CDI Disk"));
}

CdiDisk::~CdiDisk()
{
    m_Cache.shutdown();
}

bool CdiDisk::initialise()
{
    // Chat to the partition service and let it pick up that we're around now
    ServiceFeatures *pFeatures = ServiceManager::instance().enumerateOperations(String("partition"));
    Service         *pService  = ServiceManager::instance().getService(String("partition"));
    NOTICE("Asking if the partition provider supports touch");
    if(pFeatures->provides(ServiceFeatures::touch))
    {
        NOTICE("It does, attempting to inform the partitioner of our presence...");
        if(pService)
        {
            if(pService->serve(ServiceFeatures::touch, 
                               reinterpret_cast<void*>(static_cast<Disk*>(this)), 
                               sizeof(*static_cast<Disk*>(this))))
            {
                NOTICE("Successful.");
            }
            else
            {
                ERROR("Failed.");
                return false;
            }
        }
        else
        {
            ERROR("FileDisk: Couldn't tell the partition service about the new disk presence");
            return false;
        }
    }
    else
    {
        ERROR("FileDisk: Partition service doesn't appear to support touch");
        return false;
    }

    return true;
}

// These are the functions that others call - they add a request to the parent controller's queue.
BufferView CdiDisk::read(uint64_t location)
{
    LockGuard<Mutex> guard(m_CacheMutex);
    assert( (location % 512) == 0 );
    const uint64_t pageLocation = getPageLocation(location);
    const size_t pageOffset = location - pageLocation;
    uintptr_t buff = m_Cache.lookup(pageLocation);
    if (!buff)
    {
        buff = m_Cache.insert(pageLocation);
        if (!buff)
            return BufferView();

        if (cdi_storage_read(
                m_Device, pageLocation, TargetInfo::getPageSize(),
                reinterpret_cast<void*>(buff)) != 0)
        {
            if (!m_Cache.discardEditing(pageLocation))
            {
                WARNING(
                    "CdiDisk::read could not discard a failed fill at "
                    << pageLocation);
            }
            return BufferView();
        }

        m_Cache.markNoLongerEditing(pageLocation);
        buff = m_Cache.lookup(pageLocation);
        if (!buff)
        {
            return BufferView();
        }
    }
    return BufferView::fromAddress(
        buff + pageOffset, TargetInfo::getPageSize() - pageOffset);
}

void CdiDisk::write(uint64_t location)
{
    LockGuard<Mutex> cacheGuard(m_CacheMutex);
    assert( (location % 512) == 0 );
    const uint64_t pageLocation = getPageLocation(location);
    uintptr_t buff = m_Cache.lookup(pageLocation);
    assert(buff);
    CachePageGuard pageGuard(m_Cache, pageLocation);

    if (cdi_storage_write(
            m_Device, pageLocation, TargetInfo::getPageSize(),
            reinterpret_cast<void*>(buff)) != 0)
        return;
}

void CdiDisk::align(uint64_t location)
{
    LockGuard<Mutex> guard(m_CacheMutex);
    for (size_t i = 0; i < m_nAlignPoints; ++i)
    {
        if (m_AlignPoints[i] == location)
        {
            return;
        }
    }
    assert(m_nAlignPoints < 8);
    m_AlignPoints[m_nAlignPoints++] = location;
}

bool CdiDisk::pin(uint64_t location)
{
    LockGuard<Mutex> guard(m_CacheMutex);
    return m_Cache.pin(getPageLocation(location));
}

void CdiDisk::unpin(uint64_t location)
{
    LockGuard<Mutex> guard(m_CacheMutex);
    m_Cache.release(getPageLocation(location));
}

uint64_t CdiDisk::getPageLocation(uint64_t location) const
{
    uint64_t alignPoint = 0;
    for (size_t i = 0; i < m_nAlignPoints; ++i)
    {
        if (
            m_AlignPoints[i] <= location &&
            m_AlignPoints[i] > alignPoint)
        {
            alignPoint = m_AlignPoints[i];
        }
    }

    return location - ((location - alignPoint) % TargetInfo::getPageSize());
}

void cdi_cpp_disk_register(struct cdi_storage_device* device)
{
    // Create a new CdiDisk node.
    CdiDisk *pCdiDisk = new CdiDisk(0, device);
    if(!pCdiDisk->initialise())
    {
        delete pCdiDisk;
        return;
    }

    // Insert into the tree, properly
    Device::addToRoot(pCdiDisk);
}
