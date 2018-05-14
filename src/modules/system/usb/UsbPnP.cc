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

#include "modules/system/usb/UsbPnP.h"
#include "modules/system/usb/UsbDevice.h"
#include "pedigree/kernel/processor/Processor.h"

UsbPnP UsbPnP::m_Instance;

UsbPnP::UsbPnP() : m_Callbacks()
{
}

UsbPnP::~UsbPnP()
{
}

bool UsbPnP::probeDevice(Device *pDeviceBase)
{
    Device *pResult = doProbe(pDeviceBase);
    return pResult == pDeviceBase;
}

Device *UsbPnP::doProbe(Device *pDeviceBase)
{
    // Sanity check.
    if (!(pDeviceBase->getType() == Device::UsbContainer))
    {
        return pDeviceBase;
    }

    UsbDevice *pDevice =
        static_cast<UsbDeviceContainer *>(pDeviceBase)->getUsbDevice();

    // Is this device already handled by a driver?
    if (pDevice->getUsbState() == UsbDevice::HasDriver)
    {
        return pDeviceBase;
    }
    else if (!m_Callbacks.count())
    {
        return pDeviceBase;
    }

    UsbDevice::DeviceDescriptor *pDes = pDevice->getDescriptor();
    UsbDevice::Interface *pIface = pDevice->getInterface();

    for (List<CallbackItem *>::Iterator it = m_Callbacks.begin();
         it != m_Callbacks.end(); it++)
    {
        CallbackItem *item = *it;
        if (!item)
            continue;

        if ((item->nVendorId != VendorIdNone) &&
            (item->nVendorId != pDes->nVendorId))
            continue;
        if ((item->nProductId != ProductIdNone) &&
            (item->nProductId != pDes->nProductId))
            continue;
        if ((item->nClass != ClassNone) && (item->nClass != pIface->nClass))
            continue;
        if ((item->nSubclass != SubclassNone) &&
            (item->nSubclass != pIface->nSubclass))
            continue;
        if ((item->nProtocol != ProtocolNone) &&
            (item->nProtocol != pIface->nProtocol))
            continue;

        // Call the callback, which will give us (hopefully) a copy of pDevice,
        // in the form of a driver class
        UsbDevice *pNewDevice = item->callback(pDevice);

        // Was this device rejected by the driver?
        if (!pNewDevice)
            continue;

        // Initialise the driver
        pNewDevice->initialiseDriver();

        // Did the device go into the driver state?
        if (pNewDevice->getUsbState() == UsbDevice::HasDriver)
        {
            // Replace the old device with the new one
            UsbDeviceContainer *pNewContainer =
                new UsbDeviceContainer(pNewDevice);
            return pNewContainer;
        }
        else
        {
            delete pNewDevice;
        }
    }
    return pDeviceBase;
}

void UsbPnP::reprobeDevices(Device *pParent)
{
    auto performReprobe = [](Device *p) {
        if (p->getType() == Device::UsbContainer)
        {
            return UsbPnP::instance().doProbe(p);
        }

        // don't edit the tree - just iterating
        return p;
    };

    auto c = pedigree_std::make_callable(performReprobe);
    Device::foreach (c, pParent);
}

void UsbPnP::registerCallback(
    uint16_t nVendorId, uint16_t nProductId, callback_t callback)
{
    CallbackItem *item = new CallbackItem;
    item->callback = callback;
    item->nVendorId = nVendorId;
    item->nProductId = nProductId;
    item->nClass = ClassNone;
    item->nSubclass = SubclassNone;
    item->nProtocol = ProtocolNone;

    m_Callbacks.pushBack(item);

    reprobeDevices(nullptr);
}

void UsbPnP::registerCallback(
    uint8_t nClass, uint8_t nSubclass, uint8_t nProtocol, callback_t callback)
{
    CallbackItem *item = new CallbackItem;
    item->callback = callback;
    item->nVendorId = VendorIdNone;
    item->nProductId = ProductIdNone;
    item->nClass = nClass;
    item->nSubclass = nSubclass;
    item->nProtocol = nProtocol;

    m_Callbacks.pushBack(item);

    reprobeDevices(nullptr);
}
