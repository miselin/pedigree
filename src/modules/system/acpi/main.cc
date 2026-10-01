/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/PciFirmware.h"
#include "pedigree/kernel/utilities/StaticString.h"

#include "AcpiEc.h"
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
uacpi_status sleepPreparationResult = UACPI_STATUS_OK;
StaticString<LOG_LENGTH> sleepPreparationDetail;
char sleepLogSnapshot[4096];

bool prepareShutdown(bool powerOff) {
  if (powerOff && powerManagement.powerOff) {
    Machine::setShutdownPhase(Machine::ShutdownPhase::Firmware, "ACPI: preparing S5");
    installAcpiEcHandlersForShutdown();
    NOTICE("ACPI: preparing S5 via uACPI");
    const auto result = uacpi_prepare_for_sleep_state(UACPI_SLEEP_STATE_S5);
    sleepPreparationResult = result;
    sleepPrepared = result == UACPI_STATUS_OK;
    sleepPreparationDetail.clear();
    if (!sleepPrepared) {
      // The final shutdown screen replaces the log, so retain the operation
      // region error that identifies a missing firmware handler.
      const size_t bytes = Log::instance().copyText(sleepLogSnapshot, sizeof(sleepLogSnapshot));
      size_t start = 0;
      for (size_t i = 0; i < bytes; ++i) {
        if (sleepLogSnapshot[i] != '\n') {
          continue;
        }
        StaticString<LOG_LENGTH> entry(sleepLogSnapshot + start, i - start);
        start = i + 1;
        if (entry.contains("ACPI: preparing S5 via uACPI")) {
          sleepPreparationDetail.clear();
        } else if (entry.contains("unable to attach") && entry.contains("operation region")) {
          sleepPreparationDetail = entry;
        } else if (!sleepPreparationDetail.length() && entry.contains("error while evaluating")) {
          sleepPreparationDetail = entry;
        }
      }
      ERROR("ACPI: S5 preparation failed: " << uacpi_status_to_string(result));
    }
  }
  Machine::setShutdownPhase(Machine::ShutdownPhase::Firmware, "ACPI: draining event workers");
  return shutdownAcpiEvents();
}

const char* powerOff() {
  // This pinned provider retains the result until the kernel copies it for
  // display after processor teardown. Terminal shutdown has one owner.
  static StaticString<256> message;
  message.clear();
  if (!sleepPrepared) {
    message += "ACPI: S5 preparation failed: ";
    message += uacpi_status_to_string(sleepPreparationResult);
    if (sleepPreparationDetail.length()) {
      message += "\n";
      message += sleepPreparationDetail;
    }
    return message;
  }
  NOTICE_NOLOCK("ACPI: entering S5 via uACPI");
  Machine::setShutdownPhase(Machine::ShutdownPhase::FinalAction, "ACPI: entering S5");
  const auto result = uacpi_enter_sleep_state(UACPI_SLEEP_STATE_S5);
  message += "ACPI: S5 entry returned: ";
  message += uacpi_status_to_string(result);
  ERROR_NOLOCK(static_cast<const char*>(message));
  return message;
}

void reset() {
  NOTICE_NOLOCK("ACPI: resetting via uACPI");
  Machine::setShutdownPhase(Machine::ShutdownPhase::FinalAction, "ACPI: requesting reset");
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
