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

#include "UsbMassStorageDevice.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/utilities/PointerGuard.h"
#include "pedigree/kernel/utilities/Vector.h"

#include "modules/system/usb/Usb.h"
#include "modules/system/usb/UsbDevice.h"

UsbMassStorageDevice::UsbMassStorageDevice(UsbDevice* dev)
    : ScsiController(),
      UsbDevice(dev),
      m_nUnits(0),
      m_pInEndpoint(0),
      m_pOutEndpoint(0),
      m_NextTag(1),
      m_ResetRecoveryRequired(false) {
  setSpecificType(String("usb-msd-controller"));
}

UsbMassStorageDevice::~UsbMassStorageDevice() {
  shutdownDiskCaches();
  RequestQueue::destroy();
}

void UsbMassStorageDevice::initialiseDriver() {
  if (!m_pInterface || m_pInterface->nSubclass != 6 || m_pInterface->nProtocol != 0x50)
    return;
  for (size_t i = 0; i < m_pInterface->endpointList.count(); i++) {
    Endpoint* pEndpoint = m_pInterface->endpointList[i];
    if (!m_pInEndpoint && (pEndpoint->nTransferType == Endpoint::Bulk) && pEndpoint->bIn)
      m_pInEndpoint = pEndpoint;
    if (!m_pOutEndpoint && (pEndpoint->nTransferType == Endpoint::Bulk) && pEndpoint->bOut)
      m_pOutEndpoint = pEndpoint;
    if (m_pInEndpoint && m_pOutEndpoint)
      break;
  }

  if (!m_pInEndpoint) {
    ERROR("USB: MSD: No IN endpoint");
    return;
  }

  if (!m_pOutEndpoint) {
    ERROR("USB: MSD: No OUT endpoint");
    return;
  }

  if (!massStorageReset())
    return;
  uint8_t maxLun = 0;
  const ssize_t result = controlRequestResult(
      static_cast<uint8_t>(UsbRequestDirection::In) | static_cast<uint8_t>(MassStorageRequest),
      MassStorageGetMaxLUN, 0, m_pInterface->nInterface, 1, reinterpret_cast<uintptr_t>(&maxLun));
  if (result == -Stall)
    maxLun = 0;
  else if (result != 1 || maxLun > 15) {
    WARNING("USB: MSD: invalid GET_MAX_LUN response");
    return;
  }
  m_nUnits = maxLun + 1;

  searchDisks();

  m_UsbState = HasDriver;
}

bool UsbMassStorageDevice::massStorageReset() {
  return controlRequest(MassStorageRequest, MassStorageReset, 0, m_pInterface->nInterface);
}

bool UsbMassStorageDevice::performResetRecovery() {
  m_ResetRecoveryRequired = true;
  if (!m_pInterface || !m_pInEndpoint || !m_pOutEndpoint)
    return false;

  const bool reset = massStorageReset();
  const bool clearedIn = clearEndpointHalt(m_pInEndpoint);
  const bool clearedOut = clearEndpointHalt(m_pOutEndpoint);
  if (reset && clearedIn && clearedOut)
    m_ResetRecoveryRequired = false;
  return !m_ResetRecoveryRequired;
}

UsbMassStorageDevice::BotStatus UsbMassStorageDevice::readStatus(uint32_t tag,
                                                                 uint32_t expectedBytes) {
  Csw* pCsw = new Csw;
  PointerGuard<Csw> guard(pCsw);
  ByteSet(pCsw, 0, sizeof(Csw));

  ssize_t result = syncIn(m_pInEndpoint, reinterpret_cast<uintptr_t>(pCsw), sizeof(Csw));
  if (result == -Stall) {
    if (!clearEndpointHalt(m_pInEndpoint))
      return BotStatus::RecoveryRequired;
    ByteSet(pCsw, 0, sizeof(Csw));
    result = syncIn(m_pInEndpoint, reinterpret_cast<uintptr_t>(pCsw), sizeof(Csw));
  }

  if (result != static_cast<ssize_t>(sizeof(Csw)) || pCsw->nSig != CswSig ||
      pCsw->nTag != HOST_TO_LITTLE32(tag))
    return BotStatus::RecoveryRequired;

  const uint32_t residue = LITTLE_TO_HOST32(pCsw->nResidue);
  if (residue > expectedBytes)
    return BotStatus::RecoveryRequired;

  if (pCsw->nStatus == 0)
    return residue ? BotStatus::Failed : BotStatus::Passed;
  if (pCsw->nStatus == 1)
    return BotStatus::Failed;
  return BotStatus::RecoveryRequired;
}

bool UsbMassStorageDevice::sendCommand(size_t nUnit, uintptr_t pCommand, uint8_t nCommandSize,
                                       uintptr_t pRespBuffer, uint16_t nRespBytes, bool bWrite) {
  if (!pCommand || !nCommandSize || nCommandSize > 16 || nUnit >= m_nUnits || nUnit > 0xf ||
      (nRespBytes && !pRespBuffer) || !m_pInterface || !m_pInEndpoint || !m_pOutEndpoint)
    return false;
  LockGuard<Mutex> commandLock(m_CommandLock);
  if (m_ResetRecoveryRequired && !performResetRecovery())
    return false;

  Cbw* pCbw = new Cbw;
  PointerGuard<Cbw> guard(pCbw);
  ByteSet(pCbw, 0, sizeof(Cbw));
  const uint32_t tag = m_NextTag++;
  pCbw->nSig = CbwSig;
  pCbw->nTag = HOST_TO_LITTLE32(tag);
  pCbw->nDataBytes = HOST_TO_LITTLE32(nRespBytes);
  pCbw->nFlags = !bWrite && nRespBytes ? 0x80 : 0;
  pCbw->nLUN = nUnit;
  pCbw->nCommandSize = nCommandSize;
  MemoryCopy(pCbw->pCommand, reinterpret_cast<void*>(pCommand), nCommandSize);

  auto recover = [this]() {
    if (!performResetRecovery())
      WARNING("USB: MSD: reset recovery incomplete; new commands remain blocked");
    return false;
  };
  if (syncOut(m_pOutEndpoint, reinterpret_cast<uintptr_t>(pCbw), sizeof(Cbw)) != sizeof(Cbw))
    return recover();

  ssize_t transferred = 0;
  if (nRespBytes) {
    transferred = bWrite ? syncOut(m_pOutEndpoint, pRespBuffer, nRespBytes)
                         : syncIn(m_pInEndpoint, pRespBuffer, nRespBytes);
    if (transferred == -Stall) {
      if (!clearEndpointHalt(bWrite ? m_pOutEndpoint : m_pInEndpoint))
        return recover();
    } else if (transferred < 0 || transferred > nRespBytes || (bWrite && transferred != nRespBytes))
      return recover();
  }
  const BotStatus status = readStatus(tag, nRespBytes);
  if (status == BotStatus::RecoveryRequired)
    return recover();
  // A valid CSW may complete an OUT STALL, but an IN failure never supplies
  // bytes the controller did not receive. Short IN data must not become a
  // successful SCSI cache fill even when the device reports zero residue.
  return status == BotStatus::Passed && (bWrite || transferred == nRespBytes);
}
