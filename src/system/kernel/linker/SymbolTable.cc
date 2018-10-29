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
#include "pedigree/kernel/utilities/Iterator.h"

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

void SymbolTable::preallocate(
    size_t numGlobal, size_t numWeak, Elf *localElf, size_t numLocal)
{
    auto tree = getOrInsertTree(localElf);
    tree->reserve(numLocal);

    tree = getOrInsertTree(localElf, Global);
    tree->reserve(numGlobal);

    tree = getOrInsertTree(localElf, Weak);
    tree->reserve(numWeak);
}

void SymbolTable::preallocateAdditional(
    size_t numGlobal, size_t numWeak, Elf *localElf, size_t numLocal)
{
    auto tree = getOrInsertTree(localElf, Global);
    tree->reserve(tree->count() + numGlobal);

    tree = getOrInsertTree(localElf, Weak);
    tree->reserve(tree->count() + numWeak);

    tree = getOrInsertTree(localElf);
    tree->reserve(tree->count() + numLocal);
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
    auto tree = getOrInsertTree(symbol->getParent(), symbol->getBinding());
    tree->insert(name, symbol);
}

void SymbolTable::eraseByElf(Elf *pParent)
{
    RAII_LOCK;

    // Will wipe out recursively by destroying the SharedPointers within.
    m_LocalSymbols.remove(pParent);
    m_GlobalSymbols.remove(pParent);
    m_WeakSymbols.remove(pParent);
}

uintptr_t SymbolTable::lookup(
    const HashedStringView &name, Elf *pElf, Policy policy, Binding *pBinding)
{
    RAII_LOCK;

    // safe empty SharedPointer we can use for lookupRef()'s failed result
    static SharedPointer<symbolTree_t> failedLookup;

    uintptr_t lookupResult = 0;

    // Local to the ELF file itself.
    if (policy != NotOriginatingElf)
    {
        const SharedPointer<symbolTree_t> &symbolTree = m_LocalSymbols.lookupRef(pElf, failedLookup);
        if (symbolTree)
        {
            symbolTree_t::LookupResult result = symbolTree->lookup(name);
            if (result.hasValue())
            {
                lookupResult = result.value()->getValue();
            }
        }
    }

    // Global lookup across all ELFs that expose global symbols.
    if (!lookupResult)
    {
        for (parentedSymbolTree_t::Iterator it = m_GlobalSymbols.begin();
             it != m_GlobalSymbols.end();
             ++it)
        {
            symbolTree_t::LookupResult result = it.value(failedLookup)->lookup(name);
            if (result.hasValue())
            {
                lookupResult = result.value()->getValue();
                break;
            }
        }
    }

    // Finally we try and find a usable weak symbol.
    if (!lookupResult)
    {
        for (parentedSymbolTree_t::Iterator it = m_WeakSymbols.begin();
             it != m_WeakSymbols.end();
             ++it)
        {
            symbolTree_t::LookupResult result = it.value(failedLookup)->lookup(name);
            if (result.hasValue())
            {
                lookupResult = result.value()->getValue();
                break;
            }
        }
    }

    // NOTICE("SymbolTable::lookup(" << name << ", " << pElf->getName() << ")
    // ==> " << Hex << lookupResult);

    return lookupResult;
}

SymbolTable::symbolTree_t *SymbolTable::getOrInsertTree(Elf *p, Binding table)
{
    // safe empty SharedPointer we can use for lookupRef()'s failed result
    static SharedPointer<symbolTree_t> v;

    Tree<Elf *, SharedPointer<symbolTree_t>> *tree = nullptr;
    switch (table)
    {
        case Local:
            tree = &m_LocalSymbols;
            break;
        case Global:
            tree = &m_GlobalSymbols;
            break;
        default:
            tree = &m_WeakSymbols;
            break;
    }

    auto symbolTree = tree->lookupRef(p, v);
    if (symbolTree)
    {
        return symbolTree.get();
    }

    auto newTree = SharedPointer<symbolTree_t>::allocate();
    auto result = newTree.get();
    tree->insert(p, pedigree_std::move(newTree));
    return result;
}
