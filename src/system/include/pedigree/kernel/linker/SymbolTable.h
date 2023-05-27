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

#ifndef KERNEL_LINKER_SYMBOLTABLE_H
#define KERNEL_LINKER_SYMBOLTABLE_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/HashTable.h"
#include "pedigree/kernel/utilities/SharedPointer.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/StringView.h"
#include "pedigree/kernel/utilities/Tree.h"
#include "pedigree/kernel/utilities/utility.h"

class Elf;

/** This class allows quick access to symbol information held
 *  within ELF files. The lookup operation allows multiple
 *  policies to retrieve the wanted symbol.
 *
 *  \note Deletion is not implemented - the normal use case
 *        for this class is insertion and lookup. Deletion
 *        would almost never occur, and so the class is
 *        optimised solely for the first two operations. */
class SymbolTable
{
  public:
    /** Binding types, to define how symbols interact. */
    enum Binding
    {
        Local,
        Global,
        Weak
    };

    /** Lookup policies - given multiple definitions of a symbol,
     *  how do we determine the best response? */
    enum Policy
    {
        LocalFirst,  ///< Default policy - searches for local definitions of a
                     /// symbol first.
        NotOriginatingElf  ///< Does not search the ELF given as pElf. This is
                           /// used during lookups for
                           ///  R_COPY relocations, where one symbol must be
                           ///  linked to another.
    };

    /** Class constructor - creates an empty table. */
    SymbolTable(Elf *pElf);
    /** Destructor - destroys all information. */
    ~SymbolTable();

    /** Copy constructor. */
    SymbolTable(const SymbolTable &symtab);

    /** Copies the symbol table */
    void copyTable(Elf *pNewElf, const SymbolTable &newSymtab);

    /** Insert a symbol into the table. */
    void
    insert(const String &name, Binding binding, Elf *pParent, uintptr_t value);

    /** Insert a symbol into two SymbolTables, using the memory once. */
    void insertMultiple(
        SymbolTable *pOther, const String &name, Binding binding, Elf *pParent,
        uintptr_t value);

    /**  Preallocate at least the minimum space for the given symbol tables. */
    void preallocate(
        size_t numGlobal, size_t numWeak, Elf *localElf, size_t numLocal);
    /**
     * Preallocate additional symbols to the existing count.
     */
    void preallocateAdditional(
        size_t numGlobal, size_t numWeak, Elf *localElf, size_t numLocal);

    /** Has a preallocation already taken place on this SymbolTable? */
    bool hasPreallocated() const;

    void eraseByElf(Elf *pParent);

    /** Looks up a symbol in the table, optionally outputting the
     *  binding value.
     *
     *  If the policy is set as "LocalFirst" (the default), then
     *  Local and Global definitions from pElf are given
     *  priority.
     *
     *  If the policy is set as "NotOriginatingElf", no symbols
     *  in pElf will ever be matched, preferring those from other
     *  ELFs. This is used for R_COPY relocations.
     *
     *  \return The value of the found symbol. */
    uintptr_t EXPORTED_PUBLIC lookup(
        const HashedStringView &name, Elf *pElf, Policy policy = LocalFirst,
        Binding *pBinding = 0);

  private:
    /** Copy constructor.
        \note NOT implemented. */
    SymbolTable &operator=(const SymbolTable &);

    class Symbol
    {
      public:
        Symbol() : m_pParent(0), m_Binding(Global), m_Value(0)
        {
        }
        Symbol(Elf *pP, Binding b, uintptr_t v)
            : m_pParent(pP), m_Binding(b), m_Value(v)
        {
        }

        Elf *getParent() const
        {
            return m_pParent;
        }
        Binding getBinding() const
        {
            return m_Binding;
        }
        uintptr_t getValue() const
        {
            return m_Value;
        }

      private:
        Elf *m_pParent;
        Binding m_Binding;
        uintptr_t m_Value;
    };

    /** Insert doer. */
    SharedPointer<Symbol> doInsert(
        const String &name, Binding binding, Elf *pParent, uintptr_t value);
    /** Insert the given shared symbol. */
    void insertShared(const String &name, SharedPointer<Symbol> &symbol);

    typedef HashTable<String, SharedPointer<Symbol>, HashedStringView> symbolTree_t;
    typedef Tree<Elf *, SharedPointer<symbolTree_t>> parentedSymbolTree_t;

    /** Get or insert a Symbol tree. */
    symbolTree_t *getOrInsertTree(Elf *, Binding table = Local);

    parentedSymbolTree_t m_LocalSymbols;
    parentedSymbolTree_t m_GlobalSymbols;
    parentedSymbolTree_t m_WeakSymbols;

    Elf *m_pOriginatingElf;

    Mutex m_Lock;

    bool m_bPreallocated;
};

#endif
