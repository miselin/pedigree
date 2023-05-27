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

#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/processor/IoBase.h"
#include "pedigree/kernel/processor/IoPort.h"
#include "pedigree/kernel/processor/MemoryMappedIo.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"

#if THREADS
#include "pedigree/kernel/LockGuard.h"
#define RAII_LOCK() LockGuard<Mutex> guard(m_TreeLock)

Mutex Device::m_TreeLock;
#else
#define RAII_LOCK()
#endif

/** Singleton Device instantiation. */
Device Device::m_Root;

Device::Device()
    : m_Addresses(), m_Children(), m_pParent(0),
#ifdef OPENFIRMWARE
      m_OfHandle(0),
#endif
      m_InterruptNumber(0), m_SpecificType(), m_ConfigHeader(), m_ClassCode(0),
      m_SubclassCode(0), m_VendorId(0), m_DeviceId(0), m_ProgInterface(0),
      m_PciBusPos(0), m_PciDevicePos(0), m_PciFunctionNum(0)
{
}

Device::Device(Device *p)
    : m_Addresses(), m_Children(), m_pParent(0),
      m_InterruptNumber(p->m_InterruptNumber),
#ifdef OPENFIRMWARE
      m_OfHandle(0),
#endif
      m_SpecificType(p->m_SpecificType), m_ConfigHeader(p->m_ConfigHeader),
      m_ClassCode(p->m_ClassCode), m_SubclassCode(p->m_SubclassCode),
      m_VendorId(p->m_VendorId), m_DeviceId(p->m_DeviceId),
      m_ProgInterface(p->m_ProgInterface), m_PciBusPos(p->m_PciBusPos),
      m_PciDevicePos(p->m_PciDevicePos), m_PciFunctionNum(p->m_PciFunctionNum)
{
    m_pParent = p->m_pParent;
    for (unsigned int i = 0; i < p->m_Children.count(); i++)
    {
        m_Children.pushBack(p->m_Children[i]);
    }

    p->removeIoMappings();

    for (unsigned int i = 0; i < p->m_Addresses.count(); i++)
    {
        Address *pa = p->m_Addresses[i];
        Address *a = new Address(
            pa->m_Name, pa->m_Address, pa->m_Size, pa->m_IsIoSpace,
            pa->m_Padding);
        NOTICE("address=" << Hex << a << ", m_Io=" << a->m_Io);
        m_Addresses.pushBack(a);
    }
}

Device::~Device()
{
    for (unsigned int i = 0; i < m_Addresses.count(); i++)
    {
        delete m_Addresses[i];
    }
    for (unsigned int i = 0; i < m_Children.count(); i++)
    {
        delete m_Children[i];
    }
}

void Device::foreach (Device::Callback callback, Device * root)
{
    // Forward to the Callable<> version.
    pedigree_std::Callable<decltype(callback)> cb(callback);
    foreach (cb, root)
        ;
}

void Device::addToRoot(Device *device)
{
    RAII_LOCK();

    device->setParent(&Device::root());
    Device::root().addChild(device);
}

void Device::removeIoMappings()
{
    for (unsigned int i = 0; i < m_Addresses.count(); i++)
    {
        Address *pa = m_Addresses[i];
        if (pa->m_Io)
            delete pa->m_Io;
        pa->m_Io = 0;
    }
}

void Device::getName(String &str)
{
    str.assign("Root", 5);
}

/** Adds a device as a child of this device. */
void Device::addChild(Device *pDevice)
{
    m_Children.pushBack(pDevice);
}
/** Retrieves the n'th child of this device. */
Device *Device::getChild(size_t n)
{
    return m_Children[n];
}
/** Retrieves the number of children of this device. */
size_t Device::getNumChildren()
{
    return m_Children.count();
}
/** Removes the n'th child of this device. */
void Device::removeChild(size_t n)
{
    unsigned int i = 0;
    for (Vector<Device *>::Iterator it = m_Children.begin();
         it != m_Children.end(); it++, i++)
    {
        if (i == n)
        {
            m_Children.erase(it);
            break;
        }
    }
}
/** Removes the given Device from this device's child list. */
void Device::removeChild(Device *d)
{
    int i = 0;
    for (Vector<Device *>::Iterator it = m_Children.begin();
         it != m_Children.end(); it++, i++)
        if (*it == d)
        {
            m_Children.erase(it);
            break;
        }
}

void Device::replaceChild(Device *src, Device *dest)
{
    int i = 0;
    for (Vector<Device *>::Iterator it = m_Children.begin();
         it != m_Children.end(); it++, i++)
        if (*it == src)
        {
            *it = dest;
            break;
        }
}

/** Search functions */

void Device::searchByVendorId(
    uint16_t vendorId, void (*callback)(Device *), Device *root)
{
    RAII_LOCK();

    if (!root)
    {
        root = &Device::root();
    }

    searchByVendorIdInternal(vendorId, callback, root);
}

void Device::searchByVendorIdAndDeviceId(
    uint16_t vendorId, uint16_t deviceId, void (*callback)(Device *),
    Device *root)
{
    RAII_LOCK();

    if (!root)
    {
        root = &Device::root();
    }

    searchByVendorIdAndDeviceIdInternal(vendorId, deviceId, callback, root);
}

void Device::searchByClass(
    uint16_t classCode, void (*callback)(Device *), Device *root)
{
    RAII_LOCK();

    if (!root)
    {
        root = &Device::root();
    }

    searchByClassInternal(classCode, callback, root);
}

void Device::searchByClassAndSubclass(
    uint16_t classCode, uint16_t subclassCode, void (*callback)(Device *),
    Device *root)
{
    RAII_LOCK();

    if (!root)
    {
        root = &Device::root();
    }

    searchByClassAndSubclassInternal(classCode, subclassCode, callback, root);
}

void Device::searchByClassSubclassAndProgInterface(
    uint16_t classCode, uint16_t subclassCode, uint8_t progInterface,
    void (*callback)(Device *), Device *root)
{
    RAII_LOCK();

    if (!root)
    {
        root = &Device::root();
    }

    searchByClassSubclassAndProgInterfaceInternal(
        classCode, subclassCode, progInterface, callback, root);
}

void Device::searchByVendorIdInternal(
    uint16_t vendorId, void (*callback)(Device *), Device *root)
{
    for (unsigned int i = 0; i < root->getNumChildren(); i++)
    {
        Device *pChild = root->getChild(i);
        if (pChild->getPciVendorId() == vendorId)
        {
            callback(pChild);

            // if the callback replaced this object, we need to re-attain it
            // before we can recurse
            pChild = root->getChild(i);
        }

        searchByVendorIdInternal(vendorId, callback, pChild);
    }
}

void Device::searchByVendorIdAndDeviceIdInternal(
    uint16_t vendorId, uint16_t deviceId, void (*callback)(Device *),
    Device *root)
{
    for (unsigned int i = 0; i < root->getNumChildren(); i++)
    {
        Device *pChild = root->getChild(i);
        if ((pChild->getPciVendorId() == vendorId) &&
            (pChild->getPciDeviceId() == deviceId))
        {
            callback(pChild);

            // if the callback replaced this object, we need to re-attain it
            // before we can recurse
            pChild = root->getChild(i);
        }

        searchByVendorIdAndDeviceIdInternal(
            vendorId, deviceId, callback, pChild);
    }
}

void Device::searchByClassInternal(
    uint16_t classCode, void (*callback)(Device *), Device *root)
{
    for (unsigned int i = 0; i < root->getNumChildren(); i++)
    {
        Device *pChild = root->getChild(i);
        if (pChild->getPciClassCode() == classCode)
        {
            callback(pChild);

            // if the callback replaced this object, we need to re-attain it
            // before we can recurse
            pChild = root->getChild(i);
        }

        searchByClassInternal(classCode, callback, pChild);
    }
}

void Device::searchByClassAndSubclassInternal(
    uint16_t classCode, uint16_t subclassCode, void (*callback)(Device *),
    Device *root)
{
    for (unsigned int i = 0; i < root->getNumChildren(); i++)
    {
        Device *pChild = root->getChild(i);
        if ((pChild->getPciClassCode() == classCode) &&
            (pChild->getPciSubclassCode() == subclassCode))
        {
            callback(pChild);

            // if the callback replaced this object, we need to re-attain it
            // before we can recurse
            pChild = root->getChild(i);
        }

        searchByClassAndSubclassInternal(
            classCode, subclassCode, callback, pChild);
    }
}

void Device::searchByClassSubclassAndProgInterfaceInternal(
    uint16_t classCode, uint16_t subclassCode, uint8_t progInterface,
    void (*callback)(Device *), Device *root)
{
    for (unsigned int i = 0; i < root->getNumChildren(); i++)
    {
        Device *pChild = root->getChild(i);
        if ((pChild->getPciClassCode() == classCode) &&
            (pChild->getPciSubclassCode() == subclassCode) &&
            (pChild->getPciProgInterface() == progInterface))
        {
            callback(pChild);

            // if the callback replaced this object, we need to re-attain it
            // before we can recurse
            pChild = root->getChild(i);
        }

        searchByClassSubclassAndProgInterfaceInternal(
            classCode, subclassCode, progInterface, callback, pChild);
    }
}

Device::Address::Address(
    const String &n, uintptr_t a, size_t s, bool io, size_t pad)
    : m_Name(n), m_Address(a), m_Size(s), m_IsIoSpace(io), m_Io(0),
      m_Padding(pad), m_bMapped(false)
{
#ifndef DEVICE_IGNORE_ADDRESSES
    EMIT_IF(!KERNEL_PROCESSOR_NO_PORT_IO)
    {
        if (m_IsIoSpace)
        {
            IoPort *pIo = new IoPort(m_Name.cstr());
            pIo->allocate(a, s);
            m_Io = pIo;
            return;
        }
    }

    // In this case, IO accesses go through MemoryMappedIo too.
    size_t pageSize = PhysicalMemoryManager::getPageSize();
    uint32_t numPages = s / pageSize;
    if (s % pageSize)
        numPages++;

    MemoryMappedIo *pIo = new MemoryMappedIo(m_Name.cstr(), a % pageSize, pad);
    m_Io = pIo;
#endif  // DEVICE_IGNORE_ADDRESSES
}

void Device::Address::map(
    size_t forcedSize, bool bUser, bool bWriteCombine, bool bWriteThrough)
{
#ifndef DEVICE_IGNORE_ADDRESSES
    if (!m_Io)
        return;
    EMIT_IF(!KERNEL_PROCESSOR_NO_PORT_IO)
    {
        if (m_IsIoSpace)
            return;
    }
    if (m_bMapped)
        return;

    size_t pageSize = PhysicalMemoryManager::getPageSize();
    size_t s = forcedSize ? forcedSize : m_Size;
    uint32_t numPages = s / pageSize;
    if (s % pageSize)
        numPages++;

    size_t cacheFlags = 0;
    if (bWriteCombine)
        cacheFlags |= VirtualAddressSpace::WriteCombine;
    else if (bWriteThrough)
        cacheFlags |= VirtualAddressSpace::WriteThrough;
    else
        cacheFlags |= VirtualAddressSpace::CacheDisable;
    PhysicalMemoryManager &physicalMemoryManager =
        PhysicalMemoryManager::instance();
    if (!physicalMemoryManager.allocateRegion(
            *static_cast<MemoryMappedIo *>(m_Io), numPages,
            PhysicalMemoryManager::continuous |
                PhysicalMemoryManager::nonRamMemory |
                PhysicalMemoryManager::force,
            (bUser ? 0 : VirtualAddressSpace::KernelMode) |
                VirtualAddressSpace::Write | cacheFlags,
            m_Address))
    {
        ERROR("Device::Address - map for " << Hex << m_Address << " failed!");
    }

    NOTICE(
        "Device::Address: mapped "
        << Hex << m_Address << " -> "
        << static_cast<MemoryMappedIo *>(m_Io)->virtualAddress());

    m_bMapped = true;
#endif  // DEVICE_IGNORE_ADDRESSES
}

Device::Address::~Address()
{
#ifndef DEVICE_IGNORE_ADDRESSES
    if (m_Io)
        delete m_Io;
#endif
}
