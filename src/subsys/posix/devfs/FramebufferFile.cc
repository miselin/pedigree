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

#include "FramebufferFile.h"

#define MACHINE_FORWARD_DECL_ONLY
#include <machine/Machine.h>
#include <machine/Vga.h>
#include <sys/fb.h>

FramebufferFile::FramebufferFile(String str, size_t inode, Filesystem *pParentFS, File *pParentNode) :
    File(str, 0, 0, 0, inode, pParentFS, 0, pParentNode), m_pProvider(0), m_bTextMode(false), m_nDepth(0)
{
}

FramebufferFile::~FramebufferFile()
{
    delete m_pProvider;
}

bool FramebufferFile::initialise()
{
    ServiceFeatures *pFeatures = ServiceManager::instance().enumerateOperations(String("graphics"));
    Service         *pService  = ServiceManager::instance().getService(String("graphics"));
    if(pFeatures && pFeatures->provides(ServiceFeatures::probe))
    {
        if(pService)
        {
            m_pProvider = new GraphicsService::GraphicsProvider;
            if(!pService->serve(ServiceFeatures::probe, m_pProvider, sizeof(*m_pProvider)))
            {
                delete m_pProvider;
                m_pProvider = 0;

                return false;
            }
            else
            {
                // Set the file size to reflect the size of the framebuffer.
                setSize(m_pProvider->pFramebuffer->getHeight() * m_pProvider->pFramebuffer->getBytesPerLine());
            }
        }
    }

    return pFeatures && pService;
}

uintptr_t FramebufferFile::readBlock(uint64_t location)
{
    if(!m_pProvider)
        return 0;

    if(location > getSize())
    {
        ERROR("FramebufferFile::readBlock with location > size: " << location);
        return 0;
    }

    /// \todo If this is NOT virtual, we need to do something about that.
    return reinterpret_cast<uintptr_t>(m_pProvider->pFramebuffer->getRawBuffer()) + location;
}

bool FramebufferFile::supports(const int command)
{
    return (PEDIGREE_FB_CMD_MIN <= command) && (command <= PEDIGREE_FB_CMD_MAX);
}

int FramebufferFile::command(const int command, void *buffer)
{
    if(!m_pProvider)
    {
        ERROR("FramebufferFile::command called on an invalid FramebufferFile");
        return -1;
    }

    Display *pDisplay = m_pProvider->pDisplay;
    Framebuffer *pFramebuffer = m_pProvider->pFramebuffer;

    switch(command)
    {
        case PEDIGREE_FB_SETMODE:
            {
                pedigree_fb_modeset *arg = reinterpret_cast<pedigree_fb_modeset *>(buffer);
                size_t desiredWidth = arg->width;
                size_t desiredHeight = arg->height;
                size_t desiredDepth = arg->depth;

                // Are we seeking a text mode?
                if(!(desiredWidth && desiredHeight && desiredDepth))
                {
                    bool bSuccess = false;
                    if(!m_pProvider->bTextModes)
                    {
                        bSuccess = pDisplay->setScreenMode(0);
                    }
                    else
                    {
                        // Set via VGA method.
                        if(Machine::instance().getNumVga())
                        {
                            /// \todo What if there is no text mode!?
                            Vga *pVga = Machine::instance().getVga(0);
                            pVga->setMode(3); /// \todo Magic number.
                            pVga->rememberMode();
                            pVga->setLargestTextMode();

                            m_nDepth = 0;
                            m_bTextMode = true;

                            bSuccess = true;
                        }
                    }

                    if(bSuccess)
                    {
                        NOTICE("FramebufferFile: set text mode");
                        return 0;
                    }
                    else
                    {
                        return -1;
                    }
                }

                bool bSet = false;
                while(desiredDepth > 8)
                {
                    if(pDisplay->setScreenMode(desiredWidth, desiredHeight, desiredDepth))
                    {
                        NOTICE("FramebufferFile: set mode " << Dec << desiredWidth << "x" << desiredHeight << "x" << desiredDepth << Hex << ".");
                        bSet = true;
                        break;
                    }
                    desiredDepth -= 8;
                }

                if(bSet)
                {
                    m_nDepth = desiredDepth;

                    setSize(pFramebuffer->getHeight() * pFramebuffer->getBytesPerLine());

                    if(m_pProvider->bTextModes && m_bTextMode)
                    {
                        // Okay, we need to 'undo' the text mode.
                        if(Machine::instance().getNumVga())
                        {
                            /// \todo What if there is no text mode!?
                            Vga *pVga = Machine::instance().getVga(0);
                            pVga->restoreMode();

                            m_bTextMode = false;
                        }
                    }
                }

                return bSet ? 0 : -1;
            }
            break;
        case PEDIGREE_FB_GETMODE:
            {
                pedigree_fb_mode *arg = reinterpret_cast<pedigree_fb_mode *>(buffer);
                if(m_bTextMode)
                {
                    ByteSet(arg, 0, sizeof(*arg));
                }
                else
                {
                    arg->width = pFramebuffer->getWidth();
                    arg->height = pFramebuffer->getHeight();
                    arg->depth = m_nDepth;
                    arg->bytes_per_pixel = pFramebuffer->getBytesPerPixel();
                    arg->format = pFramebuffer->getFormat();
                }

                return 0;
            }
            break;
        case PEDIGREE_FB_REDRAW:
            {
                pedigree_fb_rect *arg = reinterpret_cast<pedigree_fb_rect *>(buffer);
                if(!arg)
                {
                    // Redraw all.
                    pFramebuffer->redraw(0, 0, pFramebuffer->getWidth(), pFramebuffer->getHeight(), true);
                }
                else
                {
                    pFramebuffer->redraw(arg->x, arg->y, arg->w, arg->h, true);
                }

                return 0;
            }
            break;
        default:
            return -1;
    }

    return -1;
}
