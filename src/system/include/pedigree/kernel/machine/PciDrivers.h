/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef PEDIGREE_PCI_DRIVERS_H
#define PEDIGREE_PCI_DRIVERS_H

#include "pedigree/kernel/compiler.h"

#include <stdint.h>

class Device;

namespace PciDrivers {
struct Driver {
  uint8_t classCode;
  uint8_t subclass;
  uint8_t progInterface;
  Device* (*attach)(Device* pci);
  bool (*prepareRemove)(Device* controller);
  void (*cancelRemove)(Device* controller);
  void (*remove)(Device* controller);
};

/** Lifecycle calls run in sleepable context, outside Device tree locks.
 * Callbacks are serialized and must not reenter this registry. A driver keeps
 * the generic PCI parent alive and owns its controller child until remove().
 * A failed prepareRemove callback must leave its device operational.
 */
EXPORTED_PUBLIC bool initialize();
EXPORTED_PUBLIC bool registerDriver(const Driver* driver);

/** For a module unload admission hook. Busy reopens every prepared binding;
 * success closes attachment through unregisterDriver(), which drains removal
 * callbacks before the driver module may be unmapped. Driver storage must
 * remain valid until unregisterDriver() succeeds.
 */
EXPORTED_PUBLIC bool prepareUnregisterDriver(const Driver* driver);
EXPORTED_PUBLIC bool unregisterDriver(const Driver* driver);

/** Records a published function, then attempts binding. False may simply mean
 * there is no matching driver. Enumeration retains ownership of the PCI node.
 */
EXPORTED_PUBLIC bool attach(Device* pci);

/** Reserves orderly removal. Each successful preparation must end in cancel
 * or remove. Hardware and the generic PCI node must remain present until
 * remove returns; only then may enumeration reclaim the node or power off.
 */
EXPORTED_PUBLIC bool prepareRemove(Device* pci);
EXPORTED_PUBLIC void cancelRemove(Device* pci);
EXPORTED_PUBLIC bool remove(Device* pci);
}  // namespace PciDrivers

#endif
