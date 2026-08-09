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

#include "modules/system/ramfs/RamFs.h"
#include "modules/system/vfs/VFS.h"

static String g_DeepPath("/foo/foo/foo/foo");
static String g_ShallowPath("/");
static String g_MiddlePath("/foo/foo");
static String g_DeepPathNoFs("/foo/foo/foo/foo");
static String g_ShallowPathNoFs("/");
static String g_MiddlePathNoFs("/foo/foo");

// A huge pile of paths to add to the filesystem for testing.
// Also used for randomly hitting the filesystem with lookups.
static String paths[] = {
    String("/foo"),
    String("/bar"),
    String("/baz"),
    String("/foo/foo"),
    String("/foo/bar"),
    String("/foo/baz"),
    String("/bar/foo"),
    String("/bar/bar"),
    String("/bar/baz"),
    String("/baz/foo"),
    String("/baz/bar"),
    String("/baz/baz"),
    String("/foo/foo"),
    String("/foo/bar"),
    String("/foo/baz"),
    String("/bar/foo"),
    String("/bar/bar"),
    String("/bar/baz"),
    String("/baz/foo"),
    String("/baz/bar"),
    String("/baz/baz"),
    String("/foo/foo/foo"),
    String("/foo/foo/bar"),
    String("/foo/foo/baz"),
    String("/foo/bar/foo"),
    String("/foo/bar/bar"),
    String("/foo/bar/baz"),
    String("/foo/baz/foo"),
    String("/foo/baz/bar"),
    String("/foo/baz/baz"),
    String("/bar/foo/foo"),
    String("/bar/foo/bar"),
    String("/bar/foo/baz"),
    String("/bar/bar/foo"),
    String("/bar/bar/bar"),
    String("/bar/bar/baz"),
    String("/bar/baz/foo"),
    String("/bar/baz/bar"),
    String("/bar/baz/baz"),
    String("/baz/foo/foo"),
    String("/baz/foo/bar"),
    String("/baz/foo/baz"),
    String("/baz/bar/foo"),
    String("/baz/bar/bar"),
    String("/baz/bar/baz"),
    String("/baz/baz/foo"),
    String("/baz/baz/bar"),
    String("/baz/baz/baz"),
    String("/foo/foo"),
    String("/foo/bar"),
    String("/foo/baz"),
    String("/bar/foo"),
    String("/bar/bar"),
    String("/bar/baz"),
    String("/baz/foo"),
    String("/baz/bar"),
    String("/baz/baz"),
    String("/foo/foo/foo"),
    String("/foo/foo/bar"),
    String("/foo/foo/baz"),
    String("/foo/bar/foo"),
    String("/foo/bar/bar"),
    String("/foo/bar/baz"),
    String("/foo/baz/foo"),
    String("/foo/baz/bar"),
    String("/foo/baz/baz"),
    String("/bar/foo/foo"),
    String("/bar/foo/bar"),
    String("/bar/foo/baz"),
    String("/bar/bar/foo"),
    String("/bar/bar/bar"),
    String("/bar/bar/baz"),
    String("/bar/baz/foo"),
    String("/bar/baz/bar"),
    String("/bar/baz/baz"),
    String("/baz/foo/foo"),
    String("/baz/foo/bar"),
    String("/baz/foo/baz"),
    String("/baz/bar/foo"),
    String("/baz/bar/bar"),
    String("/baz/bar/baz"),
    String("/baz/baz/foo"),
    String("/baz/baz/bar"),
    String("/baz/baz/baz"),
    String("/foo/foo/foo"),
    String("/foo/foo/bar"),
    String("/foo/foo/baz"),
    String("/foo/bar/foo"),
    String("/foo/bar/bar"),
    String("/foo/bar/baz"),
    String("/foo/baz/foo"),
    String("/foo/baz/bar"),
    String("/foo/baz/baz"),
    String("/bar/foo/foo"),
    String("/bar/foo/bar"),
    String("/bar/foo/baz"),
    String("/bar/bar/foo"),
    String("/bar/bar/bar"),
    String("/bar/bar/baz"),
    String("/bar/baz/foo"),
    String("/bar/baz/bar"),
    String("/bar/baz/baz"),
    String("/baz/foo/foo"),
    String("/baz/foo/bar"),
    String("/baz/foo/baz"),
    String("/baz/bar/foo"),
    String("/baz/bar/bar"),
    String("/baz/bar/baz"),
    String("/baz/baz/foo"),
    String("/baz/baz/bar"),
    String("/baz/baz/baz"),
    String("/foo/foo/foo/foo"),
    String("/foo/foo/foo/bar"),
    String("/foo/foo/foo/baz"),
    String("/foo/foo/bar/foo"),
    String("/foo/foo/bar/bar"),
    String("/foo/foo/bar/baz"),
    String("/foo/foo/baz/foo"),
    String("/foo/foo/baz/bar"),
    String("/foo/foo/baz/baz"),
    String("/foo/bar/foo/foo"),
    String("/foo/bar/foo/bar"),
    String("/foo/bar/foo/baz"),
    String("/foo/bar/bar/foo"),
    String("/foo/bar/bar/bar"),
    String("/foo/bar/bar/baz"),
    String("/foo/bar/baz/foo"),
    String("/foo/bar/baz/bar"),
    String("/foo/bar/baz/baz"),
    String("/foo/baz/foo/foo"),
    String("/foo/baz/foo/bar"),
    String("/foo/baz/foo/baz"),
    String("/foo/baz/bar/foo"),
    String("/foo/baz/bar/bar"),
    String("/foo/baz/bar/baz"),
    String("/foo/baz/baz/foo"),
    String("/foo/baz/baz/bar"),
    String("/foo/baz/baz/baz"),
    String("/bar/foo/foo/foo"),
    String("/bar/foo/foo/bar"),
    String("/bar/foo/foo/baz"),
    String("/bar/foo/bar/foo"),
    String("/bar/foo/bar/bar"),
    String("/bar/foo/bar/baz"),
    String("/bar/foo/baz/foo"),
    String("/bar/foo/baz/bar"),
    String("/bar/foo/baz/baz"),
    String("/bar/bar/foo/foo"),
    String("/bar/bar/foo/bar"),
    String("/bar/bar/foo/baz"),
    String("/bar/bar/bar/foo"),
    String("/bar/bar/bar/bar"),
    String("/bar/bar/bar/baz"),
    String("/bar/bar/baz/foo"),
    String("/bar/bar/baz/bar"),
    String("/bar/bar/baz/baz"),
    String("/bar/baz/foo/foo"),
    String("/bar/baz/foo/bar"),
    String("/bar/baz/foo/baz"),
    String("/bar/baz/bar/foo"),
    String("/bar/baz/bar/bar"),
    String("/bar/baz/bar/baz"),
    String("/bar/baz/baz/foo"),
    String("/bar/baz/baz/bar"),
    String("/bar/baz/baz/baz"),
    String("/baz/foo/foo/foo"),
    String("/baz/foo/foo/bar"),
    String("/baz/foo/foo/baz"),
    String("/baz/foo/bar/foo"),
    String("/baz/foo/bar/bar"),
    String("/baz/foo/bar/baz"),
    String("/baz/foo/baz/foo"),
    String("/baz/foo/baz/bar"),
    String("/baz/foo/baz/baz"),
    String("/baz/bar/foo/foo"),
    String("/baz/bar/foo/bar"),
    String("/baz/bar/foo/baz"),
    String("/baz/bar/bar/foo"),
    String("/baz/bar/bar/bar"),
    String("/baz/bar/bar/baz"),
    String("/baz/bar/baz/foo"),
    String("/baz/bar/baz/bar"),
    String("/baz/bar/baz/baz"),
    String("/baz/baz/foo/foo"),
    String("/baz/baz/foo/bar"),
    String("/baz/baz/foo/baz"),
    String("/baz/baz/bar/foo"),
    String("/baz/baz/bar/bar"),
    String("/baz/baz/bar/baz"),
    String("/baz/baz/baz/foo"),
    String("/baz/baz/baz/bar"),
    String("/baz/baz/baz/baz"),
    String("/foo/foo"),
    String("/foo/bar"),
    String("/foo/baz"),
    String("/bar/foo"),
    String("/bar/bar"),
    String("/bar/baz"),
    String("/baz/foo"),
    String("/baz/bar"),
    String("/baz/baz"),
    String("/foo/foo/foo"),
    String("/foo/foo/bar"),
    String("/foo/foo/baz"),
    String("/foo/bar/foo"),
    String("/foo/bar/bar"),
    String("/foo/bar/baz"),
    String("/foo/baz/foo"),
    String("/foo/baz/bar"),
    String("/foo/baz/baz"),
    String("/bar/foo/foo"),
    String("/bar/foo/bar"),
    String("/bar/foo/baz"),
    String("/bar/bar/foo"),
    String("/bar/bar/bar"),
    String("/bar/bar/baz"),
    String("/bar/baz/foo"),
    String("/bar/baz/bar"),
    String("/bar/baz/baz"),
    String("/baz/foo/foo"),
    String("/baz/foo/bar"),
    String("/baz/foo/baz"),
    String("/baz/bar/foo"),
    String("/baz/bar/bar"),
    String("/baz/bar/baz"),
    String("/baz/baz/foo"),
    String("/baz/baz/bar"),
    String("/baz/baz/baz"),
    String("/foo/foo/foo"),
    String("/foo/foo/bar"),
    String("/foo/foo/baz"),
    String("/foo/bar/foo"),
    String("/foo/bar/bar"),
    String("/foo/bar/baz"),
    String("/foo/baz/foo"),
    String("/foo/baz/bar"),
    String("/foo/baz/baz"),
    String("/bar/foo/foo"),
    String("/bar/foo/bar"),
    String("/bar/foo/baz"),
    String("/bar/bar/foo"),
    String("/bar/bar/bar"),
    String("/bar/bar/baz"),
    String("/bar/baz/foo"),
    String("/bar/baz/bar"),
    String("/bar/baz/baz"),
    String("/baz/foo/foo"),
    String("/baz/foo/bar"),
    String("/baz/foo/baz"),
    String("/baz/bar/foo"),
    String("/baz/bar/bar"),
    String("/baz/bar/baz"),
    String("/baz/baz/foo"),
    String("/baz/baz/bar"),
    String("/baz/baz/baz"),
    String("/foo/foo/foo/foo"),
    String("/foo/foo/foo/bar"),
    String("/foo/foo/foo/baz"),
    String("/foo/foo/bar/foo"),
    String("/foo/foo/bar/bar"),
    String("/foo/foo/bar/baz"),
    String("/foo/foo/baz/foo"),
    String("/foo/foo/baz/bar"),
    String("/foo/foo/baz/baz"),
    String("/foo/bar/foo/foo"),
    String("/foo/bar/foo/bar"),
    String("/foo/bar/foo/baz"),
    String("/foo/bar/bar/foo"),
    String("/foo/bar/bar/bar"),
    String("/foo/bar/bar/baz"),
    String("/foo/bar/baz/foo"),
    String("/foo/bar/baz/bar"),
    String("/foo/bar/baz/baz"),
    String("/foo/baz/foo/foo"),
    String("/foo/baz/foo/bar"),
    String("/foo/baz/foo/baz"),
    String("/foo/baz/bar/foo"),
    String("/foo/baz/bar/bar"),
    String("/foo/baz/bar/baz"),
    String("/foo/baz/baz/foo"),
    String("/foo/baz/baz/bar"),
    String("/foo/baz/baz/baz"),
    String("/bar/foo/foo/foo"),
    String("/bar/foo/foo/bar"),
    String("/bar/foo/foo/baz"),
    String("/bar/foo/bar/foo"),
    String("/bar/foo/bar/bar"),
    String("/bar/foo/bar/baz"),
    String("/bar/foo/baz/foo"),
    String("/bar/foo/baz/bar"),
    String("/bar/foo/baz/baz"),
    String("/bar/bar/foo/foo"),
    String("/bar/bar/foo/bar"),
    String("/bar/bar/foo/baz"),
    String("/bar/bar/bar/foo"),
    String("/bar/bar/bar/bar"),
    String("/bar/bar/bar/baz"),
    String("/bar/bar/baz/foo"),
    String("/bar/bar/baz/bar"),
    String("/bar/bar/baz/baz"),
    String("/bar/baz/foo/foo"),
    String("/bar/baz/foo/bar"),
    String("/bar/baz/foo/baz"),
    String("/bar/baz/bar/foo"),
    String("/bar/baz/bar/bar"),
    String("/bar/baz/bar/baz"),
    String("/bar/baz/baz/foo"),
    String("/bar/baz/baz/bar"),
    String("/bar/baz/baz/baz"),
    String("/baz/foo/foo/foo"),
    String("/baz/foo/foo/bar"),
    String("/baz/foo/foo/baz"),
    String("/baz/foo/bar/foo"),
    String("/baz/foo/bar/bar"),
    String("/baz/foo/bar/baz"),
    String("/baz/foo/baz/foo"),
    String("/baz/foo/baz/bar"),
    String("/baz/foo/baz/baz"),
    String("/baz/bar/foo/foo"),
    String("/baz/bar/foo/bar"),
    String("/baz/bar/foo/baz"),
    String("/baz/bar/bar/foo"),
    String("/baz/bar/bar/bar"),
    String("/baz/bar/bar/baz"),
    String("/baz/bar/baz/foo"),
    String("/baz/bar/baz/bar"),
    String("/baz/bar/baz/baz"),
    String("/baz/baz/foo/foo"),
    String("/baz/baz/foo/bar"),
    String("/baz/baz/foo/baz"),
    String("/baz/baz/bar/foo"),
    String("/baz/baz/bar/bar"),
    String("/baz/baz/bar/baz"),
    String("/baz/baz/baz/foo"),
    String("/baz/baz/baz/bar"),
    String("/baz/baz/baz/baz"),
    String("/foo/foo/foo"),
    String("/foo/foo/bar"),
    String("/foo/foo/baz"),
    String("/foo/bar/foo"),
    String("/foo/bar/bar"),
    String("/foo/bar/baz"),
    String("/foo/baz/foo"),
    String("/foo/baz/bar"),
    String("/foo/baz/baz"),
    String("/bar/foo/foo"),
    String("/bar/foo/bar"),
    String("/bar/foo/baz"),
    String("/bar/bar/foo"),
    String("/bar/bar/bar"),
    String("/bar/bar/baz"),
    String("/bar/baz/foo"),
    String("/bar/baz/bar"),
    String("/bar/baz/baz"),
    String("/baz/foo/foo"),
    String("/baz/foo/bar"),
    String("/baz/foo/baz"),
    String("/baz/bar/foo"),
    String("/baz/bar/bar"),
    String("/baz/bar/baz"),
    String("/baz/baz/foo"),
    String("/baz/baz/bar"),
    String("/baz/baz/baz"),
    String("/foo/foo/foo/foo"),
    String("/foo/foo/foo/bar"),
    String("/foo/foo/foo/baz"),
    String("/foo/foo/bar/foo"),
    String("/foo/foo/bar/bar"),
    String("/foo/foo/bar/baz"),
    String("/foo/foo/baz/foo"),
    String("/foo/foo/baz/bar"),
    String("/foo/foo/baz/baz"),
    String("/foo/bar/foo/foo"),
    String("/foo/bar/foo/bar"),
    String("/foo/bar/foo/baz"),
    String("/foo/bar/bar/foo"),
    String("/foo/bar/bar/bar"),
    String("/foo/bar/bar/baz"),
    String("/foo/bar/baz/foo"),
    String("/foo/bar/baz/bar"),
    String("/foo/bar/baz/baz"),
    String("/foo/baz/foo/foo"),
    String("/foo/baz/foo/bar"),
    String("/foo/baz/foo/baz"),
    String("/foo/baz/bar/foo"),
    String("/foo/baz/bar/bar"),
    String("/foo/baz/bar/baz"),
    String("/foo/baz/baz/foo"),
    String("/foo/baz/baz/bar"),
    String("/foo/baz/baz/baz"),
    String("/bar/foo/foo/foo"),
    String("/bar/foo/foo/bar"),
    String("/bar/foo/foo/baz"),
    String("/bar/foo/bar/foo"),
    String("/bar/foo/bar/bar"),
    String("/bar/foo/bar/baz"),
    String("/bar/foo/baz/foo"),
    String("/bar/foo/baz/bar"),
    String("/bar/foo/baz/baz"),
    String("/bar/bar/foo/foo"),
    String("/bar/bar/foo/bar"),
    String("/bar/bar/foo/baz"),
    String("/bar/bar/bar/foo"),
    String("/bar/bar/bar/bar"),
    String("/bar/bar/bar/baz"),
    String("/bar/bar/baz/foo"),
    String("/bar/bar/baz/bar"),
    String("/bar/bar/baz/baz"),
    String("/bar/baz/foo/foo"),
    String("/bar/baz/foo/bar"),
    String("/bar/baz/foo/baz"),
    String("/bar/baz/bar/foo"),
    String("/bar/baz/bar/bar"),
    String("/bar/baz/bar/baz"),
    String("/bar/baz/baz/foo"),
    String("/bar/baz/baz/bar"),
    String("/bar/baz/baz/baz"),
    String("/baz/foo/foo/foo"),
    String("/baz/foo/foo/bar"),
    String("/baz/foo/foo/baz"),
    String("/baz/foo/bar/foo"),
    String("/baz/foo/bar/bar"),
    String("/baz/foo/bar/baz"),
    String("/baz/foo/baz/foo"),
    String("/baz/foo/baz/bar"),
    String("/baz/foo/baz/baz"),
    String("/baz/bar/foo/foo"),
    String("/baz/bar/foo/bar"),
    String("/baz/bar/foo/baz"),
    String("/baz/bar/bar/foo"),
    String("/baz/bar/bar/bar"),
    String("/baz/bar/bar/baz"),
    String("/baz/bar/baz/foo"),
    String("/baz/bar/baz/bar"),
    String("/baz/bar/baz/baz"),
    String("/baz/baz/foo/foo"),
    String("/baz/baz/foo/bar"),
    String("/baz/baz/foo/baz"),
    String("/baz/baz/bar/foo"),
    String("/baz/baz/bar/bar"),
    String("/baz/baz/bar/baz"),
    String("/baz/baz/baz/foo"),
    String("/baz/baz/baz/bar"),
    String("/baz/baz/baz/baz"),
    String("/foo/foo/foo/foo"),
    String("/foo/foo/foo/bar"),
    String("/foo/foo/foo/baz"),
    String("/foo/foo/bar/foo"),
    String("/foo/foo/bar/bar"),
    String("/foo/foo/bar/baz"),
    String("/foo/foo/baz/foo"),
    String("/foo/foo/baz/bar"),
    String("/foo/foo/baz/baz"),
    String("/foo/bar/foo/foo"),
    String("/foo/bar/foo/bar"),
    String("/foo/bar/foo/baz"),
    String("/foo/bar/bar/foo"),
    String("/foo/bar/bar/bar"),
    String("/foo/bar/bar/baz"),
    String("/foo/bar/baz/foo"),
    String("/foo/bar/baz/bar"),
    String("/foo/bar/baz/baz"),
    String("/foo/baz/foo/foo"),
    String("/foo/baz/foo/bar"),
    String("/foo/baz/foo/baz"),
    String("/foo/baz/bar/foo"),
    String("/foo/baz/bar/bar"),
    String("/foo/baz/bar/baz"),
    String("/foo/baz/baz/foo"),
    String("/foo/baz/baz/bar"),
    String("/foo/baz/baz/baz"),
    String("/bar/foo/foo/foo"),
    String("/bar/foo/foo/bar"),
    String("/bar/foo/foo/baz"),
    String("/bar/foo/bar/foo"),
    String("/bar/foo/bar/bar"),
    String("/bar/foo/bar/baz"),
    String("/bar/foo/baz/foo"),
    String("/bar/foo/baz/bar"),
    String("/bar/foo/baz/baz"),
    String("/bar/bar/foo/foo"),
    String("/bar/bar/foo/bar"),
    String("/bar/bar/foo/baz"),
    String("/bar/bar/bar/foo"),
    String("/bar/bar/bar/bar"),
    String("/bar/bar/bar/baz"),
    String("/bar/bar/baz/foo"),
    String("/bar/bar/baz/bar"),
    String("/bar/bar/baz/baz"),
    String("/bar/baz/foo/foo"),
    String("/bar/baz/foo/bar"),
    String("/bar/baz/foo/baz"),
    String("/bar/baz/bar/foo"),
    String("/bar/baz/bar/bar"),
    String("/bar/baz/bar/baz"),
    String("/bar/baz/baz/foo"),
    String("/bar/baz/baz/bar"),
    String("/bar/baz/baz/baz"),
    String("/baz/foo/foo/foo"),
    String("/baz/foo/foo/bar"),
    String("/baz/foo/foo/baz"),
    String("/baz/foo/bar/foo"),
    String("/baz/foo/bar/bar"),
    String("/baz/foo/bar/baz"),
    String("/baz/foo/baz/foo"),
    String("/baz/foo/baz/bar"),
    String("/baz/foo/baz/baz"),
    String("/baz/bar/foo/foo"),
    String("/baz/bar/foo/bar"),
    String("/baz/bar/foo/baz"),
    String("/baz/bar/bar/foo"),
    String("/baz/bar/bar/bar"),
    String("/baz/bar/bar/baz"),
    String("/baz/bar/baz/foo"),
    String("/baz/bar/baz/bar"),
    String("/baz/bar/baz/baz"),
    String("/baz/baz/foo/foo"),
    String("/baz/baz/foo/bar"),
    String("/baz/baz/foo/baz"),
    String("/baz/baz/bar/foo"),
    String("/baz/baz/bar/bar"),
    String("/baz/baz/bar/baz"),
    String("/baz/baz/baz/foo"),
    String("/baz/baz/baz/bar"),
    String("/baz/baz/baz/baz"),
    String("/foo/foo/foo/foo/foo"),
    String("/foo/foo/foo/foo/bar"),
    String("/foo/foo/foo/foo/baz"),
    String("/foo/foo/foo/bar/foo"),
    String("/foo/foo/foo/bar/bar"),
    String("/foo/foo/foo/bar/baz"),
    String("/foo/foo/foo/baz/foo"),
    String("/foo/foo/foo/baz/bar"),
    String("/foo/foo/foo/baz/baz"),
    String("/foo/foo/bar/foo/foo"),
    String("/foo/foo/bar/foo/bar"),
    String("/foo/foo/bar/foo/baz"),
    String("/foo/foo/bar/bar/foo"),
    String("/foo/foo/bar/bar/bar"),
    String("/foo/foo/bar/bar/baz"),
    String("/foo/foo/bar/baz/foo"),
    String("/foo/foo/bar/baz/bar"),
    String("/foo/foo/bar/baz/baz"),
    String("/foo/foo/baz/foo/foo"),
    String("/foo/foo/baz/foo/bar"),
    String("/foo/foo/baz/foo/baz"),
    String("/foo/foo/baz/bar/foo"),
    String("/foo/foo/baz/bar/bar"),
    String("/foo/foo/baz/bar/baz"),
    String("/foo/foo/baz/baz/foo"),
    String("/foo/foo/baz/baz/bar"),
    String("/foo/foo/baz/baz/baz"),
    String("/foo/bar/foo/foo/foo"),
    String("/foo/bar/foo/foo/bar"),
    String("/foo/bar/foo/foo/baz"),
    String("/foo/bar/foo/bar/foo"),
    String("/foo/bar/foo/bar/bar"),
    String("/foo/bar/foo/bar/baz"),
    String("/foo/bar/foo/baz/foo"),
    String("/foo/bar/foo/baz/bar"),
    String("/foo/bar/foo/baz/baz"),
    String("/foo/bar/bar/foo/foo"),
    String("/foo/bar/bar/foo/bar"),
    String("/foo/bar/bar/foo/baz"),
    String("/foo/bar/bar/bar/foo"),
    String("/foo/bar/bar/bar/bar"),
    String("/foo/bar/bar/bar/baz"),
    String("/foo/bar/bar/baz/foo"),
    String("/foo/bar/bar/baz/bar"),
    String("/foo/bar/bar/baz/baz"),
    String("/foo/bar/baz/foo/foo"),
    String("/foo/bar/baz/foo/bar"),
    String("/foo/bar/baz/foo/baz"),
    String("/foo/bar/baz/bar/foo"),
    String("/foo/bar/baz/bar/bar"),
    String("/foo/bar/baz/bar/baz"),
    String("/foo/bar/baz/baz/foo"),
    String("/foo/bar/baz/baz/bar"),
    String("/foo/bar/baz/baz/baz"),
    String("/foo/baz/foo/foo/foo"),
    String("/foo/baz/foo/foo/bar"),
    String("/foo/baz/foo/foo/baz"),
    String("/foo/baz/foo/bar/foo"),
    String("/foo/baz/foo/bar/bar"),
    String("/foo/baz/foo/bar/baz"),
    String("/foo/baz/foo/baz/foo"),
    String("/foo/baz/foo/baz/bar"),
    String("/foo/baz/foo/baz/baz"),
    String("/foo/baz/bar/foo/foo"),
    String("/foo/baz/bar/foo/bar"),
    String("/foo/baz/bar/foo/baz"),
    String("/foo/baz/bar/bar/foo"),
    String("/foo/baz/bar/bar/bar"),
    String("/foo/baz/bar/bar/baz"),
    String("/foo/baz/bar/baz/foo"),
    String("/foo/baz/bar/baz/bar"),
    String("/foo/baz/bar/baz/baz"),
    String("/foo/baz/baz/foo/foo"),
    String("/foo/baz/baz/foo/bar"),
    String("/foo/baz/baz/foo/baz"),
    String("/foo/baz/baz/bar/foo"),
    String("/foo/baz/baz/bar/bar"),
    String("/foo/baz/baz/bar/baz"),
    String("/foo/baz/baz/baz/foo"),
    String("/foo/baz/baz/baz/bar"),
    String("/foo/baz/baz/baz/baz"),
    String("/bar/foo/foo/foo/foo"),
    String("/bar/foo/foo/foo/bar"),
    String("/bar/foo/foo/foo/baz"),
    String("/bar/foo/foo/bar/foo"),
    String("/bar/foo/foo/bar/bar"),
    String("/bar/foo/foo/bar/baz"),
    String("/bar/foo/foo/baz/foo"),
    String("/bar/foo/foo/baz/bar"),
    String("/bar/foo/foo/baz/baz"),
    String("/bar/foo/bar/foo/foo"),
    String("/bar/foo/bar/foo/bar"),
    String("/bar/foo/bar/foo/baz"),
    String("/bar/foo/bar/bar/foo"),
    String("/bar/foo/bar/bar/bar"),
    String("/bar/foo/bar/bar/baz"),
    String("/bar/foo/bar/baz/foo"),
    String("/bar/foo/bar/baz/bar"),
    String("/bar/foo/bar/baz/baz"),
    String("/bar/foo/baz/foo/foo"),
    String("/bar/foo/baz/foo/bar"),
    String("/bar/foo/baz/foo/baz"),
    String("/bar/foo/baz/bar/foo"),
    String("/bar/foo/baz/bar/bar"),
    String("/bar/foo/baz/bar/baz"),
    String("/bar/foo/baz/baz/foo"),
    String("/bar/foo/baz/baz/bar"),
    String("/bar/foo/baz/baz/baz"),
    String("/bar/bar/foo/foo/foo"),
    String("/bar/bar/foo/foo/bar"),
    String("/bar/bar/foo/foo/baz"),
    String("/bar/bar/foo/bar/foo"),
    String("/bar/bar/foo/bar/bar"),
    String("/bar/bar/foo/bar/baz"),
    String("/bar/bar/foo/baz/foo"),
    String("/bar/bar/foo/baz/bar"),
    String("/bar/bar/foo/baz/baz"),
    String("/bar/bar/bar/foo/foo"),
    String("/bar/bar/bar/foo/bar"),
    String("/bar/bar/bar/foo/baz"),
    String("/bar/bar/bar/bar/foo"),
    String("/bar/bar/bar/bar/bar"),
    String("/bar/bar/bar/bar/baz"),
    String("/bar/bar/bar/baz/foo"),
    String("/bar/bar/bar/baz/bar"),
    String("/bar/bar/bar/baz/baz"),
    String("/bar/bar/baz/foo/foo"),
    String("/bar/bar/baz/foo/bar"),
    String("/bar/bar/baz/foo/baz"),
    String("/bar/bar/baz/bar/foo"),
    String("/bar/bar/baz/bar/bar"),
    String("/bar/bar/baz/bar/baz"),
    String("/bar/bar/baz/baz/foo"),
    String("/bar/bar/baz/baz/bar"),
    String("/bar/bar/baz/baz/baz"),
    String("/bar/baz/foo/foo/foo"),
    String("/bar/baz/foo/foo/bar"),
    String("/bar/baz/foo/foo/baz"),
    String("/bar/baz/foo/bar/foo"),
    String("/bar/baz/foo/bar/bar"),
    String("/bar/baz/foo/bar/baz"),
    String("/bar/baz/foo/baz/foo"),
    String("/bar/baz/foo/baz/bar"),
    String("/bar/baz/foo/baz/baz"),
    String("/bar/baz/bar/foo/foo"),
    String("/bar/baz/bar/foo/bar"),
    String("/bar/baz/bar/foo/baz"),
    String("/bar/baz/bar/bar/foo"),
    String("/bar/baz/bar/bar/bar"),
    String("/bar/baz/bar/bar/baz"),
    String("/bar/baz/bar/baz/foo"),
    String("/bar/baz/bar/baz/bar"),
    String("/bar/baz/bar/baz/baz"),
    String("/bar/baz/baz/foo/foo"),
    String("/bar/baz/baz/foo/bar"),
    String("/bar/baz/baz/foo/baz"),
    String("/bar/baz/baz/bar/foo"),
    String("/bar/baz/baz/bar/bar"),
    String("/bar/baz/baz/bar/baz"),
    String("/bar/baz/baz/baz/foo"),
    String("/bar/baz/baz/baz/bar"),
    String("/bar/baz/baz/baz/baz"),
    String("/baz/foo/foo/foo/foo"),
    String("/baz/foo/foo/foo/bar"),
    String("/baz/foo/foo/foo/baz"),
    String("/baz/foo/foo/bar/foo"),
    String("/baz/foo/foo/bar/bar"),
    String("/baz/foo/foo/bar/baz"),
    String("/baz/foo/foo/baz/foo"),
    String("/baz/foo/foo/baz/bar"),
    String("/baz/foo/foo/baz/baz"),
    String("/baz/foo/bar/foo/foo"),
    String("/baz/foo/bar/foo/bar"),
    String("/baz/foo/bar/foo/baz"),
    String("/baz/foo/bar/bar/foo"),
    String("/baz/foo/bar/bar/bar"),
    String("/baz/foo/bar/bar/baz"),
    String("/baz/foo/bar/baz/foo"),
    String("/baz/foo/bar/baz/bar"),
    String("/baz/foo/bar/baz/baz"),
    String("/baz/foo/baz/foo/foo"),
    String("/baz/foo/baz/foo/bar"),
    String("/baz/foo/baz/foo/baz"),
    String("/baz/foo/baz/bar/foo"),
    String("/baz/foo/baz/bar/bar"),
    String("/baz/foo/baz/bar/baz"),
    String("/baz/foo/baz/baz/foo"),
    String("/baz/foo/baz/baz/bar"),
    String("/baz/foo/baz/baz/baz"),
    String("/baz/bar/foo/foo/foo"),
    String("/baz/bar/foo/foo/bar"),
    String("/baz/bar/foo/foo/baz"),
    String("/baz/bar/foo/bar/foo"),
    String("/baz/bar/foo/bar/bar"),
    String("/baz/bar/foo/bar/baz"),
    String("/baz/bar/foo/baz/foo"),
    String("/baz/bar/foo/baz/bar"),
    String("/baz/bar/foo/baz/baz"),
    String("/baz/bar/bar/foo/foo"),
    String("/baz/bar/bar/foo/bar"),
    String("/baz/bar/bar/foo/baz"),
    String("/baz/bar/bar/bar/foo"),
    String("/baz/bar/bar/bar/bar"),
    String("/baz/bar/bar/bar/baz"),
    String("/baz/bar/bar/baz/foo"),
    String("/baz/bar/bar/baz/bar"),
    String("/baz/bar/bar/baz/baz"),
    String("/baz/bar/baz/foo/foo"),
    String("/baz/bar/baz/foo/bar"),
    String("/baz/bar/baz/foo/baz"),
    String("/baz/bar/baz/bar/foo"),
    String("/baz/bar/baz/bar/bar"),
    String("/baz/bar/baz/bar/baz"),
    String("/baz/bar/baz/baz/foo"),
    String("/baz/bar/baz/baz/bar"),
    String("/baz/bar/baz/baz/baz"),
    String("/baz/baz/foo/foo/foo"),
    String("/baz/baz/foo/foo/bar"),
    String("/baz/baz/foo/foo/baz"),
    String("/baz/baz/foo/bar/foo"),
    String("/baz/baz/foo/bar/bar"),
    String("/baz/baz/foo/bar/baz"),
    String("/baz/baz/foo/baz/foo"),
    String("/baz/baz/foo/baz/bar"),
    String("/baz/baz/foo/baz/baz"),
    String("/baz/baz/bar/foo/foo"),
    String("/baz/baz/bar/foo/bar"),
    String("/baz/baz/bar/foo/baz"),
    String("/baz/baz/bar/bar/foo"),
    String("/baz/baz/bar/bar/bar"),
    String("/baz/baz/bar/bar/baz"),
    String("/baz/baz/bar/baz/foo"),
    String("/baz/baz/bar/baz/bar"),
    String("/baz/baz/bar/baz/baz"),
    String("/baz/baz/baz/foo/foo"),
    String("/baz/baz/baz/foo/bar"),
    String("/baz/baz/baz/foo/baz"),
    String("/baz/baz/baz/bar/foo"),
    String("/baz/baz/baz/bar/bar"),
    String("/baz/baz/baz/bar/baz"),
    String("/baz/baz/baz/baz/foo"),
    String("/baz/baz/baz/baz/bar"),
    String("/baz/baz/baz/baz/baz"),
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

    vfs.registerFilesystem(ramfs.get(), String("ramfs"));
    vfs.setRootFilesystem(ramfs.get());

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

    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(vfs.find(g_ShallowPath));
    }

    state.SetItemsProcessed(int64_t(state.iterations()));

    vfs.unregisterFilesystem(ramfs.get(), false);
}

static void BM_VFSMediumDirectoryTraverse(benchmark::State &state)
{
    VFS vfs;
    auto ramfs = prepareVFS(vfs);

    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(vfs.find(g_MiddlePath));
    }

    state.SetItemsProcessed(int64_t(state.iterations()));

    vfs.unregisterFilesystem(ramfs.get(), false);
}

static void BM_VFSDeepDirectoryTraverse(benchmark::State &state)
{
    VFS vfs;
    auto ramfs = prepareVFS(vfs);

    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(vfs.find(g_DeepPath));
    }

    state.SetItemsProcessed(int64_t(state.iterations()));

    vfs.unregisterFilesystem(ramfs.get(), false);
}

static void BM_VFSRandomDirectoryTraverse(benchmark::State &state)
{
    VFS vfs;
    auto ramfs = prepareVFS(vfs);

    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(vfs.find(randomPath()));
    }

    state.SetItemsProcessed(int64_t(state.iterations()));

    vfs.unregisterFilesystem(ramfs.get(), false);
}

static void BM_VFSShallowDirectoryTraverseNoFs(benchmark::State &state)
{
    VFS vfs;
    auto ramfs = prepareVFS(vfs);

    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(vfs.find(g_ShallowPathNoFs, ramfs->getRoot()));
    }

    state.SetItemsProcessed(int64_t(state.iterations()));

    vfs.unregisterFilesystem(ramfs.get(), false);
}

static void BM_VFSMediumDirectoryTraverseNoFs(benchmark::State &state)
{
    VFS vfs;
    auto ramfs = prepareVFS(vfs);

    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(vfs.find(g_MiddlePathNoFs, ramfs->getRoot()));
    }

    state.SetItemsProcessed(int64_t(state.iterations()));

    vfs.unregisterFilesystem(ramfs.get(), false);
}

static void BM_VFSDeepDirectoryTraverseNoFs(benchmark::State &state)
{
    VFS vfs;
    auto ramfs = prepareVFS(vfs);

    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(vfs.find(g_DeepPathNoFs, ramfs->getRoot()));
    }

    state.SetItemsProcessed(int64_t(state.iterations()));

    vfs.unregisterFilesystem(ramfs.get(), false);
}

static void BM_VFSRandomDirectoryTraverseNoFs(benchmark::State &state)
{
    VFS vfs;
    auto ramfs = prepareVFS(vfs);

    while (state.KeepRunning())
    {
        /// \todo VFS::find() should be able to accept a StringView
        benchmark::DoNotOptimize(vfs.find(randomPath(), ramfs->getRoot()));
    }

    state.SetItemsProcessed(int64_t(state.iterations()));

    vfs.unregisterFilesystem(ramfs.get(), false);
}

BENCHMARK(BM_VFSDeepDirectoryTraverse);
BENCHMARK(BM_VFSMediumDirectoryTraverse);
BENCHMARK(BM_VFSShallowDirectoryTraverse);
BENCHMARK(BM_VFSRandomDirectoryTraverse);

BENCHMARK(BM_VFSDeepDirectoryTraverseNoFs);
BENCHMARK(BM_VFSMediumDirectoryTraverseNoFs);
BENCHMARK(BM_VFSShallowDirectoryTraverseNoFs);
BENCHMARK(BM_VFSRandomDirectoryTraverseNoFs);
