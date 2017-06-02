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

#include <fstream>
#include <vector>

#include <benchmark/benchmark.h>

#include "pedigree/kernel/utilities/RadixTree.h"

#define RANDOM_MAX 0x1000000

static const int RandomNumber(const int maximum = RANDOM_MAX)
{
    static bool seeded = false;
    if (!seeded)
    {
        srand(time(0));
        seeded = true;
    }

    // Artificially limit the random number range so we get collisions.
    return rand() % maximum;
}

static void LoadWords(std::vector<String> &result)
{
    size_t i = 0;

    std::ifstream ifs("/usr/share/dict/words");
    while (ifs.good())
    {
        std::string s;
        std::getline(ifs, s);

        String word(s.c_str());
        result.push_back(word);
    }
}

static void BM_RadixTreeInsert(benchmark::State &state)
{
    std::vector<String> words;
    const int64_t value = 1;

    LoadWords(words);

    RadixTree<int64_t> tree;
    while (state.KeepRunning())
    {
        state.PauseTiming();
        tree.clear();
        state.ResumeTiming();

        for (auto word : words)
        {
            tree.insert(word, value);
        }
    }

    state.SetItemsProcessed(
        int64_t(state.iterations()) * int64_t(words.size()));
}

static void BM_RadixTreeInsertSame(benchmark::State &state)
{
    std::vector<String> words;
    const int64_t value = 1;

    LoadWords(words);

    RadixTree<int64_t> tree;
    while (state.KeepRunning())
    {
        tree.insert(words[0], value);
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
}

static void BM_RadixTreeLookupHit(benchmark::State &state)
{
    std::vector<String> words;
    const int64_t value = 1;

    LoadWords(words);

    RadixTree<int64_t> tree;
    for (auto word : words)
    {
        tree.insert(word, value);
    }

    String key;
    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(
            tree.lookup(words[RandomNumber(words.size())]));
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
}

static void BM_RadixTreeLookupMiss(benchmark::State &state)
{
    std::vector<String> words;
    const int64_t value = 1;

    LoadWords(words);

    RadixTree<int64_t> tree;
    for (auto word : words)
    {
        String copy = word;
        copy += "_";  // to never allow a match
        tree.insert(copy, value);
    }

    String key;
    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(
            tree.lookup(words[RandomNumber(words.size())]));
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
}

static void BM_RadixTreeCaseInsensitiveInsert(benchmark::State &state)
{
    std::vector<String> words;
    const int64_t value = 1;

    LoadWords(words);

    RadixTree<int64_t> tree(false);
    while (state.KeepRunning())
    {
        state.PauseTiming();
        tree.clear();
        state.ResumeTiming();

        for (auto word : words)
        {
            tree.insert(word, value);
        }
    }

    state.SetItemsProcessed(
        int64_t(state.iterations()) * int64_t(words.size()));
}

static void BM_RadixTreeCaseInsensitiveInsertSame(benchmark::State &state)
{
    std::vector<String> words;
    const int64_t value = 1;

    LoadWords(words);

    RadixTree<int64_t> tree(false);
    while (state.KeepRunning())
    {
        tree.insert(words[0], value);
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
}

static void BM_RadixTreeCaseInsensitiveLookupHit(benchmark::State &state)
{
    std::vector<String> words;
    const int64_t value = 1;

    LoadWords(words);

    RadixTree<int64_t> tree(false);
    for (auto word : words)
    {
        tree.insert(word, value);
    }

    String key;
    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(
            tree.lookup(words[RandomNumber(words.size())]));
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
}

static void BM_RadixTreeCaseInsensitiveLookupMiss(benchmark::State &state)
{
    std::vector<String> words;
    const int64_t value = 1;

    LoadWords(words);

    RadixTree<int64_t> tree(false);
    for (auto word : words)
    {
        String copy = word;
        copy += "_";  // to never allow a match
        tree.insert(copy, value);
    }

    String key;
    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(
            tree.lookup(words[RandomNumber(words.size())]));
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
}

/*
BENCHMARK(BM_RadixTreeInsert);
BENCHMARK(BM_RadixTreeInsertSame);
*/
BENCHMARK(BM_RadixTreeLookupHit);
/*
BENCHMARK(BM_RadixTreeLookupMiss);

BENCHMARK(BM_RadixTreeCaseInsensitiveInsert);
BENCHMARK(BM_RadixTreeCaseInsensitiveInsertSame);
*/
BENCHMARK(BM_RadixTreeCaseInsensitiveLookupHit);
/*
BENCHMARK(BM_RadixTreeCaseInsensitiveLookupMiss);
*/
