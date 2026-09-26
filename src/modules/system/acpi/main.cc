/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/PciFirmware.h"

#include "AcpiEvents.h"
#include "PciRouting.h"
#include "modules/Module.h"
#include <uacpi/uacpi.h>

namespace {
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
  if (result == UACPI_STATUS_OK) {
    result = uacpi_namespace_load();
  }
  if (result == UACPI_STATUS_OK) {
    result = uacpi_namespace_initialize();
  }
  if (result != UACPI_STATUS_OK) {
    ERROR("ACPI: namespace unavailable: " << uacpi_status_to_string(result));
    return true;
  }
  NOTICE("ACPI: AML namespace initialized");
  (void)PciFirmware::discover();
  initialisePciRouting();
  return true;
}
void exit() {}
}  // namespace

MODULE_INFO_NON_UNLOADABLE("acpi", &entry, &exit);
