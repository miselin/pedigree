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
  bool identify(uint32_t nsid, uint8_t kind, void* buffer, bool interruptProbe = false);
  bool primaryControllerCapabilities(Nvme::PrimaryControllerCapabilities& capabilities);
  bool secondaryControllers(uint16_t firstControllerId, Nvme::SecondaryControllerList& controllers);
  // The caller must quiesce a secondary controller before taking it offline.
  // Primary allocation changes require a controller-level reset other than CC.EN.
  bool virtualizationManagement(uint16_t controllerId, Nvme::VirtualResource resource,
                                Nvme::VirtualizationAction action, uint16_t count,
                                uint16_t* modified = nullptr);
  // The caller must quiesce the target controller's namespace I/O and retire
  // its published disk/cache before changing attachment. No disk is rescanned here.
  bool setNamespaceAttachment(uint32_t nsid, uint16_t controllerId, bool attached);
  uint16_t controllerId() const {
    return m_ControllerId;
  }
  bool supportsVirtualizationManagement() const {
    return m_VirtualizationSupported;
  }
  bool readWrite(uint32_t nsid, uint64_t lba, uint32_t blocks, void* buffer, size_t bytes,
                 bool writing);
  bool flush(uint32_t nsid);
  size_t interruptCompletions() const;
  size_t maximumOutstanding() const;
  size_t ioQueueCount() const {
    return m_IoCount;
  }
  size_t ioQueueInterruptCompletions(size_t queue) const;
  Device* pciDevice() const {
    return m_Pci;
  }
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
  bool acceptsSpuriousInterrupts() const override {
    return true;
  }
  bool sendCommand(size_t, uintptr_t, uint8_t, uintptr_t, uint16_t, bool) override {
    return false;
  }

 protected:
  size_t getNumUnits() override {
    return getNumChildren();
  }

 private:
  friend class NvmeDisk;
  bool waitReady(bool ready);
  bool disable();
  void failController();
  bool command(NvmeQueue& queue, Nvme::Command command, void* buffer = nullptr, size_t bytes = 0,
               bool writing = false, uint32_t* result = nullptr, bool interruptProbe = false,
               bool cacheFill = false, physical_uintptr_t directWritePhysical = 0);
  bool readWrite(uint32_t nsid, uint64_t lba, uint32_t blocks, void* buffer, size_t bytes,
                 bool writing, bool cacheFill, physical_uintptr_t directWritePhysical = 0);
  bool createIoQueue(size_t index, uint16_t interruptVector);
  NvmeQueue& ioQueue();
  bool primaryControllerCapabilitiesLocked(Nvme::PrimaryControllerCapabilities& capabilities);
  bool secondaryControllersLocked(uint16_t firstControllerId,
                                  Nvme::SecondaryControllerList& controllers);
  bool discoverNamespaces(uint32_t maximumId, bool& activeNamespaces);
  NvmeDisk* findNamespace(uint32_t nsid);
  Device* m_Pci;
  IoBase* m_Registers;
  NvmeQueue m_Admin;
  NvmeQueue* m_Io[Nvme::MaxIoQueues];
  size_t m_IoCount;
  size_t m_IoProcessor[Nvme::MaxIoQueues];
  OperationBarrier m_Commands;
  Mutex m_WriteLock;
  Mutex m_ResetLock;
  Mutex m_VirtualizationLock;
  mutable Mutex m_IrqLock;
  size_t m_ReadyMilliseconds;
  size_t m_MaxTransfer;
  irq_id_t m_Irq;
  irq_id_t m_IoIrq[Nvme::MaxIoQueues];
  uint16_t m_OriginalCommand;
  uint16_t m_ControllerId;
  bool m_VirtualizationSupported;
  bool m_NamespaceManagementSupported;
  bool m_PciChanged;
  bool m_HardwareOwned;
  bool m_DmaInstalled;
  bool m_DmaIsolated;
  bool m_Interrupts;
  bool m_BatchInitialising;
  bool m_Failed;
  bool m_Stopping;
  bool m_Shutdown;
  char m_Model[41];
};
#endif
