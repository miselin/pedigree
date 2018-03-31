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

#ifndef SCSICOMMANDS_H
#define SCSICOMMANDS_H

#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/utility.h"

class ScsiCommand
{
  public:
    ScsiCommand();
    virtual ~ScsiCommand();

    virtual size_t serialise(uintptr_t &addr) = 0;
};

namespace ScsiCommands
{
class Inquiry : public ScsiCommand
{
  public:
    Inquiry(
        uint16_t len = 0, bool enableVitalData = false, uint8_t pageCode = 0,
        uint8_t ctl = 0);

    virtual size_t serialise(uintptr_t &addr);

    struct
    {
        uint8_t opcode;
        uint8_t epvd;
        uint8_t pageCode;
        uint16_t len;
        uint8_t control;
    } PACKED command;
};

class UnitReady : public ScsiCommand
{
  public:
    UnitReady(uint8_t ctl = 0);

    virtual size_t serialise(uintptr_t &addr);

    struct
    {
        uint8_t opcode;
        uint32_t rsvd;
        uint8_t control;
    } PACKED command;
};

class ReadSense : public ScsiCommand
{
  public:
    ReadSense(uint8_t desc, uint8_t len, uint8_t ctl = 0);

    virtual size_t serialise(uintptr_t &addr);

    struct
    {
        uint8_t opcode;
        uint8_t desc;
        uint16_t rsvd;
        uint8_t len;
        uint8_t control;
    } PACKED command;
};

class StartStop : public ScsiCommand
{
  public:
    StartStop(
        bool imm, uint8_t newpower, bool eject_load, bool start,
        uint8_t ctl = 0);

    virtual size_t serialise(uintptr_t &addr);

    struct
    {
        uint8_t opcode;
        uint8_t imm;
        uint16_t rsvd;
        uint8_t setup;
        uint8_t control;
    } PACKED command;
};

class SendDiagnostic : public ScsiCommand
{
  public:
    SendDiagnostic(
        bool selfTest, uint8_t selfTestCode = 0, uintptr_t params = 0,
        size_t paramLen = 0, bool deviceOffline = false,
        bool unitOffline = false, uint8_t ctl = 0);

    virtual size_t serialise(uintptr_t &addr);

    struct
    {
        uint8_t opcode;
        uint32_t unitOffline : 1;
        uint32_t devOffline : 1;
        uint32_t selfTest : 1;
        uint32_t rsvd1 : 1;
        uint32_t pf : 1;
        uint32_t selfTestCode : 3;
        uint8_t rsvd2;
        uint16_t paramListLen;
        uint8_t control;
    } PACKED command;
};

class ReadTocCommand : public ScsiCommand
{
  public:
    ReadTocCommand(uint16_t nativeBlockSize, uint8_t ctl = 0);

    virtual size_t serialise(uintptr_t &addr);

    struct
    {
        uint8_t opcode;
        uint8_t flags;
        uint8_t format;
        uint8_t rsvd1;
        uint8_t rsvd2;
        uint8_t rsvd3;
        uint8_t track;
        uint16_t len;
        uint8_t control;
    } PACKED command;

    struct TocEntry
    {
        uint8_t Rsvd1;
        uint8_t Flags;
        uint8_t TrackNum;
        uint8_t Rsvd2;
        uint32_t TrackStart;
    } PACKED;
};

class ReadCapacity10 : public ScsiCommand
{
  public:
    ReadCapacity10(uint8_t ctl = 0);

    virtual size_t serialise(uintptr_t &addr);

    struct
    {
        uint8_t opcode;
        uint8_t obsolete_rsvd;
        uint32_t lba;
        uint8_t rsvd[2];
        uint8_t pmi;
        uint8_t control;
    } PACKED command;
};

class Read10 : public ScsiCommand
{
  public:
    Read10(uint32_t nLba, uint32_t nSectors);

    virtual size_t serialise(uintptr_t &addr);

    struct command
    {
        uint8_t nOpCode;
        uint8_t bRelAddr : 1;
        uint8_t res0 : 2;
        uint8_t bFUA : 1;
        uint8_t bDPO : 1;
        uint8_t res1 : 3;
        uint32_t nLba;
        uint8_t res2;
        uint16_t nSectors;
        uint8_t nControl;
    } PACKED command;
};

class Read12 : public ScsiCommand
{
  public:
    Read12(uint32_t nLba, uint32_t nSectors);

    virtual size_t serialise(uintptr_t &addr);

    struct command
    {
        uint8_t nOpCode;
        uint8_t bRelAddr : 1;
        uint8_t res0 : 2;
        uint8_t bFUA : 1;
        uint8_t bDPO : 1;
        uint8_t res1 : 3;
        uint32_t nLba;
        uint32_t nSectors;
        uint8_t res2;
        uint8_t nControl;
    } PACKED command;
};

class Read16 : public ScsiCommand
{
  public:
    Read16(uint32_t nLba, uint32_t nSectors);

    virtual size_t serialise(uintptr_t &addr);

    struct command
    {
        uint8_t nOpCode;
        uint8_t bRelAddr : 1;
        uint8_t res0 : 2;
        uint8_t bFUA : 1;
        uint8_t bDPO : 1;
        uint8_t res1 : 3;
        uint64_t nLba;
        uint32_t nSectors;
        uint8_t res2;
        uint8_t nControl;
    } PACKED command;
};

class Write10 : public ScsiCommand
{
  public:
    Write10(uint32_t nLba, uint32_t nSectors);

    virtual size_t serialise(uintptr_t &addr);

    struct command
    {
        uint8_t nOpCode;
        uint8_t obs : 1;
        uint8_t bFUA_NV : 1;
        uint8_t res1 : 1;
        uint8_t bFUA : 1;
        uint8_t bDPO : 1;
        uint8_t nWrProtect : 3;
        uint32_t nLba;
        uint8_t res2;
        uint16_t nSectors;
        uint8_t nControl;
    } PACKED command;
};

class Write12 : public ScsiCommand
{
  public:
    Write12(uint32_t nLba, uint32_t nSectors);

    virtual size_t serialise(uintptr_t &addr);

    struct command
    {
        uint8_t nOpCode;
        uint8_t obs : 1;
        uint8_t bFUA_NV : 1;
        uint8_t res1 : 1;
        uint8_t bFUA : 1;
        uint8_t bDPO : 1;
        uint8_t nWrProtect : 3;
        uint32_t nLba;
        uint32_t nSectors;
        uint8_t res2;
        uint8_t nControl;
    } PACKED command;
};

class Write16 : public ScsiCommand
{
  public:
    Write16(uint32_t nLba, uint32_t nSectors);

    virtual size_t serialise(uintptr_t &addr);

    struct command
    {
        uint8_t nOpCode;
        uint8_t obs : 1;
        uint8_t bFUA_NV : 1;
        uint8_t res1 : 1;
        uint8_t bFUA : 1;
        uint8_t bDPO : 1;
        uint8_t nWrProtect : 3;
        uint64_t nLba;
        uint32_t nSectors;
        uint8_t res2;
        uint8_t nControl;
    } PACKED command;
};

class Synchronise10 : public ScsiCommand
{
  public:
    Synchronise10(uint32_t nLba, uint32_t nSectors);

    virtual size_t serialise(uintptr_t &addr);

    struct command
    {
        uint8_t nOpCode;
        uint8_t obs : 1;
        uint8_t bImmed : 1;
        uint8_t bSyncNV : 1;
        uint8_t rsvd1 : 5;
        uint32_t nLba;
        uint8_t nGroup : 5;
        uint8_t rsvd2 : 3;
        uint16_t nBlocks;
        uint8_t nControl;
    } PACKED command;
};

class Synchronise16 : public ScsiCommand
{
  public:
    Synchronise16(uint32_t nLba, uint32_t nSectors);

    virtual size_t serialise(uintptr_t &addr);

    struct command
    {
        uint8_t nOpCode;
        uint8_t obs : 1;
        uint8_t bImmed : 1;
        uint8_t bSyncNV : 1;
        uint8_t rsvd1 : 5;
        uint64_t nLba;
        uint32_t nBlocks;
        uint8_t nGroup : 5;
        uint8_t rsvd2 : 3;
        uint8_t nControl;
    } PACKED command;
};
};

#endif
