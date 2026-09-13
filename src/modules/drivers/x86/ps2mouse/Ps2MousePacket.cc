/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "Ps2MousePacket.h"

namespace {
constexpr uint8_t StatusLeftButton = 1 << 0;
constexpr uint8_t StatusRightButton = 1 << 1;
constexpr uint8_t StatusMiddleButton = 1 << 2;
constexpr uint8_t StatusAlwaysOne = 1 << 3;
constexpr uint8_t StatusXSign = 1 << 4;
constexpr uint8_t StatusYSign = 1 << 5;
constexpr uint8_t StatusXOverflow = 1 << 6;
constexpr uint8_t StatusYOverflow = 1 << 7;

ssize_t decodeMovement(uint8_t movement, uint8_t status, uint8_t signBit) {
  uint16_t value = movement;
  if (status & signBit) {
    value |= 0x100;
  }

  if (value & 0x100) {
    return static_cast<ssize_t>(value) - 0x200;
  }
  return static_cast<ssize_t>(value);
}
}  // namespace

Ps2MousePacketDecoder::Ps2MousePacketDecoder() : m_Buffer(), m_BufferIndex(0) {}

void Ps2MousePacketDecoder::reset() {
  m_BufferIndex = 0;
}

bool Ps2MousePacketDecoder::feed(uint8_t byte, Ps2MousePacket& packet) {
  // Bit 3 is permanently set in the first byte of a standard PS/2 packet.
  // Discarding until it is seen lets the stream recover from stray bytes.
  if (m_BufferIndex == 0 && !(byte & StatusAlwaysOne)) {
    return false;
  }

  m_Buffer[m_BufferIndex++] = byte;
  if (m_BufferIndex != 3) {
    return false;
  }

  const uint8_t status = m_Buffer[0];
  const bool xOverflow = status & StatusXOverflow;
  const bool yOverflow = status & StatusYOverflow;

  packet.relativeX = xOverflow ? 0 : decodeMovement(m_Buffer[1], status, StatusXSign);
  packet.relativeY = yOverflow ? 0 : decodeMovement(m_Buffer[2], status, StatusYSign);
  packet.overflow = xOverflow || yOverflow;

  // InputManager numbers buttons left, middle, right. The PS/2 status byte
  // numbers its middle and right buttons in the opposite order.
  packet.buttons = 0;
  if (status & StatusLeftButton) {
    packet.buttons |= 1 << 0;
  }
  if (status & StatusMiddleButton) {
    packet.buttons |= 1 << 1;
  }
  if (status & StatusRightButton) {
    packet.buttons |= 1 << 2;
  }

  m_BufferIndex = 0;
  return true;
}
