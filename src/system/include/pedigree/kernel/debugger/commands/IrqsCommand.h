/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef IRQSCOMMAND_H
#define IRQSCOMMAND_H

#include "pedigree/kernel/debugger/DebuggerCommand.h"
#include "pedigree/kernel/machine/IrqManager.h"

/** Displays detached interrupt-line diagnostics in the kernel debugger. */
class IrqsCommand : public DebuggerCommand
{
  public:
    IrqsCommand();
    ~IrqsCommand();

    void autocomplete(const HugeStaticString &input, HugeStaticString &output);

    bool execute(
        const HugeStaticString &input, HugeStaticString &output,
        InterruptState &state, DebuggerIO *screen);

    const NormalStaticString getString()
    {
        return NormalStaticString("irqs");
    }

  private:
    static constexpr size_t SnapshotCapacity = 256;
    static_assert(
        SnapshotCapacity == (static_cast<size_t>(1) << (sizeof(uint8_t) * 8)),
        "the IRQ debugger buffer must cover every uint8_t line number");

    IrqLineDiagnosticSnapshot m_Snapshots[SnapshotCapacity];
};

#endif
