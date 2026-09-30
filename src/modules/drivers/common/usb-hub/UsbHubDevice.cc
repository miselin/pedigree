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
constexpr uint8_t HubDescriptorType = 0x29;
constexpr size_t PortDebouncePollLimit = 10;
}  // namespace

UsbHubDevice::UsbHubDevice(UsbDevice* dev)
    : UsbDevice(dev),
      UsbHub(),
      RequestQueue(MakeConstantString("USB hub")),
      m_nPorts(0),
      m_PowerGoodDelay(50),
      m_StatusBuffer(nullptr),
      m_StatusBytes(0),
      m_StatusInterrupt(),
      m_StatusChange() {
  attachToUpstreamHub(m_pHub, {m_nRootPort, m_nRootPortGeneration});
}

UsbHubDevice::~UsbHubDevice() {
  stopHotplug();
  disconnectAllDevices();
  delete[] m_StatusBuffer;
}

void UsbHubDevice::prepareForDriverRetirement() {
  stopHotplug();
  retainDisconnectedAddressesUntilControllerTeardown();
}

void UsbHubDevice::prepareForDisconnection() {
  stopHotplug();
}

void UsbHubDevice::quiesceForRetirement() {
  stopHotplug();
}

void UsbHubDevice::stopHotplug() {
  if (!m_StatusInterrupt.reset()) {
    panic("USB hub destroyed from a running interrupt callback");
  }
  m_StatusChange.stopAfterQuiesce();
  RequestQueue::destroy();
}

void UsbHubDevice::initialiseDriver() {
  StartupActivity startup(this);
  if (!startup) {
    return;
  }
  uint8_t len = getDescriptorLength(HubDescriptorType, 0, UsbRequestType::Class);
  void* pDesc = 0;
  if (len >= sizeof(HubDescriptor::Descriptor)) {
    pDesc = getDescriptor(HubDescriptorType, 0, len, UsbRequestType::Class);
    if (!pDesc) {
      return;
    }
  } else {
    return;
  }

  HubDescriptor pDescriptor(pDesc);
  DEBUG_LOG("USB: HUB: Found a hub with "
            << Dec << pDescriptor.nPorts << Hex
            << " ports and hubCharacteristics = " << pDescriptor.hubCharacteristics);
  m_nPorts = pDescriptor.nPorts;
  if (!m_nPorts || !m_pInterface) {
    startup.failed();
    return;
  }
  const size_t powerGoodDelay = static_cast<size_t>(pDescriptor.powerGoodDelay) * 2;
  if (powerGoodDelay > m_PowerGoodDelay) {
    m_PowerGoodDelay = powerGoodDelay;
  }
  for (size_t i = 0; i < m_nPorts; i++) {
    // Grab this port's status
    uint32_t portStatus = 0;
    if (!getPortStatus(i, portStatus)) {
      WARNING("USB: HUB: couldn't read port " << Dec << i << Hex);
      startup.failed();
      continue;
    }

    // Is power on?
    if (!(portStatus & (1 << 8))) {
      DEBUG_LOG("USB: HUB: Powering up port " << Dec << i << Hex << " [status = " << portStatus
                                              << "]...");

      // Power it on
      if (!setPortFeature(i, PortPower)) {
        WARNING("USB: HUB: couldn't power port " << Dec << i << Hex);
        startup.failed();
        continue;
      }

      // Delay while the power goes on
      if (!Time::delay(m_PowerGoodDelay * Time::Multiplier::Millisecond)) {
        startup.failed();
        continue;
      }

      // Done.
      if (!getPortStatus(i, portStatus)) {
        startup.failed();
        continue;
      }

      // If port power never went on, skip this port
      if (!(portStatus & (1 << 8))) {
        DEBUG_LOG("USB: HUB: Port " << Dec << i << Hex << " couldn't be powered up.");
        startup.failed();
        continue;
      }

      DEBUG_LOG("USB: HUB: Powered up port " << Dec << i << Hex << " [status = " << portStatus
                                             << "]...");
    }

    if (!clearPortChanges(i, portStatus)) {
      startup.failed();
      continue;
    }
    if ((portStatus & (1 << PortConnection)) && !connectPort(i)) {
      startup.failed();
    }
  }

  Endpoint* statusEndpoint = nullptr;
  for (size_t i = 0; i < m_pInterface->endpointList.count(); ++i) {
    Endpoint* endpoint = m_pInterface->endpointList[i];
    if (endpoint->bIn && endpoint->nTransferType == Endpoint::Interrupt) {
      statusEndpoint = endpoint;
      break;
    }
  }
  m_StatusBytes = (m_nPorts + 8) / 8;
  if (!statusEndpoint || statusEndpoint->nMaxPacketSize < m_StatusBytes) {
    ERROR("USB: HUB: no usable status-change endpoint");
    startup.failed();
    return;
  }
  m_StatusBuffer = new uint8_t[m_StatusBytes]();
  if (!m_StatusChange.configure(*this, 0)) {
    startup.failed();
    return;
  }
  RequestQueue::initialise();
  if (getLifecycleState() != RequestQueue::LifecycleState::Accepting ||
      !UsbDevice::addInterruptInHandler(statusEndpoint, reinterpret_cast<uintptr_t>(m_StatusBuffer),
                                        m_StatusBytes, statusChanged, m_StatusInterrupt,
                                        reinterpret_cast<uintptr_t>(this))) {
    ERROR("USB: HUB: status-change submission failed");
    startup.failed();
    stopHotplug();
    return;
  }

  m_UsbState = HasDriver;
  NOTICE("USB: HUB: hotplug ready, address " << Dec << m_nAddress << ", ports " << m_nPorts << Hex);
}

void UsbHubDevice::statusChanged(uintptr_t parameter, ssize_t result) {
  auto* hub = reinterpret_cast<UsbHubDevice*>(parameter);
  if (result <= 0 || static_cast<size_t>(result) > hub->m_StatusBytes) {
    return;
  }
  bool changed = false;
  for (size_t i = 0; i < static_cast<size_t>(result); ++i) {
    changed = changed || hub->m_StatusBuffer[i];
  }
  if (!changed) {
    return;
  }

  // Control requests must run outside HCD callback delivery, where they can
  // wait for completions. Port-change latches retain the bitmap's meaning.
  const auto observation = hub->m_StatusChange.observe();
  if (UsbHcd::PortChangeRequest::canAcknowledge(observation.result)) {
    hub->m_StatusChange.acknowledge(observation.generation);
  }
}

uint64_t UsbHubDevice::executeRequest(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                      uint64_t, uint64_t generation) {
  UsbHcd::PortChangeRequest::Completion completion(m_StatusChange, generation);
  if (!completion) {
    return 0;
  }

  uint32_t hubStatus = 0;
  if (controlRequest(static_cast<uint8_t>(UsbRequestDirection::In) | HubRequest,
                     UsbRequest::GetStatus, 0, 0, sizeof(hubStatus),
                     reinterpret_cast<uintptr_t>(&hubStatus))) {
    for (size_t feature = HubLocalPower; feature <= HubOverCurrent; ++feature) {
      if (hubStatus & (1U << (feature + 16))) {
        if (!controlRequest(HubRequest, UsbRequest::ClearFeature, feature, 0)) {
          WARNING("USB: HUB: couldn't clear hub change " << Dec << feature << Hex);
        }
      }
    }
  }
  for (size_t port = 0; port < m_nPorts; ++port) {
    if (getLifecycleState() != RequestQueue::LifecycleState::Accepting) {
      break;
    }
    portChanged(port);
  }
  return 0;
}

void UsbHubDevice::cancelRequest(const Request& request) {
  m_StatusChange.cancel(request.p8);
}

bool UsbHubDevice::clearPortChanges(size_t port, uint32_t status) {
  bool cleared = true;
  for (size_t feature = CPortConnection; feature <= CPortReset; ++feature) {
    if (status & (1U << feature)) {
      if (!clearPortFeature(port, static_cast<PortFeatureSelectors>(feature))) {
        cleared = false;
      }
    }
  }
  return cleared;
}

bool UsbHubDevice::debouncePort(size_t port, uint32_t& status) {
  for (size_t poll = 0; poll < PortDebouncePollLimit; ++poll) {
    const auto state = getLifecycleState();
    if (state == RequestQueue::LifecycleState::Stopping ||
        state == RequestQueue::LifecycleState::Destroyed) {
      return false;
    }
    if (!clearPortFeature(port, CPortConnection) ||
        !Time::delay(100 * Time::Multiplier::Millisecond) || !getPortStatus(port, status)) {
      return false;
    }
    if (!(status & (1 << PortConnection))) {
      return false;
    }
    if (!(status & (1U << CPortConnection))) {
      return true;
    }
  }
  WARNING("USB: HUB: connection on port " << Dec << port << Hex << " did not settle");
  return false;
}

bool UsbHubDevice::connectPort(size_t port) {
  uint32_t status = 0;
  if (!debouncePort(port, status)) {
    return false;
  }
  return deviceConnected(port, FullSpeed);
}

bool UsbHubDevice::portSpeed(uint8_t port, UsbSpeed& speed) {
  uint32_t status = 0;
  if (!getPortStatus(port, status) || (status & 3) != 3) {
    return false;
  }
  if (status & (1 << 10)) {
    speed = HighSpeed;
  } else if (status & (1 << PortLowSpeed)) {
    speed = LowSpeed;
  } else {
    speed = FullSpeed;
  }
  return true;
}

void UsbHubDevice::portChanged(size_t port) {
  uint32_t status = 0;
  if (!getPortStatus(port, status) || !(status >> 16)) {
    return;
  }
  if (!clearPortChanges(port, status)) {
    WARNING("USB: HUB: couldn't clear changes on port " << Dec << port << Hex);
    return;
  }

  const bool connectionChanged = status & (1U << CPortConnection);
  const bool disabled = (status & (1U << CPortEnable)) && !(status & (1 << PortEnable));
  const bool overCurrentChanged = status & (1U << CPortOverCurrent);
  if (!connectionChanged && !disabled && !overCurrentChanged) {
    return;
  }

  deviceDisconnected(port);
  if (status & (1 << PortOverCurrent)) {
    return;
  }
  if (!(status & (1 << PortPower))) {
    if (!setPortFeature(port, PortPower) ||
        !Time::delay(m_PowerGoodDelay * Time::Multiplier::Millisecond)) {
      return;
    }
    if (!getPortStatus(port, status)) {
      return;
    }
  }
  if (!(status & (1 << PortConnection))) {
    return;
  }
  if (!connectPort(port)) {
    WARNING("USB: HUB: couldn't enumerate changed port " << Dec << port << Hex);
  }
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

  // Wait for completion
  uint32_t portStatus = 0;
  size_t poll = 0;
  for (; poll < PortResetPollLimit; ++poll) {
    const auto state = getLifecycleState();
    if (state == RequestQueue::LifecycleState::Stopping ||
        state == RequestQueue::LifecycleState::Destroyed) {
      return false;
    }
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
  if (!clearPortFeature(nPort, CPortReset))
    return false;

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
  return controlRequest(static_cast<uint8_t>(static_cast<uint8_t>(UsbRequestDirection::In) |
                                             static_cast<uint8_t>(HubPortRequest)),
                        UsbRequest::GetStatus, 0, (port + 1) & 0xFF, sizeof(status),
                        reinterpret_cast<uintptr_t>(&status));
}

void UsbHubDevice::addTransferToTransaction(uintptr_t pTransaction, bool bToggle, UsbPid pid,
                                            uintptr_t pBuffer, size_t nBytes) {
  m_pHub->addTransferToTransaction(pTransaction, bToggle, pid, pBuffer, nBytes);
}

uintptr_t UsbHubDevice::createTransaction(UsbEndpoint endpointInfo) {
  if (endpointInfo.speed != HighSpeed && !endpointInfo.nHubAddress) {
    if (m_Speed == HighSpeed) {
      endpointInfo.nHubAddress = m_nAddress;
      ++endpointInfo.nHubPort;
    } else
      endpointInfo.nHubPort = m_nPort;
  }
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
  if (endpointInfo.speed != HighSpeed && !endpointInfo.nHubAddress) {
    if (m_Speed == HighSpeed) {
      endpointInfo.nHubAddress = m_nAddress;
      ++endpointInfo.nHubPort;
    } else
      endpointInfo.nHubPort = m_nPort;
  }
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
