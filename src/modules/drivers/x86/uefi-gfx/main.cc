/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */

#include "pedigree/kernel/BootstrapInfo.h"
#include "pedigree/kernel/Service.h"
#include "pedigree/kernel/ServiceFeatures.h"
#include "pedigree/kernel/ServiceManager.h"
#include "pedigree/kernel/graphics/Graphics.h"
#include "pedigree/kernel/graphics/GraphicsService.h"
#include "pedigree/kernel/machine/Display.h"
#include "pedigree/kernel/machine/Framebuffer.h"
#include "pedigree/kernel/processor/MemoryRegion.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/String.h"

#include "modules/Module.h"

namespace {

class UefiFramebuffer final : public Framebuffer {
 public:
  UefiFramebuffer() : Framebuffer(), m_Region("UEFI GOP framebuffer"), m_Offset(0) {}

  bool initialise(const BootstrapStruct_t::FramebufferInfo& info) {
    if (!info.address || !info.width || !info.height ||
        info.pitch < static_cast<uint64_t>(info.width) * 4 ||
        info.format > 1) {
      return false;
    }

    const uint64_t bytes = static_cast<uint64_t>(info.pitch) * info.height;
    if (!bytes || bytes > SIZE_MAX) {
      return false;
    }

    const size_t pageSize = PhysicalMemoryManager::getPageSize();
    m_Offset = info.address & (pageSize - 1);
    const size_t pages = (static_cast<size_t>(bytes) + m_Offset + pageSize - 1) / pageSize;
    if (!PhysicalMemoryManager::instance().allocateRegion(
            m_Region, pages,
            PhysicalMemoryManager::continuous | PhysicalMemoryManager::nonRamMemory |
                PhysicalMemoryManager::force,
            VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write |
                VirtualAddressSpace::CacheDisable,
            info.address - m_Offset)) {
      return false;
    }

    setWidth(info.width);
    setHeight(info.height);
    setBytesPerPixel(4);
    setBytesPerLine(info.pitch);
    setFormat(info.format == 0 ? Graphics::Bits32_Bgr : Graphics::Bits32_Rgb);
    setXPos(0);
    setYPos(0);
    setParent(0);
    setFramebuffer(reinterpret_cast<uintptr_t>(m_Region.virtualAddress()) + m_Offset);
    return true;
  }

  physical_uintptr_t getPhysicalPage(size_t offset) const override {
    const size_t bytes = getHeight() * getBytesPerLine();
    if (offset >= bytes) {
      return ~physical_uintptr_t(0);
    }

    offset &= ~(PhysicalMemoryManager::getPageSize() - 1);
    return m_Region.physicalAddress() + m_Offset + offset;
  }

 protected:
  void hwRedraw(size_t, size_t, size_t, size_t) override {}

 private:
  MemoryRegion m_Region;
  size_t m_Offset;
};

class UefiDisplay final : public Display {
 public:
  UefiDisplay(UefiFramebuffer* framebuffer, const BootstrapStruct_t::FramebufferInfo& info)
      : Display(), m_pFramebuffer(framebuffer), m_Mode() {
    m_Mode.id = 1;
    m_Mode.width = info.width;
    m_Mode.height = info.height;
    m_Mode.refresh = 0;
    m_Mode.framebuffer = info.address;
    m_Mode.pf.mRed = 8;
    m_Mode.pf.pRed = info.format == 0 ? 0 : 16;
    m_Mode.pf.mGreen = 8;
    m_Mode.pf.pGreen = 8;
    m_Mode.pf.mBlue = 8;
    m_Mode.pf.pBlue = info.format == 0 ? 16 : 0;
    m_Mode.pf.mAlpha = 0;
    m_Mode.pf.pAlpha = 24;
    m_Mode.pf.nBpp = 32;
    m_Mode.pf.nPitch = info.pitch;
    m_Mode.pf2 = framebuffer->getFormat();
    m_Mode.bytesPerLine = info.pitch;
    m_Mode.bytesPerPixel = 4;
    m_Mode.textMode = false;
    setSpecificType(String("UEFI GOP display"));
  }

  void* getFramebuffer() override {
    return m_pFramebuffer->getRawBuffer();
  }

  bool getPixelFormat(PixelFormat& format) override {
    format = m_Mode.pf;
    return true;
  }

  bool getCurrentScreenMode(ScreenMode& mode) override {
    mode = m_Mode;
    return true;
  }

  bool getScreenModes(List<ScreenMode*>& modes) override {
    ScreenMode* mode = new ScreenMode(m_Mode);
    if (!mode) {
      return false;
    }
    modes.pushBack(mode);
    return true;
  }

  bool setScreenMode(ScreenMode mode) override {
    return mode.id == m_Mode.id && mode.width == m_Mode.width && mode.height == m_Mode.height &&
           mode.pf.nBpp == m_Mode.pf.nBpp;
  }

  bool setScreenMode(size_t modeId) override {
    // There is no VGA text mode to switch to after ExitBootServices(). Keep
    // the firmware-selected GOP mode when clients request text mode.
    return modeId == 0 || modeId == m_Mode.id;
  }

  bool setScreenMode(size_t width, size_t height, size_t bpp) override {
    return width == m_Mode.width && height == m_Mode.height && bpp == m_Mode.pf.nBpp;
  }

 private:
  UefiFramebuffer* m_pFramebuffer;
  ScreenMode m_Mode;
};

UefiDisplay* g_pDisplay = nullptr;
UefiFramebuffer* g_pFramebuffer = nullptr;
GraphicsService::GraphicsProvider* g_pProvider = nullptr;

bool entry() {
  EMIT_IF(NOGFX) {
    return false;
  }

  if (!g_pBootstrapInfo || !g_pBootstrapInfo->isUefi()) {
    return false;
  }

  BootstrapStruct_t::FramebufferInfo info;
  if (!g_pBootstrapInfo->getFramebuffer(info)) {
    return false;
  }

  g_pFramebuffer = new UefiFramebuffer;
  if (!g_pFramebuffer || !g_pFramebuffer->initialise(info)) {
    delete g_pFramebuffer;
    g_pFramebuffer = nullptr;
    return false;
  }

  g_pDisplay = new UefiDisplay(g_pFramebuffer, info);
  g_pProvider = new GraphicsService::GraphicsProvider;
  if (!g_pDisplay || !g_pProvider) {
    delete g_pProvider;
    delete g_pDisplay;
    delete g_pFramebuffer;
    g_pProvider = nullptr;
    g_pDisplay = nullptr;
    g_pFramebuffer = nullptr;
    return false;
  }

  g_pProvider->pDisplay = g_pDisplay;
  g_pProvider->pFramebuffer = g_pFramebuffer;
  g_pProvider->maxWidth = info.width;
  g_pProvider->maxHeight = info.height;
  g_pProvider->maxDepth = 32;
  g_pProvider->maxTextWidth = 0;
  g_pProvider->maxTextHeight = 0;
  g_pProvider->bHardwareAccel = false;
  g_pProvider->bTextModes = false;

  ServiceFeatures* features = ServiceManager::instance().enumerateOperations(String("graphics"));
  Service* service = ServiceManager::instance().getService(String("graphics"));
  if (!features || !service || !features->provides(ServiceFeatures::touch) ||
      !service->serve(ServiceFeatures::touch, g_pProvider, sizeof(*g_pProvider))) {
    delete g_pProvider;
    delete g_pDisplay;
    delete g_pFramebuffer;
    g_pProvider = nullptr;
    g_pDisplay = nullptr;
    g_pFramebuffer = nullptr;
    return false;
  }

  NOTICE("UEFI GOP: registered fixed " << Dec << info.width << "x" << info.height << "x32 display");
  return true;
}

void exit() {}

}  // namespace

MODULE_INFO("uefi-gfx", &entry, &exit, "config");
