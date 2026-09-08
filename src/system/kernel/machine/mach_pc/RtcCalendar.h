/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_MACHINE_MACH_PC_RTCCALENDAR_H
#define PEDIGREE_KERNEL_MACHINE_MACH_PC_RTCCALENDAR_H

#include "RtcTimeAccounting.h"

namespace RtcCalendar {
inline bool validCenturyIndex(uint8_t index) {
  return index == 0 || (index >= 0x0e && index <= 0x7f);
}

inline bool decode(uint8_t raw, bool binary, uint8_t& value) {
  if (!binary && ((raw & 15) > 9 || (raw >> 4) > 9)) {
    return false;
  }
  value = binary ? raw : (raw >> 4) * 10 + (raw & 15);
  return true;
}

inline uint8_t encode(uint8_t value, bool binary) {
  return binary ? value : ((value / 10) << 4) | (value % 10);
}

inline bool valid(const RtcTimeAccounting::CivilTime& time) {
  return time.year >= 1970 && time.year <= 9999 && time.month >= 1 && time.month <= 12 &&
         time.day >= 1 && time.day <= RtcTimeAccounting::daysInMonth(time.year, time.month) &&
         time.hour < 24 && time.minute < 60 && time.second < 60;
}

// Callers hold the CMOS transaction lock and inhibit updates for the snapshot.
template <typename Read>
bool read(Read readRegister, uint8_t centuryIndex, RtcTimeAccounting::CivilTime& result) {
  if (!validCenturyIndex(centuryIndex) || !(readRegister(0x0d) & 0x80)) {
    return false;
  }
  const uint8_t status = readRegister(0x0b);
  const bool binary = status & 4;
  const bool hours24 = status & 2;
  const uint8_t rawHour = readRegister(4);
  uint8_t year = 0, century = 0;
  RtcTimeAccounting::CivilTime time = {};
  if (!decode(readRegister(0), binary, time.second) ||
      !decode(readRegister(2), binary, time.minute) ||
      !decode(hours24 ? rawHour : (rawHour & 0x7f), binary, time.hour) ||
      !decode(readRegister(7), binary, time.day) || !decode(readRegister(8), binary, time.month) ||
      !decode(readRegister(9), binary, year) || year > 99) {
    return false;
  }
  if (!hours24) {
    if (time.hour < 1 || time.hour > 12) {
      return false;
    }
    time.hour = (time.hour % 12) + ((rawHour & 0x80) ? 12 : 0);
  }
  if (centuryIndex) {
    if (!decode(readRegister(centuryIndex), binary, century) || century > 99) {
      return false;
    }
    time.year = century * 100 + year;
  } else {
    // A missing FADT century field provides only a two-digit year. Keep the
    // interpretation fixed and reversible rather than guessing a CMOS byte.
    time.year = (year >= 70 ? 1900 : 2000) + year;
  }
  if (!valid(time)) {
    return false;
  }
  result = time;
  return true;
}

template <typename Read, typename Write>
bool write(Read readRegister, Write writeRegister, uint8_t centuryIndex,
           const RtcTimeAccounting::CivilTime& time) {
  if (!validCenturyIndex(centuryIndex) || !valid(time) || (!centuryIndex && time.year > 2069)) {
    return false;
  }
  const uint8_t status = readRegister(0x0b);
  const bool binary = status & 4;
  const bool hours24 = status & 2;
  uint8_t hour = time.hour;
  if (!hours24) {
    hour = time.hour % 12;
    if (!hour) {
      hour = 12;
    }
  }
  const uint8_t rawHour = encode(hour, binary) | ((!hours24 && time.hour >= 12) ? 0x80 : 0);
  writeRegister(0, encode(time.second, binary));
  writeRegister(2, encode(time.minute, binary));
  writeRegister(4, rawHour);
  writeRegister(7, encode(time.day, binary));
  writeRegister(8, encode(time.month, binary));
  writeRegister(9, encode(time.year % 100, binary));
  if (centuryIndex) {
    writeRegister(centuryIndex, encode(time.year / 100, binary));
  }
  return true;
}
}  // namespace RtcCalendar

#endif
