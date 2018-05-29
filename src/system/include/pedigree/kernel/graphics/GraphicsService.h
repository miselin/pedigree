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

#ifndef _GRAPHICS_SERVICE_H
#define _GRAPHICS_SERVICE_H

#include "pedigree/kernel/ServiceManager.h"
#include "pedigree/kernel/Service.h"
#include "pedigree/kernel/ServiceFeatures.h"
#include "pedigree/kernel/processor/types.h"

#include "pedigree/kernel/machine/Display.h"
#include "pedigree/kernel/machine/Framebuffer.h"

class GraphicsService : public Service
{
  public:
    GraphicsService() : m_Providers(), m_pCurrentProvider(0)
    {
    }
    virtual ~GraphicsService()
    {
    }

    struct GraphicsProvider
    {
        Display *pDisplay;

        /* Some form of hardware caps here... */
        bool bHardwareAccel;

        Framebuffer *pFramebuffer;

        size_t maxWidth;
        size_t maxHeight;
        size_t maxDepth;

        size_t maxTextWidth;
        size_t maxTextHeight;

        /// Set to true if this display can drop back to a text-based mode
        /// with x86's int 10h thing. If this is false, the driver should
        /// handle "mode zero" as a "disable the video device" mode.
        bool bTextModes;
    };

    struct GraphicsParameters
    {
        // Typically, the current "best" provider will be used for a probe.
        // However, setting this adjusts the determination of the best
        // provider to give one with the largest possible text mode.
        bool wantTextMode;

        // Provider target, the resulting provider will be copied into this.
        // It is only valid if providerFound is true.
        bool providerFound;
        GraphicsProvider providerResult;
    };

    /** serve: Interface through which clients interact with the Service */
    bool serve(ServiceFeatures::Type type, void *pData, size_t dataLen);

  private:
    struct ProviderPair
    {
        GraphicsProvider *bestBase;
        GraphicsProvider *bestText;
    };

    ProviderPair determineBestProvider();

    List<GraphicsProvider *> m_Providers;

    GraphicsProvider *m_pCurrentProvider;
    GraphicsProvider *m_pCurrentTextProvider;
};

#endif
