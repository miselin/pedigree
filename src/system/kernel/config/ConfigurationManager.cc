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

#include "pedigree/kernel/config/ConfigurationManager.h"
#include "pedigree/kernel/config/ConfigurationBackend.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/Result.h"
#include "pedigree/kernel/utilities/new"

ConfigurationManager ConfigurationManager::m_Instance;

ConfigurationManager::ConfigurationManager() : m_Backends()
{
}

ConfigurationManager::~ConfigurationManager()
{
}

ConfigurationManager &ConfigurationManager::instance()
{
    return m_Instance;
}

size_t ConfigurationManager::createTable(
    const String &configStore, const String &table)
{
    // Lookup the backend
    RadixTree<ConfigurationBackend *>::LookupType result =
        m_Backends.lookup(configStore);
    if (!result.hasValue())
        return 0;
    return result.value()->createTable(table);
}

void ConfigurationManager::insert(
    const String &configStore, const String &table, const String &key,
    const ConfigValue &value)
{
    // Lookup the backend
    RadixTree<ConfigurationBackend *>::LookupType result =
        m_Backends.lookup(configStore);
    if (!result.hasValue())
        return;
    return result.value()->insert(table, key, value);
}

ConfigValue &ConfigurationManager::select(
    const String &configStore, const String &table, const String &key)
{
    static ConfigValue v;

    // Lookup the backend
    RadixTree<ConfigurationBackend *>::LookupType result =
        m_Backends.lookup(configStore);
    if (!result.hasValue())
        return v;
    return result.value()->select(table, key);
}

void ConfigurationManager::watch(
    const String &configStore, const String &table, const String &key,
    ConfigurationWatcher watcher)
{
    // Lookup the backend
    RadixTree<ConfigurationBackend *>::LookupType result =
        m_Backends.lookup(configStore);
    if (!result.hasValue())
        return;
    return result.value()->watch(table, key, watcher);
}

void ConfigurationManager::unwatch(
    const String &configStore, const String &table, const String &key,
    ConfigurationWatcher watcher)
{
    // Lookup the backend
    RadixTree<ConfigurationBackend *>::LookupType result =
        m_Backends.lookup(configStore);
    if (!result.hasValue())
        return;
    return result.value()->unwatch(table, key, watcher);
}

bool ConfigurationManager::installBackend(
    ConfigurationBackend *pBackend, const String &configStore)
{
    // Check for sanity
    if (!pBackend)
        return false;

    // Get the real config store to use
    const String &realConfigStore =
        configStore.length() ? configStore : pBackend->getConfigStore();

    // Install into the list
    if (!backendExists(realConfigStore))
    {
        m_Backends.insert(realConfigStore, pBackend);
        return true;
    }
    else
        return false;
}

void ConfigurationManager::removeBackend(const String &configStore)
{
    // Lookup the backend
    RadixTree<ConfigurationBackend *>::LookupType result =
        m_Backends.lookup(configStore);
    if (!result.hasValue())
        return;

    // Remove it from the list and free used memory
    m_Backends.remove(configStore);
    delete result.value();
}

bool ConfigurationManager::backendExists(const String &configStore)
{
    return m_Backends.lookup(configStore).hasValue();
}
