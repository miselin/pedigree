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

#ifndef TERMINAL_H
#define TERMINAL_H

#include "pedigree/native/graphics/Graphics.h"

#include <syslog.h>

#include "VtermWidget.h"
#include "environment.h"
#include <cairo/cairo.h>

/** A Terminal owns the PTY and the terminal screen adapter. */
class Terminal {
 public:
  Terminal(char* pName, size_t nWidth, size_t nHeight, size_t offsetLeft, size_t offsetTop,
           rgb_t* pBackground, cairo_t* pCairo, class Widget* pWidget, class Tui* pTui,
           class Font* pNormalFont, class Font* pBoldFont);
  ~Terminal();

  bool initialise();

  /** Verifies whether the terminal and its child are alive. */
  bool isAlive();

  /** Kills and recreates the terminal's buffer, with a change in size. */
  void renewBuffer(size_t nWidth, size_t nHeight);

  /** Adds a 64-bit keycode from the Keyboard class. */
  void processKey(uint64_t key);

  void sendInput(const char* bytes, size_t length);

  /** Wipes out the queue for this terminal. */
  void clearQueue();

  /** Grabs 1 byte of the utf-8 encoded queue. */
  char getFromQueue();

  /** Returns the queue length. */
  size_t queueLength() {
    return m_Len;
  }

  /** Gets descriptor to select() on for readability to see if we have pending
   * output. */
  int getSelectFd() const {
    return m_MasterPty;
  }

  /** Writes the given byte sequence to the terminal emulator. */
  void write(const char* pStr, size_t length, DirtyRectangle& rect);

  void setHasPendingRequest(bool b, size_t sz) {
    m_bHasPendingRequest = b;
    m_PendingRequestSz = sz;
  }

  bool hasPendingRequest() {
    return m_bHasPendingRequest;
  }
  size_t getPendingRequestSz() {
    return m_PendingRequestSz;
  }

  void setActive(bool b, DirtyRectangle& rect);

  size_t getRows() {
    return m_pVterm->getRows();
  }
  size_t getCols() {
    return m_pVterm->getCols();
  }

  void redrawAll(DirtyRectangle& rect) {
    m_pVterm->renderAll(rect);
  }

  void refresh() {
    // Force our buffer to the screen
    if (m_pFramebuffer)
      m_pFramebuffer->redraw(0, 0, m_pFramebuffer->getWidth(), m_pFramebuffer->getHeight(), false);
  }

  rgb_t* getBuffer() {
    return m_pBuffer;
  }

  int getPid() {
    return m_Pid;
  }

  void showCursor(DirtyRectangle& rect) {
    m_pVterm->showCursor(rect);
  }

  void hideCursor(DirtyRectangle& rect) {
    m_pVterm->hideCursor(rect);
  }

  void setCursorStyle(bool bFilled = true) {
    m_pVterm->setCursorStyle(bFilled);
  }

  void setCairo(cairo_t* pCairo, cairo_surface_t* pSurface) {
    m_pVterm->setCairo(pCairo, pSurface);
  }

  void setFonts(Font* pNormalFont, Font* pBoldFont) {
    m_pVterm->setFonts(pNormalFont, pBoldFont);
  }

 private:
  Terminal(const Terminal&);
  Terminal& operator=(const Terminal&);

  void addToQueue(char c, bool bFlush = false);

  rgb_t* m_pBuffer;

  PedigreeGraphics::Framebuffer* m_pFramebuffer;
  Vterm* m_pVterm;

  char m_pName[256];

  char m_pQueue[256];
  size_t m_Len;

  bool m_bHasPendingRequest;
  size_t m_PendingRequestSz;

  int m_Pid;

  int m_MasterPty;

  size_t m_OffsetLeft, m_OffsetTop;
};

#endif
