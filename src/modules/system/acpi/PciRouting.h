/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef PEDIGREE_ACPI_PCI_ROUTING_H
#define PEDIGREE_ACPI_PCI_ROUTING_H

#include <uacpi/types.h>

void addPciRoutingRoot(uacpi_namespace_node* node, uint8_t firstBus, uint8_t lastBus);
void initialisePciRouting();

#endif
