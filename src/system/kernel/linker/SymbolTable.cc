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

#include "pedigree/kernel/linker/SymbolTable.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/linker/Elf.h"
#include "pedigree/kernel/utilities/smhasher/MurmurHash3.h"

#ifdef THREADS
#define RAII_LOCK LockGuard<Mutex> guard(m_Lock)
#else
#define RAII_LOCK
#endif

SymbolTable::SymbolTable(Elf *pElf)
    : m_LocalSymbols(), m_GlobalSymbols(), m_WeakSymbols(),
      m_pOriginatingElf(pElf)
{
}

SymbolTable::~SymbolTable()
{
}

void SymbolTable::copyTable(Elf *pNewElf, const SymbolTable &newSymtab)
{
    RAII_LOCK;

    // Safe to do this, all members are SharedPointers and will be copy
    // constructed by these operations.
    m_LocalSymbols = newSymtab.m_LocalSymbols;
    m_GlobalSymbols = newSymtab.m_GlobalSymbols;
    m_WeakSymbols = newSymtab.m_WeakSymbols;
}

void SymbolTable::insert(
    const String &name, Binding binding, Elf *pParent, uintptr_t value)
{
    RAII_LOCK;

    doInsert(name, binding, pParent, value);
}

void SymbolTable::insertMultiple(
    SymbolTable *pOther, const String &name, Binding binding, Elf *pParent,
    uintptr_t value)
{
    RAII_LOCK;
#ifdef THREADS
    LockGuard<Mutex> guard2(pOther->m_Lock);
#endif

    SharedPointer<Symbol> ptr = doInsert(name, binding, pParent, value);
    if (pOther)
        pOther->insertShared(name, ptr);
}

SharedPointer<SymbolTable::Symbol> SymbolTable::doInsert(
    const String &name, Binding binding, Elf *pParent, uintptr_t value)
{
    Symbol *pSymbol = new Symbol(pParent, binding, value);
    SharedPointer<Symbol> newSymbol(pSymbol);

    insertShared(name, newSymbol);
    return newSymbol;
}

void SymbolTable::insertShared(
    const String &name, SharedPointer<SymbolTable::Symbol> &symbol)
{
    SharedPointer<symbolTree_t> tree = getOrInsertTree(symbol->getParent());
    tree->insert(name, symbol);

    // Insert global/weak as well - if the lookup fails in the ELF's table,
    // it'll fall back to these.
    if (symbol->getBinding() == Global)
    {
        m_GlobalSymbols.insert(name, symbol);
    }
    else if (symbol->getBinding() == Weak)
    {
        m_WeakSymbols.insert(name, symbol);
    }
    return;
}

void SymbolTable::eraseByElf(Elf *pParent)
{
    RAII_LOCK;

    // Will wipe out recursively by destroying the SharedPointers within.
    m_LocalSymbols.remove(pParent);

    /// \todo wipe out global/weak symbols.

    /// \todo HashTable needs iteration support
#if 0
    for (auto it = m_GlobalSymbols.begin(); it != m_GlobalSymbols.end();)
    {
        if (it->getParent() == pParent)
        {
            it = m_GlobalSymbols.erase(it);
        }
        else
        {
            ++it;
        }
    }

    for (auto it = m_WeakSymbols.begin(); it != m_WeakSymbols.end();)
    {
        if (it->getParent() == pParent)
        {
            it = m_WeakSymbols.erase(it);
        }
        else
        {
            ++it;
        }
    }
#endif
}

uintptr_t
SymbolTable::lookup(const String &name, Elf *pElf, Policy policy, Binding *pBinding)
{
    RAII_LOCK;

    // Local to the ELF file itself.
    SharedPointer<Symbol> sym;
    SharedPointer<symbolTree_t> symbolTree = m_LocalSymbols.lookup(pElf);
    if (symbolTree)
    {
        symbolTree_t::LookupResult result = symbolTree->lookup(name);
        if (result.hasValue())
        {
            return result.value()->getValue();
        }
    }

    // Global lookup across all ELFs that expose global symbols.
    symbolTree_t::LookupResult globalResult = m_GlobalSymbols.lookup(name);
    if (globalResult.hasValue())
    {
        return globalResult.value()->getValue();
    }

    // Finally we try and find a usable weak symbol.
    symbolTree_t::LookupResult weakResult = m_WeakSymbols.lookup(name);
    if (weakResult.hasValue())
    {
        return weakResult.value()->getValue();
    }

    return 0;
}

SharedPointer<SymbolTable::symbolTree_t> SymbolTable::getOrInsertTree(Elf *p)
{
    SharedPointer<symbolTree_t> symbolTree = m_LocalSymbols.lookup(p);
    if (symbolTree)
    {
        return symbolTree;
    }

    auto tree = SharedPointer<symbolTree_t>::allocate();
    m_LocalSymbols.insert(p, tree);
    return tree;
}
