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

#include "UsbHubDevice.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/time/Time.h"

#include "modules/system/usb/UsbDevice.h"
#include "modules/system/usb/UsbHub.h"

namespace {
constexpr size_t PortResetPollLimit = 100;
}

UsbHubDevice::UsbHubDevice(UsbDevice* dev) : UsbDevice(dev), UsbHub() {
  attachToUpstreamHub(m_pHub, {m_nRootPort, m_nRootPortGeneration});
}

UsbHubDevice::~UsbHubDevice() {
  disconnectAllDevices();
}

void UsbHubDevice::prepareForDriverRetirement() {
  retainDisconnectedAddressesUntilControllerTeardown();
}

void UsbHubDevice::initialiseDriver() {
  uint8_t len = getDescriptorLength(0, 0, UsbRequestType::Class);
  void* pDesc = 0;
  if (len) {
    pDesc = getDescriptor(0, 0, len, UsbRequestType::Class);
    if (!pDesc)
      return;
  } else
    return;

  HubDescriptor pDescriptor(pDesc);
  DEBUG_LOG("USB: HUB: Found a hub with "
            << Dec << pDescriptor.nPorts << Hex
            << " ports and hubCharacteristics = " << pDescriptor.hubCharacteristics);
  m_nPorts = pDescriptor.nPorts;
  for (size_t i = 0; i < m_nPorts; i++) {
    // Grab this port's status
    uint32_t portStatus = 0;
    if (!getPortStatus(i, portStatus)) {
      WARNING("USB: HUB: couldn't read port " << Dec << i << Hex);
      continue;
    }

    // Is power on?
    if (!(portStatus & (1 << 8))) {
      DEBUG_LOG("USB: HUB: Powering up port " << Dec << i << Hex << " [status = " << portStatus
                                              << "]...");

      // Power it on
      if (!setPortFeature(i, PortPower)) {
        WARNING("USB: HUB: couldn't power port " << Dec << i << Hex);
        continue;
      }

      // Delay while the power goes on
      if (!Time::delay(50 * Time::Multiplier::Millisecond))
        continue;

      // Done.
      if (!getPortStatus(i, portStatus))
        continue;

      // If port power never went on, skip this port
      if (!(portStatus & (1 << 8))) {
        DEBUG_LOG("USB: HUB: Port " << Dec << i << Hex << " couldn't be powered up.");
        continue;
      }

      DEBUG_LOG("USB: HUB: Powered up port " << Dec << i << Hex << " [status = " << portStatus
                                             << "]...");
    }

    if (portReset(i)) {
      // Got a device - what type?
      if (!getPortStatus(i, portStatus))
        continue;
      if (portStatus & (1 << 10)) {
        // High-speed
        DEBUG_LOG("USB: HUB: Hub port " << Dec << i << Hex
                                        << " has a high-speed device attached to it.");
        deviceConnected(i, HighSpeed);
      } else if (portStatus & (1 << 9)) {
        // Low-speed
        DEBUG_LOG("USB: HUB: Hub port " << Dec << i << Hex
                                        << " has a low-speed device attached to it.");
        deviceConnected(i, LowSpeed);
      } else {
        // Full-speed
        DEBUG_LOG("USB: HUB: Hub port " << Dec << i << Hex
                                        << " has a full-speed device attached to it.");
        deviceConnected(i, FullSpeed);
      }
    }
  }

  m_UsbState = HasDriver;
}

bool UsbHubDevice::portReset(uint8_t nPort, bool bErrorResponse) {
  (void)bErrorResponse;
  if (nPort >= m_nPorts)
    return false;

  // Reset the port
  if (!setPortFeature(nPort, PortReset))
    return false;

  // Delay while the reset completes
  if (!Time::delay(50 * Time::Multiplier::Millisecond))
    return false;

  // Done with reset
  if (!clearPortFeature(nPort, PortReset))
    return false;

  // Wait for completion
  uint32_t portStatus = 0;
  size_t poll = 0;
  for (; poll < PortResetPollLimit; ++poll) {
    if (!getPortStatus(nPort, portStatus))
      return false;
    if (!(portStatus & (1 << 4)))
      break;
    if (!Time::delay(Time::Multiplier::Millisecond))
      return false;
  }
  if (poll == PortResetPollLimit) {
    ERROR("USB: HUB: reset on port " << Dec << static_cast<size_t>(nPort) << Hex << " timed out");
    return false;
  }

  // Port has been powered on and now reset, check to see if it's enabled and
  // a device is connected
  return ((portStatus & 0x3) == 0x3);
}

bool UsbHubDevice::setPortFeature(size_t port, PortFeatureSelectors feature) {
  return controlRequest(HubPortRequest, UsbRequest::SetFeature, feature, (port + 1) & 0xFF, 0, 0);
}

bool UsbHubDevice::clearPortFeature(size_t port, PortFeatureSelectors feature) {
  return controlRequest(HubPortRequest, UsbRequest::ClearFeature, feature, (port + 1) & 0xFF, 0, 0);
}

bool UsbHubDevice::getPortStatus(size_t port, uint32_t& status) {
  status = 0;
  return controlRequest(UsbRequestDirection::In | HubPortRequest, UsbRequest::GetStatus, 0,
                        (port + 1) & 0xFF, sizeof(status), reinterpret_cast<uintptr_t>(&status));
}

void UsbHubDevice::addTransferToTransaction(uintptr_t pTransaction, bool bToggle, UsbPid pid,
                                            uintptr_t pBuffer, size_t nBytes) {
  m_pHub->addTransferToTransaction(pTransaction, bToggle, pid, pBuffer, nBytes);
}

uintptr_t UsbHubDevice::createTransaction(UsbEndpoint endpointInfo) {
  if ((m_Speed == HighSpeed) && (endpointInfo.speed != HighSpeed) && !endpointInfo.nHubAddress)
    endpointInfo.nHubAddress = m_nAddress;
  return m_pHub->createTransaction(endpointInfo);
}

bool UsbHubDevice::doAsync(uintptr_t pTransaction, void (*pCallback)(uintptr_t, ssize_t),
                           uintptr_t pParam) {
  return m_pHub->doAsync(pTransaction, pCallback, pParam);
}

void UsbHubDevice::cancelAsyncAndDrain(uintptr_t pTransaction,
                                       void (*pCallback)(uintptr_t, ssize_t), uintptr_t pParam) {
  m_pHub->cancelAsyncAndDrain(pTransaction, pCallback, pParam);
}

bool UsbHubDevice::addInterruptInHandler(UsbEndpoint endpointInfo, uintptr_t pBuffer,
                                         uint16_t nBytes, void (*pCallback)(uintptr_t, ssize_t),
                                         UsbInterruptInHandle& handle, uintptr_t pParam) {
  if ((m_Speed == HighSpeed) && (endpointInfo.speed != HighSpeed) && (!endpointInfo.nHubAddress))
    endpointInfo.nHubAddress = m_nAddress;
  // The upstream call publishes the handle directly against the root HCD.
  return m_pHub->addInterruptInHandler(endpointInfo, pBuffer, nBytes, pCallback, handle, pParam);
}

bool UsbHubDevice::cancelInterruptInAndDrain(const UsbInterruptInToken& token,
                                             void (*callback)(uintptr_t, ssize_t),
                                             uintptr_t parameter, bool producerAlreadyStopped) {
  (void)token;
  (void)callback;
  (void)parameter;
  (void)producerAlreadyStopped;
  panic("downstream USB hub unexpectedly owned an interrupt-IN handle");
  return false;
}
