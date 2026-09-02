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

#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Serial.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/time/Time.h"

#include "modules/Module.h"
#include "modules/system/network-stack/Filter.h"

#define PCAP_MAGIC 0xa1b2c3d4

#define PCAP_MAJOR 2
#define PCAP_MINOR 4

#define PCAP_NETWORK 1

static size_t g_FilterEntry = 0;

static Mutex g_PcapMutex;

static void writePcapUint16(Serial* pSerial, uint16_t value) {
  pSerial->write(static_cast<char>(value & 0xff));
  pSerial->write(static_cast<char>((value >> 8) & 0xff));
}

static void writePcapUint32(Serial* pSerial, uint32_t value) {
  pSerial->write(static_cast<char>(value & 0xff));
  pSerial->write(static_cast<char>((value >> 8) & 0xff));
  pSerial->write(static_cast<char>((value >> 16) & 0xff));
  pSerial->write(static_cast<char>((value >> 24) & 0xff));
}

static Serial* getSerial() PURE;
static Serial* getSerial() {
  // Ignore if we don't have a machine abstraction yet.
  if (!Machine::instance().isInitialised()) {
    return 0;
  }

  // Serial port to write PCAP data to.
  if (Machine::instance().getNumSerial() < 3) {
    return 0;
  }

  return Machine::instance().getSerial(2);
}

bool pcapLogPacket(uintptr_t packet, size_t size) {
  LockGuard<Mutex> guard(g_PcapMutex);

  Serial* pSerial = getSerial();
  if (!pSerial) {
    return true;
  }

  // 256K is the max packet we ever want to capture.
  if (size >= 262144) {
    ERROR("pcap: packet is way too big - size is " << size);
    return true;  // don't write to the serial port at all.
  }

  static uint64_t time = 0;

  // Time::Timestamp time = Time::getTimeNanoseconds();

  // Don't care about timing in Wireshark but do care about ordering, and
  // so we want timestamps to always increase.
  time += Time::Multiplier::Millisecond;

  // PCAP readers use the magic to determine byte order. Emit one canonical
  // little-endian representation rather than relying on the target ABI.
  writePcapUint32(pSerial, static_cast<uint32_t>(time / Time::Multiplier::Second));
  writePcapUint32(pSerial, static_cast<uint32_t>((time % Time::Multiplier::Second) /
                                                 Time::Multiplier::Microsecond));
  writePcapUint32(pSerial, static_cast<uint32_t>(size));
  writePcapUint32(pSerial, static_cast<uint32_t>(size));

  // Write the packet data now.
  const uint8_t* data = reinterpret_cast<const uint8_t*>(packet);
  for (size_t i = 0; i < size; ++i) {
    pSerial->write(data[i]);
  }

  // Always let the packet through.
  return true;
}

static bool entry() {
  LockGuard<Mutex> guard(g_PcapMutex);

  Serial* pSerial = getSerial();
  if (!pSerial) {
    NOTICE("pcap: could not find a useful serial port");
    return false;
  }

  g_FilterEntry = NetworkFilter::instance().installCallback(1, pcapLogPacket);
  if (g_FilterEntry == static_cast<size_t>(-1)) {
    NOTICE("pcap: could not install callback");
    return false;
  }

  writePcapUint32(pSerial, PCAP_MAGIC);
  writePcapUint16(pSerial, PCAP_MAJOR);
  writePcapUint16(pSerial, PCAP_MINOR);
  writePcapUint32(pSerial, 0);  // UTC timezone
  writePcapUint32(pSerial, 0);  // timestamp accuracy is unspecified
  writePcapUint32(pSerial, 0xFFFF);
  writePcapUint32(pSerial, PCAP_NETWORK);

  return true;
}

static void exit() {
  NetworkFilter::instance().removeCallback(1, g_FilterEntry);
}

MODULE_INFO("pcap", &entry, &exit, "network-stack");
