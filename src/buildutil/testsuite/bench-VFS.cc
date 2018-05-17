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

#include <string.h>

#include <benchmark/benchmark.h>

#include "modules/system/vfs/VFS.h"
#include "modules/system/ramfs/RamFs.h"

#define DEEP_PATH "ramfs»/foo/foo/foo/foo"
#define SHALLOW_PATH "ramfs»/"
#define MIDDLE_PATH "ramfs»/foo/foo"

static void prepareVFS(VFS &vfs)
{
    RamFs *ramfs = new RamFs();
    ramfs->initialise(nullptr);

    vfs.addAlias(ramfs, String("ramfs"));

    // Add a bunch of directories for lookups
    const char *paths[] = {
        "ramfs»/foo",
        "ramfs»/bar",
        "ramfs»/baz",
        "ramfs»/foo/foo",
        "ramfs»/foo/bar",
        "ramfs»/foo/baz",
        "ramfs»/foo/foo/foo",
        "ramfs»/foo/bar/bar",
        "ramfs»/foo/baz/baz",
        "ramfs»/foo/foo/foo/foo",
        "ramfs»/foo/bar/bar/bar",
        "ramfs»/foo/baz/baz/baz",
    };

    for (auto p : paths)
    {
        vfs.createDirectory(p, 0777);
    }
}

static void BM_VFSShallowDirectoryTraverse(benchmark::State &state)
{
    VFS vfs;
    prepareVFS(vfs);

    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(vfs.find(SHALLOW_PATH));
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
}

static void BM_VFSMediumDirectoryTraverse(benchmark::State &state)
{
    VFS vfs;
    prepareVFS(vfs);

    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(vfs.find(MIDDLE_PATH));
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
}

static void BM_VFSDeepDirectoryTraverse(benchmark::State &state)
{
    VFS vfs;
    prepareVFS(vfs);

    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(vfs.find(DEEP_PATH));
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
}

BENCHMARK(BM_VFSDeepDirectoryTraverse);
BENCHMARK(BM_VFSMediumDirectoryTraverse);
BENCHMARK(BM_VFSShallowDirectoryTraverse);
