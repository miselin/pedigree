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
#ifndef AHCI_PORT_H
#define AHCI_PORT_H
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/processor/MemoryRegion.h"
#include "pedigree/kernel/processor/types.h"
class IoBase;
class AhciPort {
 public:
  AhciPort(IoBase* registers, size_t port);
  ~AhciPort();
  bool initialise(uint32_t capabilities, uint32_t version, uint32_t extendedCapabilities);
  void shutdown();
  void enableInterrupts();
  bool interrupt(bool pending);
  bool command(uint8_t opcode, uint64_t lba, uint16_t sectors, void* buffer, size_t bytes,
               bool write, bool interrupts, bool interruptProbe = false);
  bool readBatch(Disk::ReadBuffer* buffers, size_t count, bool interrupts);
  bool writeBatch(Disk::WriteBuffer* buffers, size_t count, bool interrupts);
  size_t interruptCompletions() const;
  size_t maximumOutstanding() const;
  void configureDisk(size_t sectorBytes, size_t queueDepth);
  size_t sectorBytes() const {
    return m_SectorBytes;
  }

 private:
  bool transferBatch(Disk::ReadBuffer* buffers, size_t count, bool interrupts, bool writing);
  uint32_t read(size_t reg) const;
  void write(size_t reg, uint32_t value);
  void waitForProgress();
  bool waitClear(size_t reg, uint32_t bits, size_t milliseconds);
  bool stopEngines();
  void acknowledge(uint32_t status);
  void observe(uint32_t status, bool fromInterrupt);
  void pollCompletions(bool interrupts);

  bool chooseSlot(bool queued, size_t& index);
  bool issueCommand(size_t index, uint8_t opcode, uint64_t lba, uint16_t sectors, void* buffer,
                    size_t bytes, bool writing, bool queued, bool interrupts);
  bool reapCommand(size_t index, uint8_t opcode, void* buffer, size_t bytes, bool writing,
                   bool queued, bool interrupts, bool interruptProbe);

  IoBase* m_Registers;
  size_t m_Port;
  MemoryRegion m_Control;
  struct Slot {
    Slot()
        : data("AHCI transfer buffer"), completion(0, false), done(false), errors(0), deadline(0) {}
    MemoryRegion data;
    physical_uintptr_t pages[16];
    Semaphore completion;
    bool done;
    uint32_t errors;
    uint64_t deadline;
  };
  Slot m_Slots[32];
  Mutex m_CommandLock;
  mutable Mutex m_StateLock;
  bool m_Online;
  // Protected by m_CommandLock, shared by every writer on this device.
  bool m_WritesPending;
  uint32_t m_Active;
  uint32_t m_Queued;
  size_t m_SlotCount;
  size_t m_QueueDepth;
  size_t m_SectorBytes;
  bool m_SupportsNcq;
  bool m_AddressesInstalled;
  bool m_PolledInterrupt;
  size_t m_InterruptCompletions;
  size_t m_MaximumOutstanding;
  size_t m_Outstanding;
};
#endif
