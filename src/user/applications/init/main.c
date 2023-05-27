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

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/klog.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utmp.h>
#include <utmpx.h>

extern void pedigree_reboot();

static int g_Running = 1;

// SIGTERM handler - shutdown.
static void sigterm(int sig)
{
    /// \todo should terminate any leftover children too.
    g_Running = 0;
}

static pid_t start(const char *proc)
{
    pid_t f = fork();
    if (f == -1)
    {
        klog(LOG_ALERT, "init: fork failed %s", strerror(errno));
        exit(errno);
    }
    if (f == 0)
    {
        klog(LOG_INFO, "init: starting %s...", proc);
        execl(proc, proc, 0);
        klog(LOG_INFO, "init: loading %s failed: %s", proc, strerror(errno));
        exit(errno);
    }
    klog(LOG_INFO, "init: %s running with pid %d", proc, f);

    // Avoid calling basename() on the given parameter, as basename is
    // non-const.
    char basename_buf[PATH_MAX];
    strncpy(basename_buf, proc, PATH_MAX);

    // Add a utmp entry.
    setutxent();
    struct utmpx init;
    struct timeval tv;
    memset(&init, 0, sizeof(init));
    gettimeofday(&tv, NULL);
    init.ut_type = INIT_PROCESS;
    init.ut_pid = f;
    init.ut_tv = tv;
    strncpy(init.ut_id, basename(basename_buf), UT_LINESIZE);
    pututxline(&init);
    endutxent();

    return f;
}

static void startAndWait(const char *proc)
{
    pid_t f = start(proc);
    waitpid(f, 0, 0);
}

static void runScripts()
{
    struct dirent **namelist;

    int count = scandir("/system/initscripts", &namelist, 0, alphasort);
    if (count < 0)
    {
        klog(
            LOG_CRIT, "could not scan /system/initscripts: %s",
            strerror(errno));
    }
    else
    {
        for (int i = 0; i < count; ++i)
        {
            char script[PATH_MAX];
            snprintf(
                script, PATH_MAX, "/system/initscripts/%s",
                namelist[i]->d_name);

            if (!strcmp(namelist[i]->d_name, ".") ||
                !strcmp(namelist[i]->d_name, ".."))
            {
                free(namelist[i]);
                continue;
            }

            free(namelist[i]);

            struct stat st;
            int r = stat(script, &st);
            if (r == 0)
            {
                if (S_ISREG(st.st_mode) &&
                    (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
                {
                    // OK - we can run this.
                    klog(LOG_INFO, "init: running %s", script);
                    startAndWait(script);
                }
                else
                {
                    klog(
                        LOG_INFO,
                        "init: not running %s (not a file, or not executable)",
                        script);
                }
            }
            else
            {
                klog(
                    LOG_INFO, "init: cannot stat %s (broken symlink?)", script);
            }
        }

        free(namelist);
    }
}

int main(int argc, char **argv)
{
    klog(LOG_INFO, "init: starting...");

    // Make sure we have a utmp file.
    int fd = open(UTMP_FILE, O_CREAT | O_RDWR, 0664);
    if (fd >= 0)
        close(fd);

    // Set default umask to be inherited by all our children.
    umask(0022);

    // Set up utmp.
    setutxent();

    // Boot time (for uptime etc).
    struct timeval tv;
    gettimeofday(&tv, NULL);

    struct utmpx boot;
    memset(&boot, 0, sizeof(boot));
    boot.ut_type = BOOT_TIME;
    boot.ut_tv = tv;
    pututxline(&boot);

    // All done with utmp.
    endutxent();

    // Prepare signals.
    signal(SIGTERM, sigterm);

    if (HOSTED)
    {
        // Reboot the system instead of starting up.
        klog(LOG_INFO, "init: hosted build, triggering a reboot");
        pedigree_reboot();
    }
    else
    {
        runScripts();
    }

    // Done, enter PID reaping loop.
    klog(LOG_INFO, "init: complete!");
    while (1)
    {
        /// \todo Do we want to eventually recognise that we have no more
        ///       children, and terminate/shutdown/restart?
        int status = 0;
        pid_t changer = waitpid(-1, &status, g_Running == 0 ? WNOHANG : 0);
        if (changer > 0)
        {
            klog(
                LOG_INFO, "init: child %d exited with status %d", changer,
                WEXITSTATUS(status));
        }
        else if (!g_Running)
        {
            klog(
                LOG_INFO, "init: no more children and have been asked to "
                          "terminate, terminating...");
            break;
        }
        else
        {
            continue;
        }

        // Register the dead process now.
        struct utmpx *p = 0;
        setutxent();
        do
        {
            p = getutxent();
            if (p && (p->ut_type == INIT_PROCESS && p->ut_pid == changer))
                break;
        } while (p);

        if (!p)
        {
            endutxent();
            continue;
        }

        setutxent();
        struct utmpx dead;
        memset(&dead, 0, sizeof(dead));
        gettimeofday(&tv, NULL);
        dead.ut_type = DEAD_PROCESS;
        dead.ut_pid = changer;
        dead.ut_tv = tv;
        strncpy(dead.ut_id, p->ut_id, UT_LINESIZE);
        pututxline(&dead);
        endutxent();
    }
    return 0;
}
