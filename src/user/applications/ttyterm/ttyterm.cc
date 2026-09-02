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

#include "pedigree/native/input/Input.h"

#include <config.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <utmp.h>

#include <sys/fb.h>
#include <sys/ioctl.h>
#include <sys/klog.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>

// PID of the process we're running
pid_t g_RunningPid = -1;

// File descriptor for our PTY master.
int g_MasterPty;

#if LIVECD
#define FIRST_PROGRAM "/usr/bin/live"
#else
#define FIRST_PROGRAM "/usr/bin/login"
#endif

// Pedigree function, defined in glue.c
extern int login(int uid, char* password);

// SIGINT handler
void sigint(int sig) {
  // Ignore, but don't log (running program)
}

int main(int argc, char** argv) {
  klog(LOG_INFO, "ttyterm: starting up...");

  // Create ourselves a lock file so we don't end up getting run twice.
  int fd = open("/run/ttyterm.lck", O_WRONLY | O_EXCL | O_CREAT);
  if (fd < 0) {
    fprintf(stderr, "ttyterm: lock file exists, terminating.\n");
    return 1;
  }
  close(fd);

  // New process group for job control. We'll ignore SIGINT for now.
  signal(SIGINT, sigint);
  setsid();

  // Ensure we are in fact in text mode.
  int fb = open("/dev/fb", O_RDWR);
  if (fb >= 0) {
    /// \todo error handling?
    klog(LOG_INFO, "ttyterm: forcing text mode");
    pedigree_fb_modeset mode = {0, 0, 0};
    int rc = ioctl(fb, PEDIGREE_FB_SETMODE, &mode);
    close(fb);

    if (rc < 0) {
      klog(LOG_INFO, "ttyterm: couldn't force text mode, exiting");
      return 1;
    }
  }

  // Get a PTY and the main TTY.
  int tty = open("/dev/textui", O_RDWR);
  if (tty < 0) {
    klog(LOG_ALERT, "ttyterm: couldn't open /dev/textui: %s", strerror(errno));
    return 1;
  }

  g_MasterPty = posix_openpt(O_RDWR);
  if (g_MasterPty < 0) {
    close(tty);
    klog(LOG_ALERT, "ttyterm: couldn't get a pseudo-terminal to use: %s", strerror(errno));
    return 1;
  }

  /// \todo Assumption of size here.
  struct winsize ptySize;
  ptySize.ws_col = 80;
  ptySize.ws_row = 25;
  ioctl(g_MasterPty, TIOCSWINSZ, &ptySize);

  char slavename[64] = {0};
  strncpy(slavename, ptsname(g_MasterPty), 64);
  slavename[15] = 0;

  // Clear the screen.
  write(tty, "\e[2J", 5);

  // Start up child process.
  g_RunningPid = fork();
  if (g_RunningPid == -1) {
    klog(LOG_ALERT, "ttyterm: couldn't fork: %s", strerror(errno));
    return EXIT_FAILURE;
  } else if (g_RunningPid == 0) {
    close(0);
    close(1);
    close(2);
    close(tty);
    close(g_MasterPty);

    // Open the slave ready for the child.
    int slave = open(slavename, O_RDWR);
    if (slave < 0) {
      klog(LOG_ALERT, "ttyterm: couldn't open pty slave: %s", strerror(errno));
      exit(1);
    }

    if (dup2(slave, STDIN_FILENO) < 0) {
      klog(LOG_ALERT, "ttyterm: couldn't attach pty slave to stdin: %s", strerror(errno));
      exit(1);
    }
    dup2(slave, 1);
    dup2(slave, 2);
    if (slave > STDERR_FILENO) {
      close(slave);
    }

    // Text UI has a custom terminfo (it can do a little more than a
    // traditional vt100 can).
    setenv("TERM", "pedigree", 1);

    // Set locale variables (but don't worry about setlocale() itself).
    setenv("LC_ALL", "en_US.UTF-8", 1);

    // Add a utmp entry for this new process.
    setutxent();
    struct utmpx ut;
    struct timeval tv;
    memset(&ut, 0, sizeof(ut));
    gettimeofday(&tv, NULL);
    ut.ut_type = LOGIN_PROCESS;
    ut.ut_pid = getpid();
    ut.ut_tv = tv;
    strncpy(ut.ut_id, "/", sizeof(ut.ut_id));
    strncpy(ut.ut_line, "console", UT_LINESIZE);  // ttyterm is the console
    pututxline(&ut);
    endutxent();

    // Enable autowrap before loading the login process.
    write(slave, "\e[?7h", 5);

    klog(LOG_INFO, "Starting up '" FIRST_PROGRAM "' on pty %s", slavename);
    execl(FIRST_PROGRAM, FIRST_PROGRAM, 0);
    klog(LOG_ALERT, "Launching " FIRST_PROGRAM " failed (next line is the error in errno...)");
    klog(LOG_ALERT, strerror(errno));
    exit(1);
  }

  // Main loop - read from PTY master, write to TTY.
  const size_t maxBuffSize = 32768;
  char buffer[maxBuffSize];
  while (1) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(g_MasterPty, &fds);
    FD_SET(tty, &fds);

    int nReady = select(g_MasterPty + 1, &fds, NULL, NULL, NULL);
    if (nReady > 0) {
      // Handle incoming data from the PTY.
      if (FD_ISSET(g_MasterPty, &fds)) {
        // We need to inhibit any input events while we read from the
        // master pty, as events must write to it. If we were to
        // receive an event during this read, we'd deadlock in the
        // kernel.
        Input::inhibitEvents();
        ssize_t len = read(g_MasterPty, buffer, maxBuffSize);
        Input::uninhibitEvents();
        if (len > 0)
          write(tty, buffer, len);
      }

      // Handle incoming data from the TTY.
      if (FD_ISSET(tty, &fds)) {
        ssize_t len = read(tty, buffer, maxBuffSize);

        // Same problem as above - if we are writing and then an event
        // fires that triggers another write, we'll deadlock in the
        // kernel.
        Input::inhibitEvents();
        if (len > 0)
          write(g_MasterPty, buffer, len);
        Input::uninhibitEvents();
      }
    }
  }

  return 0;
}
