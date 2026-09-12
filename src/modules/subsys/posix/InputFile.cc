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
 * ANY DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF,
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "modules/subsys/posix/InputFile.h"
#include "pedigree/kernel/LockGuard.h"

namespace {
constexpr InputManager::CallbackType InputStreamFilter = InputManager::Mouse | InputManager::RawKey;
constexpr size_t InputBufferRecords = 128;
}  // namespace

InputFile::InputFile(String name, size_t inode, Filesystem* filesystem, File* parent, bool endpoint)
    : File(name, 0, 0, 0, inode, filesystem, 0, parent),
      m_Endpoint(endpoint),
      m_Lock(),
      m_Buffer(InputBufferRecords * sizeof(InputManager::InputNotification)) {}

InputFile::~InputFile() {
  bool registered = false;
  {
    LockGuard<Mutex> guard(m_Lock);
    registered = m_Registered;
    m_Registered = false;
  }

  if (registered) {
    InputManager::instance().removeCallback(subscriber, this);
  }
}

bool InputFile::initialise() {
  if (!m_Endpoint) {
    setPermissionsOnly(FILE_UR | FILE_GR | FILE_OR);
    setUidOnly(0);
    setGidOnly(0);
    return true;
  }

  setPermissionsOnly(FILE_UR | FILE_GR | FILE_OR);
  setUidOnly(0);
  setGidOnly(0);

  InputManager::instance().installCallback(InputStreamFilter, subscriber, this, nullptr, 0);
  {
    LockGuard<Mutex> guard(m_Lock);
    m_Registered = true;
  }
  return true;
}

File* InputFile::open() {
  if (m_Endpoint) {
    return this;
  }

  auto* endpoint = new InputFile(getName(), 0, getFilesystem(), getParent(), true);
  if (!endpoint || !endpoint->initialise()) {
    delete endpoint;
    return nullptr;
  }
  return endpoint;
}

uint64_t InputFile::readBytewise(uint64_t location, uint64_t size, uintptr_t buffer,
                                 bool bCanBlock) {
  return m_Buffer.read(reinterpret_cast<uint8_t*>(buffer), size, bCanBlock);
}

uint64_t InputFile::writeBytewise(uint64_t location, uint64_t size, uintptr_t buffer,
                                  bool bCanBlock) {
  return 0;
}

int InputFile::select(bool bWriting, int timeout) {
  if (bWriting) {
    return 0;
  }
  return m_Buffer.canRead(timeout == 1) ? 1 : 0;
}

bool InputFile::retainVfsReference() {
  if (!m_Endpoint) {
    return File::retainVfsReference();
  }

  LockGuard<Mutex> guard(m_Lock);
  ++m_LifetimePins;
  return true;
}

void InputFile::releaseVfsReference() {
  if (!m_Endpoint) {
    File::releaseVfsReference();
    return;
  }

  bool registered = false;
  bool retire = false;
  {
    LockGuard<Mutex> guard(m_Lock);
    if (m_LifetimePins) {
      --m_LifetimePins;
    }
    retire = !m_LifetimePins;
    if (retire) {
      registered = m_Registered;
      m_Registered = false;
    }
  }

  if (registered) {
    InputManager::instance().removeCallback(subscriber, this);
  }
  if (retire) {
    delete this;
  }
}

void InputFile::subscriber(InputManager::InputNotification& notification) {
  if (notification.meta) {
    reinterpret_cast<InputFile*>(notification.meta)->handleInput(notification);
  }
}

void InputFile::handleInput(const InputManager::InputNotification& notification) {
  InputManager::InputNotification copy = notification;
  copy.meta = nullptr;
  if (m_Buffer.tryWrite(reinterpret_cast<const uint8_t*>(&copy), sizeof(copy))) {
    dataChanged();
  }
}
