/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Log.h"

#include "modules/drivers/x86/ps2mouse/Ps2MousePacket.h"

namespace {
bool check(bool condition, const char* detail) {
  if (condition) {
    return true;
  }

  ERROR("HOSTED-WAIT-TEST: FAIL ps2mouse-packet-decoder: " << detail);
  return false;
}

bool packetDecoder() {
  Ps2MousePacketDecoder decoder;
  Ps2MousePacket packet = {};
  bool passed = true;

  passed &= check(!decoder.feed(0x00, packet), "a byte without the packet sync bit was accepted");
  passed &= check(!decoder.feed(0x2D, packet), "the first packet byte completed a packet");
  passed &= check(!decoder.feed(0x01, packet), "the second packet byte completed a packet");
  passed &= check(decoder.feed(0xFF, packet), "a complete packet was not emitted");
  passed &= check(
      packet.relativeX == 1 && packet.relativeY == -1 && packet.buttons == 0x3 && !packet.overflow,
      "signed motion or left/middle button mapping was incorrect");

  decoder.reset();
  passed &= check(!decoder.feed(0x18, packet) && !decoder.feed(0x00, packet) &&
                      decoder.feed(0x00, packet) && packet.relativeX == -256,
                  "the ninth X movement bit was not decoded");

  decoder.reset();
  passed &= check(!decoder.feed(0x28, packet) && !decoder.feed(0x00, packet) &&
                      decoder.feed(0x00, packet) && packet.relativeY == -256,
                  "the ninth Y movement bit was not decoded");

  decoder.reset();
  passed &= check(!decoder.feed(0x48, packet) && !decoder.feed(0xFF, packet) &&
                      decoder.feed(0x02, packet) && packet.relativeX == 0 &&
                      packet.relativeY == 2 && packet.overflow,
                  "overflow packets were not reported safely");

  decoder.reset();
  passed &= check(!decoder.feed(0x0A, packet) && !decoder.feed(0x00, packet) &&
                      decoder.feed(0x00, packet) && packet.buttons == 0x4,
                  "the PS/2 right button was not mapped to input button three");

  decoder.reset();
  passed &=
      check(!decoder.feed(0x08, packet) && !decoder.feed(0xFA, packet) &&
                decoder.feed(0xFE, packet) && packet.relativeX == 250 && packet.relativeY == 254,
            "movement bytes that resemble command replies were discarded");

  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS ps2mouse-packet-decoder");
  }
  return passed;
}
}  // namespace

bool runHostedPs2MousePacketRegressions() {
  return packetDecoder();
}
