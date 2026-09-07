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
      m_Data("AHCI transfer buffer"),
      m_Completion(0, false),
      m_Online(false),
      m_Active(false),
      m_Done(false),
      m_AddressesInstalled(false),
      m_Errors(0),
      m_InterruptCompletions(0) {}
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
  if (pageSize < TableOffset + sizeof(CommandTable) || pageSize > MaxTransfer ||
      (MaxTransfer % pageSize))
    return false;
  auto& memory = PhysicalMemoryManager::instance();
  const size_t constraints = PhysicalMemoryManager::continuous | PhysicalMemoryManager::below4GB;
  const size_t flags = VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write;
  if (!memory.allocateRegion(m_Control, 1, constraints, flags) ||
      !memory.allocateRegion(m_Data, MaxTransfer / pageSize, constraints, flags))
    return false;
  if (m_Control.size() < TableOffset + sizeof(CommandTable) || m_Data.size() < MaxTransfer)
    return false;
  ByteSet(m_Control.virtualAddress(), 0, m_Control.size());
  ByteSet(m_Data.virtualAddress(), 0, m_Data.size());
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
void AhciPort::observe(uint32_t status, bool fromInterrupt) {
  if (!m_Active)
    return;
  m_Errors |= status & PortErrors;
  if (read(Tfd) & (TaskError | DeviceFault))
    m_Errors |= TaskFileError;
  if ((read(Ssts) & 15U) != 3U)
    m_Errors |= 1U << 22;
  if (!m_Done && (m_Errors || !(read(Ci) & 1U))) {
    m_Done = true;
    if (fromInterrupt)
      ++m_InterruptCompletions;
    m_Completion.release();
  }
}
bool AhciPort::interrupt() {
  LockGuard<Mutex> state(m_StateLock);
  const uint32_t status = read(PortIs);
  if (!status)
    return false;
  observe(status, true);
  acknowledge(status);
  return true;
}
bool AhciPort::command(uint8_t opcode, uint64_t lba, uint16_t sectors, void* buffer, size_t bytes,
                       bool writing, bool interrupts) {
  if (bytes > MaxTransfer || (bytes && (!buffer || (bytes & 1U))) || (lba >> 48))
    return false;
  LockGuard<Mutex> command(m_CommandLock);
  auto* header = static_cast<CommandHeader*>(m_Control.virtualAddress());
  auto* table = reinterpret_cast<CommandTable*>(reinterpret_cast<uintptr_t>(header) + TableOffset);
  {
    LockGuard<Mutex> state(m_StateLock);
    if (!m_Online || m_Active || read(Ci) || read(Sact) || (read(Tfd) & (Busy | DataRequest)))
      return false;
    ByteSet(header, 0, sizeof(*header));
    ByteSet(table, 0, sizeof(*table));
    if (writing && bytes)
      MemoryCopy(m_Data.virtualAddress(), buffer, bytes);
    header->flags = 5U | (writing ? 1U << 6 : 0U) | (bytes ? 1U << 16 : 0U);
    header->table = static_cast<uint32_t>(m_Control.physicalAddress() + TableOffset);
    // SATA 2.5 section 10.3.4: Register H2D FIS, command update, direct device.
    table->fis[0] = 0x27;
    table->fis[1] = 0x80;
    table->fis[2] = opcode;
    if (opcode == 0x25 || opcode == 0x35)
      table->fis[7] = 0x40;  // ATA LBA addressing.
    for (size_t i = 0; i < 3; ++i) {
      table->fis[4 + i] = static_cast<uint8_t>(lba >> (i * 8));
      table->fis[8 + i] = static_cast<uint8_t>(lba >> ((i + 3) * 8));
    }
    table->fis[12] = static_cast<uint8_t>(sectors);
    table->fis[13] = static_cast<uint8_t>(sectors >> 8);
    if (bytes) {
      table->data.address = static_cast<uint32_t>(m_Data.physicalAddress());
      table->data.byteCount = static_cast<uint32_t>(bytes - 1);
    }
    acknowledge(read(PortIs));
    [[maybe_unused]] const size_t drained = m_Completion.drainAvailable();
    m_Errors = 0;
    m_Done = false;
    m_Active = true;
    FENCE();
    write(Ci, 1U);  // PxCI is write-one-to-set; zero cannot cancel a command.
    (void)read(Ci);
  }
  // A drive may spend longer flushing persistent media than transferring data.
  const size_t timeoutSeconds = (opcode == 0xe7 || opcode == 0xea) ? 120 : 30;
  const auto deadline = Time::getTicks() + timeoutSeconds * Time::Multiplier::Second;
  bool success = false;
  uint32_t failedStatus = 0, failedCi = 0, failedTfd = 0;
  for (;;) {
    bool waitExpired = false;
    if (interrupts) {
      // Let the threaded INTx handler claim completion before the lost-IRQ fallback.
      waitExpired = !m_Completion.acquireForCompletion(1, 0, 10000);
    }
    {
      LockGuard<Mutex> state(m_StateLock);
      if (!m_Done && (!interrupts || waitExpired)) {
        const uint32_t status = read(PortIs);
        observe(status, false);
        acknowledge(status);
      }
      if (m_Done || Time::getTicks() >= deadline) {
        FENCE();
        success = m_Done && !m_Errors && !(read(Ci) & 1U) && header->transferred == bytes;
        if (success && !writing && bytes)
          MemoryCopy(buffer, m_Data.virtualAddress(), bytes);
        failedStatus = m_Errors;
        failedCi = read(Ci);
        failedTfd = read(Tfd);
        m_Active = false;
        if (!success) {
          m_Online = false;
          write(PortIe, 0);
        }
        break;
      }
    }
    if (!interrupts)
      Time::delay(Time::Multiplier::Millisecond);
  }
  if (!success) {
    ERROR("AHCI: port " << m_Port << " command " << Hex << opcode << " failed, CI=" << failedCi
                        << " TFD=" << failedTfd << " errors=" << failedStatus);
    // CI also clears on stopping/reset: that must never be mistaken for success.
    // Only persistent bounce storage is exposed to DMA, never the caller buffer.
    if (!stopEngines())
      panic("AHCI: cannot stop failed port DMA; refusing to release memory");
  }
  return success;
}
void AhciPort::shutdown() {
  LockGuard<Mutex> command(m_CommandLock);
  if (!m_AddressesInstalled)
    return;
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
