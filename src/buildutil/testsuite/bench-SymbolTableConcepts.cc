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

#include "pedigree/kernel/utilities/RadixTree.h"
#include "pedigree/kernel/utilities/HashTable.h"
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
    MurmurHash3_x86_32(static_cast<const char *>(str), str.length(), 0, &output);
    return output;
}

class ElfHashedSymbol
{
  public:
    ElfHashedSymbol() : str_()
    {
    }

    ElfHashedSymbol(const String &str) : str_(str), hash_(0)
    {
        hash_ = elf_hash(str_);
    }

    uint32_t hash() const
    {
        return hash_;
    }

    bool operator==(const ElfHashedSymbol &other) const
    {
        return str_ == other.str_;
    }

    bool operator!=(const ElfHashedSymbol &other) const
    {
        return str_ != other.str_;
    }

  private:
    String str_;
    uint32_t hash_;
};

class JenkinsHashedSymbol
{
  public:
    JenkinsHashedSymbol() : str_()
    {
    }

    JenkinsHashedSymbol(const String &str) : str_(str), hash_(0)
    {
        hash_ = jenkins_hash(str_);
    }

    uint32_t hash() const
    {
        return hash_;
    }

    bool operator==(const JenkinsHashedSymbol &other) const
    {
        return str_ == other.str_;
    }

    bool operator!=(const JenkinsHashedSymbol &other) const
    {
        return str_ != other.str_;
    }

  private:
    String str_;
    uint32_t hash_;
};

class MurmurHashedSymbol
{
  public:
    MurmurHashedSymbol() : str_()
    {
    }

    MurmurHashedSymbol(const String &str) : str_(str), hash_(0)
    {
        hash_ = murmur_hash(str_);
    }

    uint32_t hash() const
    {
        return hash_;
    }

    bool operator==(const MurmurHashedSymbol &other) const
    {
        return str_ == other.str_;
    }

    bool operator!=(const MurmurHashedSymbol &other) const
    {
        return str_ != other.str_;
    }

  private:
    String str_;
    uint32_t hash_;
};

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

static void BM_SymbolsInsert_ElfHash(benchmark::State &state)
{
    std::vector<String> symbols;
    int64_t value = 1;

    LoadSymbols(symbols);

    HashTable<ElfHashedSymbol, int64_t, 0x10000> map;
    while (state.KeepRunning())
    {
        state.PauseTiming();
        map.clear();
        state.ResumeTiming();

        for (auto &word : symbols)
        {
            map.insert(ElfHashedSymbol(word), value);
        }
    }

    state.SetItemsProcessed(
        int64_t(state.iterations()) * int64_t(symbols.size()));
}

static void BM_SymbolsInsert_JenkinsHash(benchmark::State &state)
{
    std::vector<String> symbols;
    int64_t value = 1;

    LoadSymbols(symbols);

    HashTable<JenkinsHashedSymbol, int64_t, 0x10000> map;
    while (state.KeepRunning())
    {
        state.PauseTiming();
        map.clear();
        state.ResumeTiming();

        for (auto &word : symbols)
        {
            map.insert(JenkinsHashedSymbol(word), value);
        }
    }

    state.SetItemsProcessed(
        int64_t(state.iterations()) * int64_t(symbols.size()));
}

static void BM_SymbolsInsert_MurmurHash(benchmark::State &state)
{
    std::vector<String> symbols;
    int64_t value = 1;

    LoadSymbols(symbols);

    HashTable<MurmurHashedSymbol, int64_t, 0x10000> map;
    while (state.KeepRunning())
    {
        state.PauseTiming();
        map.clear();
        state.ResumeTiming();

        for (auto &word : symbols)
        {
            map.insert(MurmurHashedSymbol(word), value);
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

static void BM_SymbolsLookup_ElfHash(benchmark::State &state)
{
    std::vector<String> symbols;
    int64_t value = 1;

    LoadSymbols(symbols);

    HashTable<ElfHashedSymbol, int64_t, 0x10000> map;
    for (auto word : symbols)
    {
        map.insert(ElfHashedSymbol(word), value);
    }

    auto it = symbols.begin();
    while (state.KeepRunning())
    {
        auto &word = *it;
        benchmark::DoNotOptimize(map.lookup(ElfHashedSymbol(word)));
        if (++it == symbols.end())
        {
            it = symbols.begin();
        }
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
}

static void BM_SymbolsLookup_JenkinsHash(benchmark::State &state)
{
    std::vector<String> symbols;
    int64_t value = 1;

    LoadSymbols(symbols);

    HashTable<JenkinsHashedSymbol, int64_t, 0x10000> map;
    for (auto word : symbols)
    {
        map.insert(JenkinsHashedSymbol(word), value);
    }

    auto it = symbols.begin();
    while (state.KeepRunning())
    {
        auto &word = *it;
        benchmark::DoNotOptimize(map.lookup(JenkinsHashedSymbol(word)));
        if (++it == symbols.end())
        {
            it = symbols.begin();
        }
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
}

static void BM_SymbolsLookup_MurmurHash(benchmark::State &state)
{
    std::vector<String> symbols;
    int64_t value = 1;

    LoadSymbols(symbols);

    HashTable<MurmurHashedSymbol, int64_t, 0x10000> map;
    for (auto word : symbols)
    {
        map.insert(MurmurHashedSymbol(word), value);
    }

    auto it = symbols.begin();
    while (state.KeepRunning())
    {
        auto &word = *it;
        benchmark::DoNotOptimize(map.lookup(MurmurHashedSymbol(word)));
        if (++it == symbols.end())
        {
            it = symbols.begin();
        }
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
}

BENCHMARK(BM_SymbolsInsert_RadixTree);
BENCHMARK(BM_SymbolsInsert_ElfHash);
BENCHMARK(BM_SymbolsInsert_JenkinsHash);
BENCHMARK(BM_SymbolsInsert_MurmurHash);
BENCHMARK(BM_SymbolsLookup_RadixTree);
BENCHMARK(BM_SymbolsLookup_ElfHash);
BENCHMARK(BM_SymbolsLookup_JenkinsHash);
BENCHMARK(BM_SymbolsLookup_MurmurHash);
