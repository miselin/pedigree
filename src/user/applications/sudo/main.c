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
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

// Pedigree function, from libpedigree-c
extern int pedigree_login(int uid, char *password);

int main(int argc, char *argv[])
{
    int iRunShell = 0, error = 0, help = 0, nStart = 0, i = 0;
    for (i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-s"))
            iRunShell = 1;
        else if (!strcmp(argv[i], "-h"))
            help = 1;
        else if (!nStart)
            nStart = i;
    }

    // If there was an error, or if the help string needs to be printed, do so
    if (error || help || (!nStart && !iRunShell))
    {
        fprintf(stderr, "Usage: sudo [-h] [-s|<command>]\n");
        fprintf(stderr, "\n");
        fprintf(stderr, "    -s: Access root shell\n");
        fprintf(stderr, "    -h: Show this help text\n");
        return error;
    }

    // Grab the root user's pw structure
    struct passwd *pw = getpwnam("root");
    if (!pw)
    {
        fprintf(stderr, "sudo: user 'root' doesn't exist!\n");
        return 1;
    }

    // Request the root password
    char password[256];
    int c;
    size_t passwordLength = 0;

    struct termios curt;
    tcgetattr(0, &curt);
    curt.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(0, TCSANOW, &curt);

    printf("[sudo] Enter password: ");
    fflush(stdout);

    while ((c = getchar()) != '\n' && c != EOF)
    {
        if (c == '\b')
        {
            if (passwordLength > 0)
            {
                password[--passwordLength] = '\0';
                printf("\b \b");
            }
        }
        else if (
            c != '\033' && passwordLength < (sizeof(password) - 1))
        {
            password[passwordLength++] = c;
            printf("•");
        }
    }
    tcgetattr(0, &curt);
    curt.c_lflag |= (ECHO | ICANON);
    tcsetattr(0, TCSANOW, &curt);
    printf("\n");

    password[passwordLength] = '\0';

    // Attempt to log in as that user
    if (pedigree_login(pw->pw_uid, password) != 0)
    {
        fprintf(stderr, "sudo: password is incorrect\n");
        return 1;
    }

    // Begin a new session so SIGINT is properly handled here
    setsid();

    // We're now running as root, so execute whatever we're supposed to execute
    if (iRunShell)
    {
        // Execute root's shell
        int pid = fork();
        if (pid == -1)
        {
            fprintf(stderr, "sudo: couldn't fork: %s\n", strerror(errno));
            exit(errno);
        }
        else if (pid == 0)
        {
            // Run the command
            execlp(pw->pw_shell, pw->pw_shell, 0);

            // Command not found!
            fprintf(stderr, "sudo: couldn't run shell: %s\n", strerror(errno));
            exit(errno);
        }
        else
        {
            // Wait for it to complete
            int status;
            waitpid(pid, &status, 0);

            // Did it exit with a non-zero status?
            if (status)
            {
                // Return error
                exit(status);
            }
        }
    }
    else
    {
        // Run the command
        int pid = fork();
        if (pid == -1)
        {
            fprintf(stderr, "sudo: couldn't fork: %s\n", strerror(errno));
            exit(errno);
        }
        else if (pid == 0)
        {
            // Run the command
            execvp(argv[nStart], &argv[nStart]);

            // Command not found!
            fprintf(
                stderr, "sudo: couldn't run command '%s': %s\n", argv[nStart],
                strerror(errno));
            exit(errno);
        }
        else
        {
            // Wait for it to complete
            int status;
            waitpid(pid, &status, 0);

            // Did it exit with a non-zero status?
            if (status)
            {
                // Return error
                exit(status);
            }
        }
    }

    // All done!
    return 0;
}
