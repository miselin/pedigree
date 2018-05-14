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

#include "pedigree/kernel/machine/Framebuffer.h"
#include "pedigree/kernel/machine/Display.h"
#include "pedigree/kernel/processor/MemoryRegion.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"

#include "pedigree/kernel/Log.h"

Framebuffer::Framebuffer() : m_pParent(0), m_FramebufferBase(0), m_bActive(true)
{
    m_Palette = new uint32_t[256];

    size_t i = 0;
    for (size_t g = 0; g <= 255; g += 0x33)
        for (size_t b = 0; b <= 255; b += 0x33)
            for (size_t r = 0; r <= 255; r += 0x33)
                m_Palette[i++] = Graphics::createRgb(r, g, b);

    NOTICE(
        "Framebuffer: created " << Dec << i << Hex
                                << " entries in the default palette");
}

Framebuffer::~Framebuffer()
{
}

size_t Framebuffer::getWidth() const
{
    return m_nWidth;
}

size_t Framebuffer::getHeight() const
{
    return m_nHeight;
}

Graphics::PixelFormat Framebuffer::getFormat() const
{
    return m_PixelFormat;
}

bool Framebuffer::getActive() const
{
    return m_bActive;
}

void Framebuffer::setActive(bool b)
{
    m_bActive = b;
}

void Framebuffer::setPalette(uint32_t *palette, size_t nEntries)
{
    delete[] m_Palette;
    m_Palette = new uint32_t[nEntries];
    MemoryCopy(m_Palette, palette, nEntries * sizeof(uint32_t));

    NOTICE(
        "Framebuffer: new palette set with " << Dec << nEntries << Hex
                                             << " entries");
}

uint32_t *Framebuffer::getPalette() const
{
    return m_Palette;
}

void *Framebuffer::getRawBuffer() const
{
    if (m_pParent)
        return m_pParent->getRawBuffer();
    return reinterpret_cast<void *>(m_FramebufferBase);
}

Graphics::Buffer *Framebuffer::createBuffer(
    const void *srcData, Graphics::PixelFormat srcFormat, size_t width,
    size_t height, uint32_t *pPalette)
{
    if (m_pParent)
        return m_pParent->createBuffer(
            srcData, srcFormat, width, height, pPalette);
    return swCreateBuffer(srcData, srcFormat, width, height, pPalette);
}

void Framebuffer::destroyBuffer(Graphics::Buffer *pBuffer)
{
    // if(m_pParent)
    //     m_pParent->destroyBuffer(pBuffer);
    // else
    swDestroyBuffer(pBuffer);
}

void Framebuffer::redraw(size_t x, size_t y, size_t w, size_t h, bool bChild)
{
    if (m_pParent)
    {
        // Redraw with parent:
        // 1. Draw our framebuffer. This will go to the top without
        //    changing intermediate framebuffers.
        // 2. Pass a redraw up the chain. This will reach the top level
        //    (with modified x/y) and cause our screen region to be redrawn.

        /// \note If none of the above makes sense, you need to read the
        ///       Pedigree graphics design doc:
        ///       http://pedigree-project.org/issues/114

        // If the redraw was not caused by a child, make sure our
        // framebuffer has precedence over any children.
        /// \todo nChildren parameter - this is not necessary if no
        /// children!
        if (!bChild)
        {
            if (m_pParent->getFormat() == m_PixelFormat)
            {
                Graphics::Buffer buf = bufferFromSelf();
                m_pParent->draw(
                    &buf, x, y, m_XPos + x, m_YPos + y, w, h, false);
                // m_pParent->draw(reinterpret_cast<void*>(m_FramebufferBase),
                // x, y, m_XPos + x, m_YPos + y, w, h, m_PixelFormat,
                // false);
            }
            else
            {
                ERROR("Child framebuffer has different pixel format to "
                      "parent!");
            }
        }

        // Now we are a child requesting a redraw, so the parent will not
        // have precedence over us.
        m_pParent->redraw(m_XPos + x, m_YPos + y, w, h, true);
    }
    else
        hwRedraw(x, y, w, h);
}

void Framebuffer::blit(
    Graphics::Buffer *pBuffer, size_t srcx, size_t srcy, size_t destx,
    size_t desty, size_t width, size_t height, bool bLowestCall)
{
    if (m_pParent)
        m_pParent->blit(
            pBuffer, srcx, srcy, m_XPos + destx, m_YPos + desty, width, height,
            false);
    if (bLowestCall || !m_pParent)
        swBlit(pBuffer, srcx, srcy, destx, desty, width, height);
}

void Framebuffer::draw(
    void *pBuffer, size_t srcx, size_t srcy, size_t destx, size_t desty,
    size_t width, size_t height, Graphics::PixelFormat format, bool bLowestCall)
{
    // Draw is implemented as a "create buffer and blit"... so we can
    // avoid checking for parent here as we don't want to poison the
    // parent's buffer.
    swDraw(
        pBuffer, srcx, srcy, destx, desty, width, height, format, bLowestCall);
}

void Framebuffer::rect(
    size_t x, size_t y, size_t width, size_t height, uint32_t colour,
    Graphics::PixelFormat format, bool bLowestCall)
{
    if (m_pParent)
        m_pParent->rect(
            m_XPos + x, m_YPos + y, width, height, colour, format, false);
    if (bLowestCall || !m_pParent)
        swRect(x, y, width, height, colour, format);
}

void Framebuffer::copy(
    size_t srcx, size_t srcy, size_t destx, size_t desty, size_t w, size_t h,
    bool bLowestCall)
{
    if (m_pParent)
        m_pParent->copy(
            m_XPos + srcx, m_YPos + srcy, m_XPos + destx, m_YPos + desty, w, h,
            false);
    if (bLowestCall || !m_pParent)
        swCopy(srcx, srcy, destx, desty, w, h);
}

void Framebuffer::line(
    size_t x1, size_t y1, size_t x2, size_t y2, uint32_t colour,
    Graphics::PixelFormat format, bool bLowestCall)
{
    if (m_pParent)
        m_pParent->line(
            m_XPos + x1, m_YPos + y1, m_XPos + x2, m_YPos + y2, colour, format,
            false);
    if (bLowestCall || !m_pParent)
        swLine(x1, y1, x2, y2, colour, format);
}

void Framebuffer::setPixel(
    size_t x, size_t y, uint32_t colour, Graphics::PixelFormat format,
    bool bLowestCall)
{
    if (m_pParent)
        m_pParent->setPixel(m_XPos + x, m_YPos + y, colour, format, false);
    if (bLowestCall || !m_pParent)
        swSetPixel(x, y, colour, format);
}

void Framebuffer::setXPos(size_t x)
{
    m_XPos = x;
}

void Framebuffer::setYPos(size_t y)
{
    m_YPos = y;
}

void Framebuffer::setWidth(size_t w)
{
    m_nWidth = w;
}

void Framebuffer::setHeight(size_t h)
{
    m_nHeight = h;
}

void Framebuffer::setFormat(Graphics::PixelFormat pf)
{
    m_PixelFormat = pf;
}

void Framebuffer::setBytesPerPixel(size_t b)
{
    m_nBytesPerPixel = b;
}

uint32_t Framebuffer::getBytesPerPixel() const
{
    return m_nBytesPerPixel;
}

void Framebuffer::setBytesPerLine(size_t b)
{
    m_nBytesPerLine = b;
}

uint32_t Framebuffer::getBytesPerLine() const
{
    return m_nBytesPerLine;
}

void Framebuffer::setParent(Framebuffer *p)
{
    m_pParent = p;
}

Framebuffer *Framebuffer::getParent() const
{
    return m_pParent;
}

void Framebuffer::setFramebuffer(uintptr_t p)
{
    m_FramebufferBase = p;
}

Graphics::Buffer Framebuffer::bufferFromSelf()
{
    Graphics::Buffer ret;
    ret.base = m_FramebufferBase;
    ret.width = m_nWidth;
    ret.height = m_nHeight;
    ret.format = m_PixelFormat;
    ret.bytesPerPixel = m_nBytesPerPixel;
    ret.bufferId = 0;
    ret.pBacking = 0;
    return ret;
}

void Framebuffer::draw(
    Graphics::Buffer *pBuffer, size_t srcx, size_t srcy, size_t destx,
    size_t desty, size_t width, size_t height, bool bLowestCall)
{
    swDraw(pBuffer, srcx, srcy, destx, desty, width, height, bLowestCall);
}

Graphics::Buffer *Framebuffer::swCreateBuffer(
    const void *srcData, Graphics::PixelFormat srcFormat, size_t width,
    size_t height, uint32_t *pPalette)
{
    if (UNLIKELY(!m_FramebufferBase))
        return 0;
    if (UNLIKELY(!(width && height)))
        return 0;

    Graphics::PixelFormat destFormat = m_PixelFormat;

    size_t sourceBytesPerPixel = bytesPerPixel(srcFormat);
    size_t sourceBytesPerLine = width * sourceBytesPerPixel;

    size_t destBytesPerPixel = m_nBytesPerPixel;
    size_t destBytesPerLine = width * destBytesPerPixel;

    size_t fullBufferSize = height * destBytesPerLine;

    MemoryRegion *pRegion = new MemoryRegion("sw-framebuffer-buffer");
    bool bSuccess = PhysicalMemoryManager::instance().allocateRegion(
        *pRegion, (fullBufferSize / 0x1000) + 1, 0, VirtualAddressSpace::Write);

    if (!bSuccess)
    {
        delete pRegion;
        return 0;
    }

    // Copy the buffer in full
    void *pAddress = pRegion->virtualAddress();
    if (((sourceBytesPerPixel == destBytesPerPixel) &&
         (sourceBytesPerLine == destBytesPerLine)) &&
        (srcFormat == destFormat))
    {
        // Same pixel format and same pixel size. Safe to do a conventional
        // memcpy. The pixel size check is to handle cases where a buffer may
        // be, say, 24-bit RGB, and the display driver also 24-bit RGB, but
        // the display's framebuffer reserves 32 bits for pixels (even though
        // the actual depth is 24 bits).
        MemoryCopy(pAddress, srcData, fullBufferSize);
    }
    else
    {
        // Have to convert and pack each pixel, much slower than memcpy.
        size_t x, y;
        for (y = 0; y < height; y++)
        {
            for (x = 0; x < width; x++)
            {
                size_t sourceOffset =
                    (y * sourceBytesPerLine) + (x * sourceBytesPerPixel);
                size_t destOffset =
                    (y * destBytesPerLine) + (x * destBytesPerPixel);

                // We'll always access the beginning of a pixel, which makes
                // things here much simpler.
                const uint32_t *pSource = reinterpret_cast<const uint32_t *>(
                    adjust_pointer(srcData, sourceOffset));

                uint32_t transform = 0;
                if (srcFormat == Graphics::Bits8_Idx)
                {
                    uint32_t source = pPalette[(*pSource) & 0xFF];
                    Graphics::convertPixel(
                        source, Graphics::Bits24_Bgr, transform, destFormat);
                }
                else
                {
                    Graphics::convertPixel(
                        *pSource, srcFormat, transform, destFormat);
                }

                if (destBytesPerPixel == 4)
                {
                    uint32_t *pDest = reinterpret_cast<uint32_t *>(
                        adjust_pointer(pAddress, destOffset));

                    if (sourceBytesPerPixel == 3)
                        transform &= 0xFFFFFF;
                    else if (sourceBytesPerPixel == 2)
                        transform &= 0xFFFF;

                    *pDest = transform;
                }
                else if (destBytesPerPixel == 3)
                {
                    // Handle existing data after this byte if it exists
                    uint32_t *pDest = reinterpret_cast<uint32_t *>(
                        adjust_pointer(pAddress, destOffset));
                    *pDest = (*pDest & 0xFF000000) | (transform & 0xFFFFFF);
                }
                else if (destBytesPerPixel == 2)
                {
                    uint16_t *pDest = reinterpret_cast<uint16_t *>(
                        adjust_pointer(pAddress, destOffset));
                    *pDest = transform & 0xFFFF;
                }
                else if (destBytesPerPixel == 1)
                {
                    /// \todo 8-bit
                }
            }
        }
    }

    Graphics::Buffer *pBuffer = new Graphics::Buffer;
    pBuffer->base = reinterpret_cast<uintptr_t>(pRegion->virtualAddress());
    pBuffer->width = width;
    pBuffer->height = height;
    pBuffer->format = m_PixelFormat;
    pBuffer->bytesPerPixel = destBytesPerPixel;
    pBuffer->bufferId = 0;
    pBuffer->pBacking = reinterpret_cast<void *>(pRegion);

    return pBuffer;
}

void Framebuffer::swDestroyBuffer(Graphics::Buffer *pBuffer)
{
    if (pBuffer && pBuffer->base)
    {
        MemoryRegion *pRegion =
            reinterpret_cast<MemoryRegion *>(pBuffer->pBacking);
        delete pRegion;  // Unmaps the memory as well

        delete pBuffer;
    }
}

void Framebuffer::swBlit(
    Graphics::Buffer *pBuffer, size_t srcx, size_t srcy, size_t destx,
    size_t desty, size_t width, size_t height)
{
    if (UNLIKELY(!m_FramebufferBase))
        return;
    if (UNLIKELY(!(width && height)))
        return;
    if (UNLIKELY(!pBuffer))
        return;

    size_t bytesPerLine = m_nBytesPerLine;
    size_t destBytesPerPixel = m_nBytesPerPixel;

    size_t sourceBytesPerPixel = pBuffer->bytesPerPixel;
    size_t sourceBytesPerLine = pBuffer->width * destBytesPerPixel;

    // Sanity check and clip
    if ((srcx > pBuffer->width) || (srcy > pBuffer->height))
        return;
    if ((destx > m_nWidth) || (desty > m_nHeight))
        return;
    if ((srcx + width) > pBuffer->width)
        width = (srcx + width) - pBuffer->width;
    if ((srcy + height) > pBuffer->height)
        height = (srcy + height) - pBuffer->height;
    if ((destx + width) > m_nWidth)
        width = m_nWidth - destx;  // (destx + width) - m_nWidth;
    if ((desty + height) > m_nHeight)
        height = m_nHeight - desty;  // (desty + height) - m_nHeight;

    /// \todo This code is broken for the case where srcx/srcy are not zero. :(

    void *pSrc = reinterpret_cast<void *>(pBuffer->base);

    // Blit across the width of the screen? How handy!
    if (UNLIKELY((srcx == destx) && (srcx == 0) && (width == m_nWidth)))
    {
        size_t sourceBufferOffset = (srcy * sourceBytesPerLine);
        size_t frameBufferOffset = (desty * bytesPerLine);

        void *dest =
            reinterpret_cast<void *>(m_FramebufferBase + frameBufferOffset);
        void *src = adjust_pointer(pSrc, sourceBufferOffset);

        MemoryCopy(dest, src, bytesPerLine * height);
    }
    else
    {
        // Line-by-line copy
        for (size_t y1 = desty, y2 = srcy; y1 < (desty + height); y1++, y2++)
        {
            size_t sourceBufferOffset =
                (y2 * sourceBytesPerLine) + (srcx * sourceBytesPerPixel);
            size_t frameBufferOffset =
                (y1 * bytesPerLine) + (destx * destBytesPerPixel);

            void *dest =
                reinterpret_cast<void *>(m_FramebufferBase + frameBufferOffset);
            void *src = adjust_pointer(pSrc, sourceBufferOffset);

            MemoryCopy(dest, src, width * destBytesPerPixel);
        }
    }
}

void Framebuffer::swRect(
    size_t x, size_t y, size_t width, size_t height, uint32_t colour,
    Graphics::PixelFormat format)
{
    if (UNLIKELY(!m_FramebufferBase))
        return;
    if (UNLIKELY(!(width && height)))
        return;

    // Sanity check and clip
    if ((x > m_nWidth) || (y > m_nHeight))
        return;
    if (width > m_nWidth)
        width = m_nWidth;
    if (height > m_nHeight)
        height = m_nHeight;
    if ((x + width) > m_nWidth)
        width = (x + width) - m_nWidth;
    if ((y + height) > m_nHeight)
        height = (y + height) - m_nHeight;

    uint32_t transformColour = 0;
    if (format == Graphics::Bits8_Idx)
    {
        uint32_t source = m_Palette[colour & 0xFF];
        Graphics::convertPixel(
            source, Graphics::Bits24_Bgr, transformColour, m_PixelFormat);
    }
    else
        Graphics::convertPixel(colour, format, transformColour, m_PixelFormat);

    size_t bytesPerPixel = m_nBytesPerPixel;
    size_t bytesPerLine = m_nBytesPerLine;

    // Can we just do an easy memset?
    if (UNLIKELY((!x) && (width == m_nWidth)))
    {
        size_t frameBufferOffset = (y * bytesPerLine) + (x * bytesPerPixel);

        void *dest =
            reinterpret_cast<void *>(m_FramebufferBase + frameBufferOffset);

        size_t copySize = (bytesPerLine * height);
        if (bytesPerPixel == 4)  /// \todo Handle 24-bit properly
        {
            if ((copySize % 8) == 0)  // QWORD boundary
            {
                uint64_t val = (static_cast<uint64_t>(transformColour) << 32) |
                               transformColour;
                QuadWordSet(dest, val, copySize / 8);
            }
            else
                DoubleWordSet(dest, transformColour, copySize / 4);
        }
        else if (bytesPerPixel == 3)
        {
            // 24-bit has to set three bytes at a time and leave the top byte
            // untouched. Painful.
            for (size_t i = 0; i < (copySize / 3); i++)
            {
                uint32_t *p = reinterpret_cast<uint32_t *>(
                    m_FramebufferBase + frameBufferOffset + (i * 3));
                *p = (*p & 0xFF000000) | transformColour;
            }
        }
        else if (bytesPerPixel == 2)
        {
            if ((copySize % 8) == 0)  // QWORD boundary
            {
                uint64_t val = (static_cast<uint64_t>(transformColour) << 48) |
                               (static_cast<uint64_t>(transformColour) << 32) |
                               (transformColour << 16) | transformColour;
                QuadWordSet(dest, val, copySize / 8);
            }
            else if ((copySize % 4) == 0)  // DWORD boundary
            {
                uint32_t val = (transformColour << 16) | transformColour;
                DoubleWordSet(dest, val, copySize / 4);
            }
            else
                WordSet(dest, transformColour, copySize / 2);
        }
        else
            ByteSet(dest, transformColour, (bytesPerLine * height));
    }
    else
    {
        // Line-by-line fill
        for (size_t desty = y; desty < (y + height); desty++)
        {
            size_t frameBufferOffset =
                (desty * bytesPerLine) + (x * bytesPerPixel);

            void *dest =
                reinterpret_cast<void *>(m_FramebufferBase + frameBufferOffset);

            size_t copySize = width * bytesPerPixel;
            if (bytesPerPixel == 4)  /// \todo Handle 24-bit properly
            {
                if ((copySize % 8) == 0)  // QWORD boundary
                {
                    uint64_t val =
                        (static_cast<uint64_t>(transformColour) << 32) |
                        transformColour;
                    QuadWordSet(dest, val, copySize / 8);
                }
                else
                    DoubleWordSet(dest, transformColour, copySize / 4);
            }
            else if (bytesPerPixel == 3)
            {
                // 24-bit has to set three bytes at a time and leave the top
                // byte untouched. Painful.
                for (size_t i = 0; i < (copySize / 3); i++)
                {
                    uint32_t *p = reinterpret_cast<uint32_t *>(
                        m_FramebufferBase + frameBufferOffset + (i * 3));
                    *p = (*p & 0xFF000000) | transformColour;
                }
            }
            else if (bytesPerPixel == 2)
            {
                if ((copySize % 8) == 0)  // QWORD boundary
                {
                    uint64_t val =
                        (static_cast<uint64_t>(transformColour) << 48) |
                        (static_cast<uint64_t>(transformColour) << 32) |
                        (transformColour << 16) | transformColour;
                    QuadWordSet(dest, val, copySize / 8);
                }
                else if ((copySize % 4) == 0)  // DWORD boundary
                {
                    uint32_t val = (transformColour << 16) | transformColour;
                    DoubleWordSet(dest, val, copySize / 4);
                }
                else
                    WordSet(dest, transformColour, copySize / 2);
            }
            else
                ByteSet(dest, transformColour, (width * bytesPerPixel));
        }
    }
}

void Framebuffer::swCopy(
    size_t srcx, size_t srcy, size_t destx, size_t desty, size_t w, size_t h)
{
    if (UNLIKELY(!m_FramebufferBase))
        return;
    if (UNLIKELY(!(w && h)))
        return;
    if (UNLIKELY((srcx == destx) && (srcy == desty)))
        return;

    // Sanity check and clip
    if ((srcx > m_nWidth) || (srcy > m_nHeight))
        return;
    if ((destx > m_nWidth) || (desty > m_nHeight))
        return;
    if ((srcx + w) > m_nWidth)
        w = (srcx + w) - m_nWidth;
    if ((srcy + h) > m_nHeight)
        h = (srcy + h) - m_nHeight;
    if ((destx + w) > m_nWidth)
        w = (destx + w) - m_nWidth;
    if ((desty + h) > m_nHeight)
        h = (desty + h) - m_nHeight;

    if ((srcx == destx) && (srcy == desty))
        return;
    if (!(w && h))
        return;

    size_t bytesPerLine = m_nBytesPerLine;
    size_t bytesPerPixel = m_nBytesPerPixel;
    /// \todo consider source bytes per line?

    // Easy memcpy?
    if (UNLIKELY(((!srcx) && (!destx)) && (w == m_nWidth)))
    {
        size_t frameBufferOffsetSrc =
            (srcy * bytesPerLine) + (srcx * bytesPerPixel);
        size_t frameBufferOffsetDest =
            (desty * bytesPerLine) + (destx * bytesPerPixel);

        void *dest =
            reinterpret_cast<void *>(m_FramebufferBase + frameBufferOffsetDest);
        void *src =
            reinterpret_cast<void *>(m_FramebufferBase + frameBufferOffsetSrc);

        MemoryCopy(dest, src, h * bytesPerLine);
    }
    else
    {
        // Not so easy, but still not too hard
        for (size_t yoff = 0; yoff < h; yoff++)
        {
            size_t frameBufferOffsetSrc =
                ((srcy + yoff) * bytesPerLine) + (srcx * bytesPerPixel);
            size_t frameBufferOffsetDest =
                ((desty + yoff) * bytesPerLine) + (destx * bytesPerPixel);

            void *dest = reinterpret_cast<void *>(
                m_FramebufferBase + frameBufferOffsetDest);
            void *src = reinterpret_cast<void *>(
                m_FramebufferBase + frameBufferOffsetSrc);

            MemoryCopy(dest, src, w * bytesPerPixel);
        }
    }
}

void Framebuffer::swLine(
    size_t x1, size_t y1, size_t x2, size_t y2, uint32_t colour,
    Graphics::PixelFormat format)
{
    if (UNLIKELY(!m_FramebufferBase))
        return;

    // Clip co-ordinates where necessary
    if (x1 > m_nWidth)
        x1 = m_nWidth;
    if (x2 > m_nWidth)
        x2 = m_nWidth;
    if (y1 > m_nHeight)
        y1 = m_nHeight;
    if (y2 > m_nHeight)
        y2 = m_nHeight;

    // NOTICE("swLine(" << Dec << x1 << ", " << y1 << " -> " << x2 << ", " << y2
    // << Hex << ")");

    if (UNLIKELY((x1 == x2) && (y1 == y2)))
        return;

    uint32_t transformColour = 0;
    if (format == Graphics::Bits8_Idx)
    {
        uint32_t source = m_Palette[colour & 0xFF];
        Graphics::convertPixel(
            source, Graphics::Bits24_Bgr, transformColour, m_PixelFormat);
    }
    else
        Graphics::convertPixel(colour, format, transformColour, m_PixelFormat);

    // Special cases
    if (x1 == x2)  // Vertical line
    {
        if (y1 > y2)
            swap(y1, y2);
        for (size_t y = y1; y < y2; y++)
            setPixel(x1, y, transformColour, m_PixelFormat);
        return;
    }
    else if (y1 == y2)
    {
        if (x1 > x2)
            swap(x1, x2);
        for (size_t x = x1; x < x2; x++)
            setPixel(x, y1, transformColour, m_PixelFormat);
        return;
    }

    // Bresenham's algorithm, referred to Computer Graphics, C Version (2nd
    // Edition) from 1997, by D. Hearn and M. Pauline Baker (page 88)
    // http://www.amazon.com/Computer-Graphics-C-Version-2nd/dp/0135309247

    ssize_t dx = static_cast<ssize_t>(x2) - static_cast<ssize_t>(x1);
    ssize_t dy = static_cast<ssize_t>(y2) - static_cast<ssize_t>(y1);
    ssize_t p = 2 * (dy - dx);
    ssize_t x, y, xEnd;

    if (x1 > x2)
    {
        x = x2;
        y = y2;
        xEnd = x1;
    }
    else
    {
        x = x1;
        y = y1;
        xEnd = x2;
    }

    setPixel(x, y, transformColour, m_PixelFormat);

    while (x < xEnd)
    {
        x++;
        if (p < 0)
            p += (dy * 2);
        else
        {
            y++;
            p += 2 * (dy - dx);
        }

        setPixel(x, y, transformColour, m_PixelFormat);
    }
}

void Framebuffer::swSetPixel(
    size_t x, size_t y, uint32_t colour, Graphics::PixelFormat format)
{
    if (UNLIKELY(!m_FramebufferBase))
        return;

    if (x > m_nWidth)
        x = m_nWidth;
    if (y > m_nHeight)
        y = m_nHeight;

    size_t bytesPerPixel = m_nBytesPerPixel;
    size_t bytesPerLine = m_nBytesPerLine;

    uint32_t transformColour = 0;
    if (format == Graphics::Bits8_Idx)
    {
        uint32_t source = m_Palette[colour & 0xFF];
        Graphics::convertPixel(
            source, Graphics::Bits24_Bgr, transformColour, m_PixelFormat);
    }
    else
        Graphics::convertPixel(colour, format, transformColour, m_PixelFormat);

    size_t frameBufferOffset = (y * bytesPerLine) + (x * bytesPerPixel);

    if (bytesPerPixel == 4)
    {
        uint32_t *pDest =
            reinterpret_cast<uint32_t *>(m_FramebufferBase + frameBufferOffset);
        *pDest = transformColour;
    }
    else if (bytesPerPixel == 3)
    {
        // Handle existing data after this byte if it exists
        uint32_t *pDest =
            reinterpret_cast<uint32_t *>(m_FramebufferBase + frameBufferOffset);
        *pDest = (*pDest & 0xFF000000) | (transformColour & 0xFFFFFF);
    }
    else if (bytesPerPixel == 2)
    {
        uint16_t *pDest =
            reinterpret_cast<uint16_t *>(m_FramebufferBase + frameBufferOffset);
        *pDest = transformColour & 0xFFFF;
    }
    else if (bytesPerPixel == 1)
    {
        /// \todo 8-bit
    }
}

void Framebuffer::swDraw(
    void *pBuffer, size_t srcx, size_t srcy, size_t destx, size_t desty,
    size_t width, size_t height, Graphics::PixelFormat format, bool bLowestCall)
{
    // Potentially inefficient use of RAM and VRAM, but best way to avoid
    // redundant copies of code lying around.
    Graphics::Buffer *p =
        createBuffer(pBuffer, format, srcx + width, srcy + height, m_Palette);
    if (!p)
        return;
    blit(p, srcx, srcy, destx, desty, width, height, bLowestCall);
    destroyBuffer(p);
}

void Framebuffer::swDraw(
    Graphics::Buffer *pBuffer, size_t srcx, size_t srcy, size_t destx,
    size_t desty, size_t width, size_t height, bool bLowestCall)
{
    blit(pBuffer, srcx, srcy, destx, desty, width, height, bLowestCall);
}

void Framebuffer::hwRedraw(size_t x, size_t y, size_t w, size_t h)
{
}
