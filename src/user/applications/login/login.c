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

#include <config.h>

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
#include <termios.h>
#include <unistd.h>
#include <utmp.h>
#include <utmpx.h>

#include <sys/ioctl.h>
#include <sys/klog.h>
#include <sys/stat.h>
#include <sys/wait.h>

// Immediate login credentials if we're running a live CD.
#define FORCE_LOGIN_USER "root"
#define FORCE_LOGIN_PASS "root"

// PID of the process we're running
int g_RunningPid = -1;

// Pedigree function, from libpedigree-c
extern int pedigree_login(int uid, const char* password);

// SIGINT handler
void sigint(int sig) {
  // If we're in the background...
  if (g_RunningPid != -1) {
    // Ignore, but don't log (running program)
  } else {
    // Do not kill us! CTRL-C does not do anything while the login prompt
    // is active
    klog(LOG_NOTICE, "SIGINT ignored");
  }
}

int main(int argc, char** argv) {
  setlocale(LC_ALL, "");
  bindtextdomain("login", "/usr/share/locale");
  bind_textdomain_codeset("login", "UTF-8");
  textdomain("login");

#ifdef INSTALLER
  // For the installer, just run Python
  printf("Loading installer, please wait...\n");

  static const char* app_argv[] = {"/usr/bin/python", "/code/installer/install.py", 0};
  static const char* app_env[] = {"TERM=xterm", "PATH=/usr/bin:/usr/sbin", "PYTHONHOME=/", 0};
  execve("/usr/bin/python", (char* const*)app_argv, (char* const*)app_env);

  printf("FATAL: Couldn't load Python!\n");

  return 0;
#endif

  // New process group for job control. We'll ignore SIGINT for now.
  signal(SIGINT, sigint);
  setsid();

  // Make sure we still have the terminal, though.
  ioctl(1, TIOCSCTTY, 0);

  // Set ourselves as the terminal's foreground process group.
  tcsetpgrp(1, getpgrp());

  // Get/fix $TERM.
  const char* TERM = getenv("TERM");
  if (!TERM) {
    TERM = "pedigree";
    setenv("TERM", TERM, 1);
  }

  const char* envLcAll = getenv("LC_ALL");
  if (!envLcAll) {
    envLcAll = "en_US.UTF-8";
    setenv("LC_ALL", envLcAll, 1);
  }

  // Turn on output processing if it's not already on (we depend on it)
  struct termios curt;
  tcgetattr(1, &curt);
  if (!(curt.c_oflag & OPOST))
    curt.c_oflag |= OPOST;
  tcsetattr(1, TCSANOW, &curt);

  while (1) {
    // Clear screen before from a previous session before we do anything
    // else.
    printf("\033[2J");

    // Write the login greeting.
    printf(gettext("Welcome to Pedigree\n"));

    // Set terminal title, if we can.
    if (!strcmp(TERM, "xterm")) {
      printf("\033]0;");
      printf(gettext("Pedigree Login"));
      printf("\007");
    }

    // Not running anything
    g_RunningPid = -1;

    // This handles the case where a bad character goes into the stream and
    // is impossible to get out. Everything else I've tried does not work...
    int fd = open("/dev/tty", O_RDONLY);
    if (fd < 0) {
      klog(LOG_ERR, "Opening /dev/tty failed: %s", strerror(errno));
      return 1;
    }
    if (fd != STDIN_FILENO) {
      if (dup2(fd, STDIN_FILENO) < 0) {
        klog(LOG_ERR, "Replacing stdin failed: %s", strerror(errno));
        close(fd);
        return 1;
      }
      close(fd);
    }

    // Get username
    printf(gettext("Username: "));

    char buffer[256];
    char* username = NULL;

    if (LIVECD) {
      username = FORCE_LOGIN_USER;
      printf("%s\n", username);
    } else {
      fflush(stdout);

      username = fgets(buffer, 256, stdin);
      if (!username) {
        continue;
      }

      // Knock off the newline character
      username[strlen(username) - 1] = '\0';
      if (!strlen(username)) {
        continue;
      }
    }

    struct passwd* pw = getpwnam(username);
    if (!pw) {
      printf(gettext("\nUnknown user: '%s'\n"), username);
      continue;
    }

    // Get password
    printf(gettext("Password: "));

    char* password = NULL;

    if (LIVECD) {
      password = FORCE_LOGIN_PASS;
      printf(gettext("(forced)\n"));
    } else {
      // Use own way - display *
      fflush(stdout);
      int c;
      size_t i = 0;

      tcgetattr(0, &curt);
      curt.c_lflag &= ~(ECHO | ICANON);
      tcsetattr(0, TCSANOW, &curt);
      while ((c = getchar()) != '\n' && c != EOF) {
        if (!c) {
          continue;
        } else if (c == '\b') {
          if (i > 0) {
            buffer[--i] = '\0';
            printf("\b \b");
          }
        } else if (c != '\033' && i < (sizeof(buffer) - 1)) {
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
    if (pedigree_login(pw->pw_uid, password) != 0) {
      printf(gettext("Password incorrect.\n"));
      continue;
    } else {
      // Terminal title -> shell name.
      if (!strcmp(TERM, "xterm"))
        printf("\033]0;%s\007", pw->pw_shell);

      // Successful login.
      struct utmpx* p = 0;
      setutxent();
      do {
        p = getutxent();
        if (p && (p->ut_type == LOGIN_PROCESS && p->ut_pid == getpid()))
          break;
      } while (p);

      if (p) {
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

      if (pid == -1) {
        perror("fork");
        exit(errno);
      } else if (pid == 0) {
        // Child...
        g_RunningPid = -1;

        // Environment - only pass certain variables to the new process.
        char* newenv[4];
        size_t homeSize = strlen(pw->pw_dir) + sizeof("HOME=");
        size_t termSize = strlen(TERM) + sizeof("TERM=");
        size_t localeSize = strlen(envLcAll) + sizeof("LC_ALL=");

        newenv[0] = (char*)malloc(homeSize);
        newenv[1] = (char*)malloc(termSize);
        newenv[2] = (char*)malloc(localeSize);
        newenv[3] = 0;

        // Make sure we're starting a login shell.
        size_t shellSize = strlen(pw->pw_shell) + 2;
        char* shell = (char*)malloc(shellSize);

        if (!newenv[0] || !newenv[1] || !newenv[2] || !shell) {
          perror("malloc");
          exit(1);
        }

        snprintf(newenv[0], homeSize, "HOME=%s", pw->pw_dir);
        snprintf(newenv[1], termSize, "TERM=%s", TERM);
        snprintf(newenv[2], localeSize, "LC_ALL=%s", envLcAll);
        snprintf(shell, shellSize, "-%s", pw->pw_shell);

        // Child.
        execle(pw->pw_shell, shell, 0, newenv);

        // If we got here, the exec failed.
        perror("execve");
        exit(1);
      } else {
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
