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
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/klog.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <native/graphics/Graphics.h>
#include <native/input/Input.h>

#include <pedigree_fb.h>
#include <tui.h>

static Tui *g_Tui = nullptr;

class GfxConTuiRedrawer : public TuiRedrawer
{
    public:
    GfxConTuiRedrawer(Framebuffer *pFramebuffer)
    {
        m_pFramebuffer = pFramebuffer;
    }

    virtual void redraw(size_t x, size_t y, size_t w, size_t h)
    {
        m_pFramebuffer->flush(x, y, w, h);
    }

    private:
    Framebuffer *m_pFramebuffer;
};

/**
 * This is the TUI input handler. It is registered with the kernel at startup
 * and handles every keypress that occurs, via an Event sent from the kernel's
 * InputManager object.
 */
void input_handler(Input::InputNotification &note)
{
    if (!g_Tui)  // No terminal yet!
        return;

    if (note.type != Input::Key)
        return;

    uint64_t c = note.data.key.key;
    g_Tui->keyInput(c);
}

int main(int argc, char *argv[])
{
    // Create ourselves a lock file so we don't end up getting run twice.
    /// \todo Revisit this when exiting the window manager is possible.
    int fd = open("runtime»/gfxcon.lck", O_WRONLY | O_EXCL | O_CREAT, 0500);
    if (fd < 0)
    {
        fprintf(stderr, "gfxcon: lock file exists, terminating.\n");
        return EXIT_FAILURE;
    }
    close(fd);

    Framebuffer *pFramebuffer = new Framebuffer();
    if (!pFramebuffer->initialise())
    {
        fprintf(stderr, "gfxcon: framebuffer initialisation failed\n");
        return EXIT_FAILURE;
    }

    // Save current mode so we can restore it on quit.
    pFramebuffer->storeMode();

    // Kick off a process group, fork to run the modeset shim.
    setpgid(0, 0);
    pid_t child = fork();
    if (child == -1)
    {
        fprintf(stderr, "gfxcon: could not fork: %s\n", strerror(errno));
        return 1;
    }
    else if (child != 0)
    {
        // Wait for the child (ie, real window manager process) to terminate.
        int status = 0;
        waitpid(child, &status, 0);

        // Restore old graphics mode.
        pFramebuffer->restoreMode();
        delete pFramebuffer;

        // Termination information
        if (WIFEXITED(status))
        {
            fprintf(
                stderr, "gfxcon: terminated with status %d\n",
                WEXITSTATUS(status));
        }
        else if (WIFSIGNALED(status))
        {
            fprintf(
                stderr, "gfxcon: terminated by signal %d\n", WTERMSIG(status));
        }
        else
        {
            fprintf(stderr, "gfxcon: terminated by unknown means\n");
        }

        // Terminate our process group.
        kill(0, SIGTERM);
        return 0;
    }

    // Can we set the graphics mode we want?
    /// \todo Read from a config file!
    int result = pFramebuffer->enterMode(1024, 768, 32);
    if (result != 0)
    {
        return result;
    }

    size_t nWidth = pFramebuffer->getWidth();
    size_t nHeight = pFramebuffer->getHeight();

    Input::installCallback(Input::Key, input_handler);

    GfxConTuiRedrawer *redrawer = new GfxConTuiRedrawer(pFramebuffer);

    g_Tui = new Tui(redrawer);
    g_Tui->resize(nWidth, nHeight);
    g_Tui->recreateSurfaces(pFramebuffer->getFramebuffer());
    if (!g_Tui->initialise(nWidth, nHeight))
    {
        return 1;
    }

    g_Tui->run();

    delete g_Tui;
    delete pFramebuffer;

    return 0;
}
