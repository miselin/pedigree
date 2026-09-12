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

#include "Terminal.h"
#include "pedigree/native/graphics/Graphics.h"

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <utmp.h>

#include "environment.h"
#include <cairo/cairo.h>
#include <pedigree/log.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

extern PedigreeGraphics::Framebuffer* g_pFramebuffer;

Terminal::Terminal(char* pName, size_t nWidth, size_t nHeight, size_t offsetLeft, size_t offsetTop,
                   rgb_t* pBackground, cairo_t* pCairo, class Widget* pWidget, class Tui* pTui,
                   class Font* pNormalFont, class Font* pBoldFont)
    : m_pBuffer(0),
      m_pFramebuffer(0),
      m_pVterm(0),
      m_Len(0),
      m_bHasPendingRequest(false),
      m_PendingRequestSz(0),
      m_Pid(0),
      m_OffsetLeft(offsetLeft),
      m_OffsetTop(offsetTop) {
  cairo_save(pCairo);
  cairo_set_operator(pCairo, CAIRO_OPERATOR_SOURCE);

  cairo_set_source_rgba(pCairo, 0, 0, 0, 0.8);

  cairo_rectangle(pCairo, m_OffsetLeft, m_OffsetTop, nWidth, nHeight);
  cairo_fill(pCairo);

  cairo_restore(pCairo);

  strncpy(m_pName, pName, 256);
  m_pName[255] = 0;

  m_pVterm = new Vterm(0, nWidth, nHeight, m_OffsetLeft, m_OffsetTop, this, pWidget, pTui,
                       pNormalFont, pBoldFont);
}

bool Terminal::initialise() {
  m_MasterPty = posix_openpt(O_RDWR | O_NOCTTY);
  if (m_MasterPty < 0) {
    pedigree_log(LOG_INFO, "TUI: Couldn't create terminal: %s", strerror(errno));
    return false;
  }

#ifdef TARGET_LINUX
  grantpt(m_MasterPty);
  unlockpt(m_MasterPty);
#endif
  char slavename[16] = {0};
  strncpy(slavename, ptsname(m_MasterPty), 16);
  slavename[15] = 0;

  struct winsize ptySize;
  memset(&ptySize, 0, sizeof(ptySize));
  ptySize.ws_row = m_pVterm->getRows();
  ptySize.ws_col = m_pVterm->getCols();
  ioctl(m_MasterPty, TIOCSWINSZ, &ptySize);

  // Fire up a shell session.
  int pid = m_Pid = fork();
  if (pid == -1) {
    pedigree_log(LOG_INFO, "TUI: Couldn't fork: %s", strerror(errno));
    DirtyRectangle rect;
    const char* message = "Couldn't fork: ";
    write(message, strlen(message), rect);
    const char* error = strerror(errno);
    write(error, strlen(error), rect);
    redrawAll(rect);
    return false;
  } else if (pid == 0) {
    close(0);
    close(1);
    close(2);
    close(m_MasterPty);

    // Create a new session for the shell, which will also wipe any
    // existing CTTY.
    setsid();

    // Open the slave terminal with correct rights.
    int n = open(slavename, O_RDONLY);
    open(slavename, O_WRONLY);
    open(slavename, O_WRONLY);

    if (n < 0) {
      pedigree_log(LOG_INFO, "opening %s failed", slavename);
      pedigree_log(LOG_INFO, "opening stdin failed %d %s", errno, strerror(errno));
    }

    // Mark opened slave as our ctty.
    pedigree_log(LOG_INFO, "Trying to set CTTY");
    ioctl(1, TIOCSCTTY, 0);

    // Set ourselves as the terminal's foreground process group.
    tcsetpgrp(1, getpgrp());

    // libvterm implements the xterm 256-colour control set exposed here.
    setenv("TERM", "xterm-256color", 1);

    // Get current user's shell.
    struct passwd* pw = getpwuid(getuid());

    // Program to run.
    const char* prog = pw->pw_shell;
    if (!prog) {
      prog = getenv("SHELL");
      if (!prog) {
        // Fall back to bash
        pedigree_log(LOG_WARNING, "$SHELL unset, falling back to /bin/bash");
        prog = "/bin/bash";
      }
    }

// Create utmpx entry.
#ifndef TARGET_LINUX
    /// \todo Clean it up when the child terminates.
    struct utmpx ut;
    ut.ut_type = USER_PROCESS;
    ut.ut_pid = getpid();
    const char* ttyid = slavename + strlen("/dev/");
    strncpy(ut.ut_id, ttyid + strlen("tty"), sizeof(ut.ut_id));
    strncpy(ut.ut_line, ttyid, UT_LINESIZE);
    strncpy(ut.ut_user, pw->pw_name, UT_NAMESIZE);
    gettimeofday(&ut.ut_tv, NULL);

    setutxent();
    pututxline(&ut);
    endutxent();
#endif

    // Launch the shell now.
    execl(prog, prog, NULL);
    pedigree_log(LOG_ALERT, "Launching shell failed (next line is the error in errno...)");
    pedigree_log(LOG_ALERT, "error: %s", strerror(errno));

    DirtyRectangle rect;
    const char* message = "Couldn't load shell for this terminal... ";
    write(message, strlen(message), rect);
    const char* error = strerror(errno);
    write(error, strlen(error), rect);
    message =
        "\r\n\r\nYour installation of Pedigree may not be complete, or you may have hit a bug.";
    write(message, strlen(message), rect);
    redrawAll(rect);

    exit(1);
  }

  m_Pid = pid;

  return true;
}

Terminal::~Terminal() {
  if (m_Pid) {
    // Kill child.
    kill(m_Pid, SIGTERM);

    // Reap child.
    waitpid(m_Pid, 0, 0);
  }

  delete m_pVterm;
}

bool Terminal::isAlive() {
  if (m_Pid) {
    // Is our child still alive?
    pid_t result = waitpid(m_Pid, 0, WNOHANG);
    if (result == m_Pid) {
      m_Pid = 0;
      return false;
    }
  }
  return true;
}

void Terminal::renewBuffer(size_t nWidth, size_t nHeight) {
  m_pVterm->resize(nWidth, nHeight, 0);

  /// \todo Send SIGWINCH in console layer.
  struct winsize ptySize;
  ptySize.ws_row = m_pVterm->getRows();
  ptySize.ws_col = m_pVterm->getCols();
  ioctl(m_MasterPty, TIOCSWINSZ, &ptySize);
}

void Terminal::processKey(uint64_t key) {
  m_pVterm->processKey(key);
}

char Terminal::getFromQueue() {
  if (m_Len > 0) {
    char c = m_pQueue[0];
    for (size_t i = 0; i < m_Len - 1; i++)
      m_pQueue[i] = m_pQueue[i + 1];
    m_Len--;
    return c;
  } else
    return 0;
}

void Terminal::clearQueue() {
  m_Len = 0;
}

void Terminal::write(const char* pStr, size_t length, DirtyRectangle& rect) {
  m_pVterm->hideCursor(rect);
  m_pVterm->write(pStr, length, rect);
  m_pVterm->showCursor(rect);
}

void Terminal::sendInput(const char* bytes, size_t length) {
  for (size_t i = 0; i < length; ++i)
    addToQueue(bytes[i]);
  addToQueue(0, true);
}

void Terminal::addToQueue(char c, bool bFlush) {
  // Don't allow keys to be pressed past the buffer's size
  if ((c != 0) && (m_Len >= 256)) {
    return;
  }
  if ((c == 0) && (!m_Len)) {
    return;
  }

  if (c)
    m_pQueue[m_Len++] = c;

  if (bFlush) {
    ssize_t result = ::write(m_MasterPty, m_pQueue, m_Len);
    if (result >= 0) {
      size_t missing = m_Len - result;
      if (missing)
        memmove(m_pQueue, &m_pQueue[result], missing);
      m_Len = missing;
    } else {
      pedigree_log(LOG_ALERT, "Terminal::addToQueue flush failed");
    }
  }
}

void Terminal::setActive(bool b, DirtyRectangle& rect) {
  // Force complete redraw
  // m_pFramebuffer->redraw(0, 0, m_pFramebuffer->getWidth(),
  // m_pFramebuffer->getHeight(), false);

  // if (b)
  //    Syscall::setCurrentBuffer(m_pBuffer);
}
