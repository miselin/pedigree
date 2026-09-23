/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef VIRTIO_BLK_CONTROLLER_H
#define VIRTIO_BLK_CONTROLLER_H

#include "pedigree/kernel/machine/IrqHandler.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/processor/MemoryRegion.h"

#include "modules/drivers/common/scsi/ScsiController.h"
#include "modules/drivers/common/virtio/VirtioPci.h"
#include "modules/drivers/common/virtio/Virtqueue.h"

class VirtioBlkDisk;

class EXPORTED_PUBLIC VirtioBlkController final : public ScsiController, public IrqHandler {
 public:
  explicit VirtioBlkController(Device* pci);
  ~VirtioBlkController() override;

  bool initialiseController();
  void shutdown();
  bool readWrite(uint64_t location, void* buffer, size_t bytes, bool writing);
  bool flush();
  size_t sizeBytes() const {
    return m_Bytes;
  }
  size_t sectorBytes() const {
    return m_SectorBytes;
  }
  bool readOnly() const {
    return m_ReadOnly;
  }

  IrqDisposition irq(irq_id_t number) override;
  bool sendCommand(size_t, uintptr_t, uint8_t, uintptr_t, uint16_t, bool) override {
    return false;
  }

 protected:
  size_t getNumUnits() override {
    return getNumChildren();
  }

 private:
  bool command(uint32_t type, uint64_t sector, void* buffer, size_t bytes, size_t timeoutSeconds,
               bool interruptProbe);
  void drainCompletions(bool fromInterrupt);
  void failController();

  Device* m_Pci;
  Virtio::PciTransport m_Transport;
  Virtio::Queue m_Queue;
  MemoryRegion m_Control;
  MemoryRegion m_Data;
  Mutex m_CommandLock;
  Mutex m_IrqLock;
  Semaphore m_Completion;
  irq_id_t m_Irq;
  size_t m_Bytes;
  size_t m_SectorBytes;
  size_t m_ExpectedUsed;
  size_t m_InterruptCompletions;
  bool m_ReadOnly;
  bool m_Flush;
  bool m_TransportInitialised;
  bool m_Ready;
  bool m_CommandSeen;
  bool m_CommandValid;
  bool m_Stopping;
  bool m_Failed;
  bool m_Shutdown;
};

#endif
