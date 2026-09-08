/*
 * Copyright (c) 2026, Pedigree Developers
 * SPDX-License-Identifier: ISC
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#ifndef AHCI_REGISTERS_H
#define AHCI_REGISTERS_H
#include "pedigree/kernel/processor/types.h"

// Intel AHCI 1.3.1, sections 3 and 4. Direct SATA command lists.
namespace Ahci {
constexpr size_t Cap = 0x00, Ghc = 0x04, Is = 0x08, Pi = 0x0c, Vs = 0x10;
constexpr size_t CccCtl = 0x14, Cap2 = 0x24, Bohc = 0x28;
constexpr uint32_t Enable = 1U << 31, InterruptEnable = 1U << 1, Reset = 1U;
constexpr uint32_t StaggeredSpinup = 1U << 27;
constexpr size_t PortBase = 0x100, PortStride = 0x80;
constexpr size_t Clb = 0x00, Clbu = 0x04, Fb = 0x08, Fbu = 0x0c;
constexpr size_t PortIs = 0x10, PortIe = 0x14, Cmd = 0x18, Tfd = 0x20;
constexpr size_t Sig = 0x24, Ssts = 0x28, Sctl = 0x2c, Serr = 0x30;
constexpr size_t Sact = 0x34, Ci = 0x38, Devslp = 0x44;
constexpr uint32_t Start = 1U, Spinup = 1U << 1, PowerOn = 1U << 2;
constexpr uint32_t FisEnable = 1U << 4, FisRunning = 1U << 14, CommandRunning = 1U << 15;
constexpr uint32_t ColdPresence = 1U << 20, Atapi = 1U << 24;
constexpr uint32_t AggressivePower = (1U << 26) | (1U << 27);
constexpr uint32_t IccMask = 15U << 28, IccActive = 1U << 28;
constexpr uint32_t Busy = 1U << 7, DataRequest = 1U << 3;
constexpr uint32_t TaskError = 1U, DeviceFault = 1U << 5;
constexpr uint32_t TaskFileError = 1U << 30;
constexpr uint32_t PortErrors = (0x1fU << 26) | (1U << 24) | (1U << 23) | (1U << 4);
constexpr uint32_t PortInterrupts =
    PortErrors | (1U << 22) | (1U << 6) | (1U << 3) | (1U << 1) | 1U;
constexpr uint32_t SataDisk = 0x00000101;
constexpr size_t MaxTransfer = 64 * 1024;
constexpr size_t FisOffset = 1024, TableOffset = 1280, TableStride = 384;

struct CommandHeader {
  uint32_t flags;
  volatile uint32_t transferred;
  uint32_t table;
  uint32_t tableUpper;
  uint32_t reserved[4];
};
struct Prd {
  uint32_t address;
  uint32_t addressUpper;
  uint32_t reserved;
  uint32_t byteCount;
};
struct CommandTable {
  uint8_t fis[64];
  uint8_t atapi[16];
  uint8_t reserved[48];
  Prd data[16];
};
static_assert(sizeof(CommandHeader) == 32, "AHCI command header size");
static_assert(sizeof(Prd) == 16, "AHCI physical region descriptor size");
static_assert(sizeof(CommandTable) == 384, "AHCI command table with sixteen PRDs");
static_assert((FisOffset % 256) == 0 && (TableOffset % 128) == 0, "AHCI DMA alignment");
}  // namespace Ahci
#endif
