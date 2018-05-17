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

#ifndef KERNEL_UTILITIES_LRUCACHE_H
#define KERNEL_UTILITIES_LRUCACHE_H

/** @addtogroup kernelutilities
 * @{ */

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/utility.h"

/**
 * \brief LruCache provides a least-recently-used cache abstraction.
 *
 * A key type is used to find items within the cache.
 *
 * It is recommended that the type T is relatively small and has a low cost to
 * copy (a scalar type is best) for maximum performance.
 */
template <class K, class T, size_t Slots = 32>
class LruCache
{
    static_assert(Slots >= 4, "At least four slots are needed for LruCache.");

    private:
        struct Slot
        {
            K key;
            T object;
            bool set = false;
        };

    public:
        LruCache() = default;
        virtual ~LruCache() = default;

        /** Potentially get an item with the given key. */
        bool get(const K &key, T &object)
        {
            for (size_t i = 0; i < Slots; ++i)
            {
                if (!m_Slots[i].set)
                {
                    continue;
                }
                else if (m_Slots[i].key != key)
                {
                    continue;
                }

                object = m_Slots[i].object;
                return true;
            }

            return false;
        }

        /** Store an item as the most recently used item. */
        void store(const K &key, const T &object)
        {
            // Already the most recently used item.
            if (m_Slots[0].set && m_Slots[0].key == key)
            {
                return;
            }

            pedigree_std::copy(&m_Slots[1], &m_Slots[0], Slots - 1);
            m_Slots[0].key = key;
            m_Slots[0].object = object;
            m_Slots[0].set = true;
        }

    private:
        Slot m_Slots[Slots];
};

/** @} */

#endif  // KERNEL_UTILITIES_LRUCACHE_H
