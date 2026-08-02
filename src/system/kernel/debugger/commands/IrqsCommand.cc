/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/debugger/commands/IrqsCommand.h"
#include "pedigree/kernel/debugger/DebuggerIO.h"
#include "pedigree/kernel/debugger/commands/IrqDiagnosticRenderer.h"
#include "pedigree/kernel/machine/Machine.h"

namespace
{
const char *skipSpaces(const char *text)
{
    while (*text == ' ' || *text == '\t')
    {
        ++text;
    }
    return text;
}

bool consumeCommandName(const char *&text)
{
    static constexpr char Name[] = "irqs";
    const char *cursor = text;
    for (size_t i = 0; Name[i]; ++i, ++cursor)
    {
        if (*cursor != Name[i])
        {
            return false;
        }
    }

    if (*cursor && *cursor != ' ' && *cursor != '\t')
    {
        return false;
    }
    text = skipSpaces(cursor);
    return true;
}

bool parseLineFilter(
    const HugeStaticString &input, bool &hasFilter, uint8_t &line)
{
    const char *text = skipSpaces(static_cast<const char *>(input));
    if (!*text)
    {
        hasFilter = false;
        return true;
    }

    // Also accept direct callers which have not stripped the command name.
    if (*text == 'i')
    {
        if (!consumeCommandName(text))
        {
            return false;
        }
        if (!*text)
        {
            hasFilter = false;
            return true;
        }
    }

    size_t value = 0;
    bool sawDigit = false;
    while (*text >= '0' && *text <= '9')
    {
        sawDigit = true;
        value = value * 10 + static_cast<size_t>(*text - '0');
        if (value > 255)
        {
            return false;
        }
        ++text;
    }

    text = skipSpaces(text);
    if (!sawDigit || *text)
    {
        return false;
    }

    hasFilter = true;
    line = static_cast<uint8_t>(value);
    return true;
}
}  // namespace

IrqsCommand::IrqsCommand() : DebuggerCommand(), m_Snapshots()
{
}

IrqsCommand::~IrqsCommand()
{
}

void IrqsCommand::autocomplete(
    const HugeStaticString &input, HugeStaticString &output)
{
}

bool IrqsCommand::execute(
    const HugeStaticString &input, HugeStaticString &output,
    InterruptState &state, DebuggerIO *screen)
{
    bool hasFilter = false;
    uint8_t requestedLine = 0;
    if (!parseLineFilter(input, hasFilter, requestedLine))
    {
        output += "usage: irqs [line]\n";
        return true;
    }

    if (!screen)
    {
        output += "IRQ diagnostics require a debugger display.\n";
        return true;
    }

    IrqManager *manager = Machine::instance().getIrqManager();
    if (!manager)
    {
        output += "IRQ diagnostics are unavailable on this platform.\n";
        return true;
    }

    size_t count = manager->snapshotIrqLines(m_Snapshots, SnapshotCapacity);
    if (count > SnapshotCapacity)
    {
        count = SnapshotCapacity;
    }
    if (!count)
    {
        output += "IRQ diagnostics are unavailable on this platform.\n";
        return true;
    }

    bool rendered = false;
    for (size_t i = 0; i < count; ++i)
    {
        if (hasFilter && m_Snapshots[i].line != requestedLine)
        {
            continue;
        }

        HugeStaticString line;
        IrqDiagnosticRenderer::render(m_Snapshots[i], line);
        screen->writeCli(line, DebuggerIO::LightGrey, DebuggerIO::Black);
        rendered = true;
    }

    if (hasFilter && !rendered)
    {
        output += "IRQ line ";
        output.append(requestedLine);
        output += " is not present in this snapshot.\n";
    }
    return true;
}
