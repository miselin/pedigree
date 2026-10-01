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

#ifndef PEDIGREE_FB_FRAMEBUFFER_H
#define PEDIGREE_FB_FRAMEBUFFER_H

#include <signal.h>

#include <cairo/cairo.h>
#include <pedigree/fb.h>
#include <sys/vt.h>

/** Abstracts the system's framebuffer offering. */
class Framebuffer {
 public:
  Framebuffer();
  virtual ~Framebuffer();

  /** General system-specific initialisation. */
  bool initialise();

  /** Claim the active VT on the thread which will present every frame. */
  bool claimTerminal();

  /** Keeps VT release notifications deferred until scanout writes finish. */
  class FrameGuard {
   public:
    explicit FrameGuard(const Framebuffer& framebuffer);
    ~FrameGuard();
    explicit operator bool() const {
      return m_Active;
    }

   private:
    FrameGuard(const FrameGuard&) = delete;
    FrameGuard& operator=(const FrameGuard&) = delete;
    sigset_t m_PreviousMask;
    bool m_Masked;
    bool m_Active;
  };

  /** A reacquired VT needs its retained scene presented again. */
  bool takeRedraw();

  /** Store current mode (before switching). */
  void storeMode();

  /** Restore saved mode. */
  void restoreMode();

  /** Create framebuffer, enter desired mode. */
  int enterMode(size_t desiredW, size_t desiredH, size_t desiredBpp);

  /** Create framebuffer using the display mode selected by the bootloader. */
  int useCurrentMode();

  /** Retrieve base address of the framebuffer. */
  void* getFramebuffer() const {
    return m_pFramebuffer;
  }

  /** Flush framebuffer in the given region. */
  void flush(size_t x, size_t y, size_t w, size_t h);

  /** Get framebuffer colour format. */
  cairo_format_t getFormat() const {
    return m_Format;
  }

  /** Get framebuffer dimensions. */
  size_t getWidth() const {
    return m_Width;
  }
  size_t getHeight() const {
    return m_Height;
  }
  size_t getBytesPerLine() const {
    return m_BytesPerLine;
  }

 private:
  int mapMode(const pedigree_fb_mode& mode);
  static void terminalSignal(int signal);
  void releaseTerminal();

  void* m_pFramebuffer;
  size_t m_FramebufferSize;

  cairo_format_t m_Format;

  size_t m_Width;
  size_t m_Height;
  size_t m_BytesPerLine;

  int m_Fb;

  bool m_bStoredMode;
  pedigree_fb_mode m_StoredMode;

  int m_Terminal = -1;
  volatile sig_atomic_t m_TerminalActive = 1;
  volatile sig_atomic_t m_TerminalRedraw = 0;
  int m_PreviousDisplayMode = 0;
  vt_mode m_PreviousTerminalMode = {};
  struct sigaction m_PreviousReleaseAction = {};
  struct sigaction m_PreviousAcquireAction = {};
  sigset_t m_PreviousTerminalMask;
};

#endif
