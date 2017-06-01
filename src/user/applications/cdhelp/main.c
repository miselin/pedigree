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

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    if (argc == 1)
    {
        // Nothing to do here (cd with no parameters).
        return 0;
    }

    // Do we need to help out?
    struct stat st, st_root;
    int r = stat(argv[1], &st);
    if (r < 0)
    {
        // Maybe the directory doesn't exist.
        return 0;
    }

    r = stat("root»/", &st_root);
    if (r < 0)
    {
        // ???
        return 0;
    }

    if (st.st_dev == st_root.st_dev)
    {
        // Same filesystem, no need to help out.
        return 0;
    }

    struct passwd *pw = getpwuid(getuid());
    const char *home = pw->pw_dir;

    char buf[PATH_MAX];
    snprintf(buf, PATH_MAX, "%s/.cdhelp", home);
    r = open(buf, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (r < 0 && errno == EEXIST)
    {
        // Already helped out before - nothing to do here.
        return 0;
    }
    else if (r < 0)
    {
        // Some other error - don't make noise.
        return 0;
    }
    close(r);

    printf("You're about to cd from root» to another mount.\n");
    printf("`cd /` will take you to the base of your new mount.\n");
    printf("To return, use `cd root»` (Type '»' using Right ALT + .).\n");
    printf("To see this message again, remove $HOME/.cdhelp.\n");

    return 0;
}
