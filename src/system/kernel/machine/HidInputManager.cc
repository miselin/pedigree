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
#include "pedigree/kernel/machine/HidInputManager.h"
#include "pedigree/kernel/machine/InputManager.h"
#include "pedigree/kernel/machine/KeymapManager.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Timer.h"
#include "pedigree/kernel/utilities/Iterator.h"
#include "pedigree/kernel/utilities/utility.h"

HidInputManager HidInputManager::m_Instance;

HidInputManager::HidInputManager() : m_pTimer(nullptr), m_NextArmed(0) {}

HidInputManager::~HidInputManager() {
  Timer* timer = nullptr;
  {
    LockGuard<NoIrqSpinlock> guard(m_KeyLock);
    timer = m_pTimer;
    m_pTimer = nullptr;
  }
  if (timer && !timer->unregisterHandler(this)) {
    FATAL("HidInputManager could not drain its timer callback");
  }
}

void HidInputManager::keyDown(uint8_t keyCode) {
  KeymapManager& keymapManager = KeymapManager::instance();

  InputManager::instance().rawKeyUpdate(keyCode, false);

  // Check for modifiers
  if (keymapManager.handleHidModifier(keyCode, true)) {
    updateKeys();
    return;
  }

  LockGuard<NoIrqSpinlock> guard(m_KeyLock);

  // Is the key already considered "down"?
  if (!m_KeyStates.lookup(keyCode)) {
    // If there was no key before, register the timer handler
    if (!m_KeyStates.count() && !m_pTimer) {
      Timer* timer = Machine::instance().getTimer();
      if (timer && timer->registerHandler(this)) {
        m_pTimer = timer;
      } else {
        ERROR("HidInputManager could not register key repeat");
      }
    }

    // Resolve the key
    uint64_t key = keymapManager.resolveHidKeycode(keyCode);

    // Create a key state structure and fill it
    KeyState* keyState = new KeyState;
    keyState->key = key;
    keyState->nextRepeat = Time::getTicks() + 600000000;

    // Insert the key state
    m_KeyStates.insert(keyCode, keyState);
    armNextRepeatLocked();

    // First keypress always sent straight away, repeating keystrokes
    // are transferred as necessary
    if (key)
      InputManager::instance().keyPressed(key);
  }
}

void HidInputManager::keyUp(uint8_t keyCode) {
  KeymapManager& keymapManager = KeymapManager::instance();

  InputManager::instance().rawKeyUpdate(keyCode, true);

  // Check for modifiers
  if (keymapManager.handleHidModifier(keyCode, false)) {
    updateKeys();
    return;
  }

  LockGuard<NoIrqSpinlock> guard(m_KeyLock);

  // Is the key actually pressed?
  KeyState* keyState = m_KeyStates.lookup(keyCode);
  if (keyState) {
    // Remove the key from the key states tree
    m_KeyStates.remove(keyCode);
    // Delete the key state structure
    delete keyState;
    armNextRepeatLocked();
  }
}

void HidInputManager::timer(uint64_t delta) {
  (void)delta;
  LockGuard<NoIrqSpinlock> guard(m_KeyLock);

  const uint64_t now = Time::getTicks();
  for (Tree<uint8_t, KeyState*>::Iterator it = m_KeyStates.begin(); it != m_KeyStates.end(); ++it) {
    KeyState* keyState = it.value();
    if (keyState->nextRepeat <= now) {
      keyState->nextRepeat = now + 40000000;
      if (keyState->key)
        InputManager::instance().keyPressed(keyState->key);
    }
  }

  armNextRepeatLocked();

  // If we've got no more keys being held down, release the handler
  if (!m_KeyStates.count() && m_pTimer && !m_pTimer->supportsDeadlines()) {
    Timer* timer = m_pTimer;
    m_pTimer = nullptr;

    // Self-removal is intentionally deferred until this callback returns.
    // A new key can reactivate the same slot after m_KeyLock is released.
    timer->unregisterHandler(this);
  }
}

void HidInputManager::armNextRepeatLocked() {
  if (!m_pTimer || !m_pTimer->supportsDeadlines()) {
    return;
  }
  uint64_t next = 0;
  for (Tree<uint8_t, KeyState*>::Iterator it = m_KeyStates.begin(); it != m_KeyStates.end(); ++it) {
    const uint64_t deadline = it.value()->nextRepeat;
    if (!next || deadline < next) {
      next = deadline;
    }
  }
  if (next == m_NextArmed) {
    return;
  }
  if (!m_pTimer->armHandler(this, next)) {
    FATAL("HidInputManager could not arm key repeat");
  }
  m_NextArmed = next;
}

void HidInputManager::updateKeys() {
  LockGuard<NoIrqSpinlock> guard(m_KeyLock);

  KeymapManager& keymapManager = KeymapManager::instance();
  for (Tree<uint8_t, KeyState*>::Iterator it = m_KeyStates.begin(); it != m_KeyStates.end(); ++it) {
    KeyState* keyState = it.value();
    keyState->key = keymapManager.resolveHidKeycode(it.key());
  }
}
