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
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef POSIX_INPUTFILE_H
#define POSIX_INPUTFILE_H

#include "pedigree/kernel/machine/InputManager.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/utilities/Buffer.h"

#include "linux-input-abi.h"
#include "modules/system/vfs/File.h"

/** A pollable, per-open stream of raw keyboard and mouse notifications. */
class InputFile final : public File {
 public:
  InputFile(String name, size_t inode, Filesystem* filesystem, File* parent, bool endpoint = false);
  ~InputFile() override;

  bool initialise();

  File* open() override;

  uint64_t readBytewise(uint64_t location, uint64_t size, uintptr_t buffer,
                        bool bCanBlock = true) override;
  uint64_t writeBytewise(uint64_t location, uint64_t size, uintptr_t buffer,
                         bool bCanBlock = true) override;
  int select(bool bWriting = false, int timeout = 0) override;

  bool isSeekable() const override {
    return false;
  }

  bool retainVfsReference() override;
  void releaseVfsReference() override;

 private:
  static void subscriber(InputManager::InputNotification& notification);
  void handleInput(const InputManager::InputNotification& notification);

  bool isBytewise() const override {
    return true;
  }

  const bool m_Endpoint;
  Mutex m_Lock;
  Buffer<uint8_t> m_Buffer;
  size_t m_LifetimePins = 0;
  bool m_Registered = false;
};

/** A Linux evdev-compatible keyboard or pointer endpoint. */
class EvdevFile final : public File {
 public:
  enum DeviceType { Keyboard, Pointer, AbsolutePointer };

  EvdevFile(String name, size_t inode, Filesystem* filesystem, File* parent, DeviceType type,
            bool endpoint = false);
  ~EvdevFile() override;

  bool initialise();
  File* open() override;

  uint64_t readBytewise(uint64_t location, uint64_t size, uintptr_t buffer,
                        bool bCanBlock = true) override;
  uint64_t writeBytewise(uint64_t location, uint64_t size, uintptr_t buffer,
                         bool bCanBlock = true) override;
  int select(bool bWriting = false, int timeout = 0) override;
  bool supports(size_t command) const override;
  int command(size_t command, void* buffer) override;

  bool isSeekable() const override {
    return false;
  }

  bool retainVfsReference() override;
  void releaseVfsReference() override;

 private:
  static void subscriber(InputManager::InputNotification& notification);
  void handleInput(const InputManager::InputNotification& notification);
  void emit(const LinuxInputEvent* events, size_t count);

  bool isBytewise() const override {
    return true;
  }

  const DeviceType m_Type;
  const bool m_Endpoint;
  Mutex m_Lock;
  Buffer<uint8_t> m_Buffer;
  uint8_t m_KeyState[96];
  bool m_ButtonState[5];
  uint32_t m_AbsoluteX = 0;
  uint32_t m_AbsoluteY = 0;
  bool m_HaveAbsolute = false;
  size_t m_LifetimePins = 0;
  bool m_Registered = false;
};

#endif
