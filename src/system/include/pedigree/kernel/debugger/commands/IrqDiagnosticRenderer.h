/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef IRQDIAGNOSTICRENDERER_H
#define IRQDIAGNOSTICRENDERER_H

#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/utilities/StaticString.h"

/** Large enough for a complete worst-case detached line snapshot. */
using IrqDiagnosticString = StaticString<2048>;

/** Renders one detached IRQ snapshot without consulting live kernel state. */
class IrqDiagnosticRenderer {
 public:
  static void render(const IrqLineDiagnosticSnapshot& snapshot, IrqDiagnosticString& line);
};

#endif
