/*
 * Copyright (c) 2026, Pedigree Developers
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

#ifndef KERNEL_TARGETINFO_H
#define KERNEL_TARGETINFO_H
#include <config.h>

#include "pedigree/kernel/processor/types.h"

#if !defined(TARGET_IS_LITTLE_ENDIAN)
#error TARGET_IS_LITTLE_ENDIAN not defined
#endif

/** Immutable compile-time properties of the selected Pedigree build target. */
class TargetInfo final {
 public:
  TargetInfo() = delete;

  /**
   * The Pedigree base page size: the smallest unit which this build
   * independently allocates, maps, protects, and unmaps. A hosted platform's
   * VM granule is a separate runtime property.
   */
  static constexpr size_t getPageSize() noexcept {
    return PEDIGREE_TARGET_PAGE_SIZE;
  }

  static constexpr size_t getPageShift() noexcept {
    size_t pageSize = getPageSize();
    size_t shift = 0;
    while (pageSize > 1) {
      pageSize >>= 1;
      ++shift;
    }
    return shift;
  }

  static constexpr size_t getPageOffsetMask() noexcept {
    return getPageSize() - 1;
  }

  static constexpr size_t getPointerBits() noexcept {
    return sizeof(void*) * __CHAR_BIT__;
  }

  static constexpr bool isLittleEndian() noexcept {
    return TARGET_IS_LITTLE_ENDIAN != 0;
  }
};

static_assert(TargetInfo::getPageSize() != 0, "Target page size must be nonzero.");
static_assert(TargetInfo::getPageSize() == PAGE_SIZE,
              "The compatibility page-size macro must match TargetInfo.");
static_assert((TargetInfo::getPageSize() & TargetInfo::getPageOffsetMask()) == 0,
              "Target page size must be a power of two.");
static_assert((size_t{1} << TargetInfo::getPageShift()) == TargetInfo::getPageSize(),
              "Target page size and shift must agree.");
static_assert(sizeof(uintptr_t) == sizeof(void*), "uintptr_t must have the target pointer width.");
static_assert(sizeof(size_t) == sizeof(uintptr_t),
              "size_t and uintptr_t must have the same target width.");

#if defined(BITS_64) && BITS_64
static_assert(TargetInfo::getPointerBits() == 64, "BITS_64 disagrees with the compiler target.");
#endif

#if defined(BITS_32) && BITS_32
static_assert(TargetInfo::getPointerBits() == 32, "BITS_32 disagrees with the compiler target.");
#endif

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
static_assert(TargetInfo::isLittleEndian() == (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__),
              "Configured target endianness disagrees with the compiler target.");
#endif

#endif
