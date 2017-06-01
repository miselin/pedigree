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

#include "AtaController.h"
#include "AtaDisk.h"

bool AtaController::compareRequests(
    const RequestQueue::Request &a, const RequestQueue::Request &b)
{
    // Request type, ATA disk, and request location match.
    if (a.p2 != b.p2)
    {
        return false;
    }
    else if (a.p1 != b.p1)
    {
        return false;
    }

    AtaDisk *pDisk = reinterpret_cast<AtaDisk *>(a.p2);

    // Align location to block size before comparing, as the disks only do
    // operations on aligned locations (and so we should compare the same
    // here to reduce duplication).
    uint64_t a_aligned_location = a.p3 & ~(pDisk->getBlockSize() - 1);
    uint64_t b_aligned_location = b.p3 & ~(pDisk->getBlockSize() - 1);

    return a_aligned_location == b_aligned_location;
}
