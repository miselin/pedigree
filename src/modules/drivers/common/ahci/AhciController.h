/*
 * Copyright (c) 2026, Pedigree Developers
 * SPDX-License-Identifier: ISC
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#ifndef AHCI_CONTROLLER_H
#define AHCI_CONTROLLER_H
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/machine/IrqHandler.h"
#include "pedigree/kernel/process/Mutex.h"

#include "modules/drivers/common/scsi/ScsiController.h"
class AhciPort;
class IoBase;
class AhciController : public ScsiController, public IrqHandler {
 public:
  explicit AhciController(Device* pci);
  ~AhciController() override;
  bool initialiseController();
  bool supportsConcurrentReads() const override {
    return true;
  }
  void shutdown();
  bool identify(size_t port, uint16_t* words, bool interruptProbe = false);
  EXPORTED_PUBLIC bool readWrite(size_t port, uint64_t lba, uint16_t sectors, void* buffer,
                                 size_t bytes, bool write);
  bool readBatch(size_t port, Disk::ReadBuffer* buffers, size_t count);
  bool flush(size_t port, bool extended);
  void configureDisk(size_t port, size_t sectorBytes, size_t queueDepth);
  EXPORTED_PUBLIC size_t interruptCompletions() const;
  EXPORTED_PUBLIC size_t maximumOutstanding(size_t port) const;
  IrqDisposition irq(irq_id_t number) override;
  bool sendCommand(size_t, uintptr_t, uint8_t, uintptr_t, uint16_t, bool) override {
    return false;
  }

 protected:
  size_t getNumUnits() override {
    return getNumChildren();
  }

 private:
  bool claimOwnership(uint32_t version);
  bool reset();
  Device* m_Pci;
  IoBase* m_Registers;
  AhciPort* m_Ports[32];
  mutable Mutex m_IrqLock;
  irq_id_t m_Irq;
  uint32_t m_Implemented;
  uint16_t m_OriginalCommand;
  bool m_PciChanged;
  bool m_HardwareOwned;
  bool m_Interrupts;
  bool m_Stopping;
  bool m_Shutdown;
};
#endif
