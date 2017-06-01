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

#ifndef TUI_TUI_H
#define TUI_TUI_H

#include <sys/types.h>

class Widget;
class DirtyRectangle;

class TuiRedrawer
{
    public:
    virtual ~TuiRedrawer() = default;

    virtual void redraw(size_t x, size_t y, size_t w, size_t h) = 0;
};

class Tui
{
    public:
    /// Default constructor which builds without using a widget.
    Tui(TuiRedrawer *pRedrawer);

    /// Constructor which builds using a widget.
    Tui(Widget *widget);

    virtual ~Tui();

    /// (Re-)initialise the terminal
    bool initialise(size_t width, size_t height);

    /// Set the cursor fill state (e.g. box outline vs shaded box)
    void setCursorStyle(bool filled);

    /// Re-create rendering surfaces from the newest framebuffer.
    void recreateSurfaces(void *fb);

    /// Handle a resize of the terminal.
    void resize(size_t newWidth, size_t newHeight);

    /// Runs the TUI main loop.
    void run();

    /// Stops the TUI main loop.
    void stop();

    /// Handles a key press with all Pedigree input special flags.
    void keyInput(uint64_t key);

    /// Performs a redraw.
    void redraw(DirtyRectangle &rect);

    private:
    struct TuiLocal *m_LocalData;

    Widget *m_pWidget;
    TuiRedrawer *m_pRedrawer;
};

#endif
