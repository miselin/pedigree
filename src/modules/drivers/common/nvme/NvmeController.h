/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef NVME_CONTROLLER_H
#define NVME_CONTROLLER_H
#include "pedigree/kernel/machine/IrqHandler.h"
#include "pedigree/kernel/process/OperationBarrier.h"

#include "NvmeQueue.h"
#include "modules/drivers/common/scsi/ScsiController.h"
class NvmeDisk;
class EXPORTED_PUBLIC NvmeController final : public ScsiController, public IrqHandler {
 public:
  explicit NvmeController(Device* pci);
  ~NvmeController() override;
  bool initialiseController();
  void shutdown();
  bool identify(uint32_t nsid, uint8_t kind, void* buffer);
  bool readWrite(uint32_t nsid, uint64_t lba, uint32_t blocks, void* buffer, size_t bytes,
                 bool writing);
  bool flush(uint32_t nsid);
  size_t interruptCompletions() const;
  size_t maximumOutstanding() const;
  size_t maxTransfer() const {
    return m_MaxTransfer;
  }
  const char* model() const {
    return m_Model;
  }
  bool supportsConcurrentReads() const override {
    return true;
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
  bool waitReady(bool ready);
  bool disable();
  void failController();
  bool command(NvmeQueue& queue, Nvme::Command command, void* buffer = nullptr, size_t bytes = 0,
               bool writing = false, uint32_t* result = nullptr);
  bool createIoQueue();
  bool discoverNamespaces(uint32_t maximumId);
  NvmeDisk* findNamespace(uint32_t nsid);
  Device* m_Pci;
  IoBase* m_Registers;
  NvmeQueue m_Admin;
  NvmeQueue m_Io;
  OperationBarrier m_Commands;
  Mutex m_WriteLock;
  Mutex m_ResetLock;
  mutable Mutex m_IrqLock;
  size_t m_ReadyMilliseconds;
  size_t m_MaxTransfer;
  irq_id_t m_Irq;
  uint16_t m_OriginalCommand;
  bool m_PciChanged;
  bool m_HardwareOwned;
  bool m_DmaInstalled;
  bool m_Interrupts;
  bool m_Failed;
  bool m_Stopping;
  bool m_Shutdown;
  char m_Model[41];
};
#endif
