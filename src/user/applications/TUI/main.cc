/*
 * Copyright (c) 2008 James Molloy, Jörg Pfähler, Matthew Iselin
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

#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include <errno.h>

#include "environment.h"
#include <tui-syscall.h>
#include <syslog.h>

#include <pthread.h>

#include <signal.h>

#include "Terminal.h"

#include "Font.h"
#include "Png.h"
#include "Header.h"

#include <graphics/Graphics.h>
#include <input/Input.h>

#define CONSOLE_READ    1
#define CONSOLE_WRITE   2
#define CONSOLE_GETROWS 3
#define CONSOLE_GETCOLS 4
#define CONSOLE_DATA_AVAILABLE 5
#define CONSOLE_REFRESH 10
#define TUI_MODE_CHANGED 98
#define TUI_CHAR_RECV   99

#define NORMAL_FONT_PATH    "/system/fonts/DejaVuSansMono.ttf"
#define BOLD_FONT_PATH      "/system/fonts/DejaVuSansMono-Bold.ttf"

#define FONT_SIZE           12

/** End code from InputManager */

void log(char *c)
{
    syscall1(TUI_LOG, reinterpret_cast<size_t>(c));
}

void log(const char *c)
{
    syscall1(TUI_LOG, reinterpret_cast<size_t>(c));
}

struct TerminalList
{
    Terminal *term;
    TerminalList *next, *prev;
};

size_t sz;
TerminalList * volatile g_pTermList = 0;
TerminalList * volatile g_pCurrentTerm = 0;
Header *g_pHeader = 0;
size_t g_nWidth, g_nHeight;
size_t nextConsoleNum = 1;
size_t g_nLastResponse = 0;

PedigreeGraphics::Framebuffer *g_pFramebuffer = 0;

void modeChanged(size_t width, size_t height)
{
    syslog(LOG_ALERT, "w: %d, h: %d\n", width, height);

    g_pHeader->setWidth(width);

    g_nWidth = width;
    g_nHeight = height;

    TerminalList *pTL = g_pTermList;
    while (pTL)
    {
        Terminal *pTerm = pTL->term;

        // Kill and renew the buffers.
        pTerm->renewBuffer(width, height-(g_pHeader->getHeight()+1));

        DirtyRectangle rect;
        g_pHeader->select(pTerm->getTabId());

        g_pHeader->render(pTerm->getBuffer(), rect);

        doRedraw(rect);

        pTL = pTL->next;
    }
}

void selectTerminal(TerminalList *pTL, DirtyRectangle &rect)
{
    if (g_pCurrentTerm)
        g_pCurrentTerm->term->setActive(false, rect);

    g_pCurrentTerm = pTL;
    Syscall::setCurrentConsole(pTL->term->getTabId());

    g_pCurrentTerm->term->setActive(true, rect);

    g_pHeader->select(pTL->term->getTabId());
    g_pHeader->render(pTL->term->getBuffer(), rect);

    pTL->term->redrawAll(rect);
    
    doRedraw(rect);
}

Terminal *addTerminal(const char *name, DirtyRectangle &rect)
{
    size_t h = g_pHeader->getHeight()+1;

    Terminal *pTerm = new Terminal(const_cast<char*>(name), g_nWidth, g_nHeight-h, g_pHeader, 0, h, 0);

    TerminalList *pTermList = new TerminalList;
    pTermList->term = pTerm;
    pTermList->next = pTermList->prev = 0;

    if (g_pTermList == 0)
        g_pTermList = pTermList;
    else
    {
        TerminalList *_pTermList = g_pTermList;
        while (_pTermList->next)
            _pTermList = _pTermList->next;
        _pTermList->next = pTermList;
        pTermList->prev = _pTermList;
    }

    selectTerminal(pTermList, rect);

    TerminalList *pTL = g_pTermList;
    while (pTL)
    {
        DirtyRectangle rect2;
        g_pHeader->select(pTL->term->getTabId());
        g_pHeader->render(pTL->term->getBuffer(), rect2);
        doRedraw(rect2);
        pTL = pTL->next;
    }
    g_pHeader->select(pTermList->term->getTabId());

    return pTerm;
}

// Restores the full contents of the active terminal
void doRefresh(Terminal *pT)
{
    // Refresh the given terminal
    /// \todo Massive caveat with more than one graphics app running
    ///       at a time - they'll get covered up by this!
    if(g_pCurrentTerm)
        if(g_pCurrentTerm->term == pT)
            pT->refresh(); // Handle any region not redrawn by above

    // Redraw the header
    DirtyRectangle rect;
    g_pHeader->render(0, rect);
    doRedraw(rect);
}

bool checkCommand(uint64_t key, DirtyRectangle &rect)
{
    if ( (key & Keyboard::Ctrl) &&
         (key & Keyboard::Shift) &&
         (key & Keyboard::Special) )
    {
        uint32_t k = key&0xFFFFFFFFULL;
        char str[5];
        memcpy(str, reinterpret_cast<char*>(&k), 4);
        str[4] = 0;

#ifdef MULTIPLE_CONSOLES
        if (!strcmp(str, "left"))
        {
            if (g_pCurrentTerm->prev)
            {
                selectTerminal(g_pCurrentTerm->prev, rect);
                return true;
            }
        }
        else if (!strcmp(str, "righ"))
        {
            if (g_pCurrentTerm->next)
            {
                selectTerminal(g_pCurrentTerm->next, rect);
                return true;
            }
        }
        else if (!strcmp(str, "ins"))
        {
            // Create a new terminal.
            if(nextConsoleNum < 6)
            {
                char *pStr = new char[64];
                sprintf(pStr, "Console%ld", nextConsoleNum++);
                addTerminal(pStr, rect);
            }
            return true;
        }
#endif
    }
    return false;
}

Font *g_NormalFont;
Font *g_BoldFont;

void sigint(int)
{
    syslog(LOG_NOTICE, "TUI sent SIGINT, oops!");
}

/**
 * This is the TUI input handler. It is registered with the kernel at startup
 * and handles every keypress that occurs, via an Event sent from the kernel's
 * InputManager object.
 */
void input_handler(size_t p1, size_t p2, size_t* pBuffer, size_t p4)
{
    if(!g_pCurrentTerm || !g_pCurrentTerm->term) // No terminal yet!
    {
        syscall0(TUI_EVENT_RETURNED);
        return;
    }

    Input::InputNotification *pNote = reinterpret_cast<Input::InputNotification*>(&pBuffer[1]);
    if(pNote->type != Input::Key)
        return;

    uint64_t c = pNote->data.key.key;

    /** Add the key to the terminal queue */

    Terminal *pT = g_pCurrentTerm->term;

    DirtyRectangle rect2;

    if (c == '\n') c = '\r';

    // CTRL + key -> unprintable characters
    if((c & Keyboard::Ctrl) && !(c & Keyboard::Special))
    {
        c &= 0x1F;
        if(c == 0x3)
        {
            // Awaken and stop the RequestQueue if it's blocking
            syscall0(TUI_STOP_REQUEST_QUEUE);

            // Cancel the current write operation, if any
            g_pCurrentTerm->term->cancel();

            // Send the kill signal
            kill(g_pCurrentTerm->term->getPid(), SIGINT);
        }
    }

    if(checkCommand(c, rect2))
    {
        doRedraw(rect2);
        syscall0(TUI_EVENT_RETURNED);
    }
    else
        pT->addToQueue(c);
    rect2.reset();
    if(!pT->hasPendingRequest() && pT->queueLength() > 0)
        sz = pT->getPendingRequestSz();

    /** We now have a key on our queue, can we complete a read? */

    char *buffer = new char[sz + 1];
    char str[64];
    size_t i = 0;
    while (i < sz)
    {
        char c = pT->getFromQueue();
        if (c)
        {
            buffer[i++] = c;
            continue;
        }
        else break;
    }
    g_nLastResponse = i;
    if (pT->hasPendingRequest())
    {
        Syscall::respondToPending(g_nLastResponse, buffer, sz);
        pT->setHasPendingRequest(false, 0);
    }
    delete [] buffer;

    syscall0(TUI_EVENT_RETURNED);
}

int main (int argc, char **argv)
{
    if(getpid()>2)
    {
        printf("TUI is already running\n");
        return 1;
    }

    /// \todo Needs userspace event handling!
    // Input::installCallback(input_handler);

    syscall1(TUI_INPUT_REGISTER_CALLBACK, reinterpret_cast<uintptr_t>(input_handler));

    signal(SIGINT, sigint);
    
    // Connect to the graphics service
    PedigreeGraphics::Framebuffer *pRootFramebuffer = new PedigreeGraphics::Framebuffer();
    g_pFramebuffer = pRootFramebuffer->createChild(0, 0, pRootFramebuffer->getWidth(), pRootFramebuffer->getHeight());
    
    // Have we got a working mode?
    if(!g_pFramebuffer->getRawBuffer())
    {
        // No!
        syslog(LOG_EMERG, "TUI Error: No framebuffer available!");
        return 0;
    }

    g_nWidth = g_pFramebuffer->getWidth();
    g_nHeight = g_pFramebuffer->getHeight();
    
    g_pFramebuffer->rect(0, 0, g_nWidth, g_nHeight, 0, PedigreeGraphics::Bits24_Rgb);
    g_pFramebuffer->redraw(0, 0, g_nWidth, g_nHeight, true);

    g_NormalFont = new Font(FONT_SIZE, NORMAL_FONT_PATH,
                            true, g_nWidth);
    if (!g_NormalFont)
    {
        syslog(LOG_EMERG, "Error: Font '%s' not loaded!", NORMAL_FONT_PATH);
        return 0;
    }
    g_BoldFont = new Font(FONT_SIZE, BOLD_FONT_PATH,
                          true, g_nWidth);
    if (!g_BoldFont)
    {
        syslog(LOG_EMERG, "Error: Font '%s' not loaded!", BOLD_FONT_PATH);
        return 0;
    }
    
    rgb_t fore = {0xff, 0xff, 0xff, 0xff};
    rgb_t back = {0, 0, 0, 0};

    g_pHeader =  new Header(g_nWidth);

    g_pHeader->addTab(const_cast<char*>("The Pedigree Operating System"), 0);
    
    DirtyRectangle rect;

    // DirtyRectangle rect;
    Terminal *pCurrentTerminal = addTerminal("Console0", rect);
    rect.point(0, 0);
    rect.point(g_nWidth, g_nHeight);

    doRedraw(rect);

    size_t maxBuffSz = (32768 * 2) - 1;
    char *buffer = new char[maxBuffSz + 1];
    char str[64];
    size_t tabId;
    while (true)
    {
        size_t cmd = Syscall::nextRequest(g_nLastResponse, buffer, &sz, maxBuffSz, &tabId);
        // syslog(LOG_NOTICE, "Command %d received. (term %d, sz %d)", cmd, tabId, sz);

        if(cmd == 0)
            continue;

        if (cmd == TUI_MODE_CHANGED)
        {
            uint64_t u = * reinterpret_cast<uint64_t*>(buffer);
            syslog(LOG_ALERT, "u: %llx", u);
            modeChanged(u&0xFFFFFFFFULL, (u>>32)&0xFFFFFFFFULL);

            continue;
        }

        Terminal *pT = 0;
        TerminalList *pTL = g_pTermList;
        while (pTL)
        {
            if (pTL->term->getTabId() == tabId)
            {
                pT = pTL->term;
                break;
            }
            pTL = pTL->next;
        }
        if (pT == 0)
        {
            sprintf(str, "Completely unrecognised terminal ID %ld, aborting.", tabId);
            log (str);
            continue;
        }

        // If the current terminal's queue is empty, set the request
        // pending further input.
        if (cmd == CONSOLE_READ && pT->queueLength() == 0)
        {
            pT->setHasPendingRequest(true, sz);
            Syscall::requestPending();
            continue;
        }

        switch(cmd)
        {
            case CONSOLE_WRITE:
            {
                DirtyRectangle rect2;
                buffer[sz] = '\0';
                buffer[maxBuffSz] = '\0';
                pT->write(buffer, rect2);
                g_nLastResponse = sz;
                doRedraw(rect2);
                break;
            }

            case CONSOLE_READ:
            {
                char str[64];
                size_t i = 0;
                while (i < sz)
                {
                    char c = pT->getFromQueue();
                    if (c)
                    {
                        buffer[i++] = c;
                        continue;
                    }
                    else break;
                }
                g_nLastResponse = i;
                if (pT->hasPendingRequest())
                {
                    Syscall::respondToPending(g_nLastResponse, buffer, sz);
                    pT->setHasPendingRequest(false, 0);
                }
                break;
            }

            case CONSOLE_DATA_AVAILABLE:
                g_nLastResponse = (pT->queueLength() > 0);
                break;

            case CONSOLE_GETROWS:
                g_nLastResponse = pT->getRows();
                break;

            case CONSOLE_GETCOLS:
                g_nLastResponse = pT->getCols();
                break;

            case CONSOLE_REFRESH:
                doRefresh(pT);
                break;

            default:
            {
                char str[64];
                sprintf(str, "Unknown command: %x", cmd);
                log(str);
            }
        }

    }

    return 0;
}

