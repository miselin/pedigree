/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_PS2_MOUSE_PACKET_H
#define PEDIGREE_PS2_MOUSE_PACKET_H

#include "pedigree/kernel/processor/types.h"

struct Ps2MousePacket {
  ssize_t relativeX;
  ssize_t relativeY;
  uint32_t buttons;
  bool overflow;
};

/** Decodes the standard three-byte PS/2 mouse packet format. */
class Ps2MousePacketDecoder {
 public:
  Ps2MousePacketDecoder();

  void reset();

  /**
   * Feeds one byte into the decoder.
   *
   * Returns true when packet contains a complete decoded packet.
   */
  bool feed(uint8_t byte, Ps2MousePacket& packet);

 private:
  uint8_t m_Buffer[3];
  size_t m_BufferIndex;
};

#endif
