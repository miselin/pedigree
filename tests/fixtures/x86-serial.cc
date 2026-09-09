/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include <cassert>
#include <cstring>
#include "Serial.h"

int main(int argc, char** argv) {
  assert(argc == 2);
  IoPort::present = std::strcmp(argv[1], "absent") != 0;
  IoPort::allocates = std::strcmp(argv[1], "allocation-failure") != 0;
  IoPort::registers[serial::scratch] = 0x36;
  X86Serial port;
  port.setBase(0x3f8);
  IoPort::dataWrites = 0;
  if (!IoPort::present || !IoPort::allocates) {
    assert(!port.hasData());
    assert(port.read() == 0);
    assert(port.readNonBlock() == 0);
    port.write('x');
    assert(IoPort::dataWrites == 0);
    return 0;
  }

  // A local UART remains usable without modem handshake inputs or a cable.
  assert(IoPort::registers[serial::scratch] == 0x36);
  IoPort::registers[serial::mstat] = 0;
  IoPort::registers[serial::lstat] = 0x60;
  assert(!port.hasData());
  assert(port.readNonBlock() == 0);
  port.write('x');
  assert(IoPort::dataWrites == 1);
  assert(IoPort::registers[serial::rxtx] == 'x');
  IoPort::registers[serial::lstat] = 0x61;
  IoPort::registers[serial::rxtx] = 'k';
  assert(port.hasData());
  assert(port.readNonBlock() == 'k');
  assert(port.read() == 'k');
  IoPort::registers[serial::lstat] = 0;
  IoPort::reads = 0;
  port.write('z');
  assert(IoPort::dataWrites == 1);
  assert(IoPort::reads <= 100010);
}
