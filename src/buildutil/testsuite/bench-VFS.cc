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
#include <string.h>
#include <time.h>

#include <memory>

#include <benchmark/benchmark.h>
#include <valgrind/callgrind.h>

#include "modules/system/ramfs/RamFs.h"
#include "modules/system/vfs/VFS.h"

static String g_DeepPath("ramfs»/foo/foo/foo/foo");
static String g_ShallowPath("ramfs»/");
static String g_MiddlePath("ramfs»/foo/foo");
static String g_DeepPathNoFs("/foo/foo/foo/foo");
static String g_ShallowPathNoFs("/");
static String g_MiddlePathNoFs("/foo/foo");
static String g_Alias("ramfs");

// A huge pile of paths to add to the filesystem for testing.
// Also used for randomly hitting the filesystem with lookups.
static String paths[] = {
    String("ramfs»/foo"),
    String("ramfs»/bar"),
    String("ramfs»/baz"),
    String("ramfs»/foo/foo"),
    String("ramfs»/foo/bar"),
    String("ramfs»/foo/baz"),
    String("ramfs»/bar/foo"),
    String("ramfs»/bar/bar"),
    String("ramfs»/bar/baz"),
    String("ramfs»/baz/foo"),
    String("ramfs»/baz/bar"),
    String("ramfs»/baz/baz"),
    String("ramfs»/foo/foo"),
    String("ramfs»/foo/bar"),
    String("ramfs»/foo/baz"),
    String("ramfs»/bar/foo"),
    String("ramfs»/bar/bar"),
    String("ramfs»/bar/baz"),
    String("ramfs»/baz/foo"),
    String("ramfs»/baz/bar"),
    String("ramfs»/baz/baz"),
    String("ramfs»/foo/foo/foo"),
    String("ramfs»/foo/foo/bar"),
    String("ramfs»/foo/foo/baz"),
    String("ramfs»/foo/bar/foo"),
    String("ramfs»/foo/bar/bar"),
    String("ramfs»/foo/bar/baz"),
    String("ramfs»/foo/baz/foo"),
    String("ramfs»/foo/baz/bar"),
    String("ramfs»/foo/baz/baz"),
    String("ramfs»/bar/foo/foo"),
    String("ramfs»/bar/foo/bar"),
    String("ramfs»/bar/foo/baz"),
    String("ramfs»/bar/bar/foo"),
    String("ramfs»/bar/bar/bar"),
    String("ramfs»/bar/bar/baz"),
    String("ramfs»/bar/baz/foo"),
    String("ramfs»/bar/baz/bar"),
    String("ramfs»/bar/baz/baz"),
    String("ramfs»/baz/foo/foo"),
    String("ramfs»/baz/foo/bar"),
    String("ramfs»/baz/foo/baz"),
    String("ramfs»/baz/bar/foo"),
    String("ramfs»/baz/bar/bar"),
    String("ramfs»/baz/bar/baz"),
    String("ramfs»/baz/baz/foo"),
    String("ramfs»/baz/baz/bar"),
    String("ramfs»/baz/baz/baz"),
    String("ramfs»/foo/foo"),
    String("ramfs»/foo/bar"),
    String("ramfs»/foo/baz"),
    String("ramfs»/bar/foo"),
    String("ramfs»/bar/bar"),
    String("ramfs»/bar/baz"),
    String("ramfs»/baz/foo"),
    String("ramfs»/baz/bar"),
    String("ramfs»/baz/baz"),
    String("ramfs»/foo/foo/foo"),
    String("ramfs»/foo/foo/bar"),
    String("ramfs»/foo/foo/baz"),
    String("ramfs»/foo/bar/foo"),
    String("ramfs»/foo/bar/bar"),
    String("ramfs»/foo/bar/baz"),
    String("ramfs»/foo/baz/foo"),
    String("ramfs»/foo/baz/bar"),
    String("ramfs»/foo/baz/baz"),
    String("ramfs»/bar/foo/foo"),
    String("ramfs»/bar/foo/bar"),
    String("ramfs»/bar/foo/baz"),
    String("ramfs»/bar/bar/foo"),
    String("ramfs»/bar/bar/bar"),
    String("ramfs»/bar/bar/baz"),
    String("ramfs»/bar/baz/foo"),
    String("ramfs»/bar/baz/bar"),
    String("ramfs»/bar/baz/baz"),
    String("ramfs»/baz/foo/foo"),
    String("ramfs»/baz/foo/bar"),
    String("ramfs»/baz/foo/baz"),
    String("ramfs»/baz/bar/foo"),
    String("ramfs»/baz/bar/bar"),
    String("ramfs»/baz/bar/baz"),
    String("ramfs»/baz/baz/foo"),
    String("ramfs»/baz/baz/bar"),
    String("ramfs»/baz/baz/baz"),
    String("ramfs»/foo/foo/foo"),
    String("ramfs»/foo/foo/bar"),
    String("ramfs»/foo/foo/baz"),
    String("ramfs»/foo/bar/foo"),
    String("ramfs»/foo/bar/bar"),
    String("ramfs»/foo/bar/baz"),
    String("ramfs»/foo/baz/foo"),
    String("ramfs»/foo/baz/bar"),
    String("ramfs»/foo/baz/baz"),
    String("ramfs»/bar/foo/foo"),
    String("ramfs»/bar/foo/bar"),
    String("ramfs»/bar/foo/baz"),
    String("ramfs»/bar/bar/foo"),
    String("ramfs»/bar/bar/bar"),
    String("ramfs»/bar/bar/baz"),
    String("ramfs»/bar/baz/foo"),
    String("ramfs»/bar/baz/bar"),
    String("ramfs»/bar/baz/baz"),
    String("ramfs»/baz/foo/foo"),
    String("ramfs»/baz/foo/bar"),
    String("ramfs»/baz/foo/baz"),
    String("ramfs»/baz/bar/foo"),
    String("ramfs»/baz/bar/bar"),
    String("ramfs»/baz/bar/baz"),
    String("ramfs»/baz/baz/foo"),
    String("ramfs»/baz/baz/bar"),
    String("ramfs»/baz/baz/baz"),
    String("ramfs»/foo/foo/foo/foo"),
    String("ramfs»/foo/foo/foo/bar"),
    String("ramfs»/foo/foo/foo/baz"),
    String("ramfs»/foo/foo/bar/foo"),
    String("ramfs»/foo/foo/bar/bar"),
    String("ramfs»/foo/foo/bar/baz"),
    String("ramfs»/foo/foo/baz/foo"),
    String("ramfs»/foo/foo/baz/bar"),
    String("ramfs»/foo/foo/baz/baz"),
    String("ramfs»/foo/bar/foo/foo"),
    String("ramfs»/foo/bar/foo/bar"),
    String("ramfs»/foo/bar/foo/baz"),
    String("ramfs»/foo/bar/bar/foo"),
    String("ramfs»/foo/bar/bar/bar"),
    String("ramfs»/foo/bar/bar/baz"),
    String("ramfs»/foo/bar/baz/foo"),
    String("ramfs»/foo/bar/baz/bar"),
    String("ramfs»/foo/bar/baz/baz"),
    String("ramfs»/foo/baz/foo/foo"),
    String("ramfs»/foo/baz/foo/bar"),
    String("ramfs»/foo/baz/foo/baz"),
    String("ramfs»/foo/baz/bar/foo"),
    String("ramfs»/foo/baz/bar/bar"),
    String("ramfs»/foo/baz/bar/baz"),
    String("ramfs»/foo/baz/baz/foo"),
    String("ramfs»/foo/baz/baz/bar"),
    String("ramfs»/foo/baz/baz/baz"),
    String("ramfs»/bar/foo/foo/foo"),
    String("ramfs»/bar/foo/foo/bar"),
    String("ramfs»/bar/foo/foo/baz"),
    String("ramfs»/bar/foo/bar/foo"),
    String("ramfs»/bar/foo/bar/bar"),
    String("ramfs»/bar/foo/bar/baz"),
    String("ramfs»/bar/foo/baz/foo"),
    String("ramfs»/bar/foo/baz/bar"),
    String("ramfs»/bar/foo/baz/baz"),
    String("ramfs»/bar/bar/foo/foo"),
    String("ramfs»/bar/bar/foo/bar"),
    String("ramfs»/bar/bar/foo/baz"),
    String("ramfs»/bar/bar/bar/foo"),
    String("ramfs»/bar/bar/bar/bar"),
    String("ramfs»/bar/bar/bar/baz"),
    String("ramfs»/bar/bar/baz/foo"),
    String("ramfs»/bar/bar/baz/bar"),
    String("ramfs»/bar/bar/baz/baz"),
    String("ramfs»/bar/baz/foo/foo"),
    String("ramfs»/bar/baz/foo/bar"),
    String("ramfs»/bar/baz/foo/baz"),
    String("ramfs»/bar/baz/bar/foo"),
    String("ramfs»/bar/baz/bar/bar"),
    String("ramfs»/bar/baz/bar/baz"),
    String("ramfs»/bar/baz/baz/foo"),
    String("ramfs»/bar/baz/baz/bar"),
    String("ramfs»/bar/baz/baz/baz"),
    String("ramfs»/baz/foo/foo/foo"),
    String("ramfs»/baz/foo/foo/bar"),
    String("ramfs»/baz/foo/foo/baz"),
    String("ramfs»/baz/foo/bar/foo"),
    String("ramfs»/baz/foo/bar/bar"),
    String("ramfs»/baz/foo/bar/baz"),
    String("ramfs»/baz/foo/baz/foo"),
    String("ramfs»/baz/foo/baz/bar"),
    String("ramfs»/baz/foo/baz/baz"),
    String("ramfs»/baz/bar/foo/foo"),
    String("ramfs»/baz/bar/foo/bar"),
    String("ramfs»/baz/bar/foo/baz"),
    String("ramfs»/baz/bar/bar/foo"),
    String("ramfs»/baz/bar/bar/bar"),
    String("ramfs»/baz/bar/bar/baz"),
    String("ramfs»/baz/bar/baz/foo"),
    String("ramfs»/baz/bar/baz/bar"),
    String("ramfs»/baz/bar/baz/baz"),
    String("ramfs»/baz/baz/foo/foo"),
    String("ramfs»/baz/baz/foo/bar"),
    String("ramfs»/baz/baz/foo/baz"),
    String("ramfs»/baz/baz/bar/foo"),
    String("ramfs»/baz/baz/bar/bar"),
    String("ramfs»/baz/baz/bar/baz"),
    String("ramfs»/baz/baz/baz/foo"),
    String("ramfs»/baz/baz/baz/bar"),
    String("ramfs»/baz/baz/baz/baz"),
    String("ramfs»/foo/foo"),
    String("ramfs»/foo/bar"),
    String("ramfs»/foo/baz"),
    String("ramfs»/bar/foo"),
    String("ramfs»/bar/bar"),
    String("ramfs»/bar/baz"),
    String("ramfs»/baz/foo"),
    String("ramfs»/baz/bar"),
    String("ramfs»/baz/baz"),
    String("ramfs»/foo/foo/foo"),
    String("ramfs»/foo/foo/bar"),
    String("ramfs»/foo/foo/baz"),
    String("ramfs»/foo/bar/foo"),
    String("ramfs»/foo/bar/bar"),
    String("ramfs»/foo/bar/baz"),
    String("ramfs»/foo/baz/foo"),
    String("ramfs»/foo/baz/bar"),
    String("ramfs»/foo/baz/baz"),
    String("ramfs»/bar/foo/foo"),
    String("ramfs»/bar/foo/bar"),
    String("ramfs»/bar/foo/baz"),
    String("ramfs»/bar/bar/foo"),
    String("ramfs»/bar/bar/bar"),
    String("ramfs»/bar/bar/baz"),
    String("ramfs»/bar/baz/foo"),
    String("ramfs»/bar/baz/bar"),
    String("ramfs»/bar/baz/baz"),
    String("ramfs»/baz/foo/foo"),
    String("ramfs»/baz/foo/bar"),
    String("ramfs»/baz/foo/baz"),
    String("ramfs»/baz/bar/foo"),
    String("ramfs»/baz/bar/bar"),
    String("ramfs»/baz/bar/baz"),
    String("ramfs»/baz/baz/foo"),
    String("ramfs»/baz/baz/bar"),
    String("ramfs»/baz/baz/baz"),
    String("ramfs»/foo/foo/foo"),
    String("ramfs»/foo/foo/bar"),
    String("ramfs»/foo/foo/baz"),
    String("ramfs»/foo/bar/foo"),
    String("ramfs»/foo/bar/bar"),
    String("ramfs»/foo/bar/baz"),
    String("ramfs»/foo/baz/foo"),
    String("ramfs»/foo/baz/bar"),
    String("ramfs»/foo/baz/baz"),
    String("ramfs»/bar/foo/foo"),
    String("ramfs»/bar/foo/bar"),
    String("ramfs»/bar/foo/baz"),
    String("ramfs»/bar/bar/foo"),
    String("ramfs»/bar/bar/bar"),
    String("ramfs»/bar/bar/baz"),
    String("ramfs»/bar/baz/foo"),
    String("ramfs»/bar/baz/bar"),
    String("ramfs»/bar/baz/baz"),
    String("ramfs»/baz/foo/foo"),
    String("ramfs»/baz/foo/bar"),
    String("ramfs»/baz/foo/baz"),
    String("ramfs»/baz/bar/foo"),
    String("ramfs»/baz/bar/bar"),
    String("ramfs»/baz/bar/baz"),
    String("ramfs»/baz/baz/foo"),
    String("ramfs»/baz/baz/bar"),
    String("ramfs»/baz/baz/baz"),
    String("ramfs»/foo/foo/foo/foo"),
    String("ramfs»/foo/foo/foo/bar"),
    String("ramfs»/foo/foo/foo/baz"),
    String("ramfs»/foo/foo/bar/foo"),
    String("ramfs»/foo/foo/bar/bar"),
    String("ramfs»/foo/foo/bar/baz"),
    String("ramfs»/foo/foo/baz/foo"),
    String("ramfs»/foo/foo/baz/bar"),
    String("ramfs»/foo/foo/baz/baz"),
    String("ramfs»/foo/bar/foo/foo"),
    String("ramfs»/foo/bar/foo/bar"),
    String("ramfs»/foo/bar/foo/baz"),
    String("ramfs»/foo/bar/bar/foo"),
    String("ramfs»/foo/bar/bar/bar"),
    String("ramfs»/foo/bar/bar/baz"),
    String("ramfs»/foo/bar/baz/foo"),
    String("ramfs»/foo/bar/baz/bar"),
    String("ramfs»/foo/bar/baz/baz"),
    String("ramfs»/foo/baz/foo/foo"),
    String("ramfs»/foo/baz/foo/bar"),
    String("ramfs»/foo/baz/foo/baz"),
    String("ramfs»/foo/baz/bar/foo"),
    String("ramfs»/foo/baz/bar/bar"),
    String("ramfs»/foo/baz/bar/baz"),
    String("ramfs»/foo/baz/baz/foo"),
    String("ramfs»/foo/baz/baz/bar"),
    String("ramfs»/foo/baz/baz/baz"),
    String("ramfs»/bar/foo/foo/foo"),
    String("ramfs»/bar/foo/foo/bar"),
    String("ramfs»/bar/foo/foo/baz"),
    String("ramfs»/bar/foo/bar/foo"),
    String("ramfs»/bar/foo/bar/bar"),
    String("ramfs»/bar/foo/bar/baz"),
    String("ramfs»/bar/foo/baz/foo"),
    String("ramfs»/bar/foo/baz/bar"),
    String("ramfs»/bar/foo/baz/baz"),
    String("ramfs»/bar/bar/foo/foo"),
    String("ramfs»/bar/bar/foo/bar"),
    String("ramfs»/bar/bar/foo/baz"),
    String("ramfs»/bar/bar/bar/foo"),
    String("ramfs»/bar/bar/bar/bar"),
    String("ramfs»/bar/bar/bar/baz"),
    String("ramfs»/bar/bar/baz/foo"),
    String("ramfs»/bar/bar/baz/bar"),
    String("ramfs»/bar/bar/baz/baz"),
    String("ramfs»/bar/baz/foo/foo"),
    String("ramfs»/bar/baz/foo/bar"),
    String("ramfs»/bar/baz/foo/baz"),
    String("ramfs»/bar/baz/bar/foo"),
    String("ramfs»/bar/baz/bar/bar"),
    String("ramfs»/bar/baz/bar/baz"),
    String("ramfs»/bar/baz/baz/foo"),
    String("ramfs»/bar/baz/baz/bar"),
    String("ramfs»/bar/baz/baz/baz"),
    String("ramfs»/baz/foo/foo/foo"),
    String("ramfs»/baz/foo/foo/bar"),
    String("ramfs»/baz/foo/foo/baz"),
    String("ramfs»/baz/foo/bar/foo"),
    String("ramfs»/baz/foo/bar/bar"),
    String("ramfs»/baz/foo/bar/baz"),
    String("ramfs»/baz/foo/baz/foo"),
    String("ramfs»/baz/foo/baz/bar"),
    String("ramfs»/baz/foo/baz/baz"),
    String("ramfs»/baz/bar/foo/foo"),
    String("ramfs»/baz/bar/foo/bar"),
    String("ramfs»/baz/bar/foo/baz"),
    String("ramfs»/baz/bar/bar/foo"),
    String("ramfs»/baz/bar/bar/bar"),
    String("ramfs»/baz/bar/bar/baz"),
    String("ramfs»/baz/bar/baz/foo"),
    String("ramfs»/baz/bar/baz/bar"),
    String("ramfs»/baz/bar/baz/baz"),
    String("ramfs»/baz/baz/foo/foo"),
    String("ramfs»/baz/baz/foo/bar"),
    String("ramfs»/baz/baz/foo/baz"),
    String("ramfs»/baz/baz/bar/foo"),
    String("ramfs»/baz/baz/bar/bar"),
    String("ramfs»/baz/baz/bar/baz"),
    String("ramfs»/baz/baz/baz/foo"),
    String("ramfs»/baz/baz/baz/bar"),
    String("ramfs»/baz/baz/baz/baz"),
    String("ramfs»/foo/foo/foo"),
    String("ramfs»/foo/foo/bar"),
    String("ramfs»/foo/foo/baz"),
    String("ramfs»/foo/bar/foo"),
    String("ramfs»/foo/bar/bar"),
    String("ramfs»/foo/bar/baz"),
    String("ramfs»/foo/baz/foo"),
    String("ramfs»/foo/baz/bar"),
    String("ramfs»/foo/baz/baz"),
    String("ramfs»/bar/foo/foo"),
    String("ramfs»/bar/foo/bar"),
    String("ramfs»/bar/foo/baz"),
    String("ramfs»/bar/bar/foo"),
    String("ramfs»/bar/bar/bar"),
    String("ramfs»/bar/bar/baz"),
    String("ramfs»/bar/baz/foo"),
    String("ramfs»/bar/baz/bar"),
    String("ramfs»/bar/baz/baz"),
    String("ramfs»/baz/foo/foo"),
    String("ramfs»/baz/foo/bar"),
    String("ramfs»/baz/foo/baz"),
    String("ramfs»/baz/bar/foo"),
    String("ramfs»/baz/bar/bar"),
    String("ramfs»/baz/bar/baz"),
    String("ramfs»/baz/baz/foo"),
    String("ramfs»/baz/baz/bar"),
    String("ramfs»/baz/baz/baz"),
    String("ramfs»/foo/foo/foo/foo"),
    String("ramfs»/foo/foo/foo/bar"),
    String("ramfs»/foo/foo/foo/baz"),
    String("ramfs»/foo/foo/bar/foo"),
    String("ramfs»/foo/foo/bar/bar"),
    String("ramfs»/foo/foo/bar/baz"),
    String("ramfs»/foo/foo/baz/foo"),
    String("ramfs»/foo/foo/baz/bar"),
    String("ramfs»/foo/foo/baz/baz"),
    String("ramfs»/foo/bar/foo/foo"),
    String("ramfs»/foo/bar/foo/bar"),
    String("ramfs»/foo/bar/foo/baz"),
    String("ramfs»/foo/bar/bar/foo"),
    String("ramfs»/foo/bar/bar/bar"),
    String("ramfs»/foo/bar/bar/baz"),
    String("ramfs»/foo/bar/baz/foo"),
    String("ramfs»/foo/bar/baz/bar"),
    String("ramfs»/foo/bar/baz/baz"),
    String("ramfs»/foo/baz/foo/foo"),
    String("ramfs»/foo/baz/foo/bar"),
    String("ramfs»/foo/baz/foo/baz"),
    String("ramfs»/foo/baz/bar/foo"),
    String("ramfs»/foo/baz/bar/bar"),
    String("ramfs»/foo/baz/bar/baz"),
    String("ramfs»/foo/baz/baz/foo"),
    String("ramfs»/foo/baz/baz/bar"),
    String("ramfs»/foo/baz/baz/baz"),
    String("ramfs»/bar/foo/foo/foo"),
    String("ramfs»/bar/foo/foo/bar"),
    String("ramfs»/bar/foo/foo/baz"),
    String("ramfs»/bar/foo/bar/foo"),
    String("ramfs»/bar/foo/bar/bar"),
    String("ramfs»/bar/foo/bar/baz"),
    String("ramfs»/bar/foo/baz/foo"),
    String("ramfs»/bar/foo/baz/bar"),
    String("ramfs»/bar/foo/baz/baz"),
    String("ramfs»/bar/bar/foo/foo"),
    String("ramfs»/bar/bar/foo/bar"),
    String("ramfs»/bar/bar/foo/baz"),
    String("ramfs»/bar/bar/bar/foo"),
    String("ramfs»/bar/bar/bar/bar"),
    String("ramfs»/bar/bar/bar/baz"),
    String("ramfs»/bar/bar/baz/foo"),
    String("ramfs»/bar/bar/baz/bar"),
    String("ramfs»/bar/bar/baz/baz"),
    String("ramfs»/bar/baz/foo/foo"),
    String("ramfs»/bar/baz/foo/bar"),
    String("ramfs»/bar/baz/foo/baz"),
    String("ramfs»/bar/baz/bar/foo"),
    String("ramfs»/bar/baz/bar/bar"),
    String("ramfs»/bar/baz/bar/baz"),
    String("ramfs»/bar/baz/baz/foo"),
    String("ramfs»/bar/baz/baz/bar"),
    String("ramfs»/bar/baz/baz/baz"),
    String("ramfs»/baz/foo/foo/foo"),
    String("ramfs»/baz/foo/foo/bar"),
    String("ramfs»/baz/foo/foo/baz"),
    String("ramfs»/baz/foo/bar/foo"),
    String("ramfs»/baz/foo/bar/bar"),
    String("ramfs»/baz/foo/bar/baz"),
    String("ramfs»/baz/foo/baz/foo"),
    String("ramfs»/baz/foo/baz/bar"),
    String("ramfs»/baz/foo/baz/baz"),
    String("ramfs»/baz/bar/foo/foo"),
    String("ramfs»/baz/bar/foo/bar"),
    String("ramfs»/baz/bar/foo/baz"),
    String("ramfs»/baz/bar/bar/foo"),
    String("ramfs»/baz/bar/bar/bar"),
    String("ramfs»/baz/bar/bar/baz"),
    String("ramfs»/baz/bar/baz/foo"),
    String("ramfs»/baz/bar/baz/bar"),
    String("ramfs»/baz/bar/baz/baz"),
    String("ramfs»/baz/baz/foo/foo"),
    String("ramfs»/baz/baz/foo/bar"),
    String("ramfs»/baz/baz/foo/baz"),
    String("ramfs»/baz/baz/bar/foo"),
    String("ramfs»/baz/baz/bar/bar"),
    String("ramfs»/baz/baz/bar/baz"),
    String("ramfs»/baz/baz/baz/foo"),
    String("ramfs»/baz/baz/baz/bar"),
    String("ramfs»/baz/baz/baz/baz"),
    String("ramfs»/foo/foo/foo/foo"),
    String("ramfs»/foo/foo/foo/bar"),
    String("ramfs»/foo/foo/foo/baz"),
    String("ramfs»/foo/foo/bar/foo"),
    String("ramfs»/foo/foo/bar/bar"),
    String("ramfs»/foo/foo/bar/baz"),
    String("ramfs»/foo/foo/baz/foo"),
    String("ramfs»/foo/foo/baz/bar"),
    String("ramfs»/foo/foo/baz/baz"),
    String("ramfs»/foo/bar/foo/foo"),
    String("ramfs»/foo/bar/foo/bar"),
    String("ramfs»/foo/bar/foo/baz"),
    String("ramfs»/foo/bar/bar/foo"),
    String("ramfs»/foo/bar/bar/bar"),
    String("ramfs»/foo/bar/bar/baz"),
    String("ramfs»/foo/bar/baz/foo"),
    String("ramfs»/foo/bar/baz/bar"),
    String("ramfs»/foo/bar/baz/baz"),
    String("ramfs»/foo/baz/foo/foo"),
    String("ramfs»/foo/baz/foo/bar"),
    String("ramfs»/foo/baz/foo/baz"),
    String("ramfs»/foo/baz/bar/foo"),
    String("ramfs»/foo/baz/bar/bar"),
    String("ramfs»/foo/baz/bar/baz"),
    String("ramfs»/foo/baz/baz/foo"),
    String("ramfs»/foo/baz/baz/bar"),
    String("ramfs»/foo/baz/baz/baz"),
    String("ramfs»/bar/foo/foo/foo"),
    String("ramfs»/bar/foo/foo/bar"),
    String("ramfs»/bar/foo/foo/baz"),
    String("ramfs»/bar/foo/bar/foo"),
    String("ramfs»/bar/foo/bar/bar"),
    String("ramfs»/bar/foo/bar/baz"),
    String("ramfs»/bar/foo/baz/foo"),
    String("ramfs»/bar/foo/baz/bar"),
    String("ramfs»/bar/foo/baz/baz"),
    String("ramfs»/bar/bar/foo/foo"),
    String("ramfs»/bar/bar/foo/bar"),
    String("ramfs»/bar/bar/foo/baz"),
    String("ramfs»/bar/bar/bar/foo"),
    String("ramfs»/bar/bar/bar/bar"),
    String("ramfs»/bar/bar/bar/baz"),
    String("ramfs»/bar/bar/baz/foo"),
    String("ramfs»/bar/bar/baz/bar"),
    String("ramfs»/bar/bar/baz/baz"),
    String("ramfs»/bar/baz/foo/foo"),
    String("ramfs»/bar/baz/foo/bar"),
    String("ramfs»/bar/baz/foo/baz"),
    String("ramfs»/bar/baz/bar/foo"),
    String("ramfs»/bar/baz/bar/bar"),
    String("ramfs»/bar/baz/bar/baz"),
    String("ramfs»/bar/baz/baz/foo"),
    String("ramfs»/bar/baz/baz/bar"),
    String("ramfs»/bar/baz/baz/baz"),
    String("ramfs»/baz/foo/foo/foo"),
    String("ramfs»/baz/foo/foo/bar"),
    String("ramfs»/baz/foo/foo/baz"),
    String("ramfs»/baz/foo/bar/foo"),
    String("ramfs»/baz/foo/bar/bar"),
    String("ramfs»/baz/foo/bar/baz"),
    String("ramfs»/baz/foo/baz/foo"),
    String("ramfs»/baz/foo/baz/bar"),
    String("ramfs»/baz/foo/baz/baz"),
    String("ramfs»/baz/bar/foo/foo"),
    String("ramfs»/baz/bar/foo/bar"),
    String("ramfs»/baz/bar/foo/baz"),
    String("ramfs»/baz/bar/bar/foo"),
    String("ramfs»/baz/bar/bar/bar"),
    String("ramfs»/baz/bar/bar/baz"),
    String("ramfs»/baz/bar/baz/foo"),
    String("ramfs»/baz/bar/baz/bar"),
    String("ramfs»/baz/bar/baz/baz"),
    String("ramfs»/baz/baz/foo/foo"),
    String("ramfs»/baz/baz/foo/bar"),
    String("ramfs»/baz/baz/foo/baz"),
    String("ramfs»/baz/baz/bar/foo"),
    String("ramfs»/baz/baz/bar/bar"),
    String("ramfs»/baz/baz/bar/baz"),
    String("ramfs»/baz/baz/baz/foo"),
    String("ramfs»/baz/baz/baz/bar"),
    String("ramfs»/baz/baz/baz/baz"),
    String("ramfs»/foo/foo/foo/foo/foo"),
    String("ramfs»/foo/foo/foo/foo/bar"),
    String("ramfs»/foo/foo/foo/foo/baz"),
    String("ramfs»/foo/foo/foo/bar/foo"),
    String("ramfs»/foo/foo/foo/bar/bar"),
    String("ramfs»/foo/foo/foo/bar/baz"),
    String("ramfs»/foo/foo/foo/baz/foo"),
    String("ramfs»/foo/foo/foo/baz/bar"),
    String("ramfs»/foo/foo/foo/baz/baz"),
    String("ramfs»/foo/foo/bar/foo/foo"),
    String("ramfs»/foo/foo/bar/foo/bar"),
    String("ramfs»/foo/foo/bar/foo/baz"),
    String("ramfs»/foo/foo/bar/bar/foo"),
    String("ramfs»/foo/foo/bar/bar/bar"),
    String("ramfs»/foo/foo/bar/bar/baz"),
    String("ramfs»/foo/foo/bar/baz/foo"),
    String("ramfs»/foo/foo/bar/baz/bar"),
    String("ramfs»/foo/foo/bar/baz/baz"),
    String("ramfs»/foo/foo/baz/foo/foo"),
    String("ramfs»/foo/foo/baz/foo/bar"),
    String("ramfs»/foo/foo/baz/foo/baz"),
    String("ramfs»/foo/foo/baz/bar/foo"),
    String("ramfs»/foo/foo/baz/bar/bar"),
    String("ramfs»/foo/foo/baz/bar/baz"),
    String("ramfs»/foo/foo/baz/baz/foo"),
    String("ramfs»/foo/foo/baz/baz/bar"),
    String("ramfs»/foo/foo/baz/baz/baz"),
    String("ramfs»/foo/bar/foo/foo/foo"),
    String("ramfs»/foo/bar/foo/foo/bar"),
    String("ramfs»/foo/bar/foo/foo/baz"),
    String("ramfs»/foo/bar/foo/bar/foo"),
    String("ramfs»/foo/bar/foo/bar/bar"),
    String("ramfs»/foo/bar/foo/bar/baz"),
    String("ramfs»/foo/bar/foo/baz/foo"),
    String("ramfs»/foo/bar/foo/baz/bar"),
    String("ramfs»/foo/bar/foo/baz/baz"),
    String("ramfs»/foo/bar/bar/foo/foo"),
    String("ramfs»/foo/bar/bar/foo/bar"),
    String("ramfs»/foo/bar/bar/foo/baz"),
    String("ramfs»/foo/bar/bar/bar/foo"),
    String("ramfs»/foo/bar/bar/bar/bar"),
    String("ramfs»/foo/bar/bar/bar/baz"),
    String("ramfs»/foo/bar/bar/baz/foo"),
    String("ramfs»/foo/bar/bar/baz/bar"),
    String("ramfs»/foo/bar/bar/baz/baz"),
    String("ramfs»/foo/bar/baz/foo/foo"),
    String("ramfs»/foo/bar/baz/foo/bar"),
    String("ramfs»/foo/bar/baz/foo/baz"),
    String("ramfs»/foo/bar/baz/bar/foo"),
    String("ramfs»/foo/bar/baz/bar/bar"),
    String("ramfs»/foo/bar/baz/bar/baz"),
    String("ramfs»/foo/bar/baz/baz/foo"),
    String("ramfs»/foo/bar/baz/baz/bar"),
    String("ramfs»/foo/bar/baz/baz/baz"),
    String("ramfs»/foo/baz/foo/foo/foo"),
    String("ramfs»/foo/baz/foo/foo/bar"),
    String("ramfs»/foo/baz/foo/foo/baz"),
    String("ramfs»/foo/baz/foo/bar/foo"),
    String("ramfs»/foo/baz/foo/bar/bar"),
    String("ramfs»/foo/baz/foo/bar/baz"),
    String("ramfs»/foo/baz/foo/baz/foo"),
    String("ramfs»/foo/baz/foo/baz/bar"),
    String("ramfs»/foo/baz/foo/baz/baz"),
    String("ramfs»/foo/baz/bar/foo/foo"),
    String("ramfs»/foo/baz/bar/foo/bar"),
    String("ramfs»/foo/baz/bar/foo/baz"),
    String("ramfs»/foo/baz/bar/bar/foo"),
    String("ramfs»/foo/baz/bar/bar/bar"),
    String("ramfs»/foo/baz/bar/bar/baz"),
    String("ramfs»/foo/baz/bar/baz/foo"),
    String("ramfs»/foo/baz/bar/baz/bar"),
    String("ramfs»/foo/baz/bar/baz/baz"),
    String("ramfs»/foo/baz/baz/foo/foo"),
    String("ramfs»/foo/baz/baz/foo/bar"),
    String("ramfs»/foo/baz/baz/foo/baz"),
    String("ramfs»/foo/baz/baz/bar/foo"),
    String("ramfs»/foo/baz/baz/bar/bar"),
    String("ramfs»/foo/baz/baz/bar/baz"),
    String("ramfs»/foo/baz/baz/baz/foo"),
    String("ramfs»/foo/baz/baz/baz/bar"),
    String("ramfs»/foo/baz/baz/baz/baz"),
    String("ramfs»/bar/foo/foo/foo/foo"),
    String("ramfs»/bar/foo/foo/foo/bar"),
    String("ramfs»/bar/foo/foo/foo/baz"),
    String("ramfs»/bar/foo/foo/bar/foo"),
    String("ramfs»/bar/foo/foo/bar/bar"),
    String("ramfs»/bar/foo/foo/bar/baz"),
    String("ramfs»/bar/foo/foo/baz/foo"),
    String("ramfs»/bar/foo/foo/baz/bar"),
    String("ramfs»/bar/foo/foo/baz/baz"),
    String("ramfs»/bar/foo/bar/foo/foo"),
    String("ramfs»/bar/foo/bar/foo/bar"),
    String("ramfs»/bar/foo/bar/foo/baz"),
    String("ramfs»/bar/foo/bar/bar/foo"),
    String("ramfs»/bar/foo/bar/bar/bar"),
    String("ramfs»/bar/foo/bar/bar/baz"),
    String("ramfs»/bar/foo/bar/baz/foo"),
    String("ramfs»/bar/foo/bar/baz/bar"),
    String("ramfs»/bar/foo/bar/baz/baz"),
    String("ramfs»/bar/foo/baz/foo/foo"),
    String("ramfs»/bar/foo/baz/foo/bar"),
    String("ramfs»/bar/foo/baz/foo/baz"),
    String("ramfs»/bar/foo/baz/bar/foo"),
    String("ramfs»/bar/foo/baz/bar/bar"),
    String("ramfs»/bar/foo/baz/bar/baz"),
    String("ramfs»/bar/foo/baz/baz/foo"),
    String("ramfs»/bar/foo/baz/baz/bar"),
    String("ramfs»/bar/foo/baz/baz/baz"),
    String("ramfs»/bar/bar/foo/foo/foo"),
    String("ramfs»/bar/bar/foo/foo/bar"),
    String("ramfs»/bar/bar/foo/foo/baz"),
    String("ramfs»/bar/bar/foo/bar/foo"),
    String("ramfs»/bar/bar/foo/bar/bar"),
    String("ramfs»/bar/bar/foo/bar/baz"),
    String("ramfs»/bar/bar/foo/baz/foo"),
    String("ramfs»/bar/bar/foo/baz/bar"),
    String("ramfs»/bar/bar/foo/baz/baz"),
    String("ramfs»/bar/bar/bar/foo/foo"),
    String("ramfs»/bar/bar/bar/foo/bar"),
    String("ramfs»/bar/bar/bar/foo/baz"),
    String("ramfs»/bar/bar/bar/bar/foo"),
    String("ramfs»/bar/bar/bar/bar/bar"),
    String("ramfs»/bar/bar/bar/bar/baz"),
    String("ramfs»/bar/bar/bar/baz/foo"),
    String("ramfs»/bar/bar/bar/baz/bar"),
    String("ramfs»/bar/bar/bar/baz/baz"),
    String("ramfs»/bar/bar/baz/foo/foo"),
    String("ramfs»/bar/bar/baz/foo/bar"),
    String("ramfs»/bar/bar/baz/foo/baz"),
    String("ramfs»/bar/bar/baz/bar/foo"),
    String("ramfs»/bar/bar/baz/bar/bar"),
    String("ramfs»/bar/bar/baz/bar/baz"),
    String("ramfs»/bar/bar/baz/baz/foo"),
    String("ramfs»/bar/bar/baz/baz/bar"),
    String("ramfs»/bar/bar/baz/baz/baz"),
    String("ramfs»/bar/baz/foo/foo/foo"),
    String("ramfs»/bar/baz/foo/foo/bar"),
    String("ramfs»/bar/baz/foo/foo/baz"),
    String("ramfs»/bar/baz/foo/bar/foo"),
    String("ramfs»/bar/baz/foo/bar/bar"),
    String("ramfs»/bar/baz/foo/bar/baz"),
    String("ramfs»/bar/baz/foo/baz/foo"),
    String("ramfs»/bar/baz/foo/baz/bar"),
    String("ramfs»/bar/baz/foo/baz/baz"),
    String("ramfs»/bar/baz/bar/foo/foo"),
    String("ramfs»/bar/baz/bar/foo/bar"),
    String("ramfs»/bar/baz/bar/foo/baz"),
    String("ramfs»/bar/baz/bar/bar/foo"),
    String("ramfs»/bar/baz/bar/bar/bar"),
    String("ramfs»/bar/baz/bar/bar/baz"),
    String("ramfs»/bar/baz/bar/baz/foo"),
    String("ramfs»/bar/baz/bar/baz/bar"),
    String("ramfs»/bar/baz/bar/baz/baz"),
    String("ramfs»/bar/baz/baz/foo/foo"),
    String("ramfs»/bar/baz/baz/foo/bar"),
    String("ramfs»/bar/baz/baz/foo/baz"),
    String("ramfs»/bar/baz/baz/bar/foo"),
    String("ramfs»/bar/baz/baz/bar/bar"),
    String("ramfs»/bar/baz/baz/bar/baz"),
    String("ramfs»/bar/baz/baz/baz/foo"),
    String("ramfs»/bar/baz/baz/baz/bar"),
    String("ramfs»/bar/baz/baz/baz/baz"),
    String("ramfs»/baz/foo/foo/foo/foo"),
    String("ramfs»/baz/foo/foo/foo/bar"),
    String("ramfs»/baz/foo/foo/foo/baz"),
    String("ramfs»/baz/foo/foo/bar/foo"),
    String("ramfs»/baz/foo/foo/bar/bar"),
    String("ramfs»/baz/foo/foo/bar/baz"),
    String("ramfs»/baz/foo/foo/baz/foo"),
    String("ramfs»/baz/foo/foo/baz/bar"),
    String("ramfs»/baz/foo/foo/baz/baz"),
    String("ramfs»/baz/foo/bar/foo/foo"),
    String("ramfs»/baz/foo/bar/foo/bar"),
    String("ramfs»/baz/foo/bar/foo/baz"),
    String("ramfs»/baz/foo/bar/bar/foo"),
    String("ramfs»/baz/foo/bar/bar/bar"),
    String("ramfs»/baz/foo/bar/bar/baz"),
    String("ramfs»/baz/foo/bar/baz/foo"),
    String("ramfs»/baz/foo/bar/baz/bar"),
    String("ramfs»/baz/foo/bar/baz/baz"),
    String("ramfs»/baz/foo/baz/foo/foo"),
    String("ramfs»/baz/foo/baz/foo/bar"),
    String("ramfs»/baz/foo/baz/foo/baz"),
    String("ramfs»/baz/foo/baz/bar/foo"),
    String("ramfs»/baz/foo/baz/bar/bar"),
    String("ramfs»/baz/foo/baz/bar/baz"),
    String("ramfs»/baz/foo/baz/baz/foo"),
    String("ramfs»/baz/foo/baz/baz/bar"),
    String("ramfs»/baz/foo/baz/baz/baz"),
    String("ramfs»/baz/bar/foo/foo/foo"),
    String("ramfs»/baz/bar/foo/foo/bar"),
    String("ramfs»/baz/bar/foo/foo/baz"),
    String("ramfs»/baz/bar/foo/bar/foo"),
    String("ramfs»/baz/bar/foo/bar/bar"),
    String("ramfs»/baz/bar/foo/bar/baz"),
    String("ramfs»/baz/bar/foo/baz/foo"),
    String("ramfs»/baz/bar/foo/baz/bar"),
    String("ramfs»/baz/bar/foo/baz/baz"),
    String("ramfs»/baz/bar/bar/foo/foo"),
    String("ramfs»/baz/bar/bar/foo/bar"),
    String("ramfs»/baz/bar/bar/foo/baz"),
    String("ramfs»/baz/bar/bar/bar/foo"),
    String("ramfs»/baz/bar/bar/bar/bar"),
    String("ramfs»/baz/bar/bar/bar/baz"),
    String("ramfs»/baz/bar/bar/baz/foo"),
    String("ramfs»/baz/bar/bar/baz/bar"),
    String("ramfs»/baz/bar/bar/baz/baz"),
    String("ramfs»/baz/bar/baz/foo/foo"),
    String("ramfs»/baz/bar/baz/foo/bar"),
    String("ramfs»/baz/bar/baz/foo/baz"),
    String("ramfs»/baz/bar/baz/bar/foo"),
    String("ramfs»/baz/bar/baz/bar/bar"),
    String("ramfs»/baz/bar/baz/bar/baz"),
    String("ramfs»/baz/bar/baz/baz/foo"),
    String("ramfs»/baz/bar/baz/baz/bar"),
    String("ramfs»/baz/bar/baz/baz/baz"),
    String("ramfs»/baz/baz/foo/foo/foo"),
    String("ramfs»/baz/baz/foo/foo/bar"),
    String("ramfs»/baz/baz/foo/foo/baz"),
    String("ramfs»/baz/baz/foo/bar/foo"),
    String("ramfs»/baz/baz/foo/bar/bar"),
    String("ramfs»/baz/baz/foo/bar/baz"),
    String("ramfs»/baz/baz/foo/baz/foo"),
    String("ramfs»/baz/baz/foo/baz/bar"),
    String("ramfs»/baz/baz/foo/baz/baz"),
    String("ramfs»/baz/baz/bar/foo/foo"),
    String("ramfs»/baz/baz/bar/foo/bar"),
    String("ramfs»/baz/baz/bar/foo/baz"),
    String("ramfs»/baz/baz/bar/bar/foo"),
    String("ramfs»/baz/baz/bar/bar/bar"),
    String("ramfs»/baz/baz/bar/bar/baz"),
    String("ramfs»/baz/baz/bar/baz/foo"),
    String("ramfs»/baz/baz/bar/baz/bar"),
    String("ramfs»/baz/baz/bar/baz/baz"),
    String("ramfs»/baz/baz/baz/foo/foo"),
    String("ramfs»/baz/baz/baz/foo/bar"),
    String("ramfs»/baz/baz/baz/foo/baz"),
    String("ramfs»/baz/baz/baz/bar/foo"),
    String("ramfs»/baz/baz/baz/bar/bar"),
    String("ramfs»/baz/baz/baz/bar/baz"),
    String("ramfs»/baz/baz/baz/baz/foo"),
    String("ramfs»/baz/baz/baz/baz/bar"),
    String("ramfs»/baz/baz/baz/baz/baz"),
};

static const String &randomPath()
{
    return paths[rand() % (sizeof(paths) / sizeof(paths[0]))];
}

static std::unique_ptr<RamFs> prepareVFS(VFS &vfs)
{
    srand(time(0));

    std::unique_ptr<RamFs> ramfs = std::make_unique<RamFs>();
    ramfs->initialise(nullptr);

    vfs.addAlias(ramfs.get(), g_Alias);

    // Add a bunch of directories for lookups
    for (auto &p : paths)
    {
        vfs.createDirectory(p, 0777);
    }

    return ramfs;
}

static void BM_VFSShallowDirectoryTraverse(benchmark::State &state)
{
    VFS vfs;
    auto ramfs = prepareVFS(vfs);

    CALLGRIND_START_INSTRUMENTATION;
    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(vfs.find(g_ShallowPath));
    }
    CALLGRIND_STOP_INSTRUMENTATION;

    state.SetItemsProcessed(int64_t(state.iterations()));

    vfs.removeAllAliases(ramfs.get(), false);
}

static void BM_VFSMediumDirectoryTraverse(benchmark::State &state)
{
    VFS vfs;
    auto ramfs = prepareVFS(vfs);

    CALLGRIND_START_INSTRUMENTATION;
    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(vfs.find(g_MiddlePath));
    }
    CALLGRIND_STOP_INSTRUMENTATION;

    state.SetItemsProcessed(int64_t(state.iterations()));

    vfs.removeAllAliases(ramfs.get(), false);
}

static void BM_VFSDeepDirectoryTraverse(benchmark::State &state)
{
    VFS vfs;
    auto ramfs = prepareVFS(vfs);

    CALLGRIND_START_INSTRUMENTATION;
    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(vfs.find(g_DeepPath));
    }
    CALLGRIND_STOP_INSTRUMENTATION;

    state.SetItemsProcessed(int64_t(state.iterations()));

    vfs.removeAllAliases(ramfs.get(), false);
}

static void BM_VFSRandomDirectoryTraverse(benchmark::State &state)
{
    VFS vfs;
    auto ramfs = prepareVFS(vfs);

    CALLGRIND_START_INSTRUMENTATION;
    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(vfs.find(randomPath()));
    }
    CALLGRIND_STOP_INSTRUMENTATION;

    state.SetItemsProcessed(int64_t(state.iterations()));

    vfs.removeAllAliases(ramfs.get(), false);
}

static void BM_VFSShallowDirectoryTraverseNoFs(benchmark::State &state)
{
    VFS vfs;
    auto ramfs = prepareVFS(vfs);

    CALLGRIND_START_INSTRUMENTATION;
    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(vfs.find(g_ShallowPathNoFs, ramfs->getRoot()));
    }
    CALLGRIND_STOP_INSTRUMENTATION;

    state.SetItemsProcessed(int64_t(state.iterations()));

    vfs.removeAllAliases(ramfs.get(), false);
}

static void BM_VFSMediumDirectoryTraverseNoFs(benchmark::State &state)
{
    VFS vfs;
    auto ramfs = prepareVFS(vfs);

    CALLGRIND_START_INSTRUMENTATION;
    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(vfs.find(g_MiddlePathNoFs, ramfs->getRoot()));
    }
    CALLGRIND_STOP_INSTRUMENTATION;

    state.SetItemsProcessed(int64_t(state.iterations()));

    vfs.removeAllAliases(ramfs.get(), false);
}

static void BM_VFSDeepDirectoryTraverseNoFs(benchmark::State &state)
{
    VFS vfs;
    auto ramfs = prepareVFS(vfs);

    CALLGRIND_START_INSTRUMENTATION;
    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(vfs.find(g_DeepPathNoFs, ramfs->getRoot()));
    }
    CALLGRIND_STOP_INSTRUMENTATION;

    state.SetItemsProcessed(int64_t(state.iterations()));

    vfs.removeAllAliases(ramfs.get(), false);
}

static void BM_VFSRandomDirectoryTraverseNoFs(benchmark::State &state)
{
    VFS vfs;
    auto ramfs = prepareVFS(vfs);

    CALLGRIND_START_INSTRUMENTATION;
    while (state.KeepRunning())
    {
        /// \todo VFS::find() should be able to accept a StringView
        StringView thisPath = randomPath().view();
        benchmark::DoNotOptimize(vfs.find(thisPath.substring(7, thisPath.length()).toString(), ramfs->getRoot()));
    }
    CALLGRIND_STOP_INSTRUMENTATION;

    state.SetItemsProcessed(int64_t(state.iterations()));

    vfs.removeAllAliases(ramfs.get(), false);
}

BENCHMARK(BM_VFSDeepDirectoryTraverse);
BENCHMARK(BM_VFSMediumDirectoryTraverse);
BENCHMARK(BM_VFSShallowDirectoryTraverse);
BENCHMARK(BM_VFSRandomDirectoryTraverse);

BENCHMARK(BM_VFSDeepDirectoryTraverseNoFs);
BENCHMARK(BM_VFSMediumDirectoryTraverseNoFs);
BENCHMARK(BM_VFSShallowDirectoryTraverseNoFs);
BENCHMARK(BM_VFSRandomDirectoryTraverseNoFs);
