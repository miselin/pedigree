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

#include "modules/drivers/common/hid/HidUtils.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/machine/HidInputManager.h"
#include "pedigree/kernel/machine/InputManager.h"

#include "modules/drivers/common/hid/HidUsages.h"

Spinlock HidUtils::InputState::m_Lock;
HidUtils::InputState* HidUtils::InputState::m_pInputs = nullptr;

HidUtils::InputState::InputState()
    : m_pNext(nullptr), m_Keys{}, m_MouseButtons(0), m_JoystickButtons(0) {
  LockGuard<Spinlock> guard(m_Lock);
  m_pNext = m_pInputs;
  m_pInputs = this;
}

HidUtils::InputState::~InputState() {
  LockGuard<Spinlock> guard(m_Lock);
  for (size_t key = 0; key < 256; ++key) {
    if (m_Keys[key / 64] & (uint64_t{1} << (key % 64))) {
      HidInputManager::instance().keyUp(key, this);
    }
  }
  InputState** input = &m_pInputs;
  while (*input != this) {
    input = &(*input)->m_pNext;
  }
  *input = m_pNext;
  if (m_MouseButtons) {
    InputManager::instance().mouseUpdate(0, 0, 0, buttons(Mouse));
  }
  if (m_JoystickButtons) {
    InputManager::instance().joystickUpdate(0, 0, 0, buttons(Joystick));
  }
}

uint32_t HidUtils::InputState::buttons(HidDeviceType deviceType) {
  uint32_t bitmap = 0;
  for (InputState* input = m_pInputs; input; input = input->m_pNext) {
    bitmap |= deviceType == Mouse ? input->m_MouseButtons : input->m_JoystickButtons;
  }
  return bitmap;
}

uint64_t HidUtils::getBufferField(uint8_t* pBuffer, size_t nStart, size_t nLength) {
  if (nLength > 64)
    return 0;
  uint64_t value = 0;
  for (size_t i = 0; i < nLength; ++i)
    value |= uint64_t{(pBuffer[(nStart + i) / 8] >> ((nStart + i) % 8)) & 1U} << i;
  return value;
}

void HidUtils::fixNegativeMinimum(int64_t& nMin, int64_t nMax) {
  // Ignore the call if nMax is larger than nMin (no sign bit set in nMin)
  if (nMax > nMin)
    return;

  // Check for all possible sign bits
  // NOTE this was "nMin = -((1 << bitNum) - nMin)" originally, now may seem
  // confusing
  if (nMin < (1LL << 8) && nMin & (1LL << 7))         // Signed 8-bit
    nMin -= 1LL << 8;                                 // Turn nMin negative
  else if (nMin < (1LL << 16) && nMin & (1LL << 15))  // Signed 16-bit
    nMin -= 1LL << 16;                                // Turn nMin negative
  else if (nMin < (1LL << 32) && nMin & (1LL << 31))  // Signed 32-bit
    nMin -= 1LL << 32;                                // Turn nMin negative
}

void HidUtils::fixNegativeValue(int64_t nMin, int64_t nMax, int64_t& nValue) {
  // Ignore the call if nMin is not negative at all
  if (nMin >= 0)
    return;

  // Ignore the call if nMax is larger than nValue (no sign bit set in nValue)
  if (nMax > nValue)
    return;

  // Check for all possible sign bits
  if (nMin > -(1LL << 8) && nValue < (1LL << 8) && nValue & (1LL << 7))  // Signed 8-bit
    nValue -= 1LL << 8;                                                  // Turn nValue negative
  else if (nMin > -(1LL << 16) && nValue < (1LL << 16) && nValue & (1LL << 15))  // Signed 16-bit
    nValue -= 1LL << 16;  // Turn nValue negative
  else if (nMin > -(1LL << 32) && nValue < (1LL << 32) && nValue & (1LL << 31))  // Signed 32-bit
    nValue -= 1LL << 32;  // Turn nValue negative
}

void HidUtils::sendInputToManager(HidDeviceType deviceType, uint16_t nUsagePage, uint16_t nUsage,
                                  int64_t nRelativeValue, InputState& state) {
  LockGuard<Spinlock> guard(InputState::m_Lock);

  // Is this a key on a keyboard/keypad?
  if ((deviceType == Keyboard) && (nUsagePage == HidUsagePages::Keyboard)) {
    if (!nUsage || nUsage > 255) {
      return;
    }
    const uint64_t bit = uint64_t{1} << (nUsage % 64);
    if (nRelativeValue > 0) {
      state.m_Keys[nUsage / 64] |= bit;
      HidInputManager::instance().keyDown(nUsage, &state);
    } else {
      state.m_Keys[nUsage / 64] &= ~bit;
      HidInputManager::instance().keyUp(nUsage, &state);
    }
  }

  const uint32_t mouseButtons = InputState::buttons(Mouse);
  const uint32_t joystickButtons = InputState::buttons(Joystick);
  // Is this an axis on a mouse?
  if ((deviceType == Mouse) && (nUsagePage == HidUsagePages::GenericDesktop)) {
    if (nUsage == HidUsages::X)
      InputManager::instance().mouseUpdate(nRelativeValue, 0, 0, mouseButtons);
    if (nUsage == HidUsages::Y)
      InputManager::instance().mouseUpdate(0, nRelativeValue, 0, mouseButtons);
    if (nUsage == HidUsages::Wheel)
      InputManager::instance().mouseUpdate(0, 0, nRelativeValue, mouseButtons);
  }

  // Is this an axis on a joystick?
  if ((deviceType == Joystick) && (nUsagePage == HidUsagePages::GenericDesktop)) {
    if (nUsage == HidUsages::X)
      InputManager::instance().joystickUpdate(nRelativeValue, 0, 0, joystickButtons);
    if (nUsage == HidUsages::Y)
      InputManager::instance().joystickUpdate(0, nRelativeValue, 0, joystickButtons);
    if (nUsage == HidUsages::Z)
      InputManager::instance().joystickUpdate(0, 0, nRelativeValue, joystickButtons);
  }

  // Is this a button on a mouse?
  if ((deviceType == Mouse) && (nUsagePage == HidUsagePages::Button)) {
    if (!nUsage || nUsage > 32) {
      return;
    }
    // Set/unset the bit in the bitmap
    if (nRelativeValue > 0) {
      state.m_MouseButtons |= uint32_t{1} << (nUsage - 1);
    } else {
      state.m_MouseButtons &= ~(uint32_t{1} << (nUsage - 1));
    }

    // Send the new bitmap to the input manager
    InputManager::instance().mouseUpdate(0, 0, 0, InputState::buttons(Mouse));
  }

  // Is this a button on a joystick?
  if ((deviceType == Joystick) && (nUsagePage == HidUsagePages::Button)) {
    if (!nUsage || nUsage > 32) {
      return;
    }
    // Set/unset the bit in the bitmap
    if (nRelativeValue > 0) {
      state.m_JoystickButtons |= uint32_t{1} << (nUsage - 1);
    } else {
      state.m_JoystickButtons &= ~(uint32_t{1} << (nUsage - 1));
    }

    // Send the new bitmap to the input manager
    InputManager::instance().joystickUpdate(0, 0, 0, InputState::buttons(Joystick));
  }
}
