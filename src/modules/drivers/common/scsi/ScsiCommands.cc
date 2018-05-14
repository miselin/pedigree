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

#include "modules/drivers/common/scsi/ScsiCommands.h"

ScsiCommand::ScsiCommand() = default;
ScsiCommand::~ScsiCommand() = default;

namespace ScsiCommands
{
Inquiry::Inquiry(
    uint16_t len, bool enableVitalData, uint8_t pageCode, uint8_t ctl)
{
    ByteSet(&command, 0, sizeof(command));
    command.opcode = 0x12;
    command.epvd = enableVitalData;
    if (enableVitalData)
        command.pageCode = pageCode;
    command.len = HOST_TO_BIG16(len);
    command.control = ctl;
}

size_t Inquiry::serialise(uintptr_t &addr)
{
    addr = reinterpret_cast<uintptr_t>(&command);
    return sizeof(command);
}

UnitReady::UnitReady(uint8_t ctl)
{
    ByteSet(&command, 0, sizeof(command));
    command.opcode = 0;
    command.control = ctl;
}

size_t UnitReady::serialise(uintptr_t &addr)
{
    addr = reinterpret_cast<uintptr_t>(&command);
    return sizeof(command);
}

ReadSense::ReadSense(uint8_t desc, uint8_t len, uint8_t ctl)
{
    ByteSet(&command, 0, sizeof(command));
    command.opcode = 0x03;
    command.desc = desc;
    command.len = len;
    command.control = ctl;
}

size_t ReadSense::serialise(uintptr_t &addr)
{
    addr = reinterpret_cast<uintptr_t>(&command);
    return sizeof(command);
}

StartStop::StartStop(
    bool imm, uint8_t newpower, bool eject_load, bool start, uint8_t ctl)
{
    ByteSet(&command, 0, sizeof(command));
    command.opcode = 0x1b;
    command.imm = imm ? 1 : 0;
    command.setup =
        (start ? 1 : 0) | ((eject_load ? 1 : 0) << 1) | (newpower << 4);
    command.control = ctl;
}

size_t StartStop::serialise(uintptr_t &addr)
{
    addr = reinterpret_cast<uintptr_t>(&command);
    return sizeof(command);
}

SendDiagnostic::SendDiagnostic(
    bool selfTest, uint8_t selfTestCode, uintptr_t params, size_t paramLen,
    bool deviceOffline, bool unitOffline, uint8_t ctl)
{
    ByteSet(&command, 0, sizeof(command));
    command.opcode = 0x1d;
    command.unitOffline = unitOffline;
    command.devOffline = deviceOffline;
    command.selfTest = selfTest;
    command.pf = 0;
    command.selfTestCode = selfTestCode;
    command.paramListLen = HOST_TO_BIG16(paramLen);
    command.control = ctl;
}

size_t SendDiagnostic::serialise(uintptr_t &addr)
{
    addr = reinterpret_cast<uintptr_t>(&command);
    return sizeof(command);
}

ReadTocCommand::ReadTocCommand(uint16_t nativeBlockSize, uint8_t ctl)
{
    ByteSet(&command, 0, sizeof(command));
    command.opcode = 0x43;
    command.len = HOST_TO_BIG16(nativeBlockSize);
}

size_t ReadTocCommand::serialise(uintptr_t &addr)
{
    addr = reinterpret_cast<uintptr_t>(&command);
    return sizeof(command);
}

ReadCapacity10::ReadCapacity10(uint8_t ctl)
{
    ByteSet(&command, 0, sizeof(command));
    command.opcode = 0x25;
    command.control = ctl;
}

size_t ReadCapacity10::serialise(uintptr_t &addr)
{
    addr = reinterpret_cast<uintptr_t>(&command);
    return sizeof(command);
}

Read10::Read10(uint32_t nLba, uint32_t nSectors)
{
    ByteSet(&command, 0, sizeof(command));
    command.nOpCode = 0x28;
    command.nLba = HOST_TO_BIG32(nLba);
    command.nSectors = HOST_TO_BIG16(nSectors);
}

size_t Read10::serialise(uintptr_t &addr)
{
    addr = reinterpret_cast<uintptr_t>(&command);
    return sizeof(command);
}

Read12::Read12(uint32_t nLba, uint32_t nSectors)
{
    ByteSet(&command, 0, sizeof(command));
    command.nOpCode = 0xa8;
    command.nLba = HOST_TO_BIG32(nLba);
    command.nSectors = HOST_TO_BIG32(nSectors);
}

size_t Read12::serialise(uintptr_t &addr)
{
    addr = reinterpret_cast<uintptr_t>(&command);
    return sizeof(command);
}

Read16::Read16(uint32_t nLba, uint32_t nSectors)
{
    ByteSet(&command, 0, sizeof(command));
    command.nOpCode = 0x88;
    command.nLba = HOST_TO_BIG64(nLba);
    command.nSectors = HOST_TO_BIG32(nSectors);
}

size_t Read16::serialise(uintptr_t &addr)
{
    addr = reinterpret_cast<uintptr_t>(&command);
    return sizeof(command);
}

Write10::Write10(uint32_t nLba, uint32_t nSectors)
{
    ByteSet(&command, 0, sizeof(command));
    command.nOpCode = 0x2A;
    command.nLba = HOST_TO_BIG32(nLba);
    command.nSectors = HOST_TO_BIG16(nSectors);
}

size_t Write10::serialise(uintptr_t &addr)
{
    addr = reinterpret_cast<uintptr_t>(&command);
    return sizeof(command);
}

Write12::Write12(uint32_t nLba, uint32_t nSectors)
{
    ByteSet(&command, 0, sizeof(command));
    command.nOpCode = 0xAA;
    command.nLba = HOST_TO_BIG32(nLba);
    command.nSectors = HOST_TO_BIG32(nSectors);
}

size_t Write12::serialise(uintptr_t &addr)
{
    addr = reinterpret_cast<uintptr_t>(&command);
    return sizeof(command);
}

Write16::Write16(uint32_t nLba, uint32_t nSectors)
{
    ByteSet(&command, 0, sizeof(command));
    command.nOpCode = 0x8A;
    command.nLba = HOST_TO_BIG64(nLba);
    command.nSectors = HOST_TO_BIG32(nSectors);
}

size_t Write16::serialise(uintptr_t &addr)
{
    addr = reinterpret_cast<uintptr_t>(&command);
    return sizeof(command);
}

Synchronise10::Synchronise10(uint32_t nLba, uint32_t nSectors)
{
    ByteSet(&command, 0, sizeof(command));
    command.nOpCode = 0x35;
    command.nLba = HOST_TO_BIG32(nLba);
    command.nBlocks = HOST_TO_BIG16(nSectors);
}

size_t Synchronise10::serialise(uintptr_t &addr)
{
    addr = reinterpret_cast<uintptr_t>(&command);
    return sizeof(command);
}

Synchronise16::Synchronise16(uint32_t nLba, uint32_t nSectors)
{
    ByteSet(&command, 0, sizeof(command));
    command.nOpCode = 0x91;
    command.nLba = HOST_TO_BIG64(nLba);
    command.nBlocks = HOST_TO_BIG32(nSectors);
}

size_t Synchronise16::serialise(uintptr_t &addr)
{
    addr = reinterpret_cast<uintptr_t>(&command);
    return sizeof(command);
}
}  // namespace ScsiCommands
