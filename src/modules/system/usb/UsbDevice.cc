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

#include "modules/system/usb/UsbDevice.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/PointerGuard.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/new"

#include "modules/system/usb/Usb.h"
#include "modules/system/usb/UsbConstants.h"
#include "modules/system/usb/UsbDescriptors.h"
#include "modules/system/usb/UsbHub.h"
#include "modules/system/usb/UsbPnP.h"

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
Atomic<size_t> g_HostedUsbDescriptorDestructions(0);
#endif

UsbDevice::UsbDevice(UsbHub* pHub, uint8_t nPort, UsbSpeed speed)
    : m_nAddress(0),
      m_ControlPacketSize(speed == SuperSpeed  ? 512
                          : speed == HighSpeed ? 64
                                               : 8),
      m_nPort(nPort),
      m_nRootPort(0xff),
      m_nRootPortGeneration(0),
      m_Speed(speed),
      m_UsbState(Connected),
      m_pDescriptor(0),
      m_pConfiguration(0),
      m_pInterface(0),
      m_pHub(pHub),
      m_pContainer(0) {
  if (pHub) {
    const auto connection = pHub->rootConnectionForChild(nPort);
    m_nRootPort = connection.port;
    m_nRootPortGeneration = connection.generation;
  }
}

UsbDevice::UsbDevice(UsbDevice* pDev)
    : m_nAddress(pDev->m_nAddress),
      m_ControlPacketSize(pDev->m_ControlPacketSize),
      m_nPort(pDev->m_nPort),
      m_nRootPort(pDev->m_nRootPort),
      m_nRootPortGeneration(pDev->m_nRootPortGeneration),
      m_Speed(pDev->m_Speed),
      m_UsbState(pDev->m_UsbState),
      m_pDescriptor(pDev->m_pDescriptor),
      m_pConfiguration(pDev->m_pConfiguration),
      m_pInterface(pDev->m_pInterface),
      m_pHub(pDev->m_pHub),
      m_pContainer(nullptr) {
  // We have the same parent as pDev
  if (m_pDescriptor)
    m_pDescriptor->retain();
}

UsbDevice::~UsbDevice() {
  if (m_pDescriptor)
    m_pDescriptor->release();
}

UsbDevice::DeviceDescriptor::DeviceDescriptor(UsbDeviceDescriptor* pDescriptor) : m_References(1) {
  nBcdUsbRelease = pDescriptor->nBcdUsbRelease;
  nClass = pDescriptor->nClass;
  nSubclass = pDescriptor->nSubclass;
  nProtocol = pDescriptor->nProtocol;
  nMaxControlPacketSize = pDescriptor->nMaxControlPacketSize;
  nVendorId = pDescriptor->nVendorId;
  nProductId = pDescriptor->nProductId;
  nBcdDeviceRelease = pDescriptor->nBcdDeviceRelease;
  nVendorString = pDescriptor->nVendorString;
  nProductString = pDescriptor->nProductString;
  nSerialString = pDescriptor->nSerialString;
  nConfigurations = pDescriptor->nConfigurations;

  delete[] reinterpret_cast<uint8_t*>(pDescriptor);
}

UsbDevice::DeviceDescriptor::~DeviceDescriptor() {
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  g_HostedUsbDescriptorDestructions += 1;
#endif
  for (size_t i = 0; i < configList.count(); i++)
    delete configList[i];
}

void UsbDevice::DeviceDescriptor::retain() {
  m_References += 1;
}

void UsbDevice::DeviceDescriptor::release() {
  const size_t remaining = m_References -= 1;
  assert(remaining != static_cast<size_t>(-1));
  if (!remaining)
    delete this;
}

UsbDevice::ConfigDescriptor::ConfigDescriptor(void* pConfigBuffer, size_t nConfigLength,
                                              UsbSpeed speed)
    : nConfig(0), nString(0), valid(false) {
  auto* buffer = static_cast<uint8_t*>(pConfigBuffer);
  PointerGuard<uint8_t> guard(buffer, true);
  if (!buffer || nConfigLength < sizeof(UsbConfigurationDescriptor) ||
      buffer[0] < sizeof(UsbConfigurationDescriptor) || buffer[0] > nConfigLength ||
      buffer[1] != UsbDescriptor::Configuration)
    return;
  auto* descriptor = reinterpret_cast<UsbConfigurationDescriptor*>(buffer);
  nConfig = descriptor->nConfig;
  nString = descriptor->nString;
  Interface* current = nullptr;
  Endpoint* precedingEndpoint = nullptr;
  for (size_t offset = buffer[0]; offset < nConfigLength;) {
    if (nConfigLength - offset < 2)
      return;
    const size_t length = buffer[offset];
    const uint8_t type = buffer[offset + 1];
    if (length < 2 || length > nConfigLength - offset)
      return;
    if (speed == SuperSpeed && precedingEndpoint && !precedingEndpoint->hasCompanion &&
        type != 0x30)
      return;
    if (type == UsbDescriptor::Interface) {
      if (length < sizeof(UsbInterfaceDescriptor))
        return;
      current = new Interface(reinterpret_cast<UsbInterfaceDescriptor*>(buffer + offset));
      interfaceList.pushBack(current);
    } else if (type == UsbDescriptor::Endpoint) {
      if (!current || length < sizeof(UsbEndpointDescriptor))
        return;
      auto* endpoint =
          new Endpoint(reinterpret_cast<UsbEndpointDescriptor*>(buffer + offset), speed);
      const bool interrupt = endpoint->nTransferType == Endpoint::Interrupt;
      const bool bulk = endpoint->nTransferType == Endpoint::Bulk;
      const size_t maximum = speed == LowSpeed ? 8
                             : speed == FullSpeed
                                 ? (endpoint->nTransferType == Endpoint::Isochronus ? 1023 : 64)
                                 : 1024;
      const uint16_t packetField = buffer[offset + 4] | (uint16_t{buffer[offset + 5]} << 8);
      if (!endpoint->nEndpoint || !endpoint->nMaxPacketSize || endpoint->nMaxPacketSize > maximum ||
          (packetField & 0xe000U) || ((packetField & 0x1800U) == 0x1800U) ||
          (speed == LowSpeed && !interrupt) ||
          (bulk && speed == HighSpeed && endpoint->nMaxPacketSize != 512) ||
          (bulk && speed == SuperSpeed && endpoint->nMaxPacketSize != 1024) ||
          (speed == SuperSpeed && (packetField & 0x1800U)) ||
          (interrupt && (!endpoint->nInterval || ((speed == HighSpeed || speed == SuperSpeed) &&
                                                  endpoint->nInterval > 16)))) {
        delete endpoint;
        return;
      }
      current->endpointList.pushBack(endpoint);
      precedingEndpoint = endpoint;
    } else if (type == 0x30 && speed == SuperSpeed) {
      if (!precedingEndpoint || precedingEndpoint->hasCompanion || length < 6 ||
          buffer[offset + 2] > 15)
        return;
      const uint8_t attributes = buffer[offset + 3];
      const uint16_t intervalBytes = buffer[offset + 4] | (uint16_t{buffer[offset + 5]} << 8);
      const bool bulk = precedingEndpoint->nTransferType == Endpoint::Bulk;
      const bool interrupt = precedingEndpoint->nTransferType == Endpoint::Interrupt;
      if ((bulk && ((attributes & 0xe0U) || (attributes & 31U) > 16 || intervalBytes)) ||
          (interrupt &&
           (attributes || !intervalBytes ||
            intervalBytes > (buffer[offset + 2] + 1U) * precedingEndpoint->nMaxPacketSize)))
        return;
      precedingEndpoint->nMaxBurst = buffer[offset + 2];
      precedingEndpoint->nStreams =
          precedingEndpoint->nTransferType == Endpoint::Bulk ? buffer[offset + 3] & 31U : 0;
      precedingEndpoint->nBytesPerInterval =
          buffer[offset + 4] | (uint16_t{buffer[offset + 5]} << 8);
      precedingEndpoint->hasCompanion = true;
    } else if (current)
      current->otherDescriptorList.pushBack(new UnknownDescriptor(buffer + offset, type, length));
    else
      otherDescriptorList.pushBack(new UnknownDescriptor(buffer + offset, type, length));
    offset += length;
  }
  if (speed == SuperSpeed && precedingEndpoint && !precedingEndpoint->hasCompanion)
    return;
  valid = nConfig && interfaceList.count();
}

UsbDevice::ConfigDescriptor::~ConfigDescriptor() {
  for (size_t i = 0; i < interfaceList.count(); i++)
    delete interfaceList[i];
  for (size_t i = 0; i < otherDescriptorList.count(); i++)
    delete otherDescriptorList[i];
}

UsbDevice::Interface::Interface(UsbInterfaceDescriptor* pDescriptor) {
  nInterface = pDescriptor->nInterface;
  nAlternateSetting = pDescriptor->nAlternateSetting;
  nClass = pDescriptor->nClass;
  nSubclass = pDescriptor->nSubclass;
  nProtocol = pDescriptor->nProtocol;
  nString = pDescriptor->nString;
}

UsbDevice::Interface::~Interface() {
  for (size_t i = 0; i < endpointList.count(); i++)
    delete endpointList[i];
  for (size_t i = 0; i < otherDescriptorList.count(); i++)
    delete otherDescriptorList[i];
}

UsbDevice::Endpoint::Endpoint(UsbEndpointDescriptor* pDescriptor, UsbSpeed speed)
    : bDataToggle(false) {
  nEndpoint = pDescriptor->nEndpoint;
  bIn = pDescriptor->bDirection;
  bOut = !bIn;
  nTransferType = pDescriptor->nTransferType;
  nMaxPacketSize = LITTLE_TO_HOST16(pDescriptor->nMaxPacketSize) & 0x7ff;
  nInterval = pDescriptor->nInterval;
  const auto* raw = reinterpret_cast<const uint8_t*>(pDescriptor);
  nTransactions = ((raw[5] >> 3) & 3U) + 1;
}

void UsbDevice::initialise(uint8_t nAddress) {
  // Check for late calls
  if (m_UsbState > Connected) {
    ERROR(
        "USB: UsbDevice::initialise called, but this device is already "
        "initialised!");
    return;
  }

  // USB 2.0 9.2.6.2 requires recovery after reset has actually deasserted.
  Time::delay(10 * Time::Multiplier::Millisecond);
  UsbEndpoint control(0, m_nPort, 0, m_Speed, m_ControlPacketSize);
  control.nRootPort = m_nRootPort;
  control.nRootPortGeneration = m_nRootPortGeneration;
  if (!m_pHub || !m_pHub->prepareDevice(nAddress, control))
    return;
  // Learn endpoint zero's packet size before asking for a full descriptor.
  auto* prefix = static_cast<uint8_t*>(getDescriptor(UsbDescriptor::Device, 0, 8));
  if (!prefix)
    return;
  const uint8_t packetSize = prefix[7];
  const bool valid =
      prefix[0] >= sizeof(UsbDeviceDescriptor) && prefix[1] == UsbDescriptor::Device &&
      ((m_Speed == HighSpeed && packetSize == 64) || (m_Speed == LowSpeed && packetSize == 8) ||
       (m_Speed == SuperSpeed && packetSize == 9) ||
       (m_Speed == FullSpeed &&
        (packetSize == 8 || packetSize == 16 || packetSize == 32 || packetSize == 64)));
  delete[] prefix;
  if (!valid)
    return;
  m_ControlPacketSize = m_Speed == SuperSpeed ? 512 : packetSize;
  control.nMaxPacketSize = m_ControlPacketSize;
  if (m_pHub->controllerAssignsAddresses()
          ? !m_pHub->addressDevice(nAddress, control)
          : !controlRequest(0, UsbRequest::SetAddress, nAddress, 0))
    return;
  m_nAddress = nAddress;
  m_UsbState = Addressed;
  Time::delay(2 * Time::Multiplier::Millisecond);
  void* pDeviceDescriptor = getDescriptor(UsbDescriptor::Device, 0, sizeof(UsbDeviceDescriptor));
  if (!pDeviceDescriptor)
    return;
  m_pDescriptor = new DeviceDescriptor(static_cast<UsbDeviceDescriptor*>(pDeviceDescriptor));
  if (m_pDescriptor->nClass == 9 && !m_pHub->supportsHubDevices()) {
    WARNING("USB: external hubs are unsupported by this controller");
    return;
  }
  m_UsbState = HasDescriptors;  // We now have the device descriptor

// Debug dump of the device descriptor
  EMIT_IF(UsbVerboseDebug) {
    DEBUG_LOG("USB version: " << Dec << (m_pDescriptor->nBcdUsbRelease >> 8) << "."
                              << (m_pDescriptor->nBcdUsbRelease & 0xFF) << ".");
    DEBUG_LOG("Device class/subclass/protocol: " << m_pDescriptor->nClass << "/"
                                                 << m_pDescriptor->nSubclass << "/"
                                                 << m_pDescriptor->nProtocol);
    DEBUG_LOG("Maximum control packet size is " << Dec << m_pDescriptor->nMaxControlPacketSize
                                                << Hex << " bytes.");
    DEBUG_LOG("Vendor and product IDs: " << m_pDescriptor->nVendorId << ":"
                                         << m_pDescriptor->nProductId << ".");
    DEBUG_LOG("Device version: " << Dec << (m_pDescriptor->nBcdDeviceRelease >> 8) << "."
                                 << (m_pDescriptor->nBcdDeviceRelease & 0xFF) << Hex << ".");
    DEBUG_LOG("Number of configurations: " << m_pDescriptor->nConfigurations << ".");
    DEBUG_LOG("String indices: " << m_pDescriptor->nVendorString << ", "
                                 << m_pDescriptor->nProductString << ", "
                                 << m_pDescriptor->nSerialString);
  }

  // Descriptor number for the configuration descriptor
  uint8_t nConfigDescriptor = UsbDescriptor::Configuration;

  // Get the vendor, product and serial strings
  m_pDescriptor->sVendor = getString(m_pDescriptor->nVendorString);
  m_pDescriptor->sProduct = getString(m_pDescriptor->nProductString);
  m_pDescriptor->sSerial = getString(m_pDescriptor->nSerialString);

  // Grab each configuration from the device
  for (size_t i = 0; i < m_pDescriptor->nConfigurations; i++) {
    // Skip extra configurations
    if (i) {
      WARNING("USB: Found a device with multiple configurations!");
      break;
    }

    // Get the total size of this configuration descriptor
    uint16_t* pPartialConfig = static_cast<uint16_t*>(getDescriptor(nConfigDescriptor, i, 4));
    if (!pPartialConfig)
      return;
    uint16_t configLength = LITTLE_TO_HOST16(pPartialConfig[1]);
    delete[] reinterpret_cast<uint8_t*>(pPartialConfig);

    if (configLength < sizeof(UsbConfigurationDescriptor))
      return;
    // Get our configuration descriptor
    ConfigDescriptor* pConfig = new ConfigDescriptor(
        getDescriptor(nConfigDescriptor, i, configLength), configLength, m_Speed);

    if (!pConfig->valid) {
      delete pConfig;
      return;
    }
    // Get the associated string
    pConfig->sString = getString(pConfig->nString);

    // Go through the interface list
    for (size_t j = 0; j < pConfig->interfaceList.count(); j++) {
      // Get this interface, for minor adjustments
      Interface* pInterface = pConfig->interfaceList[j];
      if (pInterface->nClass == 9 && !m_pHub->supportsHubDevices()) {
        WARNING("USB: external hubs are unsupported by this controller");
        delete pConfig;
        return;
      }

      // Just in case the class numbers are in the device descriptor
      if (pConfig->interfaceList.count() == 1 && m_pDescriptor->nClass && !pInterface->nClass) {
        pInterface->nClass = m_pDescriptor->nClass;
        pInterface->nSubclass = m_pDescriptor->nSubclass;
        pInterface->nProtocol = m_pDescriptor->nProtocol;
      }

      // Again, get the associated string
      pInterface->sString = getString(pInterface->nString);
    }

    // Make sure it's not empty, then add it to our list of configurations
    assert(pConfig->interfaceList.count());
    m_pDescriptor->configList.pushBack(pConfig);
  }

  // Make sure we ended up with at least a configuration
  if (!m_pDescriptor->configList.count())
    return;

  // Use the first configuration
  /// \todo support more configurations (how?)
  useConfiguration(0);
}

ssize_t UsbDevice::doSync(UsbDevice::Endpoint* pEndpoint, UsbPid pid, uintptr_t pBuffer,
                          size_t nBytes, size_t timeout) {
  if (!pEndpoint || !pEndpoint->nMaxPacketSize) {
    ERROR("USB: UsbDevice::doSync called with invalid endpoint");
    return -TransactionError;
  }

  UsbHub* pParentHub = m_pHub;
  if (!pParentHub) {
    ERROR("USB: Orphaned UsbDevice!");
    return -TransactionError;
  }

  if (!nBytes)
    return 0;

  if (pBuffer & 0xF) {
    ERROR("USB: Input pointer wasn't properly aligned [" << pBuffer << ", " << nBytes << "]");
    return -TransactionError;
  }

  UsbEndpoint endpointInfo(m_nAddress, m_nPort, pEndpoint->nEndpoint, m_Speed,
                           pEndpoint->nMaxPacketSize);
  endpointInfo.nTransferType = pEndpoint->nTransferType;
  endpointInfo.nIn = pEndpoint->bIn;
  endpointInfo.nInterval = pEndpoint->nInterval;
  endpointInfo.nMaxBurst = pEndpoint->nMaxBurst;
  endpointInfo.nStreams = pEndpoint->nStreams;
  endpointInfo.nBytesPerInterval = pEndpoint->nBytesPerInterval;
  endpointInfo.nRootPort = m_nRootPort;
  endpointInfo.nRootPortGeneration = m_nRootPortGeneration;
  uintptr_t nTransaction = pParentHub->createTransaction(endpointInfo);
  if (nTransaction == static_cast<uintptr_t>(-1)) {
    ERROR(
        "UsbDevice: couldn't get a valid transaction to work with from "
        "the parent hub");
    return -TransactionError;
  }

  const bool initialToggle = pEndpoint->bDataToggle;
  const size_t requested = nBytes;
  size_t byteOffset = 0;
  while (nBytes) {
    size_t nBytesThisTransaction =
        nBytes > pEndpoint->nMaxPacketSize ? pEndpoint->nMaxPacketSize : nBytes;

    pParentHub->addTransferToTransaction(nTransaction, pEndpoint->bDataToggle, pid,
                                         pBuffer + byteOffset, nBytesThisTransaction);
    byteOffset += nBytesThisTransaction;
    nBytes -= nBytesThisTransaction;

    pEndpoint->bDataToggle = !pEndpoint->bDataToggle;
  }

  const ssize_t result = pParentHub->doSync(nTransaction, timeout);
  if (pid == UsbPidIn && result >= 0 && static_cast<size_t>(result) < requested) {
    // A short IN ends with a short packet, including a ZLP after full packets.
    const size_t packets = result / pEndpoint->nMaxPacketSize + 1;
    pEndpoint->bDataToggle = initialToggle ^ bool(packets & 1);
  }
  return result;
}

ssize_t UsbDevice::syncIn(Endpoint* pEndpoint, uintptr_t pBuffer, size_t nBytes, size_t timeout) {
  return doSync(pEndpoint, UsbPidIn, pBuffer, nBytes, timeout);
}

ssize_t UsbDevice::syncOut(Endpoint* pEndpoint, uintptr_t pBuffer, size_t nBytes, size_t timeout) {
  return doSync(pEndpoint, UsbPidOut, pBuffer, nBytes, timeout);
}

bool UsbDevice::addInterruptInHandler(Endpoint* pEndpoint, uintptr_t pBuffer, uint16_t nBytes,
                                      void (*pCallback)(uintptr_t, ssize_t),
                                      UsbInterruptInHandle& handle, uintptr_t pParam) {
  if (!pEndpoint || pEndpoint->nTransactions != 1) {
    ERROR(
        "USB: UsbDevice::addInterruptInHandler called with invalid "
        "endpoint");
    return false;
  }

  UsbHub* pParentHub = m_pHub;
  if (!pParentHub) {
    ERROR("USB: Orphaned UsbDevice!");
    return false;
  }

  if (!nBytes)
    return false;

  if (pBuffer & 0xF) {
    ERROR("USB: Input pointer wasn't properly aligned [" << pBuffer << ", " << nBytes << "]");
    return false;
  }

  UsbEndpoint endpointInfo(m_nAddress, m_nPort, pEndpoint->nEndpoint, m_Speed,
                           pEndpoint->nMaxPacketSize);
  endpointInfo.nInterval = pEndpoint->nInterval;
  endpointInfo.nTransferType = pEndpoint->nTransferType;
  endpointInfo.nIn = pEndpoint->bIn;
  endpointInfo.nMaxBurst = pEndpoint->nMaxBurst;
  endpointInfo.nStreams = pEndpoint->nStreams;
  endpointInfo.nBytesPerInterval = pEndpoint->nBytesPerInterval;
  endpointInfo.nRootPort = m_nRootPort;
  endpointInfo.nRootPortGeneration = m_nRootPortGeneration;
  return pParentHub->addInterruptInHandler(endpointInfo, pBuffer, nBytes, pCallback, handle,
                                           pParam);
}

ssize_t UsbDevice::controlRequestResult(uint8_t nRequestType, uint8_t nRequest, uint16_t nValue,
                                        uint16_t nIndex, uint16_t nLength, uintptr_t pBuffer,
                                        uint32_t timeout) {
  if (nLength && !pBuffer)
    return -TransactionError;
  // Setup structure - holds request details
  Setup* pSetup = new Setup(nRequestType, nRequest, nValue, nIndex, nLength);
  PointerGuard<Setup> guard(pSetup);

  UsbHub* pParentHub = m_pHub;
  if (!pParentHub) {
    ERROR("USB: Orphaned UsbDevice!");
    return -TransactionError;
  }

  UsbEndpoint endpointInfo(m_nAddress, m_nPort, 0, m_Speed, m_ControlPacketSize);
  endpointInfo.nRootPort = m_nRootPort;
  endpointInfo.nRootPortGeneration = m_nRootPortGeneration;

  uintptr_t nTransaction = pParentHub->createTransaction(endpointInfo);
  if (nTransaction == static_cast<uintptr_t>(-1)) {
    ERROR(
        "UsbDevice: couldn't get a valid transaction to work with from "
        "the parent hub");
    return -TransactionError;
  }

  // Setup Transfer - handles the SETUP phase of the transfer
  pParentHub->addTransferToTransaction(nTransaction, false, UsbPidSetup,
                                       reinterpret_cast<uintptr_t>(pSetup), sizeof(Setup));

  const size_t nMaxSize = m_ControlPacketSize;

  // Data Transfer - handles data transfer
  if (nLength) {
    bool bToggle = true;
    size_t nTransferLength = nLength;
    size_t nOffset = 0;
    while (nTransferLength) {
      size_t sz = nTransferLength > nMaxSize ? nMaxSize : nTransferLength;

      pParentHub->addTransferToTransaction(
          nTransaction, bToggle, nRequestType & UsbRequestDirection::In ? UsbPidIn : UsbPidOut,
          pBuffer + nOffset, sz);
      bToggle = !bToggle;

      nTransferLength -= sz;
      nOffset += sz;
    }
  }

  // Handshake Transfer - IN when we send data to the device, OUT when we
  // receive. Zero-length.
  pParentHub->addTransferToTransaction(
      nTransaction, true, nRequestType & UsbRequestDirection::In ? UsbPidOut : UsbPidIn, 0, 0);

  // Wait for the transaction to complete
  ssize_t nResult = pParentHub->doSync(nTransaction, timeout);

  // Return false if we had an error, true otherwise
  if (nResult < 0) {
    DEBUG_LOG("USB: Control request failure - status is " << nResult);
  }
  if (nResult < 0)
    return nResult;
  if (nResult < static_cast<ssize_t>(sizeof(Setup)) ||
      nResult > static_cast<ssize_t>(sizeof(Setup) + nLength))
    return -TransactionError;
  return nResult - sizeof(Setup);
}

bool UsbDevice::controlRequest(uint8_t nRequestType, uint8_t nRequest, uint16_t nValue,
                               uint16_t nIndex, uint16_t nLength, uintptr_t pBuffer,
                               uint32_t timeout) {
  return controlRequestResult(nRequestType, nRequest, nValue, nIndex, nLength, pBuffer, timeout) ==
         nLength;
}

uint16_t UsbDevice::getStatus() {
  uint16_t* nStatus = new uint16_t(0);
  PointerGuard<uint16_t> guard(nStatus);
  controlRequest(UsbRequestDirection::In, UsbRequest::GetStatus, 0, 0, 2,
                 reinterpret_cast<uintptr_t>(nStatus));
  return *nStatus;
}

bool UsbDevice::clearEndpointHalt(Endpoint* pEndpoint) {
  const uint16_t endpointAddress =
      pEndpoint->nEndpoint | (pEndpoint->bIn ? UsbRequestDirection::In : 0);
  if (!controlRequest(UsbRequestRecipient::Endpoint, UsbRequest::ClearFeature, 0,
                      endpointAddress)) {
    return false;
  }

  pEndpoint->bDataToggle = false;
  UsbEndpoint endpoint(m_nAddress, m_nPort, pEndpoint->nEndpoint, m_Speed,
                       pEndpoint->nMaxPacketSize);
  endpoint.nIn = pEndpoint->bIn;
  endpoint.nRootPort = m_nRootPort;
  endpoint.nRootPortGeneration = m_nRootPortGeneration;
  return m_pHub && m_pHub->resetEndpoint(endpoint);
}

void UsbDevice::useConfiguration(uint8_t nConfig) {
  if (!m_pDescriptor || nConfig >= m_pDescriptor->configList.count())
    return;
  m_pConfiguration = m_pDescriptor->configList[nConfig];
  if (!controlRequest(0, UsbRequest::SetConfiguration, m_pConfiguration->nConfig, 0))
    return;
  m_UsbState = Configured;  // We now are configured
}

void UsbDevice::useInterface(uint8_t nInterface) {
  if (!m_pConfiguration || nInterface >= m_pConfiguration->interfaceList.count())
    return;
  Interface* previous = m_pInterface;
  // First check if the previous interface was an alternate setting
  bool bWasAlternateSetting = m_pInterface && m_pInterface->nAlternateSetting;

  // Set our interface to the new one
  m_pInterface = m_pConfiguration->interfaceList[nInterface];

  // If needed, change the alternate setting
  if (bWasAlternateSetting || m_pInterface->nAlternateSetting)
    if (!controlRequest(UsbRequestRecipient::Interface, UsbRequest::SetInterface,
                        m_pInterface->nAlternateSetting, m_pInterface->nInterface)) {
      m_pInterface = previous;
      return;
    }

  // Set our state to HasInterface, if it's not higher
  if (m_UsbState < HasInterface)
    m_UsbState = HasInterface;
}

void* UsbDevice::getDescriptor(uint8_t nDescriptor, uint8_t nSubDescriptor, uint16_t nBytes,
                               uint8_t requestType) {
  if (!nBytes || ((requestType & 0x1f) == UsbRequestRecipient::Interface && !m_pInterface))
    return nullptr;
  uint8_t* pBuffer = new uint8_t[nBytes]();
  uint16_t nIndex =
      (requestType & 0x1f) == UsbRequestRecipient::Interface ? m_pInterface->nInterface : 0;

  /// \todo Proper language ID handling!
  if (nDescriptor == UsbDescriptor::String)
    nIndex = 0x0409;  // English (US)

  if (!controlRequest(UsbRequestDirection::In | requestType, UsbRequest::GetDescriptor,
                      (nDescriptor << 8) | nSubDescriptor, nIndex, nBytes,
                      reinterpret_cast<uintptr_t>(pBuffer))) {
    delete[] pBuffer;
    return 0;
  }
  return pBuffer;
}

uint8_t UsbDevice::getDescriptorLength(uint8_t nDescriptor, uint8_t nSubDescriptor,
                                       uint8_t requestType) {
  if ((requestType & 0x1f) == UsbRequestRecipient::Interface && !m_pInterface)
    return 0;
  uint8_t* length = new uint8_t(0);
  PointerGuard<uint8_t> guard(length);
  uint16_t nIndex =
      (requestType & 0x1f) == UsbRequestRecipient::Interface ? m_pInterface->nInterface : 0;

  /// \todo Proper language ID handling
  if (nDescriptor == UsbDescriptor::String)
    nIndex = 0x0409;  // English (US)

  controlRequest(UsbRequestDirection::In | requestType, UsbRequest::GetDescriptor,
                 (nDescriptor << 8) | nSubDescriptor, nIndex, 1,
                 reinterpret_cast<uintptr_t>(length));
  return *length;
}

String UsbDevice::getString(uint8_t nString) {
  // A value of zero means there's no string
  if (!nString)
    return String("");

  uint8_t descriptorLength = getDescriptorLength(UsbDescriptor::String, nString);
  if (descriptorLength < 2 || descriptorLength % 2)
    return String("");

  uint8_t* pBuffer =
      static_cast<uint8_t*>(getDescriptor(UsbDescriptor::String, nString, descriptorLength));
  if (!pBuffer)
    return String("");

  // Get the number of characters in the string and allocate a new buffer for
  // the string
  size_t nStrLength = (descriptorLength - 2) / 2;
  char* pString = new char[nStrLength + 1];

  // For each character, get the lower part of the UTF-16 value
  /// \todo UTF-8 support of some kind
  for (size_t i = 0; i < nStrLength; i++)
    pString[i] = pBuffer[2 + i * 2];

  // Set the last byte of the string to 0, delete the old buffer and return
  // the string
  pString[nStrLength] = 0;
  delete[] pBuffer;
  String result(pString);
  delete[] pString;
  return result;
}

UsbDeviceContainer::UsbDeviceContainer(UsbDevice* pDev)
    : Device(),
      m_pUsbDevice(pDev),
      m_ProbeOperations(),
      m_ProbeLock(),
      m_BindingRegistry(nullptr),
      m_BindingOwner(nullptr),
      m_BindingLease(),
      m_PreviousBinding(nullptr),
      m_NextBinding(nullptr) {
  assert(pDev);
  pDev->m_pContainer = this;
  attachSubtree(pDev);
}

UsbDeviceContainer::~UsbDeviceContainer() {
  if (m_ProbeOperations.isOpen())
    m_ProbeOperations.close();
  m_ProbeOperations.wait();
  UsbPnP* bindingRegistry = m_BindingRegistry;
  if (bindingRegistry)
    bindingRegistry->detachBinding(this);

  if (m_pUsbDevice && m_pUsbDevice->hasSubtree() && m_pUsbDevice->getDevice()) {
    Device* child = m_pUsbDevice->getDevice();
    removeChild(child);
    child->setParent(nullptr);
    m_pUsbDevice->m_pContainer = nullptr;
    delete m_pUsbDevice;
    m_pUsbDevice = nullptr;
    return;
  }

  destroyUsbDevice(m_pUsbDevice);
  m_pUsbDevice = nullptr;
}

bool UsbDeviceContainer::tryAcquireProbe(OperationBarrier::Lease& lease) {
  return m_ProbeOperations.tryAcquire(lease);
}

void UsbDeviceContainer::closeProbeAdmission() {
  m_ProbeOperations.close();
}

void UsbDeviceContainer::waitForProbes() {
  m_ProbeOperations.wait();
}

UsbDevice* UsbDeviceContainer::getUsbDevice() const {
  return m_pUsbDevice;
}

bool UsbDeviceContainer::replaceUsbDevice(UsbDevice* pDev) {
  if (!pDev || pDev == m_pUsbDevice)
    return false;

  UsbDevice* oldDevice = m_pUsbDevice;
  {
    Device::TreeLockGuard treeGuard;
    if (oldDevice && oldDevice->hasSubtree()) {
      Device* child = oldDevice->getDevice();
      if (child) {
        removeChild(child);
        child->setParent(nullptr);
      }
    }
    if (oldDevice)
      oldDevice->m_pContainer = nullptr;

    m_pUsbDevice = pDev;
    pDev->m_pContainer = this;
    attachSubtree(pDev);
  }
  delete oldDevice;
  return true;
}

void UsbDeviceContainer::attachSubtree(UsbDevice* pDev) {
  if (!pDev || !pDev->hasSubtree())
    return;

  Device* child = pDev->getDevice();
  if (!child)
    return;
  addChild(child);
  child->setParent(this);
}

void UsbDeviceContainer::destroyUsbDevice(UsbDevice* pDev) {
  if (!pDev)
    return;

  if (pDev->hasSubtree()) {
    Device* child = pDev->getDevice();
    if (child) {
      removeChild(child);
      child->setParent(nullptr);
    }
  }
  pDev->m_pContainer = nullptr;
  delete pDev;
}

void UsbDeviceContainer::getName(String& str) {
  m_pUsbDevice->getUsbDeviceName(str);
}

Device::Type UsbDeviceContainer::getType() {
  return Device::UsbContainer;
}

void UsbDeviceContainer::dump(String& str) {
  str.assign("Generic USB Device", 19);
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
namespace {
Atomic<size_t> g_HostedUsbDeviceDestructions(0);

class HostedOwnedUsbDevice : public UsbDevice {
 public:
  HostedOwnedUsbDevice() : UsbDevice(nullptr, 1, FullSpeed) {
    auto* descriptor =
        reinterpret_cast<UsbDeviceDescriptor*>(new uint8_t[sizeof(UsbDeviceDescriptor)]);
    ByteSet(descriptor, 0, sizeof(UsbDeviceDescriptor));
    m_pDescriptor = new DeviceDescriptor(descriptor);
  }

  explicit HostedOwnedUsbDevice(UsbDevice* device) : UsbDevice(device) {}

  ~HostedOwnedUsbDevice() override {
    g_HostedUsbDeviceDestructions += 1;
  }
};

class HostedSubtreeUsbDevice : public Device, public HostedOwnedUsbDevice {
 public:
  explicit HostedSubtreeUsbDevice(UsbDevice* device) : Device(), HostedOwnedUsbDevice(device) {}

  bool hasSubtree() const override {
    return true;
  }

  Device* getDevice() override {
    return this;
  }
};
}  // namespace

bool runHostedUsbContainerOwnershipRegression() {
  const size_t devicesBefore = g_HostedUsbDeviceDestructions;
  const size_t descriptorsBefore = g_HostedUsbDescriptorDestructions;

  auto* original = new HostedOwnedUsbDevice;
  auto* replacement = new HostedSubtreeUsbDevice(original);
  auto* container = new UsbDeviceContainer(original);
  const bool replaced = container->replaceUsbDevice(replacement);
  const bool replacementReachable = container->getUsbDevice() == replacement;
  const bool oldDestroyedOnce =
      g_HostedUsbDeviceDestructions == static_cast<size_t>(devicesBefore + 1);
  const bool descriptorStillOwned =
      g_HostedUsbDescriptorDestructions == static_cast<size_t>(descriptorsBefore);

  delete container;

  const bool passed =
      replaced && replacementReachable && oldDestroyedOnce && descriptorStillOwned &&
      g_HostedUsbDeviceDestructions == static_cast<size_t>(devicesBefore + 2) &&
      g_HostedUsbDescriptorDestructions == static_cast<size_t>(descriptorsBefore + 1);
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS usb-container-owned-replacement");
  } else {
    ERROR(
        "HOSTED-WAIT-TEST: FAIL usb-container-owned-replacement: container replacement or "
        "shared-descriptor teardown was not exact");
  }
  return passed;
}
#endif
