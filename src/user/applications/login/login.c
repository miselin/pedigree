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

#define _GNU_SOURCE 1

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <libintl.h>
#include <locale.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/klog.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <utmp.h>
#include <utmpx.h>

// Immediate login credentials if we're running a live CD.
#define FORCE_LOGIN_USER "root"
#define FORCE_LOGIN_PASS "root"

// PID of the process we're running
int g_RunningPid = -1;

// Pedigree function, from libpedigree-c
extern int pedigree_login(int uid, const char *password);

// SIGINT handler
void sigint(int sig)
{
    // If we're in the background...
    if (g_RunningPid != -1)
    {
        // Ignore, but don't log (running program)
    }
    else
    {
        // Do not kill us! CTRL-C does not do anything while the login prompt
        // is active
        klog(LOG_NOTICE, "SIGINT ignored");
    }
}

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    bindtextdomain("login", "/system/locale");
    bind_textdomain_codeset("login", "UTF-8");
    textdomain("login");

#ifdef INSTALLER
    // For the installer, just run Python
    printf("Loading installer, please wait...\n");

    static const char *app_argv[] = {"root»/applications/python",
                                     "root»/code/installer/install.py", 0};
    static const char *app_env[] = {"TERM=xterm", "PATH=/applications",
                                    "PYTHONHOME=/", 0};
    execve(
        "root»/applications/python", (char *const *) app_argv,
        (char *const *) app_env);

    printf("FATAL: Couldn't load Python!\n");

    return 0;
#endif

    // Are we on Travis-CI?
    if (TRAVIS)
    {
        klog(LOG_INFO, "-- Hello, Travis! --");
    }

    // New process group for job control. We'll ignore SIGINT for now.
    signal(SIGINT, sigint);
    setsid();

    // Make sure we still have the terminal, though.
    ioctl(1, TIOCSCTTY, 0);

    // Set ourselves as the terminal's foreground process group.
    tcsetpgrp(1, getpgrp());

    // Get/fix $TERM.
    const char *TERM = getenv("TERM");
    if (!TERM)
    {
        TERM = "pedigree";
        setenv("TERM", TERM, 1);
    }

    const char *envLcAll = getenv("LC_ALL");
    if (!envLcAll)
    {
        envLcAll = "en_US.UTF-8";
        setenv("LC_ALL", envLcAll, 1);
    }

    // Turn on output processing if it's not already on (we depend on it)
    struct termios curt;
    tcgetattr(1, &curt);
    if (!(curt.c_oflag & OPOST))
        curt.c_oflag |= OPOST;
    tcsetattr(1, TCSANOW, &curt);

    while (1)
    {
        // Clear screen before from a previous session before we do anything
        // else.
        printf("\033[2J");

        // Write the login greeting.
        printf(gettext("Welcome to Pedigree\n"));

        // Set terminal title, if we can.
        if (!strcmp(TERM, "xterm"))
        {
            printf("\033]0;");
            printf(gettext("Pedigree Login"));
            printf("\007");
        }

        // Not running anything
        g_RunningPid = -1;

        // This handles the case where a bad character goes into the stream and
        // is impossible to get out. Everything else I've tried does not work...
        close(0);
        int fd = open("/dev/tty", 0);
        if (fd != 0)
            dup2(fd, 0);

        // Get username
        printf(gettext("Username: "));

        char buffer[256];
        char *username = NULL;

        if (LIVECD)
        {
            username = FORCE_LOGIN_USER;
            printf("%s\n", username);
        }
        else
        {
            fflush(stdout);

            username = fgets(buffer, 256, stdin);
            if (!username)
            {
                continue;
            }

            // Knock off the newline character
            username[strlen(username) - 1] = '\0';
            if (!strlen(username))
            {
                continue;
            }
        }

        struct passwd *pw = getpwnam(username);
        if (!pw)
        {
            printf(gettext("\nUnknown user: '%s'\n"), username);
            continue;
        }

        // Get password
        printf(gettext("Password: "));

        char *password = NULL;

        if (LIVECD)
        {
            const char *password = FORCE_LOGIN_PASS;
            printf(gettext("(forced)\n"));
        }
        else
        {
            // Use own way - display *
            fflush(stdout);
            char c;
            int i = 0;

            tcgetattr(0, &curt);
            curt.c_lflag &= ~(ECHO | ICANON);
            tcsetattr(0, TCSANOW, &curt);
            while (i < 256 && (c = getchar()) != '\n')
            {
                if (!c)
                {
                    continue;
                }
                else if (c == '\b')
                {
                    if (i > 0)
                    {
                        buffer[--i] = '\0';
                        printf("\b \b");
                    }
                }
                else if (c != '\033')
                {
                    buffer[i++] = c;
                    if (!strcmp(TERM, "xterm"))
                        printf("•");
                    else
                        printf("*");
                }
            }
            tcgetattr(0, &curt);
            curt.c_lflag |= (ECHO | ICANON);
            tcsetattr(0, TCSANOW, &curt);
            printf("\n");

            buffer[i] = '\0';
            password = buffer;
        }

        // Perform login - this function is in glue.c.
        if (pedigree_login(pw->pw_uid, password) != 0)
        {
            printf(gettext("Password incorrect.\n"));
            continue;
        }
        else
        {
            // Terminal title -> shell name.
            if (!strcmp(TERM, "xterm"))
                printf("\033]0;%s\007", pw->pw_shell);

            // Successful login.
            struct utmpx *p = 0;
            setutxent();
            do
            {
                p = getutxent();
                if (p && (p->ut_type == LOGIN_PROCESS && p->ut_pid == getpid()))
                    break;
            } while (p);

            if (p)
            {
                struct utmpx ut;
                memcpy(&ut, p, sizeof(ut));

                struct timeval tv;
                gettimeofday(&tv, NULL);
                ut.ut_tv = tv;
                ut.ut_type = USER_PROCESS;
                strncpy(ut.ut_user, pw->pw_name, UT_NAMESIZE);

                setutxent();
                pututxline(&ut);
            }
            endutxent();

            // Logged in successfully - launch the shell.
            int pid;
            pid = g_RunningPid = fork();

            if (pid == -1)
            {
                perror("fork");
                exit(errno);
            }
            else if (pid == 0)
            {
                // Child...
                g_RunningPid = -1;

                // Environment - only pass certain variables to the new process.
                char *newenv[4];
                newenv[0] = (char *) malloc(256);
                newenv[1] = (char *) malloc(256);
                newenv[2] = (char *) malloc(256);
                newenv[3] = 0;

                sprintf(newenv[0], "HOME=%s", pw->pw_dir);
                sprintf(newenv[1], "TERM=%s", TERM);
                sprintf(newenv[2], "LC_ALL=%s", envLcAll);

                // Make sure we're starting a login shell.
                char *shell = (char *) malloc(strlen(pw->pw_shell) + 1);
                sprintf(shell, "-%s", pw->pw_shell);

                // Child.
                execle(pw->pw_shell, shell, 0, newenv);

                // If we got here, the exec failed.
                perror("execve");
                exit(1);
            }
            else
            {
                // Parent.
                int stat;
                waitpid(pid, &stat, 0);

                g_RunningPid = -1;

                continue;
            }
        }
    }

    return 0;
}
