/*
 * Copyright (c) 2008-2014, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "pedigree/kernel/debugger/commands/DisassembleCommand.h"
#include "pedigree/kernel/linker/KernelElf.h"
#include "pedigree/kernel/processor/Disassembler.h"
#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/demangle.h"

DisassembleCommand::DisassembleCommand()
{
}

DisassembleCommand::~DisassembleCommand()
{
}

void DisassembleCommand::autocomplete(
    const HugeStaticString &input, HugeStaticString &output)
{
    // TODO: add symbols.
    output = "<address>";
}

bool DisassembleCommand::execute(
    const HugeStaticString &input, HugeStaticString &output,
    InterruptState &state, DebuggerIO *screen)
{
    // This command can take either an address or a symbol name (or nothing).
    uintptr_t address;

    // If we see just "disassemble", no parameters were matched.
    if (input == "disassemble")
        address = state.getInstructionPointer();
    else
    {
        // Is it an address?
        address = input.uintptrValue();

        if (address == 0)
        {
            // No, try a symbol name.
            // TODO.
            output = "Not a valid address or symbol name: `";
            output += input;
            output += "'.\n";
            return true;
        }
    }

    // Dissassemble around address.
    size_t nInstructions = 10;

    LargeStaticString text;
    Disassembler disassembler;
#if BITS_64
    disassembler.setMode(64);
#endif
    disassembler.setLocation(address);

    for (size_t i = 0; i < nInstructions; i++)
    {
        text.clear();
        uintptr_t location = disassembler.getLocation();
        disassembler.disassemble(text);

        // What symbol are we in?
        // TODO grep the memory map for the right ELF to look at.
        uintptr_t symStart = 0;
        const char *pSym =
            KernelElf::instance().globalLookupSymbol(location, &symStart);
        if (location == symStart)
        {
#if BITS_32
            output.append(location, 16, 8, '0');
#endif
#if BITS_64
            output.append(location, 16, 16, '0');
#endif
            output += ": <";
            LargeStaticString sym;
            demangle_full(LargeStaticString(pSym), sym);
            output += sym;
            output += ">:\n";
        }

#if BITS_32
        output.append(location, 16, 8, ' ');
#endif
#if BITS_64
        output.append(location, 16, 16, ' ');
#endif
        output += ": ";
        output += text;
        output += '\n';
    }

    return true;
}

const NormalStaticString DisassembleCommand::getString()
{
    return NormalStaticString("disassemble");
}
