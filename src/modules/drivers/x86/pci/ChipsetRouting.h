/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef PEDIGREE_CHIPSET_ROUTING_H
#define PEDIGREE_CHIPSET_ROUTING_H
#include "pedigree/kernel/utilities/Vector.h"
class Device;
void routeChipsetInterrupts(const Vector<Device*>& devices);
#endif
