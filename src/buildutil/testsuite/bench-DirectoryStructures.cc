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

class ElfHashedString
{
  public:
    ElfHashedString() : str_()
    {
    }

    ElfHashedString(const String *str) : str_(str), hash_(0)
    {
        hash_ = elf_hash(*str_);
    }

    uint32_t hash() const
    {
        return hash_;
    }

    bool operator==(const ElfHashedString &other) const
    {
        return *str_ == *other.str_;
    }

    bool operator!=(const ElfHashedString &other) const
    {
        return *str_ != *other.str_;
    }

  private:
    const String *str_;
    uint32_t hash_;
};

class JenkinsHashedString
{
  public:
    JenkinsHashedString() : str_()
    {
    }

    JenkinsHashedString(const String *str) : str_(str), hash_(0)
    {
        hash_ = jenkins_hash(*str_);
    }

    uint32_t hash() const
    {
        return hash_;
    }

    bool operator==(const JenkinsHashedString &other) const
    {
        return *str_ == *other.str_;
    }

    bool operator!=(const JenkinsHashedString &other) const
    {
        return *str_ != *other.str_;
    }

  private:
    const String *str_;
    uint32_t hash_;
};

class MurmurHashedString
{
  public:
    MurmurHashedString() : str_()
    {
    }

    MurmurHashedString(const String *str) : str_(str), hash_(0)
    {
        hash_ = murmur_hash(*str_);
    }

    uint32_t hash() const
    {
        return hash_;
    }

    bool operator==(const MurmurHashedString &other) const
    {
        return *str_ == *other.str_;
    }

    bool operator!=(const MurmurHashedString &other) const
    {
        return *str_ != *other.str_;
    }

  private:
    const String *str_;
    uint32_t hash_;
};

extern template class RadixTree<int64_t>;
extern template class HashTable<ElfHashedString, int64_t>;
extern template class HashTable<JenkinsHashedString, int64_t>;
extern template class HashTable<MurmurHashedString, int64_t>;

static void LoadDirents(std::vector<String> &result)
{
    size_t i = 0;

    std::ifstream ifs("src/buildutil/testsuite/data/dirents.dat");
    while (ifs.good())
    {
        std::string s;
        std::getline(ifs, s);

        String word(s.c_str());
        result.push_back(pedigree_std::move(word));
    }
}

template <class T>
static void CreateKeys(const std::vector<String> &dirents, std::vector<T> &keys)
{
    for (auto &key : dirents)
    {
        keys.push_back(T(&key));
    }
}

static void BM_DirentsLookup_RadixTree(benchmark::State &state)
{
    std::vector<String> symbols;
    int64_t value = 1;

    LoadDirents(symbols);

    RadixTree<int64_t> map;
    for (auto &key : symbols)
    {
        map.insert(key, value);
    }

    auto it = symbols.begin();
    while (state.KeepRunning())
    {
        auto &k = *it;
        benchmark::DoNotOptimize(map.lookup(k));
        if (++it == symbols.end())
        {
            it = symbols.begin();
        }
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
}

static void BM_DirentsLookup_ElfHash(benchmark::State &state)
{
    std::vector<String> symbols;
    std::vector<ElfHashedString> keys;
    int64_t value = 1;

    LoadDirents(symbols);
    CreateKeys(symbols, keys);

    HashTable<ElfHashedString, int64_t> map;
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

static void BM_DirentsLookup_JenkinsHash(benchmark::State &state)
{
    std::vector<String> symbols;
    std::vector<JenkinsHashedString> keys;
    int64_t value = 1;

    LoadDirents(symbols);
    CreateKeys(symbols, keys);

    HashTable<JenkinsHashedString, int64_t> map;
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

static void BM_DirentsLookup_MurmurHash(benchmark::State &state)
{
    std::vector<String> symbols;
    std::vector<MurmurHashedString> keys;
    int64_t value = 1;

    LoadDirents(symbols);
    CreateKeys(symbols, keys);

    HashTable<MurmurHashedString, int64_t> map;
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

BENCHMARK(BM_DirentsLookup_RadixTree);
BENCHMARK(BM_DirentsLookup_ElfHash);
BENCHMARK(BM_DirentsLookup_JenkinsHash);
BENCHMARK(BM_DirentsLookup_MurmurHash);

// Implementations after all usages of HashTable and RadixTree so we get
// single emitted versions, rather than inline versions.
template class RadixTree<int64_t>;
template class HashTable<ElfHashedString, int64_t>;
template class HashTable<JenkinsHashedString, int64_t>;
template class HashTable<MurmurHashedString, int64_t>;
