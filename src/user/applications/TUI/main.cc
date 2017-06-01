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

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/klog.h>
#include <sys/stat.h>
#include <unistd.h>

#include <native/graphics/Graphics.h>
#include <native/input/Input.h>

#include <Widget.h>

#include <tui.h>

class PedigreeTerminalEmulator : public Widget
{
    public:
    PedigreeTerminalEmulator() : Widget(), m_nWidth(0), m_nHeight(0){};

    virtual ~PedigreeTerminalEmulator(){};

    virtual bool
    render(PedigreeGraphics::Rect &rt, PedigreeGraphics::Rect &dirty)
    {
        return true;
    }

    void handleReposition(const PedigreeGraphics::Rect &rt)
    {
        m_nWidth = rt.getW();
        m_nHeight = rt.getH();
    }

    size_t getWidth() const
    {
        return m_nWidth;
    }

    size_t getHeight() const
    {
        return m_nHeight;
    }

    private:
    size_t m_nWidth;
    size_t m_nHeight;
};

static Tui *g_Tui = nullptr;
static PedigreeTerminalEmulator *g_pEmu = nullptr;

void sigint(int)
{
    klog(LOG_NOTICE, "TUI received SIGINT, oops!");
}

bool callback(WidgetMessages message, size_t msgSize, const void *msgData)
{
    if (!g_Tui)
    {
        return false;
    }

    switch (message)
    {
        case Reposition:
        {
            klog(LOG_INFO, "-- REPOSITION --");
            const PedigreeGraphics::Rect *rt =
                reinterpret_cast<const PedigreeGraphics::Rect *>(msgData);
            klog(LOG_INFO, " -> handling...");
            g_pEmu->handleReposition(*rt);
            klog(LOG_INFO, " -> registering the mode change");
            g_Tui->resize(rt->getW(), rt->getH());
            klog(LOG_INFO, " -> creating new framebuffer");
            g_Tui->recreateSurfaces(g_pEmu->getRawFramebuffer());
            klog(LOG_INFO, " -> reposition complete!");
        }
        break;
        case KeyUp:
            g_Tui->keyInput(*reinterpret_cast<const uint64_t *>(msgData));
            break;
        case Focus:
            g_Tui->setCursorStyle(true);
            break;
        case NoFocus:
            g_Tui->setCursorStyle(false);
            break;
        case RawKeyDown:
        case RawKeyUp:
            // Ignore.
            break;
        case Terminate:
            klog(LOG_INFO, "TUI: termination request");
            g_Tui->stop();
            break;
        default:
            klog(LOG_INFO, "TUI: unhandled callback");
    }

    return true;
}

int main(int argc, char *argv[])
{
#ifdef TARGET_LINUX
    openlog("tui", LOG_PID, LOG_USER);
#endif

    klog(LOG_INFO, "I am %d", getpid());

    char endpoint[256];
    sprintf(endpoint, "tui.%d", getpid());

    PedigreeGraphics::Rect rt;

    g_pEmu = new PedigreeTerminalEmulator();
    g_Tui = new Tui(g_pEmu);

    klog(LOG_INFO, "TUI: constructing widget '%s'...", endpoint);
    if (!g_pEmu->construct(endpoint, "Pedigree xterm Emulator", callback, rt))
    {
        klog(LOG_ERR, "tui: couldn't construct widget");
        delete g_Tui;
        delete g_pEmu;
        return 1;
    }
    klog(LOG_INFO, "TUI: widget constructed!");

    signal(SIGINT, sigint);

    // Handle initial reposition event.
    Widget::checkForEvents(true);

    if (!g_Tui->initialise(g_pEmu->getWidth(), g_pEmu->getHeight()))
    {
        return 1;
    }
    else
    {
        g_Tui->run();
    }

    delete g_Tui;
    delete g_pEmu;

    return 0;
}
