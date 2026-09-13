/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <list>
#include <string>
#include <vector>

#include "../../src/modules/subsys/pedigree-c/include/pedigree/fb.h"
#include "../../src/system/kernel/machine/mach_pc/FramebufferConsole.h"

#define DEBUG_LOG(...)
#define NOTICE(...)
#define ERROR(...)
#define SYSCALL_ERROR(...)
#define ByteSet std::memset
using String = std::string;
template <class T>
struct List : std::list<T> {
  using Iterator = typename std::list<T>::iterator;
  using std::list<T>::operator=;
};
namespace Graphics {
enum PixelFormat { Bits32_Rgb, Bits32_Bgr, Bits24_Rgb };
}
struct Spinlock {
  bool held = false;
};
template <class T>
struct LockGuard {
  explicit LockGuard(T& value) : lock(value) {
    assert(!lock.held);
    lock.held = true;
  }
  ~LockGuard() {
    lock.held = false;
  }
  T& lock;
};
struct Framebuffer {
  size_t width = 640, height = 400;
  uint32_t pitch = 640 * 4, pixelBytes = 4;
  Graphics::PixelFormat format = Graphics::Bits32_Rgb;
  void* pixels = nullptr;
  Framebuffer* parent = nullptr;
  Spinlock* consoleLock = nullptr;
  unsigned presents = 0;
  size_t getWidth() const {
    return width;
  }
  size_t getHeight() const {
    return height;
  }
  uint32_t getBytesPerLine() const {
    return pitch;
  }
  uint32_t getBytesPerPixel() const {
    return pixelBytes;
  }
  Graphics::PixelFormat getFormat() const {
    return format;
  }
  void* getRawBuffer() const {
    return pixels;
  }
  Framebuffer* getParent() const {
    return parent;
  }
  void redraw(size_t = 0, size_t = 0, size_t = 0, size_t = 0, bool = false) {
    assert(consoleLock && consoleLock->held);
    ++presents;
  }
};
class Display {
 public:
  struct ScreenMode {
    struct {
      uint32_t nBpp = 24;
    } pf;
  };
  void getName(String& name) {
    name = "fixture";
  }
  bool getCurrentScreenMode(ScreenMode& mode) {
    mode.pf.nBpp = 24;
    return true;
  }
  bool setScreenMode(size_t id) {
    assert(!id && framebuffer);
    if (!restoreSucceeds)
      return false;
    framebuffer->width = 1024;
    framebuffer->height = 768;
    framebuffer->pitch = 4096;
    return true;
  }
  bool setScreenMode(size_t, size_t, size_t) {
    return false;
  }
  Framebuffer* framebuffer = nullptr;
  bool restoreSucceeds = true;
};
class GraphicsService {
 public:
#include "graphics-provider.inc"
  struct GraphicsParameters {
    GraphicsProvider providerResult;
  };
  struct ProviderPair {
    GraphicsProvider* bestBase;
    GraphicsProvider* bestText;
  };
  ProviderPair determineBestProvider();
  List<GraphicsProvider*> m_Providers;
};
struct Module {
  struct Name : std::string {
    using std::string::string;
    const char* cstr() const {
      return c_str();
    }
  } name;
  const char** depends = nullptr;
  const char** depends_opt = nullptr;
  bool unloadComplete = false;
#include "module-status.inc"
};
template <class T>
T rebase(Module*, T pointer) {
  return pointer;
}
int StringCompare(const char* a, const char* b) {
  return std::strcmp(a, b);
}
class KernelElf {
 public:
  bool moduleDependenciesSatisfiedLocked(Module* module) const;
  std::vector<Module*> m_Modules;
};
struct X86Vga {
  bool setFramebuffer(Framebuffer* framebuffer);
  void flush();
  bool m_Uefi = true;
  Spinlock m_ConsoleLock;
  FramebufferConsole m_Console;
  Framebuffer* m_pConsoleFramebuffer = nullptr;
};
struct Vga {
  void setLargestTextMode() {}
  void setMode(int) {}
  void rememberMode() {}
  void restoreMode() {}
};
struct Machine {
  static Machine& instance() {
    static Machine machine;
    return machine;
  }
  unsigned getNumVga() {
    return 1;
  }
  Vga* getVga(unsigned) {
    return &vga;
  }
  Vga vga;
};
struct PosixSubsystem {
  static bool copyFromUser(void* to, const void* from, size_t size) {
    std::memcpy(to, from, size);
    return true;
  }
  static bool copyToUser(void* to, const void* from, size_t size) {
    std::memcpy(to, from, size);
    return true;
  }
};
struct FramebufferFile {
  int command(size_t command, void* buffer);
  void setSize(size_t size) {
    bytes = size;
  }
  GraphicsService::GraphicsParameters* m_pGraphicsParameters = nullptr;
  bool m_bTextMode = false;
  size_t m_nDepth = 32, bytes = 0;
};

#include "graphics-handover.inc"

int main() {
  Display display;
  GraphicsService graphics;
  GraphicsService::GraphicsProvider gop = {};
  gop.pDisplay = &display;
  gop.bFirmwareFallback = true;
  gop.maxWidth = 1920;
  gop.maxHeight = 1080;
  gop.maxDepth = 32;
  auto native = gop;
  native.bFirmwareFallback = false;
  graphics.m_Providers = {&gop, &native};
  assert(graphics.determineBestProvider().bestBase == &native);
  native.maxWidth = 640;
  native.maxHeight = 480;
  assert(graphics.determineBestProvider().bestBase == &native);
  graphics.m_Providers = {&native, &gop};
  assert(graphics.determineBestProvider().bestBase == &native);
  auto accelerated = native;
  accelerated.bHardwareAccel = true;
  accelerated.bTextModes = true;
  accelerated.maxTextWidth = 80;
  accelerated.maxTextHeight = 25;
  graphics.m_Providers.push_back(&accelerated);
  assert(graphics.determineBestProvider().bestBase == &accelerated);
  assert(graphics.determineBestProvider().bestText == &accelerated);
  graphics.m_Providers = {&gop};
  assert(graphics.determineBestProvider().bestBase == &gop);
  assert(!graphics.determineBestProvider().bestText);

  KernelElf kernel;
  Module consumer, driver;
  driver.name = Module::Name("intelgfx");
  const char* optional[] = {"intelgfx", nullptr};
  consumer.depends_opt = optional;
  assert(kernel.moduleDependenciesSatisfiedLocked(&consumer));
  kernel.m_Modules.push_back(&driver);
  for (auto state : {Module::Unknown, Module::Preloaded, Module::Executing, Module::Unloading}) {
    driver.status = state;
    assert(!kernel.moduleDependenciesSatisfiedLocked(&consumer));
  }
  for (auto state : {Module::Active, Module::Failed, Module::Unloaded}) {
    driver.status = state;
    assert(kernel.moduleDependenciesSatisfiedLocked(&consumer));
  }
  consumer.depends_opt = nullptr;
  consumer.depends = optional;
  for (auto state : {Module::Preloaded, Module::Executing, Module::Failed, Module::Unloaded}) {
    driver.status = state;
    assert(!kernel.moduleDependenciesSatisfiedLocked(&consumer));
  }
  driver.status = Module::Active;
  assert(kernel.moduleDependenciesSatisfiedLocked(&consumer));

  X86Vga vga;
  std::vector<uint32_t> first(640 * 400), second(656 * 402, 0x12345678);
  Framebuffer framebuffer;
  framebuffer.pixels = first.data();
  framebuffer.consoleLock = &vga.m_ConsoleLock;
  vga.m_Console.cells()[0] = 0x04db;
  assert(vga.setFramebuffer(&framebuffer));
  assert(framebuffer.presents == 1 && first[0] == 0x00aa0000);
  vga.flush();
  assert(framebuffer.presents == 2);
  const auto oldPixels = first;
  Framebuffer replacement = framebuffer;
  replacement.presents = 0;
  replacement.pixels = second.data();
  replacement.width = 648;
  replacement.height = 402;
  replacement.pitch = 656 * 4;
  replacement.format = Graphics::Bits32_Bgr;
  assert(vga.setFramebuffer(&replacement));
  assert(vga.m_Console.cells()[0] == 0x04db);
  assert(replacement.presents == 1 && second[656 + 4] == 0x000000aa);
  assert(second[648] == 0x12345678);
  assert(first == oldPixels);
  Framebuffer invalid = replacement;
  invalid.height = 399;
  assert(!vga.setFramebuffer(&invalid));
  assert(vga.m_pConsoleFramebuffer == &replacement);
  vga.m_Console.cells()[0] = 0x0020;
  vga.flush();
  assert(replacement.presents == 2 && second[656 + 4] == 0);
  assert(first == oldPixels);
  invalid = replacement;
  invalid.parent = &replacement;
  assert(!vga.setFramebuffer(&invalid));
  invalid.parent = nullptr;
  invalid.format = Graphics::Bits24_Rgb;
  assert(!vga.setFramebuffer(&invalid));
  vga.m_Uefi = false;
  assert(!vga.setFramebuffer(&replacement));

  GraphicsService::GraphicsParameters parameters{};
  parameters.providerResult.pDisplay = &display;
  parameters.providerResult.pFramebuffer = &framebuffer;
  display.framebuffer = &framebuffer;
  FramebufferFile file;
  file.m_pGraphicsParameters = &parameters;
  file.bytes = framebuffer.height * framebuffer.pitch;
  const size_t previousBytes = file.bytes;
  pedigree_fb_modeset reset = {};
  display.restoreSucceeds = false;
  assert(file.command(PEDIGREE_FB_SETMODE, &reset) == -1);
  assert(file.bytes == previousBytes && file.m_nDepth == 32);
  display.restoreSucceeds = true;
  assert(file.command(PEDIGREE_FB_SETMODE, &reset) == 0);
  assert(file.bytes == 768 * 4096 && file.m_nDepth == 24);
  assert(!file.m_bTextMode);
  pedigree_fb_mode restored = {};
  assert(file.command(PEDIGREE_FB_GETMODE, &restored) == 0);
  assert(restored.width == 1024 && restored.height == 768 && restored.depth == 24);
  assert(restored.bytes_per_line == 4096);
}
