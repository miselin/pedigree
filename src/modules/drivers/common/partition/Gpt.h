/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef PEDIGREE_GPT_H
#define PEDIGREE_GPT_H
#include <stddef.h>
#include <stdint.h>

namespace Gpt {
constexpr size_t MaxEntries = 4096, MaxArrayBytes = 1024 * 1024;
inline uint64_t little(const uint8_t* data, size_t count) {
  uint64_t value = 0;
  for (size_t i = 0; i < count; ++i)
    value |= static_cast<uint64_t>(data[i]) << (i * 8);
  return value;
}
inline uint32_t crc(const uint8_t* bytes, size_t length, bool header = false) {
  uint32_t value = 0xffffffffU;
  for (size_t i = 0; i < length; ++i) {
    value ^= header && i >= 16 && i < 20 ? 0 : bytes[i];
    for (size_t bit = 0; bit < 8; ++bit)
      value = (value >> 1) ^ ((value & 1) ? 0xedb88320U : 0);
  }
  return ~value;
}
struct Header {
  uint64_t first, last, table;
  size_t count, stride, bytes;
  uint32_t checksum;
};
inline bool decode(const uint8_t* data, size_t sectorBytes, uint64_t sectors, bool backup,
                   Header& result) {
  if (sectorBytes < 512 || sectorBytes > 4096 || (sectorBytes & (sectorBytes - 1)) || sectors < 6 ||
      little(data, 8) != 0x5452415020494645ULL || little(data + 8, 4) != 0x10000 ||
      little(data + 20, 4))
    return false;
  const size_t length = little(data + 12, 4);
  if (length < 92 || length > sectorBytes || crc(data, length, true) != little(data + 16, 4) ||
      little(data + 24, 8) != (backup ? sectors - 1 : 1) ||
      little(data + 32, 8) != (backup ? 1 : sectors - 1))
    return false;
  result.first = little(data + 40, 8);
  result.last = little(data + 48, 8);
  result.table = little(data + 72, 8);
  result.count = little(data + 80, 4);
  result.stride = little(data + 84, 4);
  result.checksum = little(data + 88, 4);
  if (!result.count || result.count > MaxEntries || result.stride < 128 ||
      (result.stride & (result.stride - 1)) || result.stride > MaxArrayBytes / result.count)
    return false;
  result.bytes = result.count * result.stride;
  const size_t blocks = (result.bytes + sectorBytes - 1) / sectorBytes;
  const size_t reserved = (result.bytes < 16384 ? 16384 : result.bytes) / sectorBytes +
                          ((result.bytes < 16384 ? 16384 : result.bytes) % sectorBytes != 0);
  if (result.first < 2 + reserved || result.first > result.last || reserved + 1 >= sectors ||
      result.last >= sectors - reserved - 1 || result.table >= sectors ||
      blocks > sectors - result.table)
    return false;
  return backup ? result.table > result.last && result.table < sectors - 1 &&
                      blocks <= sectors - 1 - result.table
                : result.table > 1 && result.table < result.first &&
                      blocks <= result.first - result.table;
}
inline bool used(const uint8_t* entry) {
  return little(entry, 8) || little(entry + 8, 8);
}
inline void formatGuid(const uint8_t* value, char (&result)[37]) {
  static constexpr char digits[] = "0123456789abcdef";
  static constexpr uint8_t order[] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
  size_t output = 0;
  for (size_t i = 0; i < sizeof(order); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10)
      result[output++] = '-';
    const uint8_t byte = value[order[i]];
    result[output++] = digits[byte >> 4];
    result[output++] = digits[byte & 0xf];
  }
  result[output] = 0;
}
inline bool validEntries(const uint8_t* data, const Header& header) {
  if (crc(data, header.bytes) != header.checksum)
    return false;
  for (size_t i = 0; i < header.count; ++i) {
    const uint8_t* entry = data + i * header.stride;
    if (!used(entry))
      continue;
    const uint64_t start = little(entry + 32, 8), end = little(entry + 40, 8);
    if (start < header.first || start > end || end > header.last ||
        !(little(entry + 16, 8) || little(entry + 24, 8)))
      return false;
    for (size_t j = 0; j < i; ++j) {
      const uint8_t* other = data + j * header.stride;
      if (used(other) && ((start <= little(other + 40, 8) && little(other + 32, 8) <= end) ||
                          (little(entry + 16, 8) == little(other + 16, 8) &&
                           little(entry + 24, 8) == little(other + 24, 8))))
        return false;
    }
  }
  return true;
}
}  // namespace Gpt
class Disk;
bool gptProbeDisk(Disk* disk);
#endif
