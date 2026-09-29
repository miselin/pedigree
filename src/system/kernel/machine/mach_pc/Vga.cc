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

#include "Vga.h"
#include "pedigree/kernel/BootstrapInfo.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/machine/Framebuffer.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/utilities/utility.h"

X86Vga::X86Vga() : m_Framebuffer("Console framebuffer") {}

X86Vga::~X86Vga() {}

bool X86Vga::setLargestTextMode() {
  LockGuard<Spinlock> guard(m_ConsoleLock);
  m_Console.invalidate();
  return true;
}

void X86Vga::pokeBuffer(uint8_t* pBuffer, size_t nBufLen) {
  if (!pBuffer) {
    return;
  }
  size_t length =
      nBufLen < FramebufferConsole::CellCount * 2 ? nBufLen : FramebufferConsole::CellCount * 2;
  MemoryCopy(m_Console.cells(), pBuffer, length);
  flush();
}

void X86Vga::peekBuffer(uint8_t* pBuffer, size_t nBufLen) {
  if (!pBuffer) {
    return;
  }
  size_t length =
      nBufLen < FramebufferConsole::CellCount * 2 ? nBufLen : FramebufferConsole::CellCount * 2;
  MemoryCopy(pBuffer, m_Console.cells(), length);
}

void X86Vga::moveCursor(size_t nX, size_t nY) {
  LockGuard<Spinlock> guard(m_ConsoleLock);
  m_Console.moveCursor(nX, nY);
  m_Console.flush();
  if (m_pConsoleFramebuffer) {
    m_pConsoleFramebuffer->redraw();
  }
}

void X86Vga::flush() {
  LockGuard<Spinlock> guard(m_ConsoleLock);
  m_Console.flush();
  if (m_pConsoleFramebuffer) {
    m_pConsoleFramebuffer->redraw();
  }
}

bool X86Vga::setFramebuffer(Framebuffer* framebuffer) {
  if (!framebuffer || framebuffer->getParent() || framebuffer->getBytesPerPixel() != 4 ||
      framebuffer->getWidth() > UINT32_MAX || framebuffer->getHeight() > UINT32_MAX ||
      (framebuffer->getFormat() != Graphics::Bits32_Rgb &&
       framebuffer->getFormat() != Graphics::Bits32_Bgr)) {
    return false;
  }

  const size_t pitch = framebuffer->getBytesPerLine();
  if (pitch && framebuffer->getHeight() > SIZE_MAX / pitch) {
    return false;
  }

  LockGuard<Spinlock> guard(m_ConsoleLock);
  if (!m_Console.initialise(framebuffer->getRawBuffer(), pitch * framebuffer->getHeight(),
                            framebuffer->getWidth(), framebuffer->getHeight(), pitch,
                            framebuffer->getFormat() == Graphics::Bits32_Rgb ? 1 : 0)) {
    return false;
  }

  m_pConsoleFramebuffer = framebuffer;
  m_Console.flush();
  m_pConsoleFramebuffer->redraw();
  return true;
}

bool X86Vga::initialise() {
  BootstrapStruct_t::FramebufferInfo info;
  // Keep serial boot available on firmware without a usable linear framebuffer.
  if (!g_pBootstrapInfo || !g_pBootstrapInfo->getFramebuffer(info) || info.width < 640 ||
      info.height < 400) {
    return true;
  }
  const uint64_t bytes = static_cast<uint64_t>(info.pitch) * info.height;
  if (bytes > 256 * 1024 * 1024) {
    return true;
  }
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  const size_t offset = info.address & (pageSize - 1);
  const size_t pages = (bytes + offset + pageSize - 1) / pageSize;
  if (!PhysicalMemoryManager::instance().allocateRegion(
          m_Framebuffer, pages,
          PhysicalMemoryManager::continuous | PhysicalMemoryManager::nonRamMemory |
              PhysicalMemoryManager::force,
          VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write |
              VirtualAddressSpace::CacheDisable,
          info.address - offset)) {
    return true;
  }
  m_Console.initialise(reinterpret_cast<uint8_t*>(m_Framebuffer.virtualAddress()) + offset, bytes,
                       info.width, info.height, info.pitch, info.format);
  return true;
}
