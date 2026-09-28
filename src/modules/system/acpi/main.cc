/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/PciFirmware.h"

#include "AcpiEvents.h"
#include "PciRouting.h"
#include "modules/Module.h"
#include <uacpi/namespace.h>
#include <uacpi/sleep.h>
#include <uacpi/uacpi.h>

#if MACH_PC && ACPI
#include "system/kernel/machine/mach_pc/Acpi.h"
#endif

namespace {
#if MACH_PC && ACPI
bool prepareShutdown(bool powerOff);
Acpi::PowerManagement powerManagement{&prepareShutdown, nullptr, nullptr};
bool sleepPrepared = false;

bool prepareShutdown(bool powerOff) {
  if (powerOff && powerManagement.powerOff) {
    NOTICE("ACPI: preparing S5 via uACPI");
    const auto result = uacpi_prepare_for_sleep_state(UACPI_SLEEP_STATE_S5);
    sleepPrepared = result == UACPI_STATUS_OK;
    if (!sleepPrepared) {
      ERROR("ACPI: S5 preparation failed: " << uacpi_status_to_string(result));
    }
  }
  return shutdownAcpiEvents();
}

void powerOff() {
  if (!sleepPrepared) {
    return;
  }
  NOTICE_NOLOCK("ACPI: entering S5 via uACPI");
  const auto result = uacpi_enter_sleep_state(UACPI_SLEEP_STATE_S5);
  ERROR_NOLOCK("ACPI: S5 returned: " << uacpi_status_to_string(result));
}

void reset() {
  NOTICE_NOLOCK("ACPI: resetting via uACPI");
  const auto result = uacpi_reboot();
  ERROR_NOLOCK("ACPI: reset returned: " << uacpi_status_to_string(result));
}
#endif

bool entry() {
  uacpi_phys_addr rsdp = 0;
  if (uacpi_kernel_get_rsdp(&rsdp) != UACPI_STATUS_OK) {
    (void)PciFirmware::discover();
    return true;
  }
  if (!initialiseAcpiEvents()) {
    return false;
  }
  uacpi_status result = uacpi_initialize(UACPI_FLAG_BAD_CSUM_FATAL);
#if MACH_PC && ACPI
  if (result == UACPI_STATUS_OK) {
    powerManagement.reset = &reset;
  }
#endif
  if (result == UACPI_STATUS_OK) {
    result = uacpi_namespace_load();
  }
  if (result == UACPI_STATUS_OK) {
    result = uacpi_namespace_initialize();
  }
  if (result != UACPI_STATUS_OK) {
    ERROR("ACPI: namespace unavailable: " << uacpi_status_to_string(result));
#if MACH_PC && ACPI
    Acpi::instance().setPowerManagement(&powerManagement);
#endif
    return true;
  }
  NOTICE("ACPI: AML namespace initialized");
#if MACH_PC && ACPI
  uacpi_namespace_node* s5 = nullptr;
  if (uacpi_namespace_node_find(uacpi_namespace_root(), "_S5", &s5) == UACPI_STATUS_OK) {
    powerManagement.powerOff = &powerOff;
  }
  Acpi::instance().setPowerManagement(&powerManagement);
#endif
  (void)PciFirmware::discover();
  initialisePciRouting();
  return true;
}
void exit() {}
}  // namespace

MODULE_INFO_NON_UNLOADABLE("acpi", &entry, &exit);
