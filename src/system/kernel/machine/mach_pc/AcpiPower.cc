/* Copyright (c) 2026, Pedigree Developers. */
#include "Acpi.h"

#if ACPI
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/utilities/utility.h"

#include "AcpiS5.h"

namespace {
uint16_t readControl(uint16_t port) {
  uint16_t value;
  asm volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
  return value;
}

void writeControl(uint16_t port, uint16_t value) {
  asm volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

void writeCommand(uint16_t port, uint8_t value) {
  asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}
}  // namespace

void Acpi::initialisePowerManagement() {
  const uint8_t* fadt = reinterpret_cast<const uint8_t*>(m_pFacp);
  if (m_pFacp->header.length >= 129 && (m_pFacp->flags & (1U << 10))) {
    uint64_t address = 0;
    MemoryCopy(&address, fadt + 120, sizeof(address));
    // FADT RESET_REG is a Generic Address Structure. The fixed register
    // permits an eight-bit write; other address spaces need their own accessors.
    if (fadt[116] == 1 && fadt[117] == 8 && fadt[118] == 0 && fadt[119] <= 1 && address &&
        address <= 0xffff) {
      m_ResetPort = address;
      m_ResetValue = fadt[128];
      NOTICE("ACPI: reset port " << Hex << m_ResetPort);
    }
  }

  // This path implements conventional PM1 I/O registers, not HW_REDUCED_ACPI.
  if ((m_pFacp->flags & (1U << 20)) || m_pFacp->pm1ControlLength < 2 ||
      !m_pFacp->pm1aControlBlock || m_pFacp->pm1aControlBlock > 0xfffe ||
      m_pFacp->pm1bControlBlock > 0xfffe)
    return;

  uint64_t dsdtAddress = m_pFacp->dsdt;
  if (m_pFacp->header.length >= 148) {
    uint64_t extendedAddress;
    MemoryCopy(&extendedAddress, fadt + 140, sizeof(extendedAddress));
    if (extendedAddress)
      dsdtAddress = extendedAddress;
  }
  if (!dsdtAddress || dsdtAddress > ~uintptr_t(0))
    return;

  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  const physical_uintptr_t page = dsdtAddress & ~(pageSize - 1);
  const size_t offset = dsdtAddress - page;
  const auto flags = PhysicalMemoryManager::continuous | PhysicalMemoryManager::nonRamMemory |
                     PhysicalMemoryManager::force;
  MemoryRegion dsdt("ACPI DSDT power data");
  auto& pmm = PhysicalMemoryManager::instance();
  if (!pmm.allocateRegion(dsdt,
                          (offset + sizeof(SystemDescriptionTableHeader) + pageSize - 1) / pageSize,
                          flags, VirtualAddressSpace::KernelMode, page))
    return;
  const auto* table = dsdt.convertPhysicalPointer<SystemDescriptionTableHeader>(dsdtAddress);
  const size_t length = table->length;
  if (table->signature != 0x54445344 || length < sizeof(*table) || length > 1024 * 1024)
    return;
  dsdt.free();
  if (!pmm.allocateRegion(dsdt, (offset + length + pageSize - 1) / pageSize, flags,
                          VirtualAddressSpace::KernelMode, page))
    return;
  table = dsdt.convertPhysicalPointer<SystemDescriptionTableHeader>(dsdtAddress);
  if (table->length != length || !checksum(table))
    return;
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(table);
  if (AcpiS5::mayHaveSleepHooks(bytes + sizeof(*table), length - sizeof(*table))) {
    WARNING("ACPI: firmware sleep methods require an interpreter; poweroff unavailable");
    return;
  }

  const size_t entries = (m_pRsdt->length - sizeof(*m_pRsdt)) / sizeof(uint32_t);
  const uint8_t* entry = reinterpret_cast<const uint8_t*>(m_pRsdt) + sizeof(*m_pRsdt);
  for (size_t i = 0; i < entries; ++i) {
    uint32_t address;
    MemoryCopy(&address, entry + i * sizeof(address), sizeof(address));
    const uint64_t base = m_AcpiMemoryRegion.physicalAddress();
    const uint64_t regionSize = m_AcpiMemoryRegion.size();
    if (address < base || address - base > regionSize ||
        regionSize - (address - base) < sizeof(SystemDescriptionTableHeader))
      return;
    const auto* secondary =
        m_AcpiMemoryRegion.convertPhysicalPointer<SystemDescriptionTableHeader>(address);
    if (secondary->signature != 0x54445353 && secondary->signature != 0x54445350)
      continue;
    if (secondary->length < sizeof(*secondary) ||
        secondary->length > regionSize - (address - base) || !checksum(secondary))
      return;
    const auto* aml = reinterpret_cast<const uint8_t*>(secondary) + sizeof(*secondary);
    if (AcpiS5::mayHaveSleepHooks(aml, secondary->length - sizeof(*secondary))) {
      WARNING(
          "ACPI: secondary firmware sleep methods require an interpreter; poweroff unavailable");
      return;
    }
  }
  m_PowerOffValid =
      AcpiS5::find(bytes + sizeof(*table), bytes + length, m_SleepTypeA, m_SleepTypeB);
  if (m_PowerOffValid) {
    NOTICE("ACPI: static S5 sleep types " << Dec << m_SleepTypeA << ", " << m_SleepTypeB);
  } else {
    WARNING("ACPI: no supported static root _S5 package; poweroff unavailable");
  }
}

void Acpi::reset() {
  if (!m_ResetPort)
    return;
  writeCommand(m_ResetPort, m_ResetValue);
  for (size_t i = 0; i < 100000; ++i)
    asm volatile("pause");
}

bool Acpi::supportsPowerOff() const {
  return m_PowerOffValid && ((readControl(m_pFacp->pm1aControlBlock) & 1) ||
                             (m_pFacp->smiCommandPort && m_pFacp->smiCommandPort <= 0xffff &&
                              m_pFacp->acpiEnableCommand));
}

void Acpi::powerOff() {
  if (!m_PowerOffValid)
    return;
  const uint16_t portA = m_pFacp->pm1aControlBlock;
  const uint16_t portB = m_pFacp->pm1bControlBlock;
  if (!(readControl(portA) & 1)) {
    if (!m_pFacp->smiCommandPort || m_pFacp->smiCommandPort > 0xffff || !m_pFacp->acpiEnableCommand)
      return;
    // SCI interrupts remain disabled during this terminal transition. Do not
    // enable ACPI mode during normal operation without an SCI handler.
    writeCommand(m_pFacp->smiCommandPort, m_pFacp->acpiEnableCommand);
    size_t attempts = 1000000;
    while (!(readControl(portA) & 1) && --attempts)
      asm volatile("pause");
    if (!attempts) {
      ERROR_NOLOCK("ACPI: timed out enabling ACPI mode for power off");
      return;
    }
  }

  const uint16_t sleepMask = (7U << 10) | (1U << 13);
  const uint16_t controlA = (readControl(portA) & ~sleepMask) | (m_SleepTypeA << 10);
  const uint16_t controlB = portB ? (readControl(portB) & ~sleepMask) | (m_SleepTypeB << 10) : 0;
  // Program both sleep types before either SLP_EN write starts the transition.
  writeControl(portA, controlA);
  if (portB)
    writeControl(portB, controlB);
  asm volatile("wbinvd" : : : "memory");
  writeControl(portA, controlA | (1U << 13));
  if (portB)
    writeControl(portB, controlB | (1U << 13));
  for (size_t i = 0; i < 1000000; ++i)
    asm volatile("pause");
}
#endif
