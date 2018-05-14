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

#define PEDIGREE_EXTERNAL_SOURCE 1

#include <stdlib.h>
#include <time.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <vector>

#include <benchmark/benchmark.h>

#include "pedigree/kernel/linker/SymbolTable.h"
#include "pedigree/kernel/utilities/HashTable.h"
#include "pedigree/kernel/utilities/RadixTree.h"
#include "pedigree/kernel/utilities/smhasher/MurmurHash3.h"

static uint32_t elf_hash(const String &str)
{
    return elfHash(static_cast<const char *>(str), str.length());
}

static uint32_t jenkins_hash(const String &str)
{
    return jenkinsHash(static_cast<const char *>(str), str.length());
}

static uint32_t murmur_hash(const String &str)
{
    uint32_t output = 0;
    MurmurHash3_x86_32(
        static_cast<const char *>(str), str.length(), 0, &output);
    return output;
}

class ElfHashedSymbol
{
  public:
    ElfHashedSymbol() : str_()
    {
    }

    ElfHashedSymbol(const String *str) : str_(str), hash_(0)
    {
        hash_ = elf_hash(*str_);
    }

    uint32_t hash() const
    {
        return hash_;
    }

    bool operator==(const ElfHashedSymbol &other) const
    {
        return *str_ == *other.str_;
    }

    bool operator!=(const ElfHashedSymbol &other) const
    {
        return *str_ != *other.str_;
    }

  private:
    const String *str_;
    uint32_t hash_;
};

class JenkinsHashedSymbol
{
  public:
    JenkinsHashedSymbol() : str_()
    {
    }

    JenkinsHashedSymbol(const String *str) : str_(str), hash_(0)
    {
        hash_ = jenkins_hash(*str_);
    }

    uint32_t hash() const
    {
        return hash_;
    }

    bool operator==(const JenkinsHashedSymbol &other) const
    {
        return *str_ == *other.str_;
    }

    bool operator!=(const JenkinsHashedSymbol &other) const
    {
        return *str_ != *other.str_;
    }

  private:
    const String *str_;
    uint32_t hash_;
};

extern template class RadixTree<int64_t>;
extern template class HashTable<ElfHashedSymbol, int64_t>;
extern template class HashTable<JenkinsHashedSymbol, int64_t>;

static void LoadSymbols(std::vector<String> &result)
{
    size_t i = 0;

    std::ifstream ifs("src/buildutil/testsuite/data/symbols.dat");
    while (ifs.good())
    {
        std::string s;
        std::getline(ifs, s);

        String word(s.c_str());
        result.push_back(word);
    }
}

template <class T>
static void CreateKeys(const std::vector<String> &symbols, std::vector<T> &keys)
{
    for (auto &word : symbols)
    {
        keys.push_back(T(&word));
    }
}

static void BM_SymbolsInsert_RadixTree(benchmark::State &state)
{
    std::vector<String> symbols;
    const int64_t value = 1;

    LoadSymbols(symbols);

    RadixTree<int64_t> tree;
    while (state.KeepRunning())
    {
        state.PauseTiming();
        tree.clear();
        state.ResumeTiming();

        for (auto &word : symbols)
        {
            tree.insert(word, value);
        }
    }

    state.SetItemsProcessed(
        int64_t(state.iterations()) * int64_t(symbols.size()));
}

static void BM_SymbolsInsert_Kernel(benchmark::State &state)
{
    SymbolTable *table = new SymbolTable(nullptr);
    std::vector<String> symbols;

    LoadSymbols(symbols);

    while (state.KeepRunning())
    {
        state.PauseTiming();
        delete table;
        table = new SymbolTable(nullptr);
        state.ResumeTiming();

        for (auto &word : symbols)
        {
            table->insert(word, SymbolTable::Local, nullptr, 0xdeadbeef);
        }
    }

    delete table;
    state.SetItemsProcessed(int64_t(state.iterations()));
}

static void BM_SymbolsInsert_KernelGlobal(benchmark::State &state)
{
    SymbolTable *table = new SymbolTable(nullptr);
    std::vector<String> symbols;

    LoadSymbols(symbols);

    while (state.KeepRunning())
    {
        state.PauseTiming();
        delete table;
        table = new SymbolTable(nullptr);
        state.ResumeTiming();

        for (auto &word : symbols)
        {
            table->insert(word, SymbolTable::Global, nullptr, 0xdeadbeef);
        }
    }

    delete table;
    state.SetItemsProcessed(int64_t(state.iterations()));
}

static void BM_SymbolsInsert_ElfHash(benchmark::State &state)
{
    std::vector<String> symbols;
    std::vector<ElfHashedSymbol> keys;
    int64_t value = 1;

    LoadSymbols(symbols);
    CreateKeys(symbols, keys);

    HashTable<ElfHashedSymbol, int64_t> map;
    while (state.KeepRunning())
    {
        state.PauseTiming();
        map.clear();
        state.ResumeTiming();

        for (auto &key : keys)
        {
            map.insert(key, value);
        }
    }

    state.SetItemsProcessed(
        int64_t(state.iterations()) * int64_t(symbols.size()));
}

static void BM_SymbolsInsert_JenkinsHash(benchmark::State &state)
{
    std::vector<String> symbols;
    std::vector<JenkinsHashedSymbol> keys;
    int64_t value = 1;

    LoadSymbols(symbols);
    CreateKeys(symbols, keys);

    HashTable<JenkinsHashedSymbol, int64_t> map;
    while (state.KeepRunning())
    {
        state.PauseTiming();
        map.clear();
        state.ResumeTiming();

        for (auto &key : keys)
        {
            map.insert(key, value);
        }
    }

    state.SetItemsProcessed(
        int64_t(state.iterations()) * int64_t(symbols.size()));
}

static void BM_SymbolsLookup_RadixTree(benchmark::State &state)
{
    std::vector<String> symbols;
    const int64_t value = 1;

    LoadSymbols(symbols);

    RadixTree<int64_t> tree;
    for (auto &word : symbols)
    {
        tree.insert(word, value);
    }

    auto it = symbols.begin();
    while (state.KeepRunning())
    {
        auto &word = *it;
        benchmark::DoNotOptimize(tree.lookup(word));
        if (++it == symbols.end())
        {
            it = symbols.begin();
        }
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
}

static void BM_SymbolsLookup_KernelLocal(benchmark::State &state)
{
    SymbolTable table(nullptr);
    std::vector<String> symbols;
    const int64_t value = 1;

    LoadSymbols(symbols);

    for (auto &word : symbols)
    {
        table.insert(word, SymbolTable::Local, nullptr, 0xdeadbeef);
    }

    auto it = symbols.begin();
    while (state.KeepRunning())
    {
        auto &word = *it;
        benchmark::DoNotOptimize(table.lookup(word, nullptr));
        if (++it == symbols.end())
        {
            it = symbols.begin();
        }
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
}

static void BM_SymbolsLookup_KernelGlobal(benchmark::State &state)
{
    SymbolTable table(nullptr);
    std::vector<String> symbols;
    const int64_t value = 1;

    LoadSymbols(symbols);

    for (auto &word : symbols)
    {
        table.insert(word, SymbolTable::Global, (Elf *) 1, 0xdeadbeef);
    }

    auto it = symbols.begin();
    while (state.KeepRunning())
    {
        auto &word = *it;
        benchmark::DoNotOptimize(table.lookup(word, nullptr));
        if (++it == symbols.end())
        {
            it = symbols.begin();
        }
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
}

static void BM_SymbolsLookup_ElfHash(benchmark::State &state)
{
    std::vector<String> symbols;
    std::vector<ElfHashedSymbol> keys;
    int64_t value = 1;

    LoadSymbols(symbols);
    CreateKeys(symbols, keys);

    HashTable<ElfHashedSymbol, int64_t> map;
    for (auto &key : keys)
    {
        map.insert(key, value);
    }

    auto it = keys.begin();
    while (state.KeepRunning())
    {
        auto &k = *it;
        benchmark::DoNotOptimize(map.lookup(k));
        if (++it == keys.end())
        {
            it = keys.begin();
        }
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
}

static void BM_SymbolsLookup_JenkinsHash(benchmark::State &state)
{
    std::vector<String> symbols;
    std::vector<JenkinsHashedSymbol> keys;
    int64_t value = 1;

    LoadSymbols(symbols);
    CreateKeys(symbols, keys);

    HashTable<JenkinsHashedSymbol, int64_t> map;
    for (auto &key : keys)
    {
        map.insert(key, value);
    }

    auto it = keys.begin();
    while (state.KeepRunning())
    {
        auto &k = *it;
        benchmark::DoNotOptimize(map.lookup(k));
        if (++it == keys.end())
        {
            it = keys.begin();
        }
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
}

BENCHMARK(BM_SymbolsInsert_RadixTree);
BENCHMARK(BM_SymbolsInsert_Kernel);
BENCHMARK(BM_SymbolsInsert_KernelGlobal);
BENCHMARK(BM_SymbolsInsert_ElfHash);
BENCHMARK(BM_SymbolsInsert_JenkinsHash);
BENCHMARK(BM_SymbolsLookup_RadixTree);
BENCHMARK(BM_SymbolsLookup_KernelLocal);
BENCHMARK(BM_SymbolsLookup_KernelGlobal);
BENCHMARK(BM_SymbolsLookup_ElfHash);
BENCHMARK(BM_SymbolsLookup_JenkinsHash);

// Implementations after all usages of HashTable and RadixTree so we get
// single emitted versions, rather than inline versions.
template class RadixTree<int64_t>;
template class HashTable<ElfHashedSymbol, int64_t>;
template class HashTable<JenkinsHashedSymbol, int64_t>;
