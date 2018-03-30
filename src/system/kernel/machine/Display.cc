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

#include "pedigree/kernel/machine/Display.h"

Display::ScreenMode::ScreenMode()
    : id(0), width(0), height(0), refresh(0), framebuffer(0), pf(), pf2(),
      bytesPerLine(0), bytesPerPixel(0), textMode(false)
{
}

Display::Display()
{
    m_SpecificType = "Generic Display";
}

Display::Display(Device *p) : Device(p)
{
}

Display::~Display()
{
}

Device::Type Display::getType()
{
    return Device::Display;
}

void Display::getName(String &str)
{
    str = "Generic Display";
}

void Display::dump(String &str)
{
    str = "Generic Display";
}

void *Display::getFramebuffer()
{
    return 0;
}

Display::rgb_t *Display::newBuffer()
{
    return 0;
}

void Display::setCurrentBuffer(rgb_t *pBuffer)
{
}

void Display::updateBuffer(
    rgb_t *pBuffer, size_t x1, size_t y1, size_t x2, size_t y2)
{
}

void Display::killBuffer(rgb_t *pBuffer)
{
}

void Display::bitBlit(
    rgb_t *pBuffer, size_t fromX, size_t fromY, size_t toX, size_t toY,
    size_t width, size_t height)
{
}

void Display::fillRectangle(
    rgb_t *pBuffer, size_t x, size_t y, size_t width, size_t height,
    rgb_t colour)
{
}

bool Display::getPixelFormat(PixelFormat &pf)
{
    return false;
}

bool Display::getCurrentScreenMode(ScreenMode &sm)
{
    return false;
}

bool Display::getScreenModes(List<ScreenMode *> &sms)
{
    return false;
}

bool Display::setScreenMode(ScreenMode sm)
{
    return false;
}

bool Display::setScreenMode(size_t modeId)
{
    Display::ScreenMode *pSm = 0;

    List<Display::ScreenMode *> modes;
    if (!getScreenModes(modes))
        return false;
    for (List<Display::ScreenMode *>::Iterator it = modes.begin();
         it != modes.end(); it++)
    {
        if ((*it)->id == modeId)
        {
            pSm = *it;
            break;
        }
    }
    if (pSm == 0)
    {
        ERROR("Screenmode not found: " << modeId);
        return false;
    }

    return setScreenMode(*pSm);
}

bool Display::setScreenMode(size_t nWidth, size_t nHeight, size_t nBpp)
{
    // This default implementation is enough for VBE
    /// \todo "Closest match": allow a threshold for a match in case the
    ///       specific mode specified cannot be set.

    Display::ScreenMode *pSm = 0;

    List<Display::ScreenMode *> modes;
    if (!getScreenModes(modes))
        return false;
    for (List<Display::ScreenMode *>::Iterator it = modes.begin();
         it != modes.end(); it++)
    {
        if (((*it)->width == nWidth) && ((*it)->height == nHeight))
        {
            if ((*it)->pf.nBpp == nBpp)
            {
                pSm = *it;
                break;
            }
        }
    }
    if (pSm == 0)
    {
        ERROR(
            "Screenmode not found: " << Dec << nWidth << "x" << nHeight
                                     << "x" << nBpp << Hex);
        return false;
    }

    return setScreenMode(*pSm);
}
