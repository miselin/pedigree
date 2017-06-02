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

#include "pedigree/kernel/utilities/Tree.h"

template class Tree<void *, void *>;
template class Tree<int8_t, void *>;
template class Tree<int16_t, void *>;
template class Tree<int32_t, void *>;
template class Tree<int64_t, void *>;
template class Tree<uint8_t, void *>;
template class Tree<uint16_t, void *>;
template class Tree<uint32_t, void *>;
template class Tree<uint64_t, void *>;
template class Tree<int8_t, int8_t>;
template class Tree<int16_t, int16_t>;
template class Tree<int32_t, int32_t>;
template class Tree<int64_t, int64_t>;
template class Tree<uint8_t, uint8_t>;
template class Tree<uint16_t, uint16_t>;
template class Tree<uint32_t, uint32_t>;
template class Tree<uint64_t, uint64_t>;
