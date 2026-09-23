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

#include "DevFs.h"

#include "DevFs-block.h"
#include "InputFile.h"
#include "PosixSubsystem.h"
#include "descriptor-path.h"
#include "linux-fb-abi.h"
#include "modules/system/vfs/Pipe.h"
#include "modules/system/vfs/Symlink.h"
#include "modules/system/vfs/VFS.h"

#define MACHINE_FORWARD_DECL_ONLY
#include "pedigree/kernel/ServiceManager.h"
#include "pedigree/kernel/graphics/Graphics.h"
#include "pedigree/kernel/graphics/GraphicsService.h"
#include "pedigree/kernel/machine/Display.h"
#include "pedigree/kernel/machine/Framebuffer.h"
#include "pedigree/kernel/machine/InputManager.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/machine/Serial.h"
#include "pedigree/kernel/machine/Vga.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/SecureRandom.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/lib.h"
#include "pedigree/kernel/utilities/utility.h"

#include "modules/system/console/Console.h"
#include <pedigree/fb.h>

/// \todo these come from somewhere - expose them properly
#define ALT_KEY (1ULL << 60)
#define SHIFT_KEY (1ULL << 61)
#define CTRL_KEY (1ULL << 62)
#define SPECIAL_KEY (1ULL << 63)

namespace {
class CttySelectorFile final : public File {
 public:
  CttySelectorFile(size_t inode, Filesystem* filesystem, File* parent)
      : File(String("tty"), 0, 0, 0, inode, filesystem, 0, parent) {
    setPermissionsOnly(FILE_UR | FILE_UW | FILE_GR | FILE_GW | FILE_OR | FILE_OW);
  }

 private:
  bool isBytewise() const override {
    return true;
  }
};

class FullFile final : public ZeroFile {
 public:
  FullFile(size_t inode, Filesystem* filesystem, File* parent)
      : ZeroFile(String("full"), inode, filesystem, parent) {}

  uint64_t writeBytewise(uint64_t, uint64_t, uintptr_t, bool) override {
    SYSCALL_ERROR(NoSpaceLeftOnDevice);
    return 0;
  }
};

class DeviceLink final : public Symlink {
 public:
  DeviceLink(const String& name, const String& target, size_t inode, Filesystem* filesystem,
             File* parent)
      : Symlink(name, 0, 0, 0, inode, filesystem, target.length(), parent) {
    m_sTarget = target;
    setPermissions(FILE_UR | FILE_UW | FILE_UX | FILE_GR | FILE_GW | FILE_GX | FILE_OR | FILE_OW |
                   FILE_OX);
  }
};

void setLinuxBitfield(LinuxFbBitfield& field, uint32_t offset, uint32_t length) {
  field.offset = offset;
  field.length = length;
}

void setLinuxPixelFormat(LinuxFbVariableInfo& info, Graphics::PixelFormat format) {
  switch (format) {
    case Graphics::Bits32_Argb:
      setLinuxBitfield(info.red, 16, 8);
      setLinuxBitfield(info.green, 8, 8);
      setLinuxBitfield(info.blue, 0, 8);
      setLinuxBitfield(info.transparency, 24, 8);
      break;
    case Graphics::Bits32_Rgba:
      setLinuxBitfield(info.red, 24, 8);
      setLinuxBitfield(info.green, 16, 8);
      setLinuxBitfield(info.blue, 8, 8);
      setLinuxBitfield(info.transparency, 0, 8);
      break;
    case Graphics::Bits32_Rgb:
    case Graphics::Bits24_Rgb:
      setLinuxBitfield(info.red, 16, 8);
      setLinuxBitfield(info.green, 8, 8);
      setLinuxBitfield(info.blue, 0, 8);
      break;
    case Graphics::Bits32_Bgr:
    case Graphics::Bits24_Bgr:
      setLinuxBitfield(info.red, 0, 8);
      setLinuxBitfield(info.green, 8, 8);
      setLinuxBitfield(info.blue, 16, 8);
      break;
    case Graphics::Bits16_Argb:
      setLinuxBitfield(info.red, 8, 4);
      setLinuxBitfield(info.green, 4, 4);
      setLinuxBitfield(info.blue, 0, 4);
      setLinuxBitfield(info.transparency, 12, 4);
      break;
    case Graphics::Bits16_Rgb565:
      setLinuxBitfield(info.red, 11, 5);
      setLinuxBitfield(info.green, 5, 6);
      setLinuxBitfield(info.blue, 0, 5);
      break;
    case Graphics::Bits16_Rgb555:
      setLinuxBitfield(info.red, 10, 5);
      setLinuxBitfield(info.green, 5, 5);
      setLinuxBitfield(info.blue, 0, 5);
      break;
    case Graphics::Bits8_Idx:
      info.red.length = info.green.length = info.blue.length = 8;
      break;
    case Graphics::Bits8_Rgb332:
      setLinuxBitfield(info.red, 5, 3);
      setLinuxBitfield(info.green, 2, 3);
      setLinuxBitfield(info.blue, 0, 2);
      break;
  }
}

void fillLinuxVariableInfo(LinuxFbVariableInfo& info, Framebuffer& framebuffer, size_t depth) {
  info.xResolution = framebuffer.getWidth();
  info.yResolution = framebuffer.getHeight();
  info.virtualXResolution = info.xResolution;
  info.virtualYResolution = info.yResolution;
  info.bitsPerPixel = depth ? depth : Graphics::bitsPerPixel(framebuffer.getFormat());
  info.heightMillimeters = UINT32_MAX;
  info.widthMillimeters = UINT32_MAX;
  setLinuxPixelFormat(info, framebuffer.getFormat());
}
}  // namespace

static void terminalSwitchHandler(InputManager::InputNotification& in) {
  if (!in.meta) {
    return;
  }

  DevFs* p = reinterpret_cast<DevFs*>(in.meta);
  p->handleInput(in);
}

uint64_t RandomFile::readBytewise(uint64_t location, uint64_t size, uintptr_t buffer,
                                  bool bCanBlock) {
  uint8_t snapshot[256];
  size_t done = 0;
  while (done < size) {
    const size_t count = size - done < sizeof(snapshot) ? size - done : sizeof(snapshot);
    const size_t produced = secure_random_bytes(snapshot, count);
    if (!produced) {
      if (!done)
        SYSCALL_ERROR(NoMoreProcesses);
      break;
    }
    // Device I/O may target a faultable caller buffer. Keep faults outside the
    // generator's IRQ-disabled critical section.
    MemoryCopy(reinterpret_cast<void*>(buffer + done), snapshot, produced);
    done += produced;
  }
  pedigree_random::erase(snapshot, sizeof(snapshot));
  return done;
}

uint64_t RandomFile::writeBytewise(uint64_t location, uint64_t size, uintptr_t buffer,
                                   bool bCanBlock) {
  // Entropy feed-back writes must make progress for buffered stdio callers.
  // Discard untrusted input without crediting it as entropy or seeding the RNG.
  return size;
}

bool RandomFile::supports(size_t command) const {
  return command == 0x40085203UL;  // Linux RNDADDENTROPY.
}

int RandomFile::command(size_t command, void* buffer) {
  if (!supports(command)) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  auto* process = Processor::information().getCurrentThread()->getParent();
  if (process->getEffectiveUserId() != 0) {
    SYSCALL_ERROR(NotEnoughPermissions);
    return -1;
  }

  struct SeedRequest {
    int32_t entropyBits;
    int32_t bytes;
    uint8_t seed[32];
  } request = {};
  // Only the privileged initializer can claim entropy. Ordinary device
  // writes cannot turn predictable bytes into a trusted seed.
  if (!PosixSubsystem::copyFromUser(&request, buffer, sizeof(request))) {
    pedigree_random::erase(&request, sizeof(request));
    SYSCALL_ERROR(BadAddress);
    return -1;
  }
  if (request.entropyBits != 256 || request.bytes != 32) {
    pedigree_random::erase(&request, sizeof(request));
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  const int accepted = secure_random_seed(request.seed, sizeof(request.seed));
  pedigree_random::erase(&request, sizeof(request));
  if (!accepted) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  return 0;
}

uint64_t NullFile::readBytewise(uint64_t location, uint64_t size, uintptr_t buffer,
                                bool bCanBlock) {
  return 0;
}

uint64_t NullFile::writeBytewise(uint64_t location, uint64_t size, uintptr_t buffer,
                                 bool bCanBlock) {
  return size;
}

PtmxFile::PtmxFile(String str, size_t inode, Filesystem* pParentFS, File* pParent,
                   DevFsDirectory* ptsDirectory)
    : File(str, 0, 0, 0, inode, pParentFS, 0, pParent),
      m_Terminals(),
      m_pPtsDirectory(ptsDirectory) {
  setPermissionsOnly(FILE_UR | FILE_UW | FILE_GR | FILE_GW | FILE_OR | FILE_OW);
  setUidOnly(0);
  setGidOnly(0);
}

PtmxFile::~PtmxFile() {}

uint64_t PtmxFile::readBytewise(uint64_t location, uint64_t size, uintptr_t buffer,
                                bool bCanBlock) {
  return 0;
}

uint64_t PtmxFile::writeBytewise(uint64_t location, uint64_t size, uintptr_t buffer,
                                 bool bCanBlock) {
  return 0;
}

File* PtmxFile::open() {
  // find a new terminal ID that we can safely use
  size_t terminal = m_Terminals.getFirstClear();
  m_Terminals.set(terminal);

  // create the terminals
  String masterName, slaveName;
  masterName.Format("pty%d", terminal);
  slaveName.Format("%d", terminal);

  ConsoleMasterFile* pMaster =
      new ConsoleMasterFile(terminal, masterName, m_pPtsDirectory->getFilesystem());
  ConsoleSlaveFile* pSlave =
      new ConsoleSlaveFile(terminal, slaveName, m_pPtsDirectory->getFilesystem(), m_pPtsDirectory);

  pMaster->setOther(pSlave);
  pSlave->setOther(pMaster);

  m_pPtsDirectory->addEntry(slaveName, pSlave);

  // we actually open the newly-created master, which does not exist in the
  // filesystem at all
  /// \todo so, when this master is closed, we'll leak these resources...
  return pMaster;
}

uint64_t ZeroFile::readBytewise(uint64_t location, uint64_t size, uintptr_t buffer,
                                bool bCanBlock) {
  ByteSet(reinterpret_cast<void*>(buffer), 0, size);
  return size;
}

uint64_t ZeroFile::writeBytewise(uint64_t location, uint64_t size, uintptr_t buffer,
                                 bool bCanBlock) {
  return size;
}

SerialFile::SerialFile(String str, size_t inode, Filesystem* pParentFS, File* pParent,
                       Serial* serial)
    : File(str, 0, 0, 0, inode, pParentFS, 0, pParent), m_Serial(serial) {
  setPermissionsOnly(FILE_UR | FILE_UW | FILE_GR | FILE_GW | FILE_OR | FILE_OW);
  setUidOnly(0);
  setGidOnly(0);
}

uint64_t SerialFile::readBytewise(uint64_t location, uint64_t size, uintptr_t buffer,
                                  bool bCanBlock) {
  if (!m_Serial || !size) {
    return 0;
  }

  char* destination = reinterpret_cast<char*>(buffer);
  size_t count = 0;
  if (bCanBlock) {
    const char c = m_Serial->read();
    if (!c) {
      return 0;
    }
    destination[count++] = c;
  }

  while (count < size) {
    const char c = m_Serial->readNonBlock();
    if (!c) {
      break;
    }
    destination[count++] = c;
  }
  return count;
}

uint64_t SerialFile::writeBytewise(uint64_t location, uint64_t size, uintptr_t buffer,
                                   bool bCanBlock) {
  if (!m_Serial) {
    return 0;
  }

  const char* source = reinterpret_cast<const char*>(buffer);
  for (size_t i = 0; i < size; ++i) {
    m_Serial->write(source[i]);
  }
  return size;
}

int SerialFile::select(bool bWriting, int timeout) {
  if (bWriting) {
    return m_Serial ? 1 : 0;
  }
  return m_Serial && m_Serial->hasData() ? 1 : 0;
}

uint64_t RtcFile::readBytewise(uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock) {
  return 0;
}

uint64_t RtcFile::writeBytewise(uint64_t location, uint64_t size, uintptr_t buffer,
                                bool bCanBlock) {
  return 0;
}

bool RtcFile::supports(const size_t command) const {
  // read/set time
  return true;
  // return static_cast<size_t>(command) == 0x80247009UL ||
  // static_cast<size_t>(command) == 0x4024700aUL;
}

int RtcFile::command(const size_t command, void* buffer) {
  NOTICE("RtcFile: command " << Hex << command << " with buffer " << buffer);
  return 0;
}

FramebufferFile::FramebufferFile(String str, size_t inode, Filesystem* pParentFS, File* pParentNode)
    : File(str, 0, 0, 0, inode, pParentFS, 0, pParentNode),
      m_pGraphicsParameters(0),
      m_bTextMode(false),
      m_nDepth(0) {
  // r/w only for root
  setPermissionsOnly(FILE_GR | FILE_GW | FILE_UR | FILE_UW);
  setUidOnly(0);
  setGidOnly(0);
  for (size_t i = 0; i < 256; ++i) {
    m_LinuxPalette[i] = Graphics::createRgb(i, i, i);
  }
}

FramebufferFile::~FramebufferFile() {
  delete m_pGraphicsParameters;
}

bool FramebufferFile::initialise() {
  ServiceFeatures* pFeatures = ServiceManager::instance().enumerateOperations(String("graphics"));
  Service* pService = ServiceManager::instance().getService(String("graphics"));
  if (pFeatures && pFeatures->provides(ServiceFeatures::probe)) {
    if (pService) {
      m_pGraphicsParameters = new GraphicsService::GraphicsParameters;
      m_pGraphicsParameters->wantTextMode = false;
      if (!pService->serve(ServiceFeatures::probe, m_pGraphicsParameters,
                           sizeof(*m_pGraphicsParameters))) {
        delete m_pGraphicsParameters;
        m_pGraphicsParameters = 0;

        return false;
      } else {
        // Set the file size to reflect the size of the framebuffer.
        setSize(m_pGraphicsParameters->providerResult.pFramebuffer->getHeight() *
                m_pGraphicsParameters->providerResult.pFramebuffer->getBytesPerLine());

        Display::ScreenMode currentMode;
        if (m_pGraphicsParameters->providerResult.pDisplay->getCurrentScreenMode(currentMode)) {
          m_nDepth = currentMode.pf.nBpp;
        }
      }
    }
  }

  return pFeatures && pService;
}

uintptr_t FramebufferFile::readBlock(uint64_t location) {
  if (!m_pGraphicsParameters)
    return 0;

  if (location > getSize()) {
    ERROR("FramebufferFile::readBlock with location > size: " << location);
    return 0;
  }

  /// \todo If this is NOT virtual, we need to do something about that.
  return reinterpret_cast<uintptr_t>(
             m_pGraphicsParameters->providerResult.pFramebuffer->getRawBuffer()) +
         location;
}

physical_uintptr_t FramebufferFile::getPhysicalPage(size_t offset) {
  if (!m_pGraphicsParameters || offset >= getSize()) {
    return ~physical_uintptr_t(0);
  }

  offset &= ~(PhysicalMemoryManager::getPageSize() - 1);
  return m_pGraphicsParameters->providerResult.pFramebuffer->getPhysicalPage(offset);
}

void FramebufferFile::returnPhysicalPage(size_t) {
  // Framebuffer pages are direct mappings, not pages borrowed from the file cache.
}

bool FramebufferFile::supports(const size_t command) const {
  return ((PEDIGREE_FB_CMD_MIN <= command) && (command <= PEDIGREE_FB_CMD_MAX)) ||
         command == LinuxFbGetVariableInfo || command == LinuxFbPutVariableInfo ||
         command == LinuxFbGetFixedInfo || command == LinuxFbGetColorMap ||
         command == LinuxFbPutColorMap || command == LinuxFbPanDisplay || command == LinuxFbBlank;
}

int FramebufferFile::command(const size_t command, void* buffer) {
  if (!m_pGraphicsParameters) {
    ERROR("FramebufferFile::command called on an invalid FramebufferFile");
    return -1;
  }

  Display* pDisplay = m_pGraphicsParameters->providerResult.pDisplay;
  Framebuffer* pFramebuffer = m_pGraphicsParameters->providerResult.pFramebuffer;

  switch (command) {
    case LinuxFbGetFixedInfo: {
      LinuxFbFixedInfo value = {};
      MemoryCopy(value.id, "Pedigree fb", 11);
      const uint64_t framebufferSize = getSize();
      value.memoryLength =
          framebufferSize > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(framebufferSize);
      value.type = LinuxFbPackedPixels;
      value.visual =
          pFramebuffer->getFormat() == Graphics::Bits8_Idx ? LinuxFbPseudoColor : LinuxFbTrueColor;
      value.lineLength = pFramebuffer->getBytesPerLine();
      if (!PosixSubsystem::copyToUser(buffer, &value, sizeof(value))) {
        SYSCALL_ERROR(BadAddress);
        return -1;
      }
      return 0;
    }
    case LinuxFbGetVariableInfo: {
      LinuxFbVariableInfo value = {};
      Display::ScreenMode currentMode;
      size_t depth = m_nDepth;
      if (pDisplay->getCurrentScreenMode(currentMode)) {
        depth = currentMode.pf.nBpp;
      }
      fillLinuxVariableInfo(value, *pFramebuffer, depth);
      if (!PosixSubsystem::copyToUser(buffer, &value, sizeof(value))) {
        SYSCALL_ERROR(BadAddress);
        return -1;
      }
      return 0;
    }
    case LinuxFbPutVariableInfo: {
      LinuxFbVariableInfo requested = {};
      if (!PosixSubsystem::copyFromUser(&requested, buffer, sizeof(requested))) {
        SYSCALL_ERROR(BadAddress);
        return -1;
      }
      if (!requested.xResolution || !requested.yResolution || requested.bitsPerPixel <= 8 ||
          requested.xOffset || requested.yOffset ||
          (requested.virtualXResolution && requested.virtualXResolution != requested.xResolution) ||
          (requested.virtualYResolution && requested.virtualYResolution != requested.yResolution)) {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
      }
      if ((requested.activate & LinuxFbActivateMask) != LinuxFbActivateTest) {
        if (!pDisplay->setScreenMode(requested.xResolution, requested.yResolution,
                                     requested.bitsPerPixel)) {
          SYSCALL_ERROR(InvalidArgument);
          return -1;
        }
        m_nDepth = requested.bitsPerPixel;
        m_bTextMode = false;
        setSize(pFramebuffer->getHeight() * pFramebuffer->getBytesPerLine());
      }
      LinuxFbVariableInfo result = {};
      fillLinuxVariableInfo(result, *pFramebuffer, m_nDepth);
      if (!PosixSubsystem::copyToUser(buffer, &result, sizeof(result))) {
        SYSCALL_ERROR(BadAddress);
        return -1;
      }
      return 0;
    }
    case LinuxFbPanDisplay: {
      LinuxFbVariableInfo requested = {};
      if (!PosixSubsystem::copyFromUser(&requested, buffer, sizeof(requested))) {
        SYSCALL_ERROR(BadAddress);
        return -1;
      }
      if (requested.xOffset || requested.yOffset) {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
      }
      LinuxFbVariableInfo result = {};
      fillLinuxVariableInfo(result, *pFramebuffer, m_nDepth);
      if (!PosixSubsystem::copyToUser(buffer, &result, sizeof(result))) {
        SYSCALL_ERROR(BadAddress);
        return -1;
      }
      return 0;
    }
    case LinuxFbGetColorMap: {
      LinuxFbColorMap map = {};
      if (!PosixSubsystem::copyFromUser(&map, buffer, sizeof(map))) {
        SYSCALL_ERROR(BadAddress);
        return -1;
      }
      if (map.start > 256 || map.length > 256 - map.start) {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
      }
      for (size_t i = 0; i < map.length; ++i) {
        const uint32_t colour = m_LinuxPalette[map.start + i];
        const uint16_t red = static_cast<uint16_t>(((colour >> 16) & 0xff) * 0x101);
        const uint16_t green = static_cast<uint16_t>(((colour >> 8) & 0xff) * 0x101);
        const uint16_t blue = static_cast<uint16_t>((colour & 0xff) * 0x101);
        const uint16_t transparency = 0xffff;
        if (!map.red || !map.green || !map.blue ||
            !PosixSubsystem::copyToUser(map.red + i, &red, sizeof(red)) ||
            !PosixSubsystem::copyToUser(map.green + i, &green, sizeof(green)) ||
            !PosixSubsystem::copyToUser(map.blue + i, &blue, sizeof(blue)) ||
            (map.transparency && !PosixSubsystem::copyToUser(map.transparency + i, &transparency,
                                                             sizeof(transparency)))) {
          SYSCALL_ERROR(BadAddress);
          return -1;
        }
      }
      return 0;
    }
    case LinuxFbPutColorMap: {
      LinuxFbColorMap map = {};
      if (!PosixSubsystem::copyFromUser(&map, buffer, sizeof(map))) {
        SYSCALL_ERROR(BadAddress);
        return -1;
      }
      if (map.start > 256 || map.length > 256 - map.start) {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
      }
      for (size_t i = 0; i < map.length; ++i) {
        uint16_t red = 0;
        uint16_t green = 0;
        uint16_t blue = 0;
        if (!map.red || !map.green || !map.blue ||
            !PosixSubsystem::copyFromUser(&red, map.red + i, sizeof(red)) ||
            !PosixSubsystem::copyFromUser(&green, map.green + i, sizeof(green)) ||
            !PosixSubsystem::copyFromUser(&blue, map.blue + i, sizeof(blue))) {
          SYSCALL_ERROR(BadAddress);
          return -1;
        }
        m_LinuxPalette[map.start + i] = Graphics::createRgb(red >> 8, green >> 8, blue >> 8);
      }
      if (pFramebuffer->getFormat() == Graphics::Bits8_Idx) {
        pFramebuffer->setPalette(m_LinuxPalette, 256);
      }
      return 0;
    }
    case LinuxFbBlank:
      return 0;
    case PEDIGREE_FB_SETMODE: {
      pedigree_fb_modeset value = {};
      if (!PosixSubsystem::copyFromUser(&value, buffer, sizeof(value))) {
        SYSCALL_ERROR(BadAddress);
        return -1;
      }
      const pedigree_fb_modeset* arg = &value;
      size_t desiredWidth = arg->width;
      size_t desiredHeight = arg->height;
      size_t desiredDepth = arg->depth;

      // Are we seeking a text mode?
      if (!(desiredWidth && desiredHeight && desiredDepth)) {
        bool bSuccess = false;
        if (!m_pGraphicsParameters->providerResult.bTextModes) {
          bSuccess = pDisplay->setScreenMode(0);
          if (bSuccess) {
            // Native drivers may restore a larger graphics mode for this request.
            setSize(pFramebuffer->getHeight() * pFramebuffer->getBytesPerLine());
            Display::ScreenMode currentMode;
            if (pDisplay->getCurrentScreenMode(currentMode))
              m_nDepth = currentMode.pf.nBpp;
            m_bTextMode = false;
            if (Machine::instance().getNumVga())
              Machine::instance().getVga(0)->setLargestTextMode();
          }
        } else {
          // Set via VGA method.
          if (Machine::instance().getNumVga()) {
            /// \todo What if there is no text mode!?
            Vga* pVga = Machine::instance().getVga(0);
            pVga->setMode(3);  /// \todo Magic number.
            pVga->rememberMode();
            pVga->setLargestTextMode();

            m_nDepth = 0;
            m_bTextMode = true;

            bSuccess = true;
          }
        }

        if (bSuccess) {
          NOTICE("FramebufferFile: " << (m_pGraphicsParameters->providerResult.bTextModes
                                             ? "set text mode"
                                             : "restored graphics mode"));
          return 0;
        } else {
          return -1;
        }
      }

      bool bSet = false;
      while (desiredDepth > 8) {
        if (pDisplay->setScreenMode(desiredWidth, desiredHeight, desiredDepth)) {
          NOTICE("FramebufferFile: set mode " << Dec << desiredWidth << "x" << desiredHeight << "x"
                                              << desiredDepth << Hex << ".");
          bSet = true;
          break;
        }
        desiredDepth -= 8;
      }

      if (bSet) {
        m_nDepth = desiredDepth;

        setSize(pFramebuffer->getHeight() * pFramebuffer->getBytesPerLine());

        if (m_pGraphicsParameters->providerResult.bTextModes && m_bTextMode) {
          // Okay, we need to 'undo' the text mode.
          if (Machine::instance().getNumVga()) {
            /// \todo What if there is no text mode!?
            Vga* pVga = Machine::instance().getVga(0);
            pVga->restoreMode();

            m_bTextMode = false;
          }
        }
      }

      return bSet ? 0 : -1;
    }
    case PEDIGREE_FB_GETMODE: {
      pedigree_fb_mode value = {};
      pedigree_fb_mode* arg = &value;
      if (m_bTextMode) {
        ByteSet(arg, 0, sizeof(*arg));
      } else {
        arg->width = pFramebuffer->getWidth();
        arg->height = pFramebuffer->getHeight();
        arg->depth = m_nDepth;
        arg->bytes_per_pixel = pFramebuffer->getBytesPerPixel();
        arg->format = pFramebuffer->getFormat();
        arg->bytes_per_line = pFramebuffer->getBytesPerLine();
      }
      if (!PosixSubsystem::copyToUser(buffer, &value, sizeof(value))) {
        SYSCALL_ERROR(BadAddress);
        return -1;
      }
      return 0;
    }
    case PEDIGREE_FB_REDRAW: {
      if (!buffer) {
        // Redraw all.
        pFramebuffer->redraw(0, 0, pFramebuffer->getWidth(), pFramebuffer->getHeight(), true);
      } else {
        pedigree_fb_rect value = {};
        if (!PosixSubsystem::copyFromUser(&value, buffer, sizeof(value))) {
          SYSCALL_ERROR(BadAddress);
          return -1;
        }
        const pedigree_fb_rect* arg = &value;
        pFramebuffer->redraw(arg->x, arg->y, arg->w, arg->h, true);
      }

      return 0;
    }
    default:
      return -1;
  }
}

Tty0File::Tty0File(String str, size_t inode, Filesystem* pParentFS, File* pParent, DevFs* devfs)
    : File(str, 0, 0, 0, inode, pParentFS, 0, pParent), m_pDevFs(devfs) {
  setPermissionsOnly(FILE_UR | FILE_UW | FILE_GR | FILE_GW | FILE_OR | FILE_OW);
  setUidOnly(0);
  setGidOnly(0);
}

Tty0File::~Tty0File() {}

uint64_t Tty0File::readBytewise(uint64_t location, uint64_t size, uintptr_t buffer,
                                bool bCanBlock) {
  return 0;
}

uint64_t Tty0File::writeBytewise(uint64_t location, uint64_t size, uintptr_t buffer,
                                 bool bCanBlock) {
  return 0;
}

File* Tty0File::open() {
  // easy - just return the currently-active VT
  return m_pDevFs->getTerminalManager().getCurrentTerminalFile();
}

physical_uintptr_t MemFile::getPhysicalPage(size_t offset) {
#if 0
    NOTICE("MemFile: giving matching physical page for offset " << Hex << offset);
#endif

  // offset is literally the physical page for /dev/mem
  return offset & ~(PhysicalMemoryManager::getPageSize() - 1);
}

void MemFile::returnPhysicalPage(size_t offset) {
  // no-op
}

DevFsDirectory::~DevFsDirectory() = default;

DevFs::~DevFs() {
  InputManager::instance().removeCallback(terminalSwitchHandler, this);

  delete m_VtManager;
  delete m_pTty;
  m_pRoot->emptyCache();
  if (!VFS::instance().untrackFile(m_pRoot)) {
    ERROR("DevFs: root directory did not get cleaned up");
  }
}

bool DevFs::initialise(Disk* pDisk) {
  // Deterministic inode assignment to each devfs node
  m_NextInode = 0;

  if (m_pRoot) {
    delete m_pRoot;
  }

  m_pRoot = new DevFsDirectory(String(""), 0, 0, 0, getNextInode(), this, 0, 0);
  // Allow user/group to read and write, but disallow all others anything
  // other than the ability to list and access files.
  m_pRoot->setPermissions(FILE_UR | FILE_UW | FILE_UX | FILE_GR | FILE_GW | FILE_GX | FILE_OR |
                          FILE_OX);

  VFS::instance().trackFile(m_pRoot);

  File* block = posix_make_block_directory(*this, m_pRoot);
  if (!block)
    return false;
  m_pRoot->addEntry(block->getName(), block);

  File* disk = posix_make_disk_directory(*this, m_pRoot);
  if (!disk)
    return false;
  m_pRoot->addEntry(disk->getName(), disk);

  File* descriptors = posix_make_dev_fd_link(*this, m_pRoot);
  if (!descriptors)
    return false;
  m_pRoot->addEntry(descriptors->getName(), descriptors);

  m_CttySelector = new CttySelectorFile(getNextInode(), this, m_pRoot);
  if (!m_CttySelector)
    return false;
  m_pRoot->addEntry(m_CttySelector->getName(), m_CttySelector);

  // Create /dev/null and /dev/zero nodes
  NullFile* pNull = new NullFile(String("null"), getNextInode(), this, m_pRoot);
  ZeroFile* pZero = new ZeroFile(String("zero"), getNextInode(), this, m_pRoot);
  FullFile* pFull = new FullFile(getNextInode(), this, m_pRoot);
  m_pRoot->addEntry(pNull->getName(), pNull);
  m_pRoot->addEntry(pZero->getName(), pZero);
  m_pRoot->addEntry(pFull->getName(), pFull);

  auto* pShm = new DevFsDirectory(String("shm"), 0, 0, 0, getNextInode(), this, 0, m_pRoot);
  pShm->setPermissions(FILE_UR | FILE_UW | FILE_UX | FILE_GR | FILE_GW | FILE_GX | FILE_OR |
                       FILE_OW | FILE_OX | FILE_STICKY);
  m_pRoot->addEntry(pShm->getName(), pShm);

  const struct {
    const char* name;
    const char* target;
  } standardStreams[] = {
      {"stdin", "/proc/self/fd/0"}, {"stdout", "/proc/self/fd/1"}, {"stderr", "/proc/self/fd/2"}};
  for (const auto& stream : standardStreams) {
    auto* link =
        new DeviceLink(String(stream.name), String(stream.target), getNextInode(), this, m_pRoot);
    m_pRoot->addEntry(link->getName(), link);
  }

  if (Machine::instance().getNumSerial()) {
    SerialFile* pSerial = new SerialFile(String("ttyS0"), getNextInode(), this, m_pRoot,
                                         Machine::instance().getSerial(0));
    m_pRoot->addEntry(pSerial->getName(), pSerial);
  }

  // Create the /dev/mem device.
  MemFile* pMem = new MemFile(String("mem"), getNextInode(), this, m_pRoot);
  m_pRoot->addEntry(pMem->getName(), pMem);

  // Create the /dev/pts directory for ptys to go into.
  DevFsDirectory* pPts =
      new DevFsDirectory(String("pts"), 0, 0, 0, getNextInode(), this, 0, m_pRoot);
  pPts->setPermissions(FILE_UR | FILE_UW | FILE_UX | FILE_GR | FILE_GX | FILE_OR | FILE_OX);
  m_pRoot->addEntry(pPts->getName(), pPts);

  // Create the /dev/ptmx device.
  PtmxFile* pPtmx = new PtmxFile(String("ptmx"), getNextInode(), this, m_pRoot, pPts);
  m_pRoot->addEntry(pPtmx->getName(), pPtmx);

  // Create /dev/urandom for the RNG.
  RandomFile* pUrandom = new RandomFile(String("urandom"), getNextInode(), this, m_pRoot);
  m_pRoot->addEntry(pUrandom->getName(), pUrandom);

  // Create /dev/random - note, won't block waiting for more entropy!
  RandomFile* pRandom = new RandomFile(String("random"), getNextInode(), this, m_pRoot);
  m_pRoot->addEntry(pRandom->getName(), pRandom);

  // Linux framebuffer userspace expects the primary device at /dev/fb0.
  FramebufferFile* pFb = new FramebufferFile(String("fb0"), getNextInode(), this, m_pRoot);
  const bool framebufferAvailable = pFb->initialise();
  if (framebufferAvailable)
    m_pRoot->addEntry(pFb->getName(), pFb);
  else {
    WARNING("POSIX: no /dev/fb0 - framebuffer failed to initialise.");
    revertInode();
    delete pFb;
  }
  if (framebufferAvailable) {
    auto* pFbAlias = new DeviceLink(String("fb"), String("fb0"), getNextInode(), this, m_pRoot);
    m_pRoot->addEntry(pFbAlias->getName(), pFbAlias);
  }

  m_VtManager = new VirtualTerminalManager(m_pRoot);
  if (!m_VtManager->initialise()) {
    WARNING("POSIX: no /dev/tty - VT manager failed to initialise");
    delete m_VtManager;
    m_VtManager = nullptr;
  }

  // tty0 == current console
  Tty0File* pTty0 = new Tty0File(String("tty0"), getNextInode(), this, m_pRoot, this);
  m_pRoot->addEntry(pTty0->getName(), pTty0);

  // The virt machine has a serial console and no virtual terminal display.
#if ARM64
  File* pConsole = new DeviceLink(String("console"), String("ttyS0"), getNextInode(), this, m_pRoot);
#else
  File* pConsole = new Tty0File(String("console"), getNextInode(), this, m_pRoot, this);
#endif
  m_pRoot->addEntry(pConsole->getName(), pConsole);

#if 0
    // Create /dev/textui for the text-only UI device.
    m_pTty = new TextIO(String("textui"), getNextInode(), this, m_pRoot);
    m_pTty->markPrimary();
    if (m_pTty->initialise(false))
    {
        m_pRoot->addEntry(m_pTty->getName(), m_pTty);
    }
    else
    {
        WARNING("POSIX: no /dev/tty - TextIO failed to initialise.");
        revertInode();
        delete m_pTty;
        m_pTty = nullptr;
    }

    // tty0 == current console
    Tty0File *pTty0 =
        new Tty0File(String("tty0"), getNextInode(), this, m_pRoot, this);
    m_pRoot->addEntry(pTty0->getName(), pTty0);

    // console == current console
    Tty0File *pConsole =
        new Tty0File(String("console"), getNextInode(), this, m_pRoot, this);
    m_pRoot->addEntry(pConsole->getName(), pConsole);

    // create tty1 which is essentially just textui but with a S_IFCHR wrapper
    if (m_pTty)
    {
        ConsolePhysicalFile *pTty1 =
            new ConsolePhysicalFile(m_pTty, String("tty1"), this);
        m_pRoot->addEntry(pTty1->getName(), pTty1);

        m_pTtys[0] = m_pTty;
        m_pTtyFiles[0] = pTty1;
    }

    // create tty2-6 as non-overloaded TextIO instances
    for (size_t i = 1; i < DEVFS_NUMTTYS; ++i)
    {
        String ttyname;
        ttyname.Format("tty%u", i + 1);

        TextIO *tio = new TextIO(ttyname, getNextInode(), this, m_pRoot);
        if (tio->initialise(true))
        {
            ConsolePhysicalFile *file =
                new ConsolePhysicalFile(tio, ttyname, this);
            m_pRoot->addEntry(tio->getName(), file);

            m_pTtys[i] = tio;
            m_pTtyFiles[i] = file;

            // activate the terminal by performing an empty write, which will
            // ensure users switching to the terminal see a blank screen if
            // nothing has actually opened it - this is better than seeing the
            // previous tty's output...
            tio->write("", 0);
        }
        else
        {
            WARNING("POSIX: failed to create " << ttyname);
            revertInode();
            delete tio;

            m_pTtys[i] = nullptr;
            m_pTtyFiles[i] = nullptr;
        }
    }
#endif

  Pipe* initctl = new Pipe(String("initctl"), 0, 0, 0, getNextInode(), this, 0, m_pRoot);
  m_pRoot->addEntry(initctl->getName(), initctl);
  // initctl->increaseRefCount(false);  // pretend to be a reader

  RtcFile* rtc = new RtcFile(getNextInode(), this, m_pRoot);
  m_pRoot->addEntry(rtc->getName(), rtc);

  auto* pInputDirectory =
      new DevFsDirectory(String("input"), 0, 0, 0, getNextInode(), this, 0, m_pRoot);
  pInputDirectory->setPermissions(FILE_UR | FILE_UX | FILE_GR | FILE_GX | FILE_OR | FILE_OX);
  m_pRoot->addEntry(pInputDirectory->getName(), pInputDirectory);

  InputFile* pInput = new InputFile(String("pedigree"), getNextInode(), this, pInputDirectory);
  if (pInput && pInput->initialise()) {
    pInputDirectory->addEntry(pInput->getName(), pInput);
  } else {
    revertInode();
    delete pInput;
  }

  auto* keyboard =
      new EvdevFile(String("event0"), getNextInode(), this, pInputDirectory, EvdevFile::Keyboard);
  if (keyboard && keyboard->initialise()) {
    pInputDirectory->addEntry(keyboard->getName(), keyboard);
  } else {
    revertInode();
    delete keyboard;
  }

  auto* pointer =
      new EvdevFile(String("event1"), getNextInode(), this, pInputDirectory, EvdevFile::Pointer);
  if (pointer && pointer->initialise()) {
    pInputDirectory->addEntry(pointer->getName(), pointer);
  } else {
    revertInode();
    delete pointer;
  }

  auto* tablet = new EvdevFile(String("event2"), getNextInode(), this, pInputDirectory,
                               EvdevFile::AbsolutePointer);
  if (tablet && tablet->initialise()) {
    pInputDirectory->addEntry(tablet->getName(), tablet);
  } else {
    revertInode();
    delete tablet;
  }

  EMIT_IF(X86_COMMON) {
    PsAuxFile* pPsAux = new PsAuxFile(String("psaux"), getNextInode(), this, m_pRoot);
    if (pPsAux->initialise()) {
      m_pRoot->addEntry(pPsAux->getName(), pPsAux);
      m_pPsAuxFile = pPsAux;
    } else {
      delete pPsAux;
    }
  }

  // add input handler for terminal switching
  InputManager::instance().installCallback(InputManager::Key, terminalSwitchHandler, this);

  m_CurrentTty = 0;

  return true;
}

size_t DevFs::getNextInode() {
  return m_NextInode++;
}

void DevFs::revertInode() {
  --m_NextInode;
}

void DevFs::handleInput(InputManager::InputNotification& in) {
  uint64_t c = in.data.key.key;
  if (c & SPECIAL_KEY) {
    uint32_t k = c & 0xFFFFFFFFUL;
    char* s = reinterpret_cast<char*>(&k);

    size_t newTty = 0;
    if (!StringCompareN(s, "f1", 3)) {
      newTty = 0;
    } else if (!StringCompareN(s, "f2", 3)) {
      newTty = 1;
    } else if (!StringCompareN(s, "f3", 3)) {
      newTty = 2;
    } else if (!StringCompareN(s, "f4", 3)) {
      newTty = 3;
    } else if (!StringCompareN(s, "f5", 3)) {
      newTty = 4;
    } else if (!StringCompareN(s, "f6", 3)) {
      newTty = 5;
    } else {
      return;
    }

    m_VtManager->activate(newTty);
  }
}
