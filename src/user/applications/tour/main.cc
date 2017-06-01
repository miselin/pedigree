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

#include <dialog.h>
#include <libintl.h>
#include <locale.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    setlocale(LC_ALL, "");
    bindtextdomain("tour", "/system/locale");
    bind_textdomain_codeset("tour", "UTF-8");
    textdomain("tour");

    // Undo any silliness.
    chdir("root»/");

    /// \todo make available in man pages too
    init_dialog(stdin, stdout);
    dialog_vars.colors = TRUE;
    dialog_vars.ok_label = gettext("OK");
    dialog_vars.nocancel = 1;

    dialog_msgbox(
        gettext("Welcome to Pedigree!"),
        gettext(
            "This tour is designed to help you understand how Pedigree "
            "differs from other UNIX-like systems. It's interactive, so you "
            "can practice along the way."),
        0, 0, 1);

    dlg_clear();

    dialog_prgbox(
        gettext("Pedigree Tour"),
        gettext("Let's run the `ls' command for you:"), "ls root»/", 20, 52, 1);

    dlg_clear();

    dialog_msgbox(
        gettext("Pedigree Tour"),
        gettext("As you can see, the typical /bin, /lib, /var (and so on) are "
                "not present. Instead, you find /applications, /libraries, "
                "/system, /config, and so on. This is designed to be intuitive "
                "but it can cause problems with some software."),
        0, 0, 1);

    dlg_clear();

    dialog_msgbox(
        gettext("Pedigree Tour"),
        gettext("After the tour completes, you can navigate around the "
                "filesystem to to get a closer look at what each directory "
                "contains."),
        0, 0, 1);

    dlg_clear();

    dialog_msgbox(
        gettext("Pedigree Tour"),
        gettext("Another significant difference in Pedigree is the path "
                "structure. In Pedigree, paths follow the format "
                "[mount]»/path/to/file."),
        0, 0, 1);

    dlg_clear();

    dialog_msgbox(
        gettext("Pedigree Tour"),
        gettext(
            "We've switched directory to root»/ if you were elsewhere. "
            "The root mount always exists; Pedigree will not start without it."
            " Your applications and configuration exist under root»/."),
        0, 0, 1);

    dlg_clear();

    dialog_msgbox(
        gettext("Pedigree Tour"),
        gettext(
            "Paths that begin with a '/' will always operate in your "
            "current mount. Because the current working directory is root»/, "
            "we can simply run `/applications/ls' to run "
            "`root»/applications/ls'."),
        0, 0, 1);

    dlg_clear();

    while (true)
    {
        dlg_clr_result();
        dialog_inputbox(
            gettext("Pedigree Tour"),
            gettext(
                "Before we dig into what other mounts may exist, it's "
                "important to know how to type these paths. You can type the "
                "'»' character in Pedigree by using 'RIGHTALT-.' - try it "
                "now. If you want to finish the tour, just type 'quit'."),
            0, 0, "", 0);

        if (!strcmp(dialog_vars.input_result, "quit"))
        {
            end_dialog();
            return 0;
        }
        else if (!strcmp(dialog_vars.input_result, "»"))
        {
            break;
        }
    }

    dlg_clear();

    dialog_msgbox(
        gettext("Pedigree Tour"),
        gettext(
            "Now that you know how to type the paths, here are a "
            "selection of standard Pedigree mounts."
            "\n\n"
            "* dev» provides device access (ala /dev).\n"
            "* raw» provides access to raw disks and partitions.\n"
            "* scratch» is an entirely in-memory filesystem.\n"
            "* runtime» is an in-memory filesystem for runfiles (like /run).\n"
            "    Files here can only be modified by their owning process.\n"
            "* unix» provides a location for named UNIX sockets."),
        0, 0, 1);

    dlg_clear();

    dialog_msgbox(
        gettext("Pedigree Tour"),
        gettext(
            "Note that there is a significant caveat with respect to the "
            "$PATH variable with this scheme. If your $PATH does not contain "
            "absolute paths, you may find that switching working directory to "
            "a "
            "different mount point can cause you to be unable to run any "
            "commands."),
        0, 0, 1);

    dlg_clear();

    dialog_msgbox(
        gettext("Pedigree Tour"),
        gettext("This image has been configured such that the default PATH "
                "does this correctly. There may still be weirdness, and if you "
                "notice "
                "things are not quite working correctly, you can always run "
                "`cd root»/` to return to the root mount."),
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
        gettext("In the debugger, you can read the kernel log, view "
                "backtraces, and do various other inspections to identify what "
                "went "
                "wrong or inspect kernel state."),
        0, 0, 1);

    dlg_clear();

    dialog_msgbox(
        gettext("Pedigree Tour"),
        gettext("You can use the `help` command to see what is available in "
                "the debugger. If you run into an issue that triggers the "
                "debugger, "
                "please try and add a serial port log if you report it to us. "
                "Thanks!"),
        0, 0, 1);

    dlg_clear();

    dialog_msgbox(
        gettext("Pedigree Tour"),
        gettext("The tour is now complete, and  you are now better-equipped to "
                "handle Pedigree! "
                "Join us in #pedigree on Freenode IRC, and raise any issues "
                "you find "
                "at https://pedigree-project.org.\n\n"
                "Thank you for trying out Pedigree!"),
        0, 0, 1);

    end_dialog();
    return 0;
}
