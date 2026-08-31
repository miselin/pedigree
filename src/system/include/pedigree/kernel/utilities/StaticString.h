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

#ifndef STATICSTRING_H
#define STATICSTRING_H
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/utility.h"

#include <config.h>

/**
 * Set to 1 to enable a variety of asserts that validate sane behavior, at a
 * rather significant performance cost.
 */
#define STATICSTRING_ASSERTS 0

#if STATICSTRING_ASSERTS
#define STATICSTRING_ASSERT(...) assert(__VA_ARGS__)
#else
#define STATICSTRING_ASSERT(...)
#endif

/** @addtogroup kernelutilities
 * @{ */

/**
 * Derivative of StringBase that uses a statically allocated chunk of memory.
 */
template <unsigned int N>
class EXPORTED_PUBLIC StaticString {
 public:
  /**
   * Default constructor.
   */
  StaticString() : m_Length(0), m_Hash(0), m_AllowHashes(false) {
    m_pData[0] = '\0';

    updateHash();
  }

  /**
   * Creates a StaticString from a const char *.
   * This creates a new copy of pSrc - pSrc can be safely
   * deallocated afterwards.
   */
  explicit StaticString(const char* pSrc) : m_Length(0), m_Hash(0), m_AllowHashes(false) {
    assign(pSrc);
  }

  /**
   * Creates a StaticString from a const char * of a specific length.
   * This creates a new copy of pSrc - pSrc can be safely
   * deallocated afterwards.
   */
  StaticString(const char* pSrc, size_t len) : m_Length(0), m_Hash(0), m_AllowHashes(false) {
    assign(pSrc, len);
  }

  /** Creates a StaticString from a String object. */
  StaticString(const String& str) : m_Length(0), m_Hash(0), m_AllowHashes(false) {
    assign(str.cstr(), str.length());
  }

  /**
   * Copy constructor - creates a StaticString from another StaticString.
   * Copies the memory associated with src.
   */
  template <unsigned int N2>
  explicit StaticString(const StaticString<N2>& src)
      : StaticString(static_cast<const char*>(src), src.length()) {}

  operator const char*() const {
    return m_pData;
  }

  template <unsigned int N2>
  StaticString& operator+=(const StaticString<N2>& str) {
    if (length() == 0) {
      assign(str);
      return *this;
    }

    append(str);
    return *this;
  }

  template <typename T>
  StaticString& operator+=(const T& i) {
    append(i);
    return *this;
  }

  void clear() {
    m_Length = 0;
    m_pData[0] = '\0';

    updateHash();
  }

  void assign(const char* str, size_t len = 0) {
    if (!str) {
      clear();
      return;
    }

    if (!len) {
      len = min(N - 1, StringLength(str));
    } else {
      len = min(len, N - 1);
    }

    MemoryCopy(m_pData, str, len);

    m_Length = len;
    m_pData[len] = 0;

    check();
    updateHash();
  }

  void assign(const String& str) {
    assign(str.cstr(), str.length());
  }

  template <unsigned int N2>
  void assign(const StaticString<N2>& other) {
    assign(other, other.length());
  }

  StaticString& operator=(const char* str) {
    assign(str);
    return *this;
  }

  StaticString& operator=(const String& str) {
    assign(str);
    return *this;
  }

  bool operator==(const char* pStr) const {
    if (StringLength(pStr) != length()) {
      return false;
    }

    return StringCompareN(m_pData, pStr, length()) == 0;
  }

  template <unsigned int N2>
  bool operator==(const StaticString<N2>& other) const {
    if (other.length() != length()) {
      return false;
    } else if ((m_AllowHashes && other.m_AllowHashes) && (m_Hash != other.hash())) {
      return false;
    }

    return StringCompareN(m_pData, other.m_pData, length()) == 0;
  }

  int last(const char search) const {
    for (int i = length(); i >= 0; i--)
      if (m_pData[i] == search)
        return i;
    return -1;
  }

  int first(const char search) const {
    for (size_t i = 0; i < length(); i++)
      if (m_pData[i] == search)
        return i;
    return -1;
  }

  void stripLast() {
    if (m_Length) {
      m_pData[--m_Length] = '\0';
      updateHash();
    }
  }

  bool contains(const char* other) const {
    return contains(m_pData, other, length(), StringLength(other));
  }

  template <unsigned int N2>
  bool contains(const StaticString<N2>& other) const {
    return contains(m_pData, other.m_pData, length(), other.length());
  }

  int intValue(int nBase = 0) const {
    char* pEnd;
    int ret = StringToUnsignedLong(m_pData, &pEnd, nBase);
    if (pEnd == m_pData)
      return -1;  // Failed to find anything.
    else
      return ret;
  }

  uintptr_t uintptrValue(int nBase = 0) const {
    char* pEnd;
    uintptr_t ret = StringToUnsignedLong(m_pData, &pEnd, nBase);
    if (pEnd == m_pData)
      return ~0UL;  // Failed to find anything.
    else
      return ret;
  }

  void truncate(size_t len) {
    if (len > m_Length)
      return;
    m_Length = len;
    m_pData[len] = '\0';

    updateHash();
  }

  StaticString left(int n) const {
    return StaticString<N>(m_pData, n);
  }

  StaticString right(int n) const {
    /// \todo this is technically off-by-one, but I don't feel comfortable
    /// changing the behavior -Matt
    return StaticString<N>(m_pData + n + 1, length() - n - 1);
  }

  StaticString& stripFirst(size_t n = 1) {
    if (n > length()) {
      m_pData[0] = '\0';
      m_Length = 0;
      return *this;
    }
    int i;
    for (i = n; m_pData[i] != '\0'; i++)
      m_pData[i - n] = m_pData[i];
    m_pData[i - n] = '\0';
    m_Length -= n;

    updateHash();

    return *this;
  }

  template <typename T>
  StaticString& operator<<(const T& t) {
    append(t);
    return *this;
  }

  void append(char Char, size_t nLen = 0, char c = '0') {
    char Characters[] = {Char, '\0'};
    append(Characters, nLen, c);
  }

  void append(short nInt, size_t nRadix = 10, size_t nLen = 0, char c = '0') {
    if (nInt < 0) {
      append("-");
      unsigned short magnitude = static_cast<unsigned short>(-(nInt + 1));
      append(++magnitude, nRadix, nLen, c);
      return;
    }
    append(static_cast<unsigned short>(nInt), nRadix, nLen, c);
  }

  void append(int nInt, size_t nRadix = 10, size_t nLen = 0, char c = '0') {
    if (nInt < 0) {
      append("-");
      unsigned int magnitude = static_cast<unsigned int>(-(nInt + 1));
      append(++magnitude, nRadix, nLen, c);
      return;
    }
    append(static_cast<unsigned int>(nInt), nRadix, nLen, c);
  }

  void append(long nInt, size_t nRadix = 10, size_t nLen = 0, char c = '0') {
    if (nInt < 0) {
      append("-");
      unsigned long magnitude = static_cast<unsigned long>(-(nInt + 1));
      append(++magnitude, nRadix, nLen, c);
      return;
    }
    append(static_cast<unsigned long>(nInt), nRadix, nLen, c);
  }

  void append(long long nInt, size_t nRadix = 10, size_t nLen = 0, char c = '0') {
    if (nInt < 0) {
      append("-");
      unsigned long long magnitude = static_cast<unsigned long long>(-(nInt + 1));
      append(++magnitude, nRadix, nLen, c);
      return;
    }
    append(static_cast<unsigned long long>(nInt), nRadix, nLen, c);
  }

  void append(unsigned char nInt, size_t nRadix = 10, size_t nLen = 0, char c = '0') {
    appendInteger<sizeof(char)>(nInt, nRadix, nLen, c);
  }

  void append(unsigned short nInt, size_t nRadix = 10, size_t nLen = 0, char c = '0') {
    appendInteger<sizeof(short)>(nInt, nRadix, nLen, c);
  }

  void append(unsigned int nInt, size_t nRadix = 10, size_t nLen = 0, char c = '0') {
    appendInteger<sizeof(int)>(nInt, nRadix, nLen, c);
  }

  void append(unsigned long nInt, size_t nRadix = 10, size_t nLen = 0, char c = '0') {
    appendInteger<sizeof(long)>(nInt, nRadix, nLen, c);
  }

  void append(unsigned long long nInt, size_t nRadix = 10, size_t nLen = 0, char c = '0') {
    appendInteger<sizeof(long long)>(nInt, nRadix, nLen, c);
  }

  template <unsigned int size, typename T>
  void appendInteger(T nInt, size_t nRadix, size_t nLen, char c) {
    if (!canAppend() || (nRadix < 2) || (nRadix > 36)) {
      // cannot append any longer
      return;
    }

    char pStr[size * 8 + 1];
    size_t index = 0;
    do {
      size_t tmp = nInt % nRadix;
      nInt /= nRadix;
      if (tmp < 10)
        pStr[index++] = '0' + tmp;
      else
        pStr[index++] = 'a' + (tmp - 10);
    } while (nInt != 0);

    for (size_t i = 0; i < (index / 2); i++) {
      char tmp = pStr[i];
      pStr[i] = pStr[index - i - 1];
      pStr[index - i - 1] = tmp;
    }

    pStr[index] = '\0';

    append(pStr, nLen, c);
  }

  void append(const char* str, size_t nLen = 0, char c = ' ') {
    if (!str || !canAppend()) {
      return;
    }

    const size_t available = (N - 1) - m_Length;
    size_t stringLength = nLen ? BoundedStringLength(str, nLen) : StringLength(str);
    size_t padding = nLen > stringLength ? nLen - stringLength : 0;
    padding = min(padding, available);

    if (padding) {
      ByteSet(m_pData + m_Length, c, padding);
      m_Length += padding;
    }

    const size_t remaining = (N - 1) - m_Length;
    const size_t copyLength = min(stringLength, remaining);
    MemoryCopy(m_pData + m_Length, str, copyLength);
    m_Length += copyLength;
    m_pData[m_Length] = 0;

    check();

    updateHash();
  }

  void append(const String& str) {
    append(str.cstr(), str.length());
  }

  void appendBytes(const char* bytes, size_t numBytes) {
    if (!canAppend()) {
      // cannot append any longer
      return;
    }

    for (size_t i = 0; i < numBytes; ++i) {
      char c = bytes[i];
      if ((c < -1) || (c >= 0x20 && c != 0x7f)) {
        // normal append (lets utf-8 and others still work in logs)
        append(c);
      } else {
        // render \xXX formatted character code instead of raw character
        append("\\x");
        append(static_cast<unsigned int>(static_cast<unsigned char>(c)), 16, 2);
      }
    }
  }

  template <unsigned int N2>
  void append(const StaticString<N2>& str, size_t nLen = 0, char c = ' ') {
    if (!canAppend()) {
      // cannot append any longer
      return;
    }

    const size_t available = (N - 1) - m_Length;
    const size_t stringLength = nLen ? min(nLen, str.length()) : str.length();
    size_t padding = nLen > stringLength ? nLen - stringLength : 0;
    padding = min(padding, available);
    if (padding) {
      ByteSet(m_pData + m_Length, c, padding);
      m_Length += padding;
    }

    const size_t remaining = (N - 1) - m_Length;
    const size_t copyLength = min(stringLength, remaining);
    MemoryCopy(m_pData + m_Length, static_cast<const char*>(str), copyLength);
    m_Length += copyLength;
    m_pData[m_Length] = 0;

    check();

    updateHash();
  }

  void pad(size_t nLen, char c = ' ') {
    if (!canAppend()) {
      // cannot append any longer
      return;
    }

    // Pad, if needed
    if (nLen > length()) {
      const size_t newLength = min(nLen, static_cast<size_t>(N - 1));
      ByteSet(m_pData + m_Length, c, newLength - m_Length);
      m_Length = newLength;
      m_pData[m_Length] = '\0';
    }

    updateHash();
  }

  size_t length() const {
    return m_Length;
  }

  uint64_t hash() const {
    return m_Hash;
  }

  /**
   * Allow computing hashes for this StaticString object.
   * Can immediately compute the hash of the string, which can be used to
   * disable hashing when performing numerous operations on a StaticString
   * and then enable at the end when modifications cease. This reduces the
   * number of pointless hashes.
   */
  void allowHashing(bool computeNow = false) {
    m_AllowHashes = true;
    if (computeNow) {
      updateHash();
    }
  }

  /** Stop computing hashes for this StaticString object. */
  void disableHashing() {
    m_AllowHashes = false;
  }

 private:
  static bool contains(const char* a, const char* b, size_t alen, size_t blen) {
    return StringContainsN(a, alen, b, blen) == 1;
  }

  void updateHash() {
    if (m_AllowHashes) {
      // sanity check
      STATICSTRING_ASSERT(StringLength(m_pData) == m_Length);

      m_Hash = spookyHash(m_pData, m_Length);
    }
  }

  void check() {
    if (m_Length >= N) {
      m_pData[N - 1] = '\0';
      m_Length = N - 1;
    }
  }

  bool canAppend() const {
    return m_Length < (N - 1);
  }

  /**
   * Our actual static data.
   */
  char m_pData[N];

  size_t m_Length;
  uint64_t m_Hash;

  bool m_AllowHashes;
};

// Specializations for the typedefs below (in StaticString.cc)
extern template class EXPORTED_PUBLIC StaticString<32>;    // IWYU pragma: keep
extern template class EXPORTED_PUBLIC StaticString<64>;    // IWYU pragma: keep
extern template class EXPORTED_PUBLIC StaticString<128>;   // IWYU pragma: keep
extern template class EXPORTED_PUBLIC StaticString<1024>;  // IWYU pragma: keep

typedef StaticString<32> TinyStaticString;
typedef StaticString<64> NormalStaticString;
typedef StaticString<128> LargeStaticString;
typedef StaticString<1024> HugeStaticString;

/** @} */

#endif
