/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef PEDIGREE_CHIPSET_QM67_H
#define PEDIGREE_CHIPSET_QM67_H
#include "pedigree/kernel/utilities/Vector.h"
class Device;
namespace Qm67 {
void routeInterrupts(const Vector<Device*>& devices);
}  // namespace Qm67
#endif
