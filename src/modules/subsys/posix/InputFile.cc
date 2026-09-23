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
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/time/Time.h"

#include "modules/subsys/posix/PosixSubsystem.h"

namespace {
constexpr InputManager::CallbackType InputStreamFilter =
    InputManager::Mouse | InputManager::AbsoluteMouse | InputManager::RawKey | InputManager::Key;
constexpr size_t InputBufferRecords = 128;

constexpr size_t LinuxInputBufferRecords = 256;
constexpr uint16_t LinuxBusVirtual = 0x06;
constexpr int LinuxEvdevVersion = 0x010001;

size_t ioctlSize(size_t command) {
  return (command >> 16) & 0x3fff;
}

uint8_t ioctlNumber(size_t command) {
  return command & 0xff;
}

uint8_t ioctlDirection(size_t command) {
  return command >> 30;
}

bool isEvdevIoctl(size_t command) {
  return ((command >> 8) & 0xff) == 'E';
}

void setBit(uint8_t* bitmap, size_t bytes, size_t bit) {
  if (bit / 8 < bytes) {
    bitmap[bit / 8] |= static_cast<uint8_t>(1U << (bit % 8));
  }
}

uint16_t linuxKeyCode(uint8_t usage) {
  static constexpr uint16_t letters[] = {
      30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50,
      49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44,
  };
  if (usage >= 4 && usage <= 29) {
    return letters[usage - 4];
  }
  if (usage >= 30 && usage <= 38) {
    return usage - 28;
  }
  if (usage == 39) {
    return 11;
  }
  if (usage >= 58 && usage <= 67) {
    return usage + 1;
  }

  switch (usage) {
    case 40:
      return 28;
    case 41:
      return 1;
    case 42:
      return 14;
    case 43:
      return 15;
    case 44:
      return 57;
    case 45:
      return 12;
    case 46:
      return 13;
    case 47:
      return 26;
    case 48:
      return 27;
    case 49:
      return 43;
    case 50:
      return 86;
    case 51:
      return 39;
    case 52:
      return 40;
    case 53:
      return 41;
    case 54:
      return 51;
    case 55:
      return 52;
    case 56:
      return 53;
    case 57:
      return 58;
    case 68:
      return 87;
    case 69:
      return 88;
    case 70:
      return 99;
    case 71:
      return 70;
    case 72:
      return 119;
    case 73:
      return 110;
    case 74:
      return 102;
    case 75:
      return 104;
    case 76:
      return 111;
    case 77:
      return 107;
    case 78:
      return 109;
    case 79:
      return 106;
    case 80:
      return 105;
    case 81:
      return 108;
    case 82:
      return 103;
    case 83:
      return 69;
    case 224:
      return 29;
    case 225:
      return 42;
    case 226:
      return 56;
    case 227:
      return 125;
    case 228:
      return 97;
    case 229:
      return 54;
    case 230:
      return 100;
    case 231:
      return 126;
    default:
      return 0;
  }
}

uint16_t linuxButtonCode(size_t button) {
  static constexpr uint16_t buttons[] = {
      LinuxBtnLeft, LinuxBtnMiddle, LinuxBtnRight, LinuxBtnSide, LinuxBtnExtra,
  };
  return button < sizeof(buttons) / sizeof(buttons[0]) ? buttons[button] : 0;
}
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

EvdevFile::EvdevFile(String name, size_t inode, Filesystem* filesystem, File* parent,
                     DeviceType type, bool endpoint)
    : File(name, 0, 0, 0, inode, filesystem, 0, parent),
      m_Type(type),
      m_Endpoint(endpoint),
      m_Lock(),
      m_Buffer(LinuxInputBufferRecords * sizeof(LinuxInputEvent)) {
  ByteSet(m_KeyState, 0, sizeof(m_KeyState));
  ByteSet(m_ButtonState, 0, sizeof(m_ButtonState));
}

EvdevFile::~EvdevFile() {
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

bool EvdevFile::initialise() {
  setPermissionsOnly(FILE_UR | FILE_GR | FILE_OR);
  setUidOnly(0);
  setGidOnly(0);
  if (!m_Endpoint) {
    return true;
  }

  const InputManager::CallbackType filter =
      m_Type == Keyboard ? InputManager::RawKey
                         : (m_Type == Pointer ? InputManager::Mouse : InputManager::AbsoluteMouse);
  InputManager::instance().installCallback(filter, subscriber, this, nullptr, 0);
  {
    LockGuard<Mutex> guard(m_Lock);
    m_Registered = true;
  }
  return true;
}

File* EvdevFile::open() {
  if (m_Endpoint) {
    return this;
  }
  auto* endpoint = new EvdevFile(getName(), 0, getFilesystem(), getParent(), m_Type, true);
  if (!endpoint || !endpoint->initialise()) {
    delete endpoint;
    return nullptr;
  }
  return endpoint;
}

uint64_t EvdevFile::readBytewise(uint64_t, uint64_t size, uintptr_t buffer, bool bCanBlock) {
  if (size < sizeof(LinuxInputEvent)) {
    SYSCALL_ERROR(InvalidArgument);
    return 0;
  }
  size -= size % sizeof(LinuxInputEvent);
  return m_Buffer.read(reinterpret_cast<uint8_t*>(buffer), size, bCanBlock);
}

uint64_t EvdevFile::writeBytewise(uint64_t, uint64_t, uintptr_t, bool) {
  SYSCALL_ERROR(PermissionDenied);
  return 0;
}

int EvdevFile::select(bool bWriting, int timeout) {
  if (bWriting) {
    return 0;
  }
  return m_Buffer.canRead(timeout == 1) ? 1 : 0;
}

bool EvdevFile::supports(size_t command) const {
  if (!isEvdevIoctl(command)) {
    return false;
  }
  const uint8_t number = ioctlNumber(command);
  if ((number == 0x40 || number == 0x41) && m_Type == AbsolutePointer) {
    return true;
  }
  return number == 0x01 || number == 0x02 || number == 0x03 || number == 0x06 || number == 0x07 ||
         number == 0x08 || number == 0x09 || (number >= 0x18 && number <= 0x1b) ||
         (number >= 0x20 && number <= 0x3f) || number == 0x90;
}

int EvdevFile::command(size_t command, void* buffer) {
  const uint8_t number = ioctlNumber(command);
  const size_t capacity = ioctlSize(command);
  auto copyOut = [buffer](const void* value, size_t size) {
    if (!PosixSubsystem::copyToUser(buffer, value, size)) {
      SYSCALL_ERROR(BadAddress);
      return false;
    }
    return true;
  };
  if (number == 0x01) {
    return copyOut(&LinuxEvdevVersion, sizeof(LinuxEvdevVersion)) ? 0 : -1;
  }
  if (number == 0x02) {
    const LinuxInputId id = {LinuxBusVirtual, 0x1, static_cast<uint16_t>(m_Type + 1), 1};
    return copyOut(&id, sizeof(id)) ? 0 : -1;
  }
  if (number == 0x03) {
    if (ioctlDirection(command) & 2) {
      const int repeat[2] = {250, 33};
      return copyOut(repeat, sizeof(repeat)) ? 0 : -1;
    }
    return 0;
  }
  if (number == 0x06 || number == 0x07 || number == 0x08) {
    const char* value =
        number == 0x06
            ? (m_Type == Keyboard ? "Pedigree keyboard"
                                  : (m_Type == Pointer ? "Pedigree pointer" : "Pedigree tablet"))
            : (number == 0x07 ? "pedigree/input0" : "");
    const size_t available = StringLength(value) + 1;
    const size_t copied = min(capacity, available);
    if (!copyOut(value, copied)) {
      return -1;
    }
    return static_cast<int>(copied);
  }
  if (number == 0x90) {
    return 0;
  }
  if (number == 0x40 || number == 0x41) {
    if (m_Type != AbsolutePointer || capacity < sizeof(LinuxInputAbsInfo)) {
      SYSCALL_ERROR(InvalidArgument);
      return -1;
    }
    LinuxInputAbsInfo info = {};
    {
      LockGuard<Mutex> guard(m_Lock);
      info.value = number == 0x40 ? m_AbsoluteX : m_AbsoluteY;
    }
    info.maximum = 0x7fff;
    return copyOut(&info, sizeof(info)) ? 0 : -1;
  }

  uint8_t result[96] = {};
  if (number == 0x09) {
    // No special input properties.
  } else if (number == 0x18) {
    if (m_Type == Keyboard) {
      LockGuard<Mutex> guard(m_Lock);
      MemoryCopy(result, m_KeyState, sizeof(m_KeyState));
    } else {
      LockGuard<Mutex> guard(m_Lock);
      for (size_t i = 0; i < 5; ++i) {
        if (m_ButtonState[i]) {
          setBit(result, sizeof(result), linuxButtonCode(i));
        }
      }
    }
  } else if (number >= 0x20 && number <= 0x3f) {
    const uint8_t eventType = number - 0x20;
    if (eventType == 0) {
      setBit(result, sizeof(result), LinuxEvSyn);
      setBit(result, sizeof(result), LinuxEvKey);
      if (m_Type == Pointer) {
        setBit(result, sizeof(result), LinuxEvRelative);
      } else if (m_Type == AbsolutePointer) {
        setBit(result, sizeof(result), LinuxEvAbsolute);
        setBit(result, sizeof(result), LinuxEvRelative);
      }
    } else if (eventType == LinuxEvKey) {
      if (m_Type == Keyboard) {
        for (size_t usage = 0; usage < 256; ++usage) {
          const uint16_t key = linuxKeyCode(usage);
          if (key) {
            setBit(result, sizeof(result), key);
          }
        }
      } else {
        for (size_t i = 0; i < 5; ++i) {
          setBit(result, sizeof(result), linuxButtonCode(i));
        }
      }
    } else if (eventType == LinuxEvRelative && m_Type != Keyboard) {
      if (m_Type == Pointer) {
        setBit(result, sizeof(result), LinuxRelX);
        setBit(result, sizeof(result), LinuxRelY);
      }
      setBit(result, sizeof(result), LinuxRelWheel);
    } else if (eventType == LinuxEvAbsolute && m_Type == AbsolutePointer) {
      setBit(result, sizeof(result), LinuxAbsX);
      setBit(result, sizeof(result), LinuxAbsY);
    }
  }

  const size_t copied = min(capacity, sizeof(result));
  if (!copyOut(result, copied)) {
    return -1;
  }
  return static_cast<int>(copied);
}

bool EvdevFile::retainVfsReference() {
  if (!m_Endpoint) {
    return File::retainVfsReference();
  }
  LockGuard<Mutex> guard(m_Lock);
  ++m_LifetimePins;
  return true;
}

void EvdevFile::releaseVfsReference() {
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

void EvdevFile::subscriber(InputManager::InputNotification& notification) {
  if (notification.meta) {
    reinterpret_cast<EvdevFile*>(notification.meta)->handleInput(notification);
  }
}

void EvdevFile::emit(const LinuxInputEvent* events, size_t count) {
  if (m_Buffer.tryWrite(reinterpret_cast<const uint8_t*>(events),
                        count * sizeof(LinuxInputEvent))) {
    dataChanged();
  }
}

void EvdevFile::handleInput(const InputManager::InputNotification& notification) {
  LinuxInputEvent events[9] = {};
  size_t count = 0;
  const Time::Timestamp now = Time::getTimeNanoseconds();
  auto append = [&](uint16_t type, uint16_t code, int32_t value) {
    LinuxInputEvent& event = events[count++];
    event.seconds = now / Time::Multiplier::Second;
    event.microseconds = (now % Time::Multiplier::Second) / Time::Multiplier::Microsecond;
    event.type = type;
    event.code = code;
    event.value = value;
  };

  if (m_Type == Keyboard && notification.type == InputManager::RawKey) {
    const uint16_t key = linuxKeyCode(notification.data.rawkey.scancode);
    if (!key) {
      return;
    }
    const bool pressed = !notification.data.rawkey.keyUp;
    {
      LockGuard<Mutex> guard(m_Lock);
      if (pressed) {
        setBit(m_KeyState, sizeof(m_KeyState), key);
      } else {
        m_KeyState[key / 8] &= static_cast<uint8_t>(~(1U << (key % 8)));
      }
    }
    append(LinuxEvKey, key, pressed ? 1 : 0);
  } else if (m_Type == Pointer && notification.type == InputManager::Mouse) {
    if (notification.data.pointy.relx) {
      append(LinuxEvRelative, LinuxRelX, notification.data.pointy.relx);
    }
    if (notification.data.pointy.rely) {
      append(LinuxEvRelative, LinuxRelY, -notification.data.pointy.rely);
    }
    if (notification.data.pointy.relz) {
      append(LinuxEvRelative, LinuxRelWheel, notification.data.pointy.relz);
    }
    {
      LockGuard<Mutex> guard(m_Lock);
      for (size_t i = 0; i < 5; ++i) {
        const bool pressed = notification.data.pointy.buttons[i];
        if (pressed != m_ButtonState[i]) {
          m_ButtonState[i] = pressed;
          append(LinuxEvKey, linuxButtonCode(i), pressed ? 1 : 0);
        }
      }
    }
  } else if (m_Type == AbsolutePointer && notification.type == InputManager::AbsoluteMouse) {
    {
      LockGuard<Mutex> guard(m_Lock);
      if (!m_HaveAbsolute || notification.data.absolute.x != m_AbsoluteX) {
        m_AbsoluteX = notification.data.absolute.x;
        append(LinuxEvAbsolute, LinuxAbsX, m_AbsoluteX);
      }
      if (!m_HaveAbsolute || notification.data.absolute.y != m_AbsoluteY) {
        m_AbsoluteY = notification.data.absolute.y;
        append(LinuxEvAbsolute, LinuxAbsY, m_AbsoluteY);
      }
      m_HaveAbsolute = true;
      for (size_t i = 0; i < 5; ++i) {
        const bool pressed = notification.data.absolute.buttons[i];
        if (pressed != m_ButtonState[i]) {
          m_ButtonState[i] = pressed;
          append(LinuxEvKey, linuxButtonCode(i), pressed ? 1 : 0);
        }
      }
    }
    if (notification.data.absolute.wheel) {
      append(LinuxEvRelative, LinuxRelWheel, notification.data.absolute.wheel);
    }
  }

  if (count) {
    append(LinuxEvSyn, 0, 0);
    emit(events, count);
  }
}
