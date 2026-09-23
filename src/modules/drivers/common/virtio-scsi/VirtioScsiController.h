/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef VIRTIO_SCSI_CONTROLLER_H
#define VIRTIO_SCSI_CONTROLLER_H

#include "pedigree/kernel/machine/IrqHandler.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/processor/MemoryRegion.h"

#include "modules/drivers/common/scsi/ScsiController.h"
#include "modules/drivers/common/virtio/VirtioPci.h"
#include "modules/drivers/common/virtio/Virtqueue.h"

class EXPORTED_PUBLIC VirtioScsiController final : public ScsiController, public IrqHandler {
 public:
  explicit VirtioScsiController(Device* pci);
  ~VirtioScsiController() override;

  bool initialiseController();
  void shutdown();

  bool sendCommand(size_t unit, uintptr_t command, uint8_t commandBytes, uintptr_t data,
                   uint16_t dataBytes, bool writing) override;
  IrqDisposition irq(irq_id_t number) override;

 protected:
  size_t getNumUnits() override {
    return m_UnitCount;
  }

 private:
  enum class Result { Success, DeviceError, TransportError };

  Result command(uint8_t target, const void* cdb, size_t cdbBytes, void* data, size_t dataBytes,
                 bool writing, bool interruptProbe);
  void drainCompletions(bool fromInterrupt);
  void failController();

  Device* m_Pci;
  Virtio::PciTransport m_Transport;
  Virtio::Queue m_RequestQueue;
  MemoryRegion m_Control;
  MemoryRegion m_Data;
  Mutex m_CommandLock;
  Mutex m_IrqLock;
  Semaphore m_Completion;
  irq_id_t m_Irq;
  uint8_t m_Targets[256];
  size_t m_UnitCount;
  size_t m_CdbBytes;
  size_t m_SenseBytes;
  size_t m_CommandUsed;
  size_t m_InterruptCompletions;
  bool m_TransportInitialised;
  bool m_Ready;
  bool m_CommandSeen;
  bool m_CommandValid;
  bool m_Stopping;
  bool m_Failed;
  bool m_Shutdown;
};

#endif
