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

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pedigree_fb.h"
#include <pedigree/fb.h>
#include <pedigree/log.h>
#include <sys/ioctl.h>
#include <sys/kd.h>
#include <sys/mman.h>

namespace {
Framebuffer* terminalFramebuffer = nullptr;

sigset_t terminalSignals() {
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGUSR1);
  sigaddset(&signals, SIGUSR2);
  return signals;
}
}  // namespace

Framebuffer::Framebuffer()
    : m_pFramebuffer(0),
      m_FramebufferSize(0),
      m_Format(),
      m_Width(0),
      m_Height(0),
      m_BytesPerLine(0),
      m_Fb(-1),
      m_bStoredMode(false),
      m_StoredMode() {}

Framebuffer::~Framebuffer() {
  releaseTerminal();
  if (m_pFramebuffer) {
    munmap(m_pFramebuffer, m_FramebufferSize);
    m_pFramebuffer = 0;
    m_FramebufferSize = 0;
  }

  if (m_Fb >= 0) {
    close(m_Fb);
    m_Fb = -1;
  }
}

Framebuffer::FrameGuard::FrameGuard(const Framebuffer& framebuffer)
    : m_Masked(false), m_Active(false) {
  if (framebuffer.m_Terminal < 0) {
    m_Active = true;
    return;
  }
  const sigset_t signals = terminalSignals();
  m_Masked = sigprocmask(SIG_BLOCK, &signals, &m_PreviousMask) == 0;
  m_Active = m_Masked && framebuffer.m_TerminalActive;
}

Framebuffer::FrameGuard::~FrameGuard() {
  if (m_Masked) {
    sigprocmask(SIG_SETMASK, &m_PreviousMask, nullptr);
  }
}

bool Framebuffer::claimTerminal() {
  if (m_Terminal >= 0) {
    return true;
  }
  if (terminalFramebuffer) {
    return false;
  }
  int terminal = open("/dev/tty0", O_RDWR | O_NOCTTY | O_CLOEXEC);
  if (terminal < 0) {
    return false;
  }
  vt_stat state = {};
  if (ioctl(terminal, VT_GETSTATE, &state) < 0 || !state.v_active) {
    close(terminal);
    return false;
  }
  const unsigned number = state.v_active;
  close(terminal);
  char path[32];
  snprintf(path, sizeof(path), "/dev/tty%u", number);
  terminal = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
  if (terminal < 0) {
    return false;
  }
  if (ioctl(terminal, VT_GETMODE, &m_PreviousTerminalMode) < 0 ||
      m_PreviousTerminalMode.mode != VT_AUTO ||
      ioctl(terminal, KDGETMODE, &m_PreviousDisplayMode) < 0) {
    close(terminal);
    return false;
  }
  const sigset_t signals = terminalSignals();
  if (sigprocmask(SIG_BLOCK, &signals, &m_PreviousTerminalMask) < 0) {
    close(terminal);
    return false;
  }
  struct sigaction action = {};
  action.sa_handler = terminalSignal;
  action.sa_mask = signals;
  if (sigaction(SIGUSR1, &action, &m_PreviousReleaseAction) < 0) {
    close(terminal);
    sigprocmask(SIG_SETMASK, &m_PreviousTerminalMask, nullptr);
    return false;
  }
  if (sigaction(SIGUSR2, &action, &m_PreviousAcquireAction) < 0) {
    sigaction(SIGUSR1, &m_PreviousReleaseAction, nullptr);
    close(terminal);
    sigprocmask(SIG_SETMASK, &m_PreviousTerminalMask, nullptr);
    return false;
  }
  m_Terminal = terminal;
  terminalFramebuffer = this;
  vt_mode mode = {};
  mode.mode = VT_PROCESS;
  mode.relsig = SIGUSR1;
  mode.acqsig = SIGUSR2;
  if (ioctl(terminal, VT_SETMODE, &mode) < 0 || ioctl(terminal, KDSETMODE, KD_GRAPHICS) < 0 ||
      ioctl(terminal, VT_GETSTATE, &state) < 0) {
    releaseTerminal();
    return false;
  }
  m_TerminalActive = state.v_active == number;
  sigprocmask(SIG_UNBLOCK, &signals, nullptr);
  return true;
}

void Framebuffer::terminalSignal(int signal) {
  const int previousError = errno;
  Framebuffer* framebuffer = terminalFramebuffer;
  if (framebuffer) {
    if (signal == SIGUSR1) {
      framebuffer->m_TerminalActive = 0;
      if (ioctl(framebuffer->m_Terminal, VT_RELDISP, 1) < 0) {
        framebuffer->m_TerminalActive = 1;
      }
    } else if (signal == SIGUSR2) {
      if (ioctl(framebuffer->m_Terminal, VT_RELDISP, VT_ACKACQ) == 0) {
        framebuffer->m_TerminalActive = 1;
        framebuffer->m_TerminalRedraw = 1;
      }
    }
  }
  errno = previousError;
}

bool Framebuffer::takeRedraw() {
  FrameGuard guard(*this);
  const bool redraw = guard && m_TerminalRedraw;
  if (redraw) {
    m_TerminalRedraw = 0;
  }
  return redraw;
}

void Framebuffer::releaseTerminal() {
  if (m_Terminal < 0) {
    return;
  }
  const sigset_t signals = terminalSignals();
  sigset_t mask;
  sigprocmask(SIG_BLOCK, &signals, &mask);
  ioctl(m_Terminal, KDSETMODE, m_PreviousDisplayMode);
  ioctl(m_Terminal, VT_SETMODE, &m_PreviousTerminalMode);
  // Notifications queued before ownership was returned must not reach the
  // application's previous handlers after this object is destroyed.
  const timespec timeout = {};
  while (sigtimedwait(&signals, nullptr, &timeout) >= 0) {
  }
  terminalFramebuffer = nullptr;
  sigaction(SIGUSR1, &m_PreviousReleaseAction, nullptr);
  sigaction(SIGUSR2, &m_PreviousAcquireAction, nullptr);
  close(m_Terminal);
  m_Terminal = -1;
  const int notifications[] = {SIGUSR1, SIGUSR2};
  for (const int signal : notifications) {
    if (sigismember(&m_PreviousTerminalMask, signal)) {
      sigaddset(&mask, signal);
    } else {
      sigdelset(&mask, signal);
    }
  }
  sigprocmask(SIG_SETMASK, &mask, nullptr);
}

bool Framebuffer::initialise() {
  // Grab a framebuffer to use.
  m_Fb = open("/dev/fb", O_RDWR);
  if (m_Fb < 0) {
    pedigree_log(LOG_INFO, "libfb: no framebuffer device");
    fprintf(stderr, "libfb: couldn't open framebuffer device");
    return false;
  }

  return true;
}

void Framebuffer::storeMode() {
  if (m_bStoredMode)
    return;

  // Grab the current mode so we can restore it if we die.
  pedigree_fb_mode current_mode;
  int result = ioctl(m_Fb, PEDIGREE_FB_GETMODE, &current_mode);
  if (result == 0) {
    m_bStoredMode = true;
    m_StoredMode = current_mode;
  }
}

void Framebuffer::restoreMode() {
  if (!m_bStoredMode)
    return;

  // Restore old graphics mode.
  pedigree_fb_modeset old_mode = {m_StoredMode.width, m_StoredMode.height, m_StoredMode.depth};
  ioctl(m_Fb, PEDIGREE_FB_SETMODE, &old_mode);

  m_bStoredMode = false;
}

int Framebuffer::mapMode(const pedigree_fb_mode& set_mode) {
  // All good, framebuffer already exists.
  if (m_pFramebuffer)
    return 0;

  if (!set_mode.width || !set_mode.height || !set_mode.depth) {
    pedigree_log(LOG_INFO, "libfb: current mode is not a graphics mode");
    return EXIT_FAILURE;
  }

  m_Width = set_mode.width;
  m_Height = set_mode.height;

  m_Format = CAIRO_FORMAT_ARGB32;
  if (set_mode.format == PEDIGREE_FB_FORMAT_RGB24) {
    if (set_mode.bytes_per_pixel != 4) {
      fprintf(stderr,
              "libfb: error: incompatible framebuffer format (bytes "
              "per pixel)\n");
      return EXIT_FAILURE;
    }
  } else if (set_mode.format == PEDIGREE_FB_FORMAT_RGB565) {
    m_Format = CAIRO_FORMAT_RGB16_565;
  } else if (set_mode.format > PEDIGREE_FB_FORMAT_RGB32) {
    fprintf(stderr,
            "libfb: error: incompatible framebuffer format (possibly "
            "BGR or similar)\n");
    return EXIT_FAILURE;
  }

  const size_t minimumStride = cairo_format_stride_for_width(m_Format, set_mode.width);
  const size_t stride = set_mode.bytes_per_line ? set_mode.bytes_per_line : minimumStride;
  if (stride < minimumStride || set_mode.height > SIZE_MAX / stride) {
    pedigree_log(LOG_CRIT, "libfb: invalid framebuffer stride");
    return EXIT_FAILURE;
  }

  // Map the framebuffer in to our address space.
  pedigree_log(LOG_INFO, "Mapping /dev/fb in (sz=%zx)...", stride * set_mode.height);
  m_pFramebuffer = mmap(0, stride * set_mode.height, PROT_READ | PROT_WRITE, MAP_SHARED, m_Fb, 0);
  pedigree_log(LOG_INFO, "Got %p...", m_pFramebuffer);

  if (m_pFramebuffer == MAP_FAILED) {
    m_pFramebuffer = 0;
    pedigree_log(LOG_CRIT, "libfb: couldn't map framebuffer into address space");
    return EXIT_FAILURE;
  } else {
    pedigree_log(LOG_INFO, "libfb: mapped framebuffer at %p", m_pFramebuffer);
  }

  m_FramebufferSize = stride * set_mode.height;
  m_BytesPerLine = stride;

  return 0;
}

int Framebuffer::useCurrentMode() {
  if (m_pFramebuffer)
    return 0;

  pedigree_fb_mode current_mode;
  if (ioctl(m_Fb, PEDIGREE_FB_GETMODE, &current_mode) < 0) {
    pedigree_log(LOG_INFO, "libfb: can't get current mode info");
    return EXIT_FAILURE;
  }

  return mapMode(current_mode);
}

int Framebuffer::enterMode(size_t desiredW, size_t desiredH, size_t desiredBpp) {
  // All good, framebuffer already exists.
  if (m_pFramebuffer)
    return 0;

  // Can we set the graphics mode we want?
  pedigree_fb_modeset mode = {desiredW, desiredH, desiredBpp};
  int result = ioctl(m_Fb, PEDIGREE_FB_SETMODE, &mode);
  if (result < 0) {
    // No! Bad!
    /// \note Mode set logic will try and find a mode in a lower colour
    /// depth
    ///       if the desired one cannot be set.
    pedigree_log(LOG_INFO, "libfb: can't set the desired mode");
    fprintf(stderr, "libfb: could not set desired mode (%zux%zu) in any colour depth.\n",
            mode.width, mode.height);
    return EXIT_FAILURE;
  }

  pedigree_fb_mode set_mode;
  result = ioctl(m_Fb, PEDIGREE_FB_GETMODE, &set_mode);
  if (result < 0) {
    pedigree_log(LOG_INFO, "libfb: can't get mode info");
    fprintf(stderr, "libfb: could not get mode information after setting mode.\n");

    // Back to text.
    memset(&mode, 0, sizeof(mode));
    ioctl(m_Fb, PEDIGREE_FB_SETMODE, &mode);
    return EXIT_FAILURE;
  }

  return mapMode(set_mode);
}

void Framebuffer::flush(size_t x, size_t y, size_t w, size_t h) {
  // Submit a redraw to the graphics card.
  pedigree_fb_rect fbdirty = {x, y, w, h};
  ioctl(m_Fb, PEDIGREE_FB_REDRAW, &fbdirty);
}
