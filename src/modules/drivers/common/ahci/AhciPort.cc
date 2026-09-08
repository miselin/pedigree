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
#include "AhciPort.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/processor/IoBase.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/utility.h"

#include "Registers.h"

using namespace Ahci;

AhciPort::AhciPort(IoBase* registers, size_t port)
    : m_Registers(registers),
      m_Port(port),
      m_Control("AHCI command storage"),
      m_Online(false),
      m_Active(0),
      m_Queued(0),
      m_SlotCount(1),
      m_QueueDepth(0),
      m_SectorBytes(512),
      m_SupportsNcq(false),
      m_AddressesInstalled(false),
      m_PolledInterrupt(false),
      m_InterruptCompletions(0),
      m_MaximumOutstanding(0),
      m_Outstanding(0) {}
AhciPort::~AhciPort() {
  shutdown();
}
uint32_t AhciPort::read(size_t reg) const {
  return m_Registers->read32(PortBase + m_Port * PortStride + reg);
}
void AhciPort::write(size_t reg, uint32_t value) {
  m_Registers->write32(value, PortBase + m_Port * PortStride + reg);
}
bool AhciPort::waitClear(size_t reg, uint32_t bits, size_t milliseconds) {
  const auto deadline = Time::getTicks() + milliseconds * Time::Multiplier::Millisecond;
  do {
    if (!(read(reg) & bits))
      return true;
    Time::delay(Time::Multiplier::Millisecond);
  } while (Time::getTicks() < deadline);
  return !(read(reg) & bits);
}
bool AhciPort::stopEngines() {
  // AHCI 10.3: FRE must remain set until the command-list engine has stopped.
  write(Cmd, read(Cmd) & ~Start);
  if (!waitClear(Cmd, CommandRunning, 500))
    return false;
  write(Cmd, read(Cmd) & ~FisEnable);
  return waitClear(Cmd, FisRunning, 500);
}
void AhciPort::acknowledge(uint32_t status) {
  // Some port status bits are backed by SError diagnostic bits.
  const uint32_t error = read(Serr);
  if (error)
    write(Serr, error);
  if (status)
    write(PortIs, status);
}
bool AhciPort::initialise(uint32_t capabilities, uint32_t version, uint32_t extendedCapabilities) {
  write(PortIe, 0);
  if (!stopEngines()) {
    ERROR("AHCI: port " << m_Port << " firmware engines did not stop");
    return false;
  }
  const size_t pageSize = TargetInfo::getPageSize();
  m_SlotCount = ((capabilities >> 8) & 31U) + 1;
  m_SupportsNcq = capabilities & (1U << 30);
  const size_t controlBytes = TableOffset + m_SlotCount * TableStride;
  if (pageSize < 4096 || pageSize > MaxTransfer || (MaxTransfer % pageSize))
    return false;
  auto& memory = PhysicalMemoryManager::instance();
  const size_t constraints = PhysicalMemoryManager::continuous | PhysicalMemoryManager::below4GB;
  const size_t flags = VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write;
  if (!memory.allocateRegion(m_Control, (controlBytes + pageSize - 1) / pageSize, constraints,
                             flags))
    return false;
  ByteSet(m_Control.virtualAddress(), 0, m_Control.size());
  for (size_t i = 0; i < m_SlotCount; ++i) {
    // Contiguous allocations currently consume the scarce ISA DMA pool.
    // PRDs allow each bounce page to come from ordinary memory below 4 GiB.
    if (!memory.allocateRegion(m_Slots[i].data, MaxTransfer / pageSize,
                               PhysicalMemoryManager::below4GB, flags))
      return false;
    ByteSet(m_Slots[i].data.virtualAddress(), 0, m_Slots[i].data.size());
    for (size_t page = 0; page < MaxTransfer / pageSize; ++page) {
      size_t mappingFlags = 0;
      VirtualAddressSpace::getKernelAddressSpace().getMapping(
          static_cast<uint8_t*>(m_Slots[i].data.virtualAddress()) + page * pageSize,
          m_Slots[i].pages[page], mappingFlags);
      if (m_Slots[i].pages[page] >= (uint64_t{1} << 32))
        return false;
    }
  }
  FENCE();
  write(Clb, static_cast<uint32_t>(m_Control.physicalAddress()));
  write(Clbu, 0);
  write(Fb, static_cast<uint32_t>(m_Control.physicalAddress() + FisOffset));
  write(Fbu, 0);
  m_AddressesInstalled = true;

  uint32_t cmd = read(Cmd) & ~(Atapi | AggressivePower | IccMask);
  if (cmd & ColdPresence)
    cmd |= PowerOn;
  if (capabilities & StaggeredSpinup)
    cmd |= Spinup;
  // Disable automatic device sleep only when its register is implemented.
  if (version >= 0x00010300 && (extendedCapabilities & (1U << 3)))
    write(Devslp, read(Devslp) & ~1U);
  write(Cmd, cmd | FisEnable);
  uint32_t control = read(Sctl) & 0xf0U;  // Retain firmware's speed restriction.
  control |= (version >= 0x00010300 ? 7U : 3U) << 8;
  write(Serr, 0xffffffffU);
  write(Sctl, control | 1U);
  const auto resetUntil = Time::getTicks() + Time::Multiplier::Millisecond;
  while (Time::getTicks() < resetUntil)
    Time::delay(Time::Multiplier::Millisecond);
  write(Sctl, control);
  const auto linkDeadline = Time::getTicks() + Time::Multiplier::Second;
  while ((read(Ssts) & 15U) != 3U && Time::getTicks() < linkDeadline)
    Time::delay(Time::Multiplier::Millisecond);
  write(Serr, 0xffffffffU);
  if ((read(Ssts) & 15U) != 3U)
    return false;
  write(Cmd, (read(Cmd) & ~IccMask) | IccActive);
  if (!waitClear(Tfd, Busy | DataRequest, 30000)) {
    WARNING("AHCI: port " << m_Port << " device not ready after COMRESET");
    return false;
  }
  if (read(Sig) != SataDisk) {
    NOTICE("AHCI: port " << m_Port << " unsupported signature " << Hex << read(Sig));
    return false;
  }
  if ((read(Cmd) & CommandRunning) || read(Ci) || read(Sact))
    return false;
  acknowledge(read(PortIs));
  write(Cmd, read(Cmd) | Start);
  (void)read(Cmd);
  m_Online = true;
  return true;
}
void AhciPort::enableInterrupts() {
  LockGuard<Mutex> state(m_StateLock);
  acknowledge(read(PortIs));
  if (m_Online)
    write(PortIe, PortInterrupts);
}
void AhciPort::configureDisk(size_t sectorBytes, size_t queueDepth) {
  m_SectorBytes = sectorBytes;
  m_QueueDepth = m_SupportsNcq ? (queueDepth < m_SlotCount ? queueDepth : m_SlotCount) : 0;
  NOTICE("AHCI: port " << m_Port << " NCQ depth " << Dec << m_QueueDepth << Hex);
}
void AhciPort::observe(uint32_t status, bool fromInterrupt) {
  if (!m_Active)
    return;
  uint32_t errors = status & PortErrors;
  if (read(Tfd) & (TaskError | DeviceFault))
    errors |= TaskFileError;
  if ((read(Ssts) & 15U) != 3U)
    errors |= 1U << 22;
  // Without READ LOG EXT attribution, no outstanding tag can be trusted after
  // an NCQ error. Stop admission before any slot is retired or reused.
  if (errors) {
    m_Online = false;
    write(PortIe, 0);
  }
  const uint32_t pending = (read(Sact) & m_Queued) | (read(Ci) & ~m_Queued);
  for (size_t i = 0; i < m_SlotCount; ++i) {
    if (!(m_Active & (1U << i)))
      continue;
    Slot& slot = m_Slots[i];
    slot.errors |= errors;
    if (!slot.done && (slot.errors || !(pending & (1U << i)))) {
      slot.done = true;
      --m_Outstanding;
      if (fromInterrupt)
        ++m_InterruptCompletions;
      slot.completion.release();
    }
  }
}
void AhciPort::pollCompletions(bool interrupts) {
  const uint32_t status = read(PortIs);
  // Polling can withdraw INTx before its already-dispatched IRQ worker runs.
  // Capture enabled causes before observe() can disable a failed port's IRQs.
  if (interrupts && (status & read(PortIe)))
    m_PolledInterrupt = true;
  observe(status, false);
  acknowledge(status);
}
bool AhciPort::interrupt(bool pending) {
  LockGuard<Mutex> state(m_StateLock);
  const bool credited = m_PolledInterrupt;
  m_PolledInterrupt = false;
  const uint32_t status = pending ? read(PortIs) : 0;
  if (!status)
    return credited;
  observe(status, true);
  acknowledge(status);
  return true;
}
bool AhciPort::command(uint8_t opcode, uint64_t lba, uint16_t sectors, void* buffer, size_t bytes,
                       bool writing, bool interrupts) {
  if (bytes > MaxTransfer || (bytes && (!buffer || (bytes & 1U))) || (lba >> 48))
    return false;
  TerminationDeferral lifetime;
  LockGuard<Mutex> command(m_CommandLock);
  const bool queued = m_QueueDepth && (opcode == 0x25 || opcode == 0x35);
  if (queued)
    opcode = writing ? 0x61 : 0x60;
  // The admission lock prevents new queued reads from overtaking a flush.
  const auto admissionDeadline = Time::getTicks() + 120 * Time::Multiplier::Second;
  size_t index = 32;
  for (;;) {
    {
      LockGuard<Mutex> state(m_StateLock);
      if (!m_Online)
        return false;
      if (queued || !m_Active) {
        const size_t count = queued ? m_QueueDepth : 1;
        for (size_t i = 0; i < count; ++i) {
          if (!(m_Active & (1U << i))) {
            index = i;
            break;
          }
        }
      }
    }
    if (index != 32)
      break;
    if (Time::getTicks() >= admissionDeadline)
      return false;
    Time::delay(Time::Multiplier::Millisecond);
  }
  Slot& slot = m_Slots[index];
  const uint32_t mask = 1U << index;
  auto* header = static_cast<CommandHeader*>(m_Control.virtualAddress()) + index;
  const size_t tableOffset = TableOffset + index * TableStride;
  auto* table = reinterpret_cast<CommandTable*>(
      reinterpret_cast<uintptr_t>(m_Control.virtualAddress()) + tableOffset);
  {
    LockGuard<Mutex> state(m_StateLock);
    if (!m_Online || (!queued && (read(Ci) || read(Sact) || (read(Tfd) & (Busy | DataRequest)))))
      return false;
    ByteSet(header, 0, sizeof(*header));
    ByteSet(table, 0, sizeof(*table));
    if (writing && bytes)
      MemoryCopy(slot.data.virtualAddress(), buffer, bytes);
    const size_t pageSize = TargetInfo::getPageSize();
    const size_t prds = (bytes + pageSize - 1) / pageSize;
    header->flags = 5U | (writing ? 1U << 6 : 0U) | (prds << 16);
    header->table = static_cast<uint32_t>(m_Control.physicalAddress() + tableOffset);
    table->fis[0] = 0x27;
    table->fis[1] = 0x80;
    table->fis[2] = opcode;
    if (queued || opcode == 0x25 || opcode == 0x35)
      table->fis[7] = 0x40;
    for (size_t i = 0; i < 3; ++i) {
      table->fis[4 + i] = static_cast<uint8_t>(lba >> (i * 8));
      table->fis[8 + i] = static_cast<uint8_t>(lba >> ((i + 3) * 8));
    }
    if (queued) {
      table->fis[3] = static_cast<uint8_t>(sectors);
      table->fis[11] = static_cast<uint8_t>(sectors >> 8);
      table->fis[12] = static_cast<uint8_t>(index << 3);
    } else {
      table->fis[12] = static_cast<uint8_t>(sectors);
      table->fis[13] = static_cast<uint8_t>(sectors >> 8);
    }
    for (size_t page = 0; page < prds; ++page) {
      const size_t remaining = bytes - page * pageSize;
      const size_t count = remaining < pageSize ? remaining : pageSize;
      table->data[page].address = static_cast<uint32_t>(slot.pages[page]);
      table->data[page].byteCount = static_cast<uint32_t>(count - 1);
    }
    // Observe previous commands before acknowledging shared port status.
    pollCompletions(interrupts);
    if (!m_Online)
      return false;
    [[maybe_unused]] const size_t drained = slot.completion.drainAvailable();
    slot.errors = 0;
    slot.done = false;
    m_Active |= mask;
    ++m_Outstanding;
    if (m_Outstanding > m_MaximumOutstanding)
      m_MaximumOutstanding = m_Outstanding;
    if (queued)
      m_Queued |= mask;
    FENCE();
    if (queued)
      write(Sact, mask);
    write(Ci, mask);
    (void)read(Ci);
  }
  if (queued) {
    m_CommandLock.release();
    command.disown();
  }
  const size_t timeoutSeconds = (opcode == 0xe7 || opcode == 0xea) ? 120 : 30;
  const auto deadline = Time::getTicks() + timeoutSeconds * Time::Multiplier::Second;
  bool success = false;
  for (;;) {
    const bool waitExpired = interrupts && !slot.completion.acquireForCompletion(1, 0, 10000);
    {
      LockGuard<Mutex> state(m_StateLock);
      if (!slot.done && (!interrupts || waitExpired)) {
        pollCompletions(interrupts);
      }
      if (slot.done || Time::getTicks() >= deadline) {
        FENCE();
        // AHCI 5.4.1: PRDBC is not defined for native queued commands.
        success = slot.done && !slot.errors && m_Online && !(read(queued ? Sact : Ci) & mask) &&
                  (queued || header->transferred == bytes);
        if (!success) {
          ERROR("AHCI: port " << m_Port << " command " << Hex << opcode << " tag " << index
                              << " failed, CI=" << read(Ci) << " SACT=" << read(Sact)
                              << " TFD=" << read(Tfd) << " errors=" << slot.errors);
          m_Online = false;
          write(PortIe, 0);
          // Mark all owners failed before stopping engines clears hardware bits.
          for (size_t i = 0; i < m_SlotCount; ++i) {
            if (m_Active & (1U << i)) {
              m_Slots[i].errors |= TaskFileError;
              if (!m_Slots[i].done) {
                m_Slots[i].done = true;
                --m_Outstanding;
                m_Slots[i].completion.release();
              }
            }
          }
          if (!stopEngines())
            panic("AHCI: cannot stop failed port DMA; refusing to release memory");
        } else if (!writing && bytes) {
          MemoryCopy(buffer, slot.data.virtualAddress(), bytes);
        }
        m_Active &= ~mask;
        m_Queued &= ~mask;
        break;
      }
    }
    if (!interrupts)
      Time::delay(Time::Multiplier::Millisecond);
  }
  return success;
}
void AhciPort::shutdown() {
  LockGuard<Mutex> command(m_CommandLock);
  if (!m_AddressesInstalled)
    return;
  const auto deadline = Time::getTicks() + 120 * Time::Multiplier::Second;
  for (;;) {
    {
      LockGuard<Mutex> state(m_StateLock);
      if (!m_Active)
        break;
    }
    if (Time::getTicks() >= deadline)
      panic("AHCI: command owners did not drain during shutdown");
    Time::delay(Time::Multiplier::Millisecond);
  }
  {
    LockGuard<Mutex> state(m_StateLock);
    m_Online = false;
    write(PortIe, 0);
  }
  if (!stopEngines())
    panic("AHCI: cannot stop port DMA during shutdown");
  write(Clb, 0);
  write(Clbu, 0);
  write(Fb, 0);
  write(Fbu, 0);
  acknowledge(read(PortIs));
  (void)read(Cmd);
  m_AddressesInstalled = false;
}
size_t AhciPort::interruptCompletions() const {
  LockGuard<Mutex> state(m_StateLock);
  return m_InterruptCompletions;
}

size_t AhciPort::maximumOutstanding() const {
  LockGuard<Mutex> state(m_StateLock);
  return m_MaximumOutstanding;
}
