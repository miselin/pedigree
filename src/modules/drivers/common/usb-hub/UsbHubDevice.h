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

#ifndef USBHUBDEVICE_H
#define USBHUBDEVICE_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/String.h"

#include "modules/drivers/common/usb-hcd/PortChangeRequest.h"
#include "modules/system/usb/Usb.h"
#include "modules/system/usb/UsbConstants.h"
#include "modules/system/usb/UsbDevice.h"
#include "modules/system/usb/UsbHub.h"

class UsbHubDevice : public UsbDevice, public UsbHub, private RequestQueue {
 public:
  UsbHubDevice(UsbDevice* dev);
  virtual ~UsbHubDevice();

  virtual void initialiseDriver();

  virtual void getName(String& str) {
    str.assign("USB Hub Device", 15);
  }

  bool hasSubtree() const override {
    return true;
  }

  Device* getDevice() override {
    return this;
  }

  void prepareForDriverRetirement() override;
  void quiesceForRetirement() override;
  void prepareForDisconnection() override;

  virtual void addTransferToTransaction(uintptr_t pTransaction, bool bToggle, UsbPid pid,
                                        uintptr_t pBuffer, size_t nBytes);
  virtual uintptr_t createTransaction(UsbEndpoint endpointInfo);
  MUST_USE_RESULT virtual bool doAsync(uintptr_t pTransaction,
                                       void (*pCallback)(uintptr_t, ssize_t) = 0,
                                       uintptr_t pParam = 0);
  MUST_USE_RESULT virtual bool addInterruptInHandler(UsbEndpoint endpointInfo, uintptr_t pBuffer,
                                                     uint16_t nBytes,
                                                     void (*pCallback)(uintptr_t, ssize_t),
                                                     UsbInterruptInHandle& handle,
                                                     uintptr_t pParam = 0);

  virtual bool portReset(uint8_t nPort, bool bErrorResponse = false);

 protected:
  virtual void cancelAsyncAndDrain(uintptr_t pTransaction, void (*pCallback)(uintptr_t, ssize_t),
                                   uintptr_t pParam);
  MUST_USE_RESULT bool cancelInterruptInAndDrain(const UsbInterruptInToken& token,
                                                 void (*callback)(uintptr_t, ssize_t),
                                                 uintptr_t parameter,
                                                 bool producerAlreadyStopped) override;
  uint64_t executeRequest(uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4, uint64_t p5,
                          uint64_t p6, uint64_t p7, uint64_t p8) override;
  void cancelRequest(const Request& request) override;
  bool portSpeed(uint8_t port, UsbSpeed& speed) override;

 private:
  enum HubFeatureSelectors {
    HubLocalPower = 0,
    HubOverCurrent = 1,
  };

  enum PortFeatureSelectors {
    PortConnection = 0,
    PortEnable = 1,
    PortSuspend = 2,
    PortOverCurrent = 3,
    PortReset = 4,
    PortPower = 8,
    PortLowSpeed = 9,
    CPortConnection = 16,
    CPortEnable = 17,
    CPortSuspend = 18,
    CPortOverCurrent = 19,
    CPortReset = 20,
    PortTest = 21,
    PortIndicator = 22,
  };

  enum HubRequests {
    HubRequest = static_cast<uint8_t>(UsbRequestType::Class),
    HubPortRequest = static_cast<uint8_t>(static_cast<uint8_t>(UsbRequestType::Class) |
                                          static_cast<uint8_t>(UsbRequestRecipient::Other))
  };

  bool setPortFeature(size_t port, PortFeatureSelectors feature);
  bool clearPortFeature(size_t port, PortFeatureSelectors feature);

  /// Top 16 bits of status hold the port-change flags.
  bool getPortStatus(size_t port, uint32_t& status);
  bool clearPortChanges(size_t port, uint32_t status);
  bool debouncePort(size_t port, uint32_t& status);
  bool connectPort(size_t port);
  void portChanged(size_t port);
  void stopHotplug();
  static void statusChanged(uintptr_t parameter, ssize_t result);

  struct HubDescriptor {
    inline HubDescriptor(void* pBuffer)
        : pDescriptor(static_cast<Descriptor*>(pBuffer)),
          nPorts(pDescriptor->nPorts),
          hubCharacteristics(pDescriptor->hubCharacteristics),
          powerGoodDelay(pDescriptor->powerGoodDelay) {}

    ~HubDescriptor() {
      delete[] reinterpret_cast<uint8_t*>(pDescriptor);
    }

    struct Descriptor {
      uint8_t nLength;
      uint8_t nType;
      uint8_t nPorts;
      uint16_t hubCharacteristics;
      uint8_t powerGoodDelay;
      uint8_t controllerCurrent;
    } PACKED* pDescriptor;

    uint8_t nPorts;
    uint16_t hubCharacteristics;
    uint8_t powerGoodDelay;
  };

  size_t m_nPorts;
  size_t m_PowerGoodDelay;
  uint8_t* m_StatusBuffer;
  size_t m_StatusBytes;
  UsbInterruptInHandle m_StatusInterrupt;
  UsbHcd::PortChangeRequest m_StatusChange;
};

#endif
