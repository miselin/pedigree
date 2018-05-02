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
#include <limits.h>
#include <locale.h>
#include <newlib.h>
#include <reent.h>
#include <setlocale.h>
#include <stdlib.h>
#include <string.h>

#ifdef TESTSUITE
#define SETLOCALE_FUNCTION_NAME pedigree_setlocale
#else
#define SETLOCALE_FUNCTION_NAME setlocale
#endif

#pragma GCC diagnostic ignored "-Wcast-qual"

#define CHAR_MAX 127

#define MAX_LOCALE_LENGTH 32

int __mb_cur_max = 1;

int __nlocale_changed = 0;
int __mlocale_changed = 0;
char *_PathLocale = NULL;

/// \todo this is lconv for the C locale, we want it for other locales too
static const struct lconv lconv = {
    (char *) ".", (char *) "", (char *) "", (char *) "", (char *) "",
    (char *) "",  (char *) "", (char *) "", (char *) "", (char *) "",
    CHAR_MAX,     CHAR_MAX,    CHAR_MAX,    CHAR_MAX,    CHAR_MAX,
    CHAR_MAX,     CHAR_MAX,    CHAR_MAX,
};

static char __locale_charset_value[ENCODING_LEN] = "ISO-8859-1";

#define NUM_LOCALES LC_MESSAGES

static char __locale_all[NUM_LOCALES * MAX_LOCALE_LENGTH] = "LC_COLLATE=C";
static char __locale_collate[MAX_LOCALE_LENGTH] = "C";
static char __locale_ctype[MAX_LOCALE_LENGTH] = "C";
static char __locale_monetary[MAX_LOCALE_LENGTH] = "C";
static char __locale_numeric[MAX_LOCALE_LENGTH] = "C";
static char __locale_time[MAX_LOCALE_LENGTH] = "C";
static char __locale_messages[MAX_LOCALE_LENGTH] = "C";

static char __locale_last_all[MAX_LOCALE_LENGTH] = "C";
static char __locale_last_collate[MAX_LOCALE_LENGTH] = "C";
static char __locale_last_ctype[MAX_LOCALE_LENGTH] = "C";
static char __locale_last_monetary[MAX_LOCALE_LENGTH] = "C";
static char __locale_last_numeric[MAX_LOCALE_LENGTH] = "C";
static char __locale_last_time[MAX_LOCALE_LENGTH] = "C";
static char __locale_last_messages[MAX_LOCALE_LENGTH] = "C";

// Needed for newlib.
char *__lc_ctype = __locale_ctype;

static char *__locale_entry[] = {
    // LC_ALL
    __locale_all, __locale_last_all,
    // LC_COLLATE
    __locale_collate, __locale_last_collate,
    // LC_CTYPE
    __locale_ctype, __locale_last_ctype,
    // LC_MONETARY
    __locale_monetary, __locale_last_monetary,
    // LC_NUMERIC
    __locale_numeric, __locale_last_numeric,
    // LC_TIME
    __locale_time, __locale_last_time,
    // LC_MESSAGES
    __locale_messages, __locale_last_time,
};

static const char *__locale_env[] = {
    "LC_ALL",     "LC_COLLATE", "LC_CTYPE",    "LC_MONETARY",
    "LC_NUMERIC", "LC_TIME",    "LC_MESSAGES",
};

#define SET_LAST(cat) \
    strncpy(__locale_last_##cat, __locale_##cat, MAX_LOCALE_LENGTH)
#define SET_TO(cat, val) strncpy(__locale_##cat, val, MAX_LOCALE_LENGTH)

char *SETLOCALE_FUNCTION_NAME(int category, const char *locale)
{
    const char *new_locale_arg = "C";
    char new_locale[MAX_LOCALE_LENGTH];

    // locale == NULL -> return current locale.
    if (!locale)
    {
        if (category < LC_ALL || category > LC_MESSAGES)
        {
            return 0;
        }

        return __locale_entry[category * 2];
    }
    // locale == "" -> obtain locale from the current environment.
    else if (!strcmp(locale, ""))
    {
        // Force "C" locale.
    }
    // locale == "C" or locale == "POSIX" -> C locale
    else if (!strcmp(locale, "C") || !strcmp(locale, "POSIX"))
    {
        // OK - new_locale_arg is already "C"...
    }
    else
    {
        // Force C locale.
    }

    /// \todo real locale support
    strncpy(new_locale, new_locale_arg, MAX_LOCALE_LENGTH);

    // No UTF-8 for default C locale.
    __mb_cur_max = 1;
    strcpy(__locale_charset_value, "ISO-8859-1");

    if (category == LC_ALL)
    {
        SET_LAST(all);
        SET_LAST(collate);
        SET_LAST(ctype);
        SET_LAST(monetary);
        SET_LAST(numeric);
        SET_LAST(time);
        SET_LAST(messages);

        SET_TO(all, new_locale);
        SET_TO(collate, new_locale);
        SET_TO(ctype, new_locale);
        SET_TO(monetary, new_locale);
        SET_TO(numeric, new_locale);
        SET_TO(time, new_locale);
        SET_TO(messages, new_locale);
    }
    else if (category == LC_COLLATE)
    {
        SET_LAST(collate);
        SET_TO(collate, new_locale);
    }
    else if (category == LC_CTYPE)
    {
        SET_LAST(ctype);
        SET_TO(ctype, new_locale);
    }
    else if (category == LC_MONETARY)
    {
        SET_LAST(monetary);
        SET_TO(monetary, new_locale);
    }
    else if (category == LC_NUMERIC)
    {
        SET_LAST(numeric);
        SET_TO(numeric, new_locale);
    }
    else if (category == LC_TIME)
    {
        SET_LAST(time);
        SET_TO(time, new_locale);
    }
    else if (category == LC_MESSAGES)
    {
        SET_LAST(messages);
        SET_TO(messages, new_locale);
    }

    // Return previous value.
    _REENT->_current_category = category;
    _REENT->_current_locale = locale;
    return __locale_entry[(category + 1) * 2];
}

struct lconv *_localeconv_r(struct _reent *data)
{
    return (struct lconv *) &lconv;
}

struct lconv *localeconv()
{
    return _localeconv_r(_REENT);
}

char *__locale_charset()
{
    return __locale_charset_value;
}
