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

#ifndef KERNEL_UTILITIES_HASHTABLE_H
#define KERNEL_UTILITIES_HASHTABLE_H

#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/lib.h"

/** @addtogroup kernelutilities
 * @{ */

/**
 * Hash table class.
 *
 * Handles hash collisions by chaining keys.
 *
 * The key type 'K' should have a method hash() which returns a
 * size_t hash that can be used to index into the bucket array.
 *
 * The key type 'K' should also be able to compare against other 'K'
 * types for equality.
 *
 * \todo check InitialBuckets for is a power of two
 */
template <class K, class V, size_t InitialBuckets = 4>
class HashTable
{
    static_assert(
        InitialBuckets > 0, "At least one initial bucket must be available.");

  public:
    HashTable() : m_Buckets(nullptr), m_Default(), m_nBuckets(0), m_nItems(0), m_nMask(0)
    {
        check();
    }

    /**
     * Constructor with custom default value.
     */
    HashTable(const V &customDefault) : HashTable()
    {
        m_Default = customDefault;
    }

    /**
     * Clear the HashTable.
     */
    void clear()
    {
        delete [] m_Buckets;
        m_Buckets = nullptr;
        m_nItems = 0;
    }

    virtual ~HashTable()
    {
        clear();
    }

    /**
     * Do a lookup of the given key, and return either the value,
     * or NULL if the key is not in the hashtable.
     *
     * O(1) in the average case, with a hash function that rarely
     * collides.
     */
    const V &lookup(const K &k) const
    {
        if ((!m_Buckets) || (!m_nItems))
        {
            return m_Default;
        }

        size_t hash = k.hash() & m_nMask;

        const bucket *b = &m_Buckets[hash];
        if (!b->set)
        {
            return m_Default;
        }

        if (b->key != k)
        {
            b = findNextSet(hash, k);
            if (!b)
            {
                return m_Default;
            }
        }

        return b->value;
    }

    /**
     * Insert the given value with the given key.
     */
    bool insert(const K &k, const V &v)
    {
        check();

        // Handle resize and associated rehash if the table is full.
        if (m_nItems >= (m_nBuckets - 1))
        {
            size_t origCount = m_nBuckets;
            m_nBuckets *= 2;
            m_nMask = m_nBuckets - 1;
            rehash(origCount);
        }

        size_t hash = k.hash() & m_nMask;

        // Do we need to chain?
        bucket *b = &m_Buckets[hash];
        if (b->set)
        {
            // If key matches, this is more than just a hash collision.
            if (b->key == k)
            {
                return false;
            }

            // Probe for an empty bucket.
            b = findNextEmpty(hash);
            if (!b)
            {
                return false;
            }
        }

        b->set = true;
        b->key = k;
        b->value = v;
        ++m_nItems;

        return true;
    }

    /**
     * Remove the given key.
     */
    void remove(const K &k)
    {
        if (!m_Buckets)
        {
            return;
        }

        size_t hash = k.hash() & m_nMask;

        bucket *b = &m_Buckets[hash];
        if (!b->set)
        {
            return;
        }

        bool didRemove = false;

        if (b->key == k)
        {
            b->set = false;
            didRemove = true;
        }
        else
        {
            b = findNextSet(hash, k);
            if (b)
            {
                b->set = false;
                didRemove = true;
            }
        }

        if (didRemove)
        {
            --m_nItems;

            if (m_nItems)
            {
                // Must rehash as we use linear probing for collision handling.
                rehash();
            }
        }
    }

    size_t count() const
    {
        return m_nItems;
    }

    /**
     * Get the index of the next bucket to try from the given one.
     *
     * 'factor' is an in/out that specifies a factor to multiply by for
     * non-linear probes. 'mult' is a multiplier to apply to 'factor' after
     * calculation. Set 'mult' to 2 to get quadratic probes.
     */
    size_t calculateNextHash(size_t currentHash, size_t &factor, bool quadratic=false) const
    {
        size_t addend = factor;
        if (quadratic)
        {
            // Next factor = N^2
            addend = factor * factor;
        }
        size_t next = (currentHash + addend) & m_nMask;
        ++factor;
        return next;
    }

  private:
    /// Probe multiplier - 2 for quadratic probing.
    static const bool QuadraticProbe = true;

    struct bucket
    {
        bucket() : key(), value(), set(false)
        {
        }

        K key;
        V value;
        bool set;
    };

    void check()
    {
        if (m_Buckets == nullptr)
        {
            m_Buckets = new bucket[InitialBuckets];
            m_nBuckets = InitialBuckets;
            m_nMask = InitialBuckets - 1;
        }
    }

    void rehash(size_t oldCount = 0)
    {
        if (oldCount == 0)
        {
            oldCount = m_nBuckets;
        }

        bucket *oldBuckets = m_Buckets;
        m_Buckets = new bucket[m_nBuckets];
        if (m_nItems)
        {
            // Performing a new insert, clear out the number of items as
            // insert() will increment otherwise.
            m_nItems = 0;

            for (size_t i = 0; i < oldCount; ++i)
            {
                if (oldBuckets[i].set)
                {
                    insert(oldBuckets[i].key, oldBuckets[i].value);
                }
            }
        }
        delete [] oldBuckets;
    }

    size_t nextIndex(size_t i, size_t &index, size_t &step) const
    {
        if (QuadraticProbe)
        {
            index = (index + step) & m_nMask;
            ++step;
        }
        else
        {
            index = i;
        }

        return index;
    }

    bucket *findNextEmpty(size_t currentHash)
    {
        size_t index = 0;
        size_t step = 1;
        for (size_t i = 0; i < m_nBuckets; ++i)
        {
            size_t nextHash = (currentHash + nextIndex(i, index, step)) & m_nMask;
            bucket *b = &m_Buckets[nextHash];
            if (!b->set)
            {
                return b;
            }
        }

        return nullptr;
    }

    bucket *findNextSet(size_t currentHash, const K &k)
    {
        size_t index = 0;
        size_t step = 1;
        for (size_t i = 0; i < m_nBuckets; ++i)
        {
            size_t nextHash = (currentHash + nextIndex(i, index, step)) & m_nMask;
            bucket *b = &m_Buckets[nextHash];

            // Hash comparison is likely to be faster than raw object
            // comparison so we save the latter for when we have a candidate.
            if (b->set && (b->key.hash() == k.hash()))
            {
                if (b->key == k)
                {
                    return b;
                }
            }
        }

        return nullptr;
    }

    const bucket *findNextSet(size_t currentHash, const K &k) const
    {
        size_t index = 0;
        size_t step = 1;
        for (size_t i = 0; i < m_nBuckets; ++i)
        {
            size_t nextHash = (currentHash + nextIndex(i, index, step)) & m_nMask;
            const bucket *b = &m_Buckets[nextHash];

            // Hash comparison is likely to be faster than raw object
            // comparison so we save the latter for when we have a candidate.
            if (b->set && (b->key.hash() == k.hash()))
            {
                if (b->key == k)
                {
                    return b;
                }
            }
        }

        return nullptr;
    }

    bucket *m_Buckets;
    V m_Default;
    size_t m_nBuckets;
    size_t m_nItems;
    size_t m_nMask;
};

/** @} */

#endif
