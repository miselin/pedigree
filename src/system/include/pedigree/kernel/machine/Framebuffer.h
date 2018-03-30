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

#ifndef _MACHINE_FRAMEBUFFER_H
#define _MACHINE_FRAMEBUFFER_H

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/graphics/Graphics.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/utility.h"

class Display;

/** This class provides a generic interface for interfacing with a framebuffer.
 *  Each display driver specialises this class to define the "base address" of
 *  the framebuffer in its own way (eg, allocate memory, or use a DMA region).
 *  There are a variety of default software-only operations, which are used by
 *  default if the main operational methods are not overridden. */
class EXPORTED_PUBLIC Framebuffer
{
  public:
    Framebuffer();
    virtual ~Framebuffer();

    size_t getWidth() const;
    size_t getHeight() const;

    Graphics::PixelFormat getFormat() const;

    bool getActive() const;

    void setActive(bool b);

    /// Sets the palette used for palette-based colour formats. Takes an
    /// array of pixels in Bits32_Argb format.
    void setPalette(uint32_t *palette, size_t nEntries);

    uint32_t *getPalette() const;

    /** Gets a raw pointer to the framebuffer itself. There is no way to
     *  know if this pointer points to an MMIO region or real RAM, so it
     *  cannot be guaranteed to be safe. */
    virtual void *getRawBuffer() const;

    /** Creates a new buffer to be used for blits from the given raw pixel
     *  data. Performs automatic conversion of the pixel format to the
     *  pixel format of the current display mode.
     *  Do not modify any of the members of the buffer structure, or attempt
     *  to inject your own pixels into the buffer.
     *  Once a buffer is created, it is only used for blitting to the screen
     *  and cannot be modified.
     *  It is expected that the buffer has been packed to its bit depth, and
     *  does not have any padding on each scanline at all.
     *  Do not delete the returned buffer yourself, pass it to destroyBuffer
     *  which performs a proper cleanup of all resources related to the
     *  buffer.
     *  The buffer should be padded to finish on a DWORD boundary. This is
     *  not padding per scanline but rather padding per buffer. */
    virtual Graphics::Buffer *createBuffer(
        const void *srcData, Graphics::PixelFormat srcFormat, size_t width,
        size_t height, uint32_t *pPalette = 0);

    /** Destroys a created buffer. Frees its memory in both the system RAM
     *  and any references still in VRAM. */
    virtual void destroyBuffer(Graphics::Buffer *pBuffer);

    /** Performs an update of a region of this framebuffer. This function
     *  can be used by drivers to request an area of the framebuffer be
     *  redrawn, but is useless for non-hardware-accelerated devices.
     *  \param x leftmost x co-ordinate of the redraw area, ~0 for "invalid"
     *  \param y topmost y co-ordinate of the redraw area, ~0 for "invalid"
     *  \param w width of the redraw area, ~0 for "invalid"
     *  \param h height of the redraw area, ~0 for "invalid"
     *  \param bChild non-zero if a child began the redraw, zero otherwise
     *  \note Because every redraw ends up redrawing a large region of the
     *        "root" framebuffer, it's not necessary to do framebuffer
     *        copies at any point. The only reason a framebuffer copy is
     *        done in this function is in the one case where a parent needs
     *        to override a child's framebuffer.
     */
    void redraw(
        size_t x = ~0UL, size_t y = ~0UL, size_t w = ~0UL, size_t h = ~0UL,
        bool bChild = false);

    /** Blits a given buffer to the screen. See createBuffer.
     *  \param bLowestCall whether this is the lowest level call for the
     *                     chain or a call being passed up from a child
     *                     somewhere in the chain.
     */
    virtual void blit(
        Graphics::Buffer *pBuffer, size_t srcx, size_t srcy, size_t destx,
        size_t desty, size_t width, size_t height, bool bLowestCall = true);

    /** Draws given raw pixel data to the screen. Used for framebuffer
     *  chains and applications which need to render constantly changing
     *  pixel buffers. */
    virtual void draw(
        void *pBuffer, size_t srcx, size_t srcy, size_t destx, size_t desty,
        size_t width, size_t height,
        Graphics::PixelFormat format = Graphics::Bits32_Argb,
        bool bLowestCall = true);

    /** Draws a single rectangle to the screen with the given colour. */
    virtual void rect(
        size_t x, size_t y, size_t width, size_t height, uint32_t colour,
        Graphics::PixelFormat format = Graphics::Bits32_Argb,
        bool bLowestCall = true);

    /** Copies a rectangle already on the framebuffer to a new location */
    virtual void copy(
        size_t srcx, size_t srcy, size_t destx, size_t desty, size_t w,
        size_t h, bool bLowestCall = true);

    /** Draws a line one pixel wide between two points on the screen */
    virtual void line(
        size_t x1, size_t y1, size_t x2, size_t y2, uint32_t colour,
        Graphics::PixelFormat format = Graphics::Bits32_Argb,
        bool bLowestCall = true);

    /** Sets an individual pixel on the framebuffer. Not inheritable. */
    void setPixel(
        size_t x, size_t y, uint32_t colour,
        Graphics::PixelFormat format = Graphics::Bits32_Argb,
        bool bLowestCall = true);

    /** Class friendship isn't inheritable, so these have to be public for
     *  graphics drivers to use. They shouldn't be touched by anything that
     *  isn't a graphics driver. */

    /// X position on our parent's framebuffer
    size_t m_XPos;
    void setXPos(size_t x);

    /// Y position on our parent's framebuffer
    size_t m_YPos;
    void setYPos(size_t y);

    /// Width of the framebuffer in pixels
    size_t m_nWidth;
    void setWidth(size_t w);

    /// Height of the framebuffer in pixels
    size_t m_nHeight;
    void setHeight(size_t h);

    /// Framebuffer pixel format
    Graphics::PixelFormat m_PixelFormat;
    void setFormat(Graphics::PixelFormat pf);

    /// Bytes per pixel in this framebuffer
    size_t m_nBytesPerPixel;
    void setBytesPerPixel(size_t b);
    uint32_t getBytesPerPixel() const;

    /// Bytes per line in this framebuffer
    size_t m_nBytesPerLine;
    void setBytesPerLine(size_t b);
    uint32_t getBytesPerLine() const;

    /// Parent of this framebuffer
    Framebuffer *m_pParent;
    void setParent(Framebuffer *p);
    Framebuffer *getParent() const;

    virtual void setFramebuffer(uintptr_t p);

  private:
    /** Sets an individual pixel on the framebuffer. Not inheritable. */
    void swSetPixel(
        size_t x, size_t y, uint32_t colour,
        Graphics::PixelFormat format = Graphics::Bits32_Argb);

  protected:
    // Base address of this framebuffer, set by whatever code inherits this
    // class, ideally in the constructor.
    uintptr_t m_FramebufferBase;

    /// Current graphics palette - an array of 256 32-bit RGBA entries
    uint32_t *m_Palette;

    /// Whether this framebuffer is active or not.
    bool m_bActive;

    Graphics::Buffer bufferFromSelf();

    /// Special implementation of draw() where a Graphics::Buffer is already
    /// available. For use in redraw, and similar.
    virtual void draw(
        Graphics::Buffer *pBuffer, size_t srcx, size_t srcy, size_t destx,
        size_t desty, size_t width, size_t height, bool bLowestCall = true);

    void swBlit(
        Graphics::Buffer *pBuffer, size_t srcx, size_t srcy, size_t destx,
        size_t desty, size_t width, size_t height);

    void swRect(
        size_t x, size_t y, size_t width, size_t height, uint32_t colour,
        Graphics::PixelFormat format);

    void swCopy(
        size_t srcx, size_t srcy, size_t destx, size_t desty, size_t w,
        size_t h);

    void swLine(
        size_t x1, size_t y1, size_t x2, size_t y2, uint32_t colour,
        Graphics::PixelFormat format);

    void swDraw(
        void *pBuffer, size_t srcx, size_t srcy, size_t destx, size_t desty,
        size_t width, size_t height,
        Graphics::PixelFormat format = Graphics::Bits32_Argb,
        bool bLowestCall = true);

    void swDraw(
        Graphics::Buffer *pBuffer, size_t srcx, size_t srcy, size_t destx,
        size_t desty, size_t width, size_t height, bool bLowestCall = true);

    Graphics::Buffer *swCreateBuffer(
        const void *srcData, Graphics::PixelFormat srcFormat, size_t width,
        size_t height, uint32_t *pPalette);

    void swDestroyBuffer(Graphics::Buffer *pBuffer);

    /// Inherited by drivers that provide a hardware redraw function
    virtual void
    hwRedraw(size_t x = ~0UL, size_t y = ~0UL, size_t w = ~0UL, size_t h = ~0UL);
};

#endif
