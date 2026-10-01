/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include "pedigree_fb.h"
#include <initializer_list>
#include <pedigree/log.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

int fixtureOpen(const char*, int);
int fixtureClose(int);
int fixtureIoctl(int, unsigned long, void*);
int fixtureIoctl(int, unsigned long, int);
void* fixtureMmap(void*, size_t, int, int, int, off_t);
int fixtureMunmap(void*, size_t);
#ifdef __APPLE__
int fixtureSigtimedwait(const sigset_t*, siginfo_t*, const timespec*);
#define sigtimedwait fixtureSigtimedwait
#endif

#define open fixtureOpen
#define close fixtureClose
#define ioctl fixtureIoctl
#define mmap fixtureMmap
#define munmap fixtureMunmap
#include "libfb-implementation.inc"
#undef open
#undef close
#undef ioctl
#undef mmap
#undef munmap
#ifdef __APPLE__
#undef sigtimedwait
#endif

namespace {
constexpr int FramebufferFd = 100, TerminalAliasFd = 101, TerminalFd = 102;
constexpr unsigned long VtGetMode = 0x5601, VtSetMode = 0x5602, VtRelDisp = 0x5605;
constexpr unsigned long VtGetState = 0x5603;
constexpr unsigned long KdSetMode = 0x4b3a;
constexpr unsigned long KdGetMode = 0x4b3b;
bool terminalOpenFailure = false;
bool switchDuringClaim = false;
unsigned short activeTerminal = 1;
int displayMode = KD_TEXT;
unsigned long ioctlFailure = 0;
unsigned releases = 0, acquisitions = 0, presents = 0;
uint32_t pixels[64];
volatile sig_atomic_t previousNotifications = 0;
void previousHandler(int) {
  ++previousNotifications;
}

sigset_t mask() {
  sigset_t result;
  assert(sigprocmask(SIG_BLOCK, nullptr, &result) == 0);
  return result;
}
void setBlocked(int signal, bool blocked) {
  sigset_t signals;
  assert(sigemptyset(&signals) == 0);
  assert(sigaddset(&signals, signal) == 0);
  assert(sigprocmask(blocked ? SIG_BLOCK : SIG_UNBLOCK, &signals, nullptr) == 0);
}
void checkRestored() {
  struct sigaction action;
  assert(sigaction(SIGUSR1, nullptr, &action) == 0);
  assert(action.sa_handler == previousHandler);
  assert(sigaction(SIGUSR2, nullptr, &action) == 0);
  assert(action.sa_handler == previousHandler);
  sigset_t signals = mask();
  assert(sigismember(&signals, SIGUSR1) == 1);
  assert(sigismember(&signals, SIGUSR2) == 0);
  assert(sigismember(&signals, SIGWINCH) == 1);
}
}  // namespace

int fixtureOpen(const char* path, int) {
  if (!std::strcmp(path, "/dev/fb")) {
    return FramebufferFd;
  }
  if (!std::strcmp(path, "/dev/tty0")) {
    return terminalOpenFailure ? -1 : TerminalAliasFd;
  }
  assert(!std::strcmp(path, "/dev/tty1"));
  return TerminalFd;
}
int fixtureClose(int descriptor) {
  assert(descriptor == FramebufferFd || descriptor == TerminalAliasFd || descriptor == TerminalFd);
  return 0;
}
int fixtureIoctl(int descriptor, unsigned long command, void* argument) {
  if (command == ioctlFailure) {
    ioctlFailure = 0;
    errno = EIO;
    return -1;
  }
  if (command == PEDIGREE_FB_GETMODE) {
    assert(descriptor == FramebufferFd);
    *static_cast<pedigree_fb_mode*>(argument) = {8, 8, 32, 4, PEDIGREE_FB_FORMAT_RGB32, 32};
  } else if (command == PEDIGREE_FB_REDRAW) {
    assert(descriptor == FramebufferFd);
    ++presents;
  } else if (command == VtGetMode) {
    assert(descriptor == TerminalFd);
    *static_cast<vt_mode*>(argument) = {};
  } else if (command == VtSetMode) {
    assert(descriptor == TerminalFd);
    const auto& mode = *static_cast<vt_mode*>(argument);
    if (mode.mode == 1) {
      assert(mode.relsig == SIGUSR1 && mode.acqsig == SIGUSR2);
    } else {
      assert(mode.mode == 0);
    }
  } else if (command == KdGetMode) {
    assert(descriptor == TerminalFd);
    *static_cast<int*>(argument) = displayMode;
  } else if (command == VtGetState) {
    assert(descriptor == TerminalAliasFd || descriptor == TerminalFd);
    *static_cast<vt_stat*>(argument) = {activeTerminal, 0, 3};
    if (descriptor == TerminalFd) {
      const sigset_t signals = mask();
      assert(sigismember(&signals, SIGUSR1) == 1);
      assert(sigismember(&signals, SIGUSR2) == 1);
    }
  } else {
    assert(false);
  }
  return 0;
}
int fixtureIoctl(int descriptor, unsigned long command, int value) {
  assert(descriptor == TerminalFd);
  if (command == ioctlFailure) {
    ioctlFailure = 0;
    errno = EIO;
    return -1;
  }
  if (command == VtRelDisp) {
    if (value == 1) {
      ++releases;
    } else {
      assert(value == 2);
      ++acquisitions;
    }
  } else {
    assert(command == KdSetMode);
    assert(value == 0 || value == 1);
    displayMode = value;
    if (value == KD_GRAPHICS && switchDuringClaim) {
      activeTerminal = 2;
      switchDuringClaim = false;
    }
  }
  return 0;
}
void* fixtureMmap(void*, size_t length, int, int, int descriptor, off_t) {
  assert(descriptor == FramebufferFd && length == sizeof(pixels));
  return pixels;
}
int fixtureMunmap(void* address, size_t length) {
  assert(address == pixels && length == sizeof(pixels));
  return 0;
}
extern "C" int pedigree_log(int, const char*, ...) {
  return 0;
}

#ifdef __APPLE__
// Darwin provides sigwait but not sigtimedwait; preserve its zero-timeout
// behavior using the kernel's actual pending signal set and signal consumption.
int fixtureSigtimedwait(const sigset_t* signals, siginfo_t*, const timespec* timeout) {
  assert(timeout->tv_sec == 0 && timeout->tv_nsec == 0);
  sigset_t pending;
  assert(sigpending(&pending) == 0);
  for (int signal : {SIGUSR1, SIGUSR2}) {
    if (sigismember(signals, signal) && sigismember(&pending, signal)) {
      int result = 0;
      assert(sigwait(signals, &result) == 0);
      return result;
    }
  }
  errno = EAGAIN;
  return -1;
}
#endif

int main() {
  struct sigaction action = {};
  action.sa_handler = previousHandler;
  assert(sigemptyset(&action.sa_mask) == 0);
  assert(sigaction(SIGUSR1, &action, nullptr) == 0);
  assert(sigaction(SIGUSR2, &action, nullptr) == 0);
  setBlocked(SIGUSR1, true);
  setBlocked(SIGUSR2, false);
  setBlocked(SIGWINCH, true);

  // Tests follow the normal initialise/claim/map lifecycle used by renderers.
  {
    Framebuffer framebuffer;
    assert(framebuffer.initialise());
    assert(framebuffer.claimTerminal());
    assert(framebuffer.claimTerminal());
    assert(framebuffer.useCurrentMode() == 0);
    assert(!framebuffer.takeRedraw());
    sigset_t signals = mask();
    assert(sigismember(&signals, SIGUSR1) == 0);
    assert(sigismember(&signals, SIGUSR2) == 0);
    assert(sigismember(&signals, SIGWINCH) == 1);
    {
      Framebuffer::FrameGuard frame(framebuffer);
      assert(frame);
      assert(raise(SIGUSR1) == 0);
      assert(releases == 0);
      {
        Framebuffer::FrameGuard nested(framebuffer);
        assert(nested);
      }
      assert(releases == 0);
      static_cast<uint32_t*>(framebuffer.getFramebuffer())[0] = 123;
      framebuffer.flush(0, 0, 8, 8);
    }
    assert(releases == 1 && presents == 1 && pixels[0] == 123);
    {
      Framebuffer::FrameGuard frame(framebuffer);
      assert(!frame);
      if (frame) {
        static_cast<uint32_t*>(framebuffer.getFramebuffer())[0] = 456;
        framebuffer.flush(0, 0, 8, 8);
      }
    }
    assert(presents == 1 && pixels[0] == 123);

    errno = EDOM;
    assert(raise(SIGUSR2) == 0);
    assert(errno == EDOM && acquisitions == 1);
    assert(framebuffer.takeRedraw());
    assert(!framebuffer.takeRedraw());
    {
      Framebuffer::FrameGuard frame(framebuffer);
      assert(frame);
    }

    // A failed release preserves ownership, and a failed acquire stays inactive.
    ioctlFailure = VtRelDisp;
    assert(raise(SIGUSR1) == 0);
    {
      Framebuffer::FrameGuard frame(framebuffer);
      assert(frame);
    }
    assert(raise(SIGUSR1) == 0);
    ioctlFailure = VtRelDisp;
    assert(raise(SIGUSR2) == 0);
    {
      Framebuffer::FrameGuard frame(framebuffer);
      assert(!frame);
    }
    assert(!framebuffer.takeRedraw());
    assert(raise(SIGUSR2) == 0);
    assert(framebuffer.takeRedraw());

    // Teardown drains outstanding VT notifications and restores only its signals.
    setBlocked(SIGALRM, true);
    setBlocked(SIGUSR1, true);
    assert(raise(SIGUSR1) == 0);
  }
  checkRestored();
  sigset_t signals = mask(), pending;
  assert(sigismember(&signals, SIGALRM) == 1);
  assert(sigpending(&pending) == 0 && sigismember(&pending, SIGUSR1) == 0);
  assert(previousNotifications == 0);
  setBlocked(SIGALRM, false);

  // Early failure and rollback after installing handlers preserve caller state.
  terminalOpenFailure = true;
  {
    Framebuffer framebuffer;
    assert(framebuffer.initialise());
    assert(!framebuffer.claimTerminal());
  }
  terminalOpenFailure = false;
  checkRestored();
  for (unsigned long command : {VtSetMode, KdSetMode}) {
    ioctlFailure = command;
    {
      Framebuffer framebuffer;
      assert(framebuffer.initialise());
      assert(!framebuffer.claimTerminal());
    }
    checkRestored();
  }
  {
    Framebuffer framebuffer, second;
    assert(framebuffer.initialise());
    assert(framebuffer.claimTerminal());
    assert(!second.claimTerminal());
  }
  checkRestored();
  // A switch during registration leaves the named VT inactive; teardown still
  // returns that VT to text mode without changing the currently active VT.
  switchDuringClaim = true;
  {
    Framebuffer framebuffer;
    assert(framebuffer.initialise());
    assert(framebuffer.claimTerminal());
    Framebuffer::FrameGuard frame(framebuffer);
    assert(!frame);
    assert(displayMode == KD_GRAPHICS);
  }
  assert(displayMode == KD_TEXT && activeTerminal == 2);
  checkRestored();
  puts("libfb VT process handover contracts passed");
}
