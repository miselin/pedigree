/* Copyright (c) 2026, Pedigree Developers. */
#ifndef PEDIGREE_MACHINE_PC_ACPI_S5_H
#define PEDIGREE_MACHINE_PC_ACPI_S5_H

#include <stddef.h>
#include <stdint.h>

// This deliberately accepts only static namespace declarations. Executable AML
// and firmware sleep methods require an interpreter before they can be used.
namespace AcpiS5 {
inline bool mayHaveSleepHooks(const uint8_t* bytes, size_t length) {
  // False positives in strings or nested objects are acceptable: without an
  // interpreter we must not silently skip firmware's orderly S5 preparation.
  for (size_t i = 0; i + 4 <= length; ++i) {
    if (bytes[i] == '_' && bytes[i + 2] == 'T' && bytes[i + 3] == 'S' &&
        (bytes[i + 1] == 'P' || bytes[i + 1] == 'T' || bytes[i + 1] == 'G'))
      return true;
  }
  return false;
}

inline bool package(const uint8_t*& cursor, const uint8_t* end, const uint8_t*& packageEnd) {
  if (cursor == end)
    return false;
  const uint8_t* start = cursor;
  const uint8_t lead = *cursor++;
  const unsigned extra = lead >> 6;
  if (size_t(end - cursor) < extra || (extra && (lead & 0x30)))
    return false;
  size_t length = lead & (extra ? 0x0f : 0x3f);
  for (unsigned i = 0; i < extra; ++i)
    length |= size_t(*cursor++) << (4 + 8 * i);
  if (length < size_t(cursor - start) || length > size_t(end - start))
    return false;
  packageEnd = start + length;
  return true;
}

inline bool integer(const uint8_t*& cursor, const uint8_t* end, uint64_t& value) {
  if (cursor == end)
    return false;
  const uint8_t op = *cursor++;
  if (op == 0 || op == 1 || op == 0xff) {
    value = op == 0xff ? UINT64_MAX : op;
    return true;
  }
  const unsigned bytes = op == 0x0a ? 1 : op == 0x0b ? 2 : op == 0x0c ? 4 : op == 0x0e ? 8 : 0;
  if (!bytes || size_t(end - cursor) < bytes)
    return false;
  value = 0;
  for (unsigned i = 0; i < bytes; ++i)
    value |= uint64_t(*cursor++) << (8 * i);
  return true;
}

struct Name {
  bool root;
  bool s5;
  bool rootScope;
};

inline bool name(const uint8_t*& cursor, const uint8_t* end, bool atRoot, Name& result) {
  if (cursor == end)
    return false;
  bool root = atRoot;
  if (*cursor == '\\') {
    root = true;
    ++cursor;
  }
  while (cursor != end && *cursor == '^') {
    root = false;
    ++cursor;
  }
  if (cursor == end)
    return false;
  unsigned count = 1;
  if (*cursor == 0) {
    count = 0;
    ++cursor;
  } else if (*cursor == 0x2e) {
    count = 2;
    ++cursor;
  } else if (*cursor == 0x2f) {
    if (++cursor == end)
      return false;
    count = *cursor++;
    if (!count)
      return false;
  }
  if (size_t(end - cursor) < count * 4)
    return false;
  result = {root,
            root && count == 1 && cursor[0] == '_' && cursor[1] == 'S' && cursor[2] == '5' &&
                cursor[3] == '_',
            root && count == 0};
  for (unsigned i = 0; i < count * 4; ++i) {
    const uint8_t c = *cursor++;
    if (c != '_' && !(c >= 'A' && c <= 'Z') && !(i % 4 && c >= '0' && c <= '9'))
      return false;
  }
  return true;
}

inline bool data(const uint8_t*& cursor, const uint8_t* end) {
  if (cursor == end)
    return false;
  const uint8_t op = *cursor;
  if (op == 0x11 || op == 0x12 || op == 0x13) {
    ++cursor;
    const uint8_t* packageEnd;
    if (!package(cursor, end, packageEnd))
      return false;
    cursor = packageEnd;
    return true;
  }
  if (op == 0x0d) {
    while (++cursor != end)
      if (*cursor == 0) {
        ++cursor;
        return true;
      }
    return false;
  }
  if (op == 0 || op == 1 || op == 0xff || op == 0x0a || op == 0x0b || op == 0x0c || op == 0x0e) {
    uint64_t value;
    return integer(cursor, end, value);
  }
  Name ignored;
  return name(cursor, end, false, ignored);
}

inline bool find(const uint8_t* cursor, const uint8_t* end, uint8_t& typeA, uint8_t& typeB,
                 bool atRoot = true, unsigned depth = 0) {
  if (depth > 16)
    return false;
  while (cursor != end) {
    const uint8_t op = *cursor++;
    Name object;
    if (op == 0x08) {
      if (!name(cursor, end, atRoot, object) || cursor == end)
        return false;
      if (object.s5 && *cursor == 0x12) {
        ++cursor;
        const uint8_t* packageEnd;
        if (!package(cursor, end, packageEnd) || cursor == packageEnd)
          return false;
        const unsigned count = *cursor++;
        uint64_t a, b;
        if (count < 2 || !integer(cursor, packageEnd, a) || !integer(cursor, packageEnd, b) ||
            a > 7 || b > 7)
          return false;
        for (unsigned i = 2; i < count; ++i) {
          uint64_t reserved;
          if (!integer(cursor, packageEnd, reserved))
            return false;
        }
        if (cursor != packageEnd)
          return false;
        typeA = a;
        typeB = b;
        return true;
      }
      if (!data(cursor, end))
        return false;
    } else if (op == 0x10 || op == 0x14 || op == 0xa0 || op == 0xa1 || op == 0xa2) {
      const uint8_t* packageEnd;
      if (!package(cursor, end, packageEnd))
        return false;
      if (op == 0x10) {
        if (!name(cursor, packageEnd, atRoot, object))
          return false;
        if (find(cursor, packageEnd, typeA, typeB, object.rootScope, depth + 1))
          return true;
      }
      cursor = packageEnd;
    } else if (op == 0x06) {
      if (!name(cursor, end, atRoot, object) || !name(cursor, end, atRoot, object))
        return false;
    } else if (op == 0x15) {
      if (!name(cursor, end, atRoot, object) || end - cursor < 2)
        return false;
      cursor += 2;
    } else if (op == 0x5b) {
      if (cursor == end)
        return false;
      const uint8_t ext = *cursor++;
      if (ext >= 0x81 && ext <= 0x87) {
        const uint8_t* packageEnd;
        if (!package(cursor, end, packageEnd))
          return false;
        cursor = packageEnd;
      } else if (ext == 0x80 || ext == 0x01 || ext == 0x02) {
        if (!name(cursor, end, atRoot, object))
          return false;
        if (ext != 0x02) {
          if (cursor == end)
            return false;
          ++cursor;
        }
        if (ext == 0x80 && (!data(cursor, end) || !data(cursor, end)))
          return false;
      } else {
        return false;
      }
    } else if (op != 0xa3) {
      return false;
    }
  }
  return false;
}
}  // namespace AcpiS5
#endif
