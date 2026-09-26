/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef PEDIGREE_PCIE_PORTS_H
#define PEDIGREE_PCIE_PORTS_H

namespace PciePorts {
// Run after firmware discovery and PCI enumeration, before endpoint drivers.
// AER and native slot events share each root port's message interrupt.
void initialize();
}  // namespace PciePorts

#endif
