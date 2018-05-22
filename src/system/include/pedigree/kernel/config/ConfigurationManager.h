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

#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/RadixTree.h"
#include "pedigree/kernel/utilities/String.h"

class ConfigurationBackend;

#define MAX_WATCHERS 4

/** A "watcher": A callback to be called when the watched item changes. */
typedef void (*ConfigurationWatcher)(void);

enum ConfigValueType
{
    Invalid,
    Number,
    Str,
    Bool
};
struct ConfigValue
{
    ConfigValue() : str(""), num(0), b(false), type(Invalid)
    {
        for (int i = 0; i < MAX_WATCHERS; i++)
            watchers[i] = 0;
    }
    ConfigValue(const ConfigValue &cv)
        : str(cv.str), num(cv.num), b(cv.b), type(cv.type)
    {
        for (int i = 0; i < MAX_WATCHERS; i++)
            watchers[i] = cv.watchers[i];
    }
    String str;
    size_t num;
    bool b;
    ConfigValueType type;
    ConfigurationWatcher watchers[MAX_WATCHERS];
};

/** The central manager for the Pedigree configuration system.
 *
 * This class provides a thin layer between the kernel and multiple
 * configuration backends.
 *
 * The configuration system is table based; like a database. Tables contain
 * columns and rows, one column being the key; At the moment the key must be a
 * String, and there can only be one piece of data per row.
 */
class ConfigurationManager
{
  public:
    ConfigurationManager();
    virtual ~ConfigurationManager();

    static ConfigurationManager &instance();

    size_t createTable(const String &configStore, const String &table);
    /** Inserts the value 'value' into the table 'table', with its key as 'key'
     */
    void
    insert(const String &configStore, const String &table, const String &key, const ConfigValue &value);
    /** Returns the value in table, with key matching 'key', or a Value with
     * "type" field as Invalid. */
    ConfigValue &select(const String &configStore, const String &table, const String &key);

    /** Watch a specific table entry. */
    void watch(
        const String &configStore, const String &table, const String &key,
        ConfigurationWatcher watcher);
    /** Remove a watcher from a table entry. */
    void unwatch(
        const String &configStore, const String &table, const String &key,
        ConfigurationWatcher watcher);

    bool installBackend(
        ConfigurationBackend *pBackend, const String &configStore = String(""));
    void removeBackend(const String &configStore);

    bool backendExists(const String &configStore);

  private:
    static ConfigurationManager m_Instance;

    RadixTree<ConfigurationBackend *> m_Backends;
};

#endif
