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

#include <curses.h>
#include <dialog.h>
#include <dirent.h>
#include <errno.h>
#include <libintl.h>
#include <locale.h>

#include <string>
#include <vector>

#ifdef __PEDIGREE__
#define LOCALE_DIR "/system/locale"
#define KEYMAP_DIR "/system/keymaps"

extern "C" int pedigree_load_keymap(char *buffer, size_t len);
#else
#define LOCALE_DIR "./build/locale"
#define KEYMAP_DIR "./images/base/system/keymaps"

int pedigree_load_keymap(char *buffer, size_t len)
{
    return 0;
}
#endif

int scan_into_vector(const char *path, std::vector<std::string> &vec)
{
    struct dirent **namelist;
    int count = scandir(path, &namelist, 0, alphasort);
    if (count < 0)
    {
        perror("scandir");
        return -1;
    }
    else
    {
        for (int i = 0; i < count; ++i)
        {
            if (!strcmp(namelist[i]->d_name, ".") ||
                !strcmp(namelist[i]->d_name, ".."))
            {
                free(namelist[i]);
                continue;
            }

            vec.push_back(std::string(namelist[i]->d_name));
            free(namelist[i]);
        }

        free(namelist);
    }

    return 0;
}

void load_keymap(const char *path)
{
    std::string real_path = std::string(KEYMAP_DIR "/") + std::string(path);
    FILE *stream = fopen(real_path.c_str(), "r");
    if (!stream)
    {
        perror("fopen");
        return;
    }

    fseek(stream, 0, SEEK_END);
    size_t len = ftell(stream);
    fseek(stream, 0, SEEK_SET);

    char *buffer = (char *) malloc(len);
    fread(buffer, len, 1, stream);
    fclose(stream);

    pedigree_load_keymap(buffer, len);

    free(buffer);
}

int languages()
{
    std::vector<std::string> languages;
    if (scan_into_vector(LOCALE_DIR, languages) < 0)
    {
        return 1;
    }

    char **languages_menu = (char **) calloc(languages.size(), sizeof(char *));
    for (size_t i = 0; i < languages.size(); ++i)
    {
        languages_menu[i] = const_cast<char *>(languages[i].c_str());
    }

    dlg_clear();

    int current_item = 0;
    dialog_vars.nocancel = TRUE;
    dialog_vars.default_item = const_cast<char *>("en");
    dialog_vars.no_items = TRUE;
    dialog_vars.item_help = FALSE;
    int status = dialog_menu(
        "Language Selection",
        "Please select your preferred language from the list below.", 0, 0, 0,
        languages.size(), languages_menu);

    free(languages_menu);

    // Switch to the chosen language now.
    std::string chosen_language = std::string(dialog_vars.input_result);
    dlg_clr_result();

    setenv("LC_ALL", chosen_language.c_str(), 1);

    return 0;
}

int keymaps()
{
    std::vector<std::string> keymaps;
    if (scan_into_vector(KEYMAP_DIR, keymaps) < 0)
    {
        return 1;
    }

    // Move on to keymap selection.
    char **keymaps_menu = (char **) calloc(keymaps.size(), sizeof(char *));
    for (size_t i = 0; i < keymaps.size(); ++i)
    {
        keymaps_menu[i] = const_cast<char *>(keymaps[i].c_str());
    }

    dlg_clear();

    dialog_vars.nocancel = TRUE;
    dialog_vars.ok_label = gettext("OK");
    dialog_vars.no_items = TRUE;
    dialog_vars.item_help = FALSE;
    dialog_menu(
        gettext("Keyboard Layout Selection"),
        gettext("Please select your preferred keyboard layout from the list "
                "below."),
        0, 0, 0, keymaps.size(), keymaps_menu);

    free(keymaps_menu);

    // Load the new keymap.
    load_keymap(dialog_vars.input_result);
    dlg_clr_result();

    dlg_clear();

    dialog_vars.ok_label = gettext("OK");
    dialog_vars.nocancel = 1;
    dialog_msgbox(
        gettext("Ready to Go"),
        gettext("Configuration is complete.\n\nPedigree is ready for you."), 0,
        0, 1);

    return 0;
}

int main(int argc, char *argv[])
{
    setlocale(LC_ALL, "");
    bindtextdomain("live", LOCALE_DIR);
    bind_textdomain_codeset("live", "UTF-8");
    textdomain("live");

    if (argc < 1 || argc > 2)
    {
        return 0;
    }

    init_dialog(stdin, stdout);
    dialog_vars.colors = TRUE;
    int status = dialog_yesno(
        "Welcome to Pedigree",
        "Thanks for trying out Pedigree. This Live CD version supports a few "
        "languages and keyboard mappings, so we're going to ask some questions "
        "to "
        "find your preferences and apply them.\n\nAlternatively, you can just "
        "accept the default configuration (English language, EN-US keyboard).\n"
        "\nDo you want to accept the defaults?",
        0, 0);

    if (status != DLG_EXIT_OK)
    {
        if (languages())
        {
            return 1;
        }

        if (keymaps())
        {
            return 1;
        }
    }

    end_dialog();

#ifdef __PEDIGREE__
    execl("/applications/login", "/applications/login", 0);
#else
    return 0;
#endif
}
