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

#ifndef MEMORY_BACKEND_H
#define MEMORY_BACKEND_H

#include "pedigree/kernel/config/ConfigurationBackend.h"
#include "pedigree/kernel/config/ConfigurationManager.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/RadixTree.h"
#include "pedigree/kernel/utilities/String.h"

/** Memory configuration backend. Stores everything in RAM,
 * won't save to file. Good for runtime-only information.
 */
class MemoryBackend : public ConfigurationBackend
{
  public:
    MemoryBackend(const String &configStore);
    virtual ~MemoryBackend();

    virtual size_t createTable(const String &table);
    /** Inserts the value 'value' into the table 'table', with its key as 'key'
     */
    virtual void
    insert(const String &table, const String &key, const ConfigValue &value);
    /** Returns the value in table, with key matching 'key', or zero. */
    virtual ConfigValue &select(const String &table, const String &key);

    /** Watch a specific table entry. */
    virtual void
    watch(const String &table, const String &key, ConfigurationWatcher watcher);
    /** Remove a watcher from a table entry. */
    virtual void unwatch(
        const String &table, const String &key, ConfigurationWatcher watcher);

    const String &getTypeName();

  private:
    // A Table
    struct Table
    {
        Table() : m_Rows()
        {
        }
        ~Table()
        {
        }

        RadixTree<ConfigValue *> m_Rows;
    };

    // Our tables
    RadixTree<Table *> m_Tables;

    String m_TypeName;
};

#endif
