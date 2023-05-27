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

#ifndef USBMASSSTORAGEDEVICE_H
#define USBMASSSTORAGEDEVICE_H

#include "modules/drivers/common/scsi/ScsiController.h"
#include "modules/system/usb/UsbConstants.h"
#include "modules/system/usb/UsbDevice.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/utility.h"

class Device;

class UsbMassStorageDevice : public ScsiController, public UsbDevice
{
  public:
    UsbMassStorageDevice(UsbDevice *dev);
    virtual ~UsbMassStorageDevice();

    virtual void initialiseDriver();

    virtual bool sendCommand(
        size_t nUnit, uintptr_t pCommand, uint8_t nCommandSize,
        uintptr_t pRespBuffer, uint16_t nRespBytes, bool bWrite);

    virtual void getUsbDeviceName(String &str)
    {
        str.assign("USB Mass Storage Device", 24);
    }

    virtual bool hasSubtree() const
    {
        return true;
    }

    virtual Device *getDevice()
    {
        return this;
    }

  private:
    bool massStorageReset();

    enum MassStorageRequests
    {
        MassStorageRequest =
            UsbRequestType::Class | UsbRequestRecipient::Interface,

        MassStorageReset = 0xFF,
        MassStorageGetMaxLUN = 0xFE
    };

    enum MassStorageSigs
    {
        CbwSig = HOST_TO_LITTLE32(0x43425355),  // USBC
        CswSig = HOST_TO_LITTLE32(0x53425355)   // USBS
    };

    struct Cbw
    {
        uint32_t nSig;
        uint32_t nTag;
        uint32_t nDataBytes;
        uint8_t nFlags;
        uint8_t nLUN;
        uint8_t nCommandSize;
        uint8_t pCommand[16];
    } PACKED;

    struct Csw
    {
        uint32_t nSig;
        uint32_t nTag;
        uint32_t nResidue;
        uint8_t nStatus;
    } PACKED;

    size_t m_nUnits;
    Endpoint *m_pInEndpoint;
    Endpoint *m_pOutEndpoint;

  protected:
    virtual size_t getNumUnits()
    {
        return m_nUnits;
    }
};

#endif
