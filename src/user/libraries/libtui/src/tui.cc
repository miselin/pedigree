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

#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/klog.h>
#include <unistd.h>

#include <Font.h>
#include <Terminal.h>
#include <Xterm.h>
#include <tui.h>

struct TuiLocal
{
    Terminal *pTerminal = nullptr;

    size_t nWidth = 0;
    size_t nHeight = 0;

    bool bKeyPressed = false;
    bool bRunning = false;

    cairo_t *pCairo = nullptr;
    cairo_surface_t *pSurface = nullptr;

    Font *pNormalFont = nullptr;
    Font *pBoldFont = nullptr;
};

Tui::Tui(TuiRedrawer *pRedrawer)
    : m_LocalData(nullptr), m_pWidget(nullptr), m_pRedrawer(pRedrawer)
{
    m_LocalData = new TuiLocal;
}

Tui::Tui(Widget *widget) : Tui(static_cast<TuiRedrawer *>(nullptr))
{
    m_pWidget = widget;
}

Tui::~Tui()
{
    delete m_LocalData->pTerminal;
    delete m_LocalData->pBoldFont;
    delete m_LocalData->pNormalFont;

    cairo_surface_destroy(m_LocalData->pSurface);
    cairo_destroy(m_LocalData->pCairo);

    delete m_LocalData;
}

bool Tui::initialise(size_t width, size_t height)
{
    m_LocalData->nWidth = width;
    m_LocalData->nHeight = height;

    if (!m_LocalData->pCairo)
    {
        klog(LOG_ALERT, "TUI: cairo instance is not yet valid!");
        return false;
    }

    cairo_set_line_cap(m_LocalData->pCairo, CAIRO_LINE_CAP_SQUARE);
    cairo_set_line_join(m_LocalData->pCairo, CAIRO_LINE_JOIN_MITER);
    cairo_set_antialias(m_LocalData->pCairo, CAIRO_ANTIALIAS_NONE);
    cairo_set_line_width(m_LocalData->pCairo, 1.0);

    cairo_set_operator(m_LocalData->pCairo, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(m_LocalData->pCairo, 0, 0, 0, 1.0);
    cairo_paint(m_LocalData->pCairo);

    if (!m_LocalData->pNormalFont)
    {
        m_LocalData->pNormalFont =
            new Font(m_LocalData->pCairo, 14, "DejaVu Sans Mono 10", true, 0);
        if (!m_LocalData->pNormalFont)
        {
            klog(LOG_EMERG, "Error: Normal font not loaded!");
            return false;
        }
    }

    if (!m_LocalData->pBoldFont)
    {
        m_LocalData->pBoldFont = new Font(
            m_LocalData->pCairo, 14, "DejaVu Sans Mono Bold 10", true, 0);
        if (!m_LocalData->pBoldFont)
        {
            klog(LOG_EMERG, "Error: Bold font not loaded!");
            return false;
        }
    }

    if (m_LocalData->pTerminal)
    {
        delete m_LocalData->pTerminal;
    }

    char newTermName[256];
    sprintf(newTermName, "Console%d", getpid());

    DirtyRectangle rect;

    m_LocalData->pTerminal = new Terminal(
        newTermName, m_LocalData->nWidth, m_LocalData->nHeight, 0, 0, 0,
        m_LocalData->pCairo, m_pWidget, this, m_LocalData->pNormalFont,
        m_LocalData->pBoldFont);
    m_LocalData->pTerminal->setCairo(
        m_LocalData->pCairo, m_LocalData->pSurface);
    if (!m_LocalData->pTerminal->initialise())
    {
        delete m_LocalData->pTerminal;
        m_LocalData->pTerminal = nullptr;
    }
    else
    {
        m_LocalData->pTerminal->setActive(true, rect);
        m_LocalData->pTerminal->redrawAll(rect);
    }

    rect.point(0, 0);
    rect.point(m_LocalData->nWidth, m_LocalData->nHeight);

    if (!m_LocalData->pTerminal)
    {
        klog(
            LOG_ALERT,
            "TUI: couldn't start up a terminal - failing gracefully...");
        m_LocalData->pBoldFont->render(
            "There are no pseudo-terminals available.", 5, 5, 0xFFFFFF,
            0x000000, false);
        m_LocalData->pBoldFont->render(
            "Press any key to close this window.", 5,
            m_LocalData->pBoldFont->getHeight() + 5, 0xFFFFFF, 0x000000, false);

        redraw(rect);

        m_LocalData->bKeyPressed = false;
        while (!m_LocalData->bKeyPressed)
        {
            if (m_pWidget)
            {
                Widget::checkForEvents(false);
            }
            else
            {
                // yield??
            }
        }

        return false;
    }

    redraw(rect);

    return true;
}

void Tui::setCursorStyle(bool filled)
{
    if (m_LocalData->pTerminal)
    {
        DirtyRectangle dirty;
        m_LocalData->pTerminal->setCursorStyle(filled);
        m_LocalData->pTerminal->showCursor(dirty);
        redraw(dirty);
    }
}

void Tui::recreateSurfaces(void *fb)
{
    if (!(m_LocalData->nWidth && m_LocalData->nHeight))
    {
        // don't yet know the size of the framebuffer
        return;
    }

    if (m_LocalData->pSurface)
    {
        cairo_surface_destroy(m_LocalData->pSurface);
        cairo_destroy(m_LocalData->pCairo);
    }

    // Wipe out the framebuffer before we do much with it.
    int stride =
        cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, m_LocalData->nWidth);
    memset(fb, 0, m_LocalData->nHeight * stride);

    m_LocalData->pSurface = cairo_image_surface_create_for_data(
        (uint8_t *) fb, CAIRO_FORMAT_ARGB32, m_LocalData->nWidth,
        m_LocalData->nHeight, stride);
    m_LocalData->pCairo = cairo_create(m_LocalData->pSurface);

    if (m_LocalData->pTerminal)
    {
        m_LocalData->pTerminal->setCairo(
            m_LocalData->pCairo, m_LocalData->pSurface);
    }

    if (m_LocalData->pNormalFont)
    {
        m_LocalData->pNormalFont->updateCairo(m_LocalData->pCairo);
    }

    if (m_LocalData->pBoldFont)
    {
        m_LocalData->pBoldFont->updateCairo(m_LocalData->pCairo);
    }
}

void Tui::resize(size_t newWidth, size_t newHeight)
{
    m_LocalData->nWidth = newWidth;
    m_LocalData->nHeight = newHeight;

    if (!(m_LocalData->pTerminal && m_LocalData->pCairo))
    {
        // Nothing more to do for us.
        return;
    }

    // Wipe out the framebuffer, start over.
    cairo_set_operator(m_LocalData->pCairo, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(m_LocalData->pCairo, 0, 0, 0, 0.8);
    cairo_rectangle(
        m_LocalData->pCairo, 0, 0, m_LocalData->nWidth, m_LocalData->nHeight);
    cairo_fill(m_LocalData->pCairo);

    if (m_LocalData->pTerminal)
    {
        m_LocalData->pTerminal->renewBuffer(newWidth, newHeight);

        DirtyRectangle rect;
        m_LocalData->pTerminal->redrawAll(rect);
        m_LocalData->pTerminal->showCursor(rect);
        redraw(rect);

        kill(m_LocalData->pTerminal->getPid(), SIGWINCH);
    }
}

void Tui::run()
{
    size_t maxBuffSz = 32768;
    char buffer[32768];

    m_LocalData->bRunning = true;
    while (m_LocalData->bRunning)
    {
        int n = 0;

        fd_set fds;
        FD_ZERO(&fds);

        if (m_pWidget)
        {
            n = std::max(m_pWidget->getSocket(), n);
            FD_SET(m_pWidget->getSocket(), &fds);
        }

        if (m_LocalData->pTerminal)
        {
            if (!m_LocalData->pTerminal->isAlive())
            {
                m_LocalData->bRunning = false;
            }
            else
            {
                int fd = m_LocalData->pTerminal->getSelectFd();
                FD_SET(fd, &fds);
                n = std::max(fd, n);
            }
        }

        if (!m_LocalData->bRunning)
        {
            continue;
        }

        int nReady = select(n + 1, &fds, NULL, NULL, 0);
        if (nReady <= 0)
        {
            continue;
        }

        // Check for widget events.
        if (m_pWidget)
        {
            if (FD_ISSET(m_pWidget->getSocket(), &fds))
            {
                // Dispatch callbacks.
                Widget::checkForEvents(true);

                // Don't do redraw processing if this was the only descriptor
                // that was found readable.
                if (nReady == 1)
                {
                    continue;
                }
            }
        }

        bool bShouldRedraw = false;

        DirtyRectangle dirtyRect;
        if (m_LocalData->pTerminal)
        {
            int fd = m_LocalData->pTerminal->getSelectFd();
            if (FD_ISSET(fd, &fds))
            {
                // Something to read.
                ssize_t len = read(fd, buffer, maxBuffSz);
                if (len > 0)
                {
                    buffer[len] = 0;
                    m_LocalData->pTerminal->write(buffer, dirtyRect);
                    bShouldRedraw = true;
                }
            }
        }

        if (bShouldRedraw)
        {
            redraw(dirtyRect);
        }
    }

    klog(LOG_INFO, "TUI shutting down cleanly.");
}

void Tui::stop()
{
    m_LocalData->bRunning = false;
}

void Tui::keyInput(uint64_t key)
{
    if (!m_LocalData->pTerminal)
        return;

    // CTRL + key -> unprintable characters
    if ((key & Keyboard::Ctrl) && !(key & Keyboard::Special))
    {
        key &= 0x1F;
    }

    m_LocalData->pTerminal->processKey(key);
}

void Tui::redraw(DirtyRectangle &rect)
{
    if (rect.getX() == ~0UL && rect.getY() == ~0UL && rect.getX2() == 0 &&
        rect.getY2() == 0)
    {
        return;
    }

    PedigreeGraphics::Rect rt(
        rect.getX(), rect.getY(), rect.getWidth(), rect.getHeight());
    if (m_LocalData->pSurface)
    {
        cairo_surface_flush(m_LocalData->pSurface);
    }

    if (m_pWidget)
    {
        m_pWidget->redraw(rt);
    }
    else if (m_pRedrawer)
    {
        m_pRedrawer->redraw(
            rect.getX(), rect.getY(), rect.getWidth(), rect.getHeight());
    }
}
