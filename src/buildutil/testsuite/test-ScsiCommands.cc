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

#define PEDIGREE_EXTERNAL_SOURCE 1

#include <cstring>

#include "modules/drivers/common/scsi/ScsiCommands.h"
#include <gtest/gtest.h>

namespace {
template <size_t N>
void expectCommandBytes(ScsiCommand& command, const uint8_t (&expected)[N]) {
  uintptr_t address = 0;
  ASSERT_EQ(command.serialise(address), N);
  ASSERT_NE(address, 0U);
  EXPECT_EQ(std::memcmp(reinterpret_cast<const void*>(address), expected, N), 0);
}
}  // namespace

TEST(ScsiCommands, Read16Preserves64BitLba) {
  ScsiCommands::Read16 command(0x0123456789ABCDEFULL, 0x0A0B0C0DU);
  const uint8_t expected[] = {0x88, 0x00, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB,
                              0xCD, 0xEF, 0x0A, 0x0B, 0x0C, 0x0D, 0x00, 0x00};

  expectCommandBytes(command, expected);
}

TEST(ScsiCommands, Write16Preserves64BitLba) {
  ScsiCommands::Write16 command(0x0123456789ABCDEFULL, 0x0A0B0C0DU);
  const uint8_t expected[] = {0x8A, 0x00, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB,
                              0xCD, 0xEF, 0x0A, 0x0B, 0x0C, 0x0D, 0x00, 0x00};

  expectCommandBytes(command, expected);
}

TEST(ScsiCommands, Synchronise16Preserves64BitLba) {
  ScsiCommands::Synchronise16 command(0x0123456789ABCDEFULL, 0x0A0B0C0DU);
  const uint8_t expected[] = {0x91, 0x00, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB,
                              0xCD, 0xEF, 0x0A, 0x0B, 0x0C, 0x0D, 0x00, 0x00};

  expectCommandBytes(command, expected);
}
