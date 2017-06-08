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

#ifndef HASH_TABLE_ON_STACK
#define HASH_TABLE_ON_STACK 0
#endif

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
 */
template <class K, class V, size_t Buckets = 100, V Default = V()>
class HashTable
{
  public:
    /**
     * To determine how many buckets you need, simply identify the
     * upper bound of the output of your hash function.
     *
     * Hashes that fall outside this range will be silently ignored.
     */
    HashTable()
    {
        if (!Buckets)
        {
            return;
        }

#if !HASH_TABLE_ON_STACK
        m_Buckets = new bucket[Buckets];
#endif

        for (size_t i = 0; i < Buckets; ++i)
        {
            m_Buckets[i].set = false;
        }
    }

    /**
     * Clear the HashTable.
     */
    void clear()
    {
        if (!Buckets)
        {
            return;
        }

        for (size_t i = 0; i < Buckets; ++i)
        {
            if (!m_Buckets[i].set)
            {
                continue;
            }

            bucket *b = m_Buckets[i].next;
            while (b)
            {
                bucket *d = b;
                b = b->next;
                delete d;
            }

            m_Buckets[i].set = false;
            m_Buckets[i].value = Default;
        }
    }

    virtual ~HashTable()
    {
        if (!Buckets)
        {
            return;
        }

        clear();

#if !HASH_TABLE_ON_STACK
        delete [] m_Buckets;
#endif
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
        if (!Buckets)
        {
            return m_Default;
        }

        size_t hash = k.hash() % Buckets;

        const bucket *b = &m_Buckets[hash];
        if (!b->set)
        {
            return m_Default;
        }

        if (b->key != k)
        {
            // Search the chain.
            bucket *chain = b->next;
            while (chain)
            {
                if (chain->key == k)
                {
                    b = chain;
                    break;
                }

                chain = chain->next;
            }

            if (!chain)
            {
                return m_Default;
            }
        }

        return b->value;
    }

    /**
     * Insert the given value with the given key.
     */
    bool insert(const K &k, const V &v, bool chain = true)
    {
        if (!Buckets)
        {
            return true;
        }

        size_t hash = k.hash() % Buckets;

        // Do we need to chain?
        if (m_Buckets[hash].set)
        {
            // If key matches, this is more than a hash collision.
            if (m_Buckets[hash].key == k)
            {
                return false;
            }

            // Yes.
            bucket *newb = new bucket;
            newb->key = k;
            newb->value = v;
            newb->set = true;
            newb->next = 0;

            bucket *bucket = &m_Buckets[hash];
            while (bucket->next)
            {
                bucket = bucket->next;
            }

            bucket->next = newb;
        }
        else
        {
            m_Buckets[hash].set = true;
            m_Buckets[hash].key = k;
            m_Buckets[hash].value = v;
            m_Buckets[hash].next = 0;
        }

        return true;
    }

    /**
     * Remove the given key.
     */
    void remove(const K &k)
    {
        if (!Buckets)
        {
            return;
        }

        size_t hash = k.hash() % Buckets;

        bucket *b = &m_Buckets[hash];
        if (!b->set)
        {
            return;
        }

        if (b->key == k)
        {
            if (b->next)
            {
                // Carry the next entry into this position.
                bucket *next = b->next;
                m_Buckets[hash] = *next;
                delete next;
            }
            else
            {
                // This entry is available, no chain present.
                m_Buckets[hash].set = false;
            }
        }
        else
        {
            // There's a chain, so we need to find the correct key in it.
            bucket *p = b;
            bucket *l = p;
            while (p)
            {
                if (p->key == k)
                {
                    l->next = p->next;
                    delete p;
                    break;
                }

                p = p->next;
            }
        }
    }

  private:
    struct bucket
    {
        bucket() : key(), value(Default), next(nullptr), set(false)
        {
        }

        K key;
        V value;

        // Where hash collisions occur, we chain another value
        // to the original bucket.
        bucket *next;

        bool set;
    };

#if HASH_TABLE_ON_STACK
    bucket m_Buckets[Buckets];
#else
    bucket *m_Buckets;
#endif
    V m_Default = Default;
};

/** @} */

#endif
