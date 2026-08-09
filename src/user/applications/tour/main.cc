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

#define __STDCPP_WANT_MATH_SPEC_FUNCS__ 0

#include <dialog.h>
#include <libintl.h>
#include <locale.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    setlocale(LC_ALL, "");
    bindtextdomain("tour", "/usr/share/locale");
    bind_textdomain_codeset("tour", "UTF-8");
    textdomain("tour");

    chdir("/");

    /// \todo make available in man pages too
    init_dialog(stdin, stdout);
    dialog_vars.colors = TRUE;
    dialog_vars.ok_label = gettext("OK");
    dialog_vars.nocancel = 1;

    dialog_msgbox(
        gettext("Welcome to Pedigree!"),
        gettext(
            "This short tour introduces the filesystem and the kernel "
            "debugger."),
        0, 0, 1);

    dlg_clear();

    dialog_prgbox(
        gettext("Pedigree Tour"), gettext("Here is the root filesystem:"),
        "ls /", 20, 52, 1);

    dlg_clear();

    dialog_msgbox(
        gettext("Pedigree Tour"),
        gettext(
            "Pedigree uses a conventional filesystem layout. Programs and "
            "libraries are under /usr, configuration is under /etc, user "
            "homes are under /home, and runtime state is under /run."),
        0, 0, 1);

    dlg_clear();

    dialog_msgbox(
        gettext("Pedigree Tour"),
        gettext(
            "Device files are available under /dev, process information "
            "under /proc, and temporary files under /tmp."),
        0, 0, 1);

    dlg_clear();

    dialog_msgbox(
        gettext("Pedigree Tour"),
        gettext(
            "If something goes wrong, you may find yourself in the "
            "Pedigree kernel debugger. This can also be accessed on-demand by "
            "pressing F12 at any time."),
        0, 0, 1);

    dlg_clear();

    dialog_msgbox(
        gettext("Pedigree Tour"),
        gettext(
            "In the debugger, you can read the kernel log, view "
            "backtraces, and do various other inspections to identify what "
            "went "
            "wrong or inspect kernel state."),
        0, 0, 1);

    dlg_clear();

    dialog_msgbox(
        gettext("Pedigree Tour"),
        gettext(
            "You can use the `help` command to see what is available in "
            "the debugger. If you run into an issue that triggers the "
            "debugger, "
            "please try and add a serial port log if you report it to us. "
            "Thanks!"),
        0, 0, 1);

    dlg_clear();

    dialog_msgbox(
        gettext("Pedigree Tour"),
        gettext(
            "The tour is now complete, and you are now better-equipped to "
            "handle Pedigree! Raise any issues you find at "
            "https://github.com/miselin/pedigree/issues.\n\n"
            "Thank you for trying out Pedigree!"),
        0, 0, 1);

    end_dialog();
    return 0;
}
