/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "pedigree/kernel/BootstrapInfo.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/Pci.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/processor/MemoryRegion.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/new"

#include <uacpi/kernel_api.h>

namespace {
struct Mapping {
  MemoryRegion region{"ACPI firmware"};
  void* address = nullptr;
  size_t bytes = 0;
  Mapping* next = nullptr;
};
Mutex mappingsLock;
Mapping* mappings = nullptr;

struct PortRange {
  uint16_t base;
  size_t bytes;
};

bool wait(Semaphore* semaphore, uint16_t milliseconds) {
  if (!milliseconds) {
    return semaphore->tryAcquire();
  }
  if (milliseconds == 0xffff) {
    return semaphore->acquireForCompletion();
  }
  return semaphore->acquireForCompletion(1, milliseconds / 1000, (milliseconds % 1000) * 1000);
}

uacpi_status status(bool success) {
  return success ? UACPI_STATUS_OK : UACPI_STATUS_INTERNAL_ERROR;
}

template <class T>
uacpi_status readPort(uacpi_handle handle, size_t offset, T* value) {
  const auto* range = static_cast<const PortRange*>(handle);
  if (!range || !value || offset >= range->bytes || sizeof(T) > range->bytes - offset) {
    return UACPI_STATUS_INVALID_ARGUMENT;
  }
#if X86 || X64
  const uint16_t port = range->base + offset;
  if constexpr (sizeof(T) == 1) {
    asm volatile("inb %1, %0" : "=a"(*value) : "Nd"(port));
  } else if constexpr (sizeof(T) == 2) {
    asm volatile("inw %1, %0" : "=a"(*value) : "Nd"(port));
  } else {
    asm volatile("inl %1, %0" : "=a"(*value) : "Nd"(port));
  }
  return UACPI_STATUS_OK;
#else
  return UACPI_STATUS_UNIMPLEMENTED;
#endif
}

template <class T>
uacpi_status writePort(uacpi_handle handle, size_t offset, T value) {
  const auto* range = static_cast<const PortRange*>(handle);
  if (!range || offset >= range->bytes || sizeof(T) > range->bytes - offset) {
    return UACPI_STATUS_INVALID_ARGUMENT;
  }
#if X86 || X64
  const uint16_t port = range->base + offset;
  if constexpr (sizeof(T) == 1) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
  } else if constexpr (sizeof(T) == 2) {
    asm volatile("outw %0, %1" : : "a"(value), "Nd"(port));
  } else {
    asm volatile("outl %0, %1" : : "a"(value), "Nd"(port));
  }
  return UACPI_STATUS_OK;
#else
  return UACPI_STATUS_UNIMPLEMENTED;
#endif
}
}  // namespace

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr* result) {
  const uintptr_t address = g_pBootstrapInfo ? g_pBootstrapInfo->getAcpiRsdp() : 0;
  if (!address || !result) {
    return UACPI_STATUS_NOT_FOUND;
  }
#if X64
  // The UEFI loader supplies a direct-map alias, backed by large pages that
  // the ordinary page-mapping lookup intentionally does not expose.
  constexpr uintptr_t DirectMapBase = 0xffff800000000000ULL;
  constexpr uintptr_t DirectMapEnd = 0xffffc00000000000ULL;
  if (address >= DirectMapBase && address < DirectMapEnd) {
    *result = address - DirectMapBase;
    return UACPI_STATUS_OK;
  }
#endif
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  physical_uintptr_t physical = 0;
  size_t flags = 0;
  if (!VirtualAddressSpace::getKernelAddressSpace().getMapping(
          reinterpret_cast<void*>(address & ~(pageSize - 1)), physical, flags)) {
    return UACPI_STATUS_MAPPING_FAILED;
  }
  *result = physical + (address & (pageSize - 1));
  return UACPI_STATUS_OK;
}

void* uacpi_kernel_map(uacpi_phys_addr address, uacpi_size bytes) {
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  const size_t offset = address & (pageSize - 1);
  if (!bytes || address > ~physical_uintptr_t{0} || bytes - 1 > ~physical_uintptr_t{0} - address ||
      bytes > ~size_t{0} - offset - (pageSize - 1)) {
    return UACPI_MAP_FAILED;
  }
  auto* mapping = new Mapping;
  size_t flags = VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write |
                 VirtualAddressSpace::CacheDisable;
  if (g_pBootstrapInfo) {
    for (void* entry = g_pBootstrapInfo->getMemoryMap(); entry;
         entry = g_pBootstrapInfo->nextMemoryMapEntry(entry)) {
      const uint64_t base = g_pBootstrapInfo->getMemoryMapEntryAddress(entry);
      const uint64_t length = g_pBootstrapInfo->getMemoryMapEntryLength(entry);
      const uint32_t type = g_pBootstrapInfo->getMemoryMapEntryType(entry);
      if ((type == 1 || type == 3 || type == 4) && address >= base && address - base < length &&
          bytes <= length - (address - base)) {
        // RAM-backed firmware tables share the direct map's cache policy.
        flags &= ~VirtualAddressSpace::CacheDisable;
        break;
      }
    }
  }
  if (!PhysicalMemoryManager::instance().allocateRegion(
          mapping->region, (bytes + offset + pageSize - 1) / pageSize,
          PhysicalMemoryManager::continuous | PhysicalMemoryManager::nonRamMemory |
              PhysicalMemoryManager::force,
          flags, address & ~(physical_uintptr_t(pageSize) - 1))) {
    delete mapping;
    return UACPI_MAP_FAILED;
  }
  mapping->address = static_cast<uint8_t*>(mapping->region.virtualAddress()) + offset;
  mapping->bytes = bytes;
  LockGuard<Mutex> guard(mappingsLock);
  mapping->next = mappings;
  mappings = mapping;
  return mapping->address;
}

void uacpi_kernel_unmap(void* address, uacpi_size bytes) {
  Mapping* found = nullptr;
  {
    LockGuard<Mutex> guard(mappingsLock);
    for (Mapping** cursor = &mappings; *cursor; cursor = &(*cursor)->next) {
      if ((*cursor)->address == address && (*cursor)->bytes == bytes) {
        found = *cursor;
        *cursor = found->next;
        break;
      }
    }
  }
  if (!found) {
    panic("ACPI: invalid firmware mapping retirement");
  }
  delete found;
}

void uacpi_kernel_log(uacpi_log_level level, const uacpi_char* message) {
  if (level <= UACPI_LOG_WARN) {
    WARNING("ACPI: " << message);
  } else {
    NOTICE("ACPI: " << message);
  }
}

uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address address, uacpi_handle* result) {
  if (!result || address.segment || address.device >= 32 || address.function >= 8) {
    return UACPI_STATUS_NOT_FOUND;
  }
  auto* device = new Device;
  device->setPciPosition(address.bus, address.device, address.function);
  *result = device;
  return UACPI_STATUS_OK;
}
void uacpi_kernel_pci_device_close(uacpi_handle device) {
  delete static_cast<Device*>(device);
}

uacpi_status uacpi_kernel_pci_read8(uacpi_handle device, uacpi_size offset, uacpi_u8* value) {
  return status(device && value && offset < 4096 &&
                PciBus::instance().readConfig8(static_cast<Device*>(device), offset, *value));
}
uacpi_status uacpi_kernel_pci_read16(uacpi_handle device, uacpi_size offset, uacpi_u16* value) {
  return status(device && value && offset < 4096 &&
                PciBus::instance().readConfig16(static_cast<Device*>(device), offset, *value));
}
uacpi_status uacpi_kernel_pci_read32(uacpi_handle device, uacpi_size offset, uacpi_u32* value) {
  return status(device && value && offset < 4096 &&
                PciBus::instance().readConfig32(static_cast<Device*>(device), offset, *value));
}
uacpi_status uacpi_kernel_pci_write8(uacpi_handle device, uacpi_size offset, uacpi_u8 value) {
  return status(device && offset < 4096 &&
                PciBus::instance().writeConfig8(static_cast<Device*>(device), offset, value));
}
uacpi_status uacpi_kernel_pci_write16(uacpi_handle device, uacpi_size offset, uacpi_u16 value) {
  return status(device && offset < 4096 &&
                PciBus::instance().writeConfig16(static_cast<Device*>(device), offset, value));
}
uacpi_status uacpi_kernel_pci_write32(uacpi_handle device, uacpi_size offset, uacpi_u32 value) {
  return status(device && offset < 4096 &&
                PciBus::instance().writeConfig32(static_cast<Device*>(device), offset, value));
}

uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size bytes, uacpi_handle* result) {
  if (!result || !bytes || base > 0xffff || bytes > 0x10000 - base) {
    return UACPI_STATUS_INVALID_ARGUMENT;
  }
#if X86 || X64
  *result = new PortRange{static_cast<uint16_t>(base), bytes};
  return UACPI_STATUS_OK;
#else
  return UACPI_STATUS_UNIMPLEMENTED;
#endif
}
void uacpi_kernel_io_unmap(uacpi_handle handle) {
  delete static_cast<PortRange*>(handle);
}
uacpi_status uacpi_kernel_io_read8(uacpi_handle h, uacpi_size o, uacpi_u8* v) {
  return readPort(h, o, v);
}
uacpi_status uacpi_kernel_io_read16(uacpi_handle h, uacpi_size o, uacpi_u16* v) {
  return readPort(h, o, v);
}
uacpi_status uacpi_kernel_io_read32(uacpi_handle h, uacpi_size o, uacpi_u32* v) {
  return readPort(h, o, v);
}
uacpi_status uacpi_kernel_io_write8(uacpi_handle h, uacpi_size o, uacpi_u8 v) {
  return writePort(h, o, v);
}
uacpi_status uacpi_kernel_io_write16(uacpi_handle h, uacpi_size o, uacpi_u16 v) {
  return writePort(h, o, v);
}
uacpi_status uacpi_kernel_io_write32(uacpi_handle h, uacpi_size o, uacpi_u32 v) {
  return writePort(h, o, v);
}

void* uacpi_kernel_alloc(uacpi_size bytes) {
  return new uint8_t[bytes];
}
void uacpi_kernel_free(void* memory) {
  delete[] static_cast<uint8_t*>(memory);
}
uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot() {
  return Time::getTicks();
}
void uacpi_kernel_stall(uacpi_u8 microseconds) {
  const auto deadline = Time::getTicks() + microseconds * Time::Multiplier::Microsecond;
  while (Time::getTicks() < deadline) {
    Processor::pause();
  }
}
void uacpi_kernel_sleep(uacpi_u64 milliseconds) {
  while (milliseconds) {
    const auto chunk = milliseconds > 1000 ? 1000 : milliseconds;
    const auto deadline = Time::getTicks() + chunk * Time::Multiplier::Millisecond;
    auto now = Time::getTicks();
    while (now < deadline) {
      Time::delay(deadline - now);
      now = Time::getTicks();
    }
    milliseconds -= chunk;
  }
}

uacpi_handle uacpi_kernel_create_mutex() {
  return new Mutex;
}
void uacpi_kernel_free_mutex(uacpi_handle handle) {
  delete static_cast<Mutex*>(handle);
}
uacpi_status uacpi_kernel_acquire_mutex(uacpi_handle handle, uacpi_u16 timeout) {
  return wait(static_cast<Mutex*>(handle), timeout) ? UACPI_STATUS_OK : UACPI_STATUS_TIMEOUT;
}
void uacpi_kernel_release_mutex(uacpi_handle handle) {
  static_cast<Mutex*>(handle)->release();
}
uacpi_handle uacpi_kernel_create_event() {
  return new Semaphore(0, false);
}
void uacpi_kernel_free_event(uacpi_handle handle) {
  delete static_cast<Semaphore*>(handle);
}
uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle handle, uacpi_u16 timeout) {
  return wait(static_cast<Semaphore*>(handle), timeout);
}
void uacpi_kernel_signal_event(uacpi_handle handle) {
  static_cast<Semaphore*>(handle)->release();
}
void uacpi_kernel_reset_event(uacpi_handle handle) {
  [[maybe_unused]] const size_t drained = static_cast<Semaphore*>(handle)->drainAvailable();
}
uacpi_thread_id uacpi_kernel_get_thread_id() {
  return Processor::information().getCurrentThread();
}
uacpi_interrupt_state uacpi_kernel_disable_interrupts() {
  const bool enabled = Processor::getInterrupts();
  Processor::setInterrupts(false);
  return enabled;
}
void uacpi_kernel_restore_interrupts(uacpi_interrupt_state state) {
  Processor::setInterrupts(state != 0);
}
uacpi_handle uacpi_kernel_create_spinlock() {
  return new NoIrqSpinlock;
}
void uacpi_kernel_free_spinlock(uacpi_handle handle) {
  delete static_cast<NoIrqSpinlock*>(handle);
}
uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle handle) {
  auto* lock = static_cast<NoIrqSpinlock*>(handle);
  lock->acquire();
  return lock->interrupts();
}
void uacpi_kernel_unlock_spinlock(uacpi_handle handle, uacpi_cpu_flags) {
  static_cast<NoIrqSpinlock*>(handle)->release();
}
uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request* request) {
  WARNING("ACPI: firmware request " << Dec << request->type);
  return request->type == UACPI_FIRMWARE_REQUEST_TYPE_BREAKPOINT ? UACPI_STATUS_OK
                                                                 : UACPI_STATUS_DENIED;
}
