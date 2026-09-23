/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "Keyboard.h"

#include "Serial.h"

VirtKeyboard::VirtKeyboard(VirtSerial& serial) : m_Serial(serial), m_DebugState(false) {}

void VirtKeyboard::initialise() {}

void VirtKeyboard::setDebugState(bool enabled) {
  m_DebugState = enabled;
}

bool VirtKeyboard::getDebugState() {
  return m_DebugState;
}

char VirtKeyboard::getChar() {
  return m_DebugState ? m_Serial.read() : 0;
}

char VirtKeyboard::getCharNonBlock() {
  return m_DebugState ? m_Serial.readNonBlock() : 0;
}
