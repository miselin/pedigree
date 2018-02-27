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

#include <list>

#include <gtest/gtest.h>

#include "pedigree/kernel/utilities/HashTable.h"

template <int hashModulo = 0>
class HashableIntegerBase
{
  public:
    HashableIntegerBase() : x_(-1)
    {
    }

    HashableIntegerBase(int x) : x_(x)
    {
    }

    virtual ~HashableIntegerBase()
    {
    }

    virtual size_t hash() const
    {
        if (hashModulo)
        {
            return x_ % hashModulo;
        }
        else
        {
            return x_;
        }
    }

    virtual bool operator==(const HashableIntegerBase &other) const
    {
        return x_ == other.x_;
    }

    virtual bool operator!=(const HashableIntegerBase &other) const
    {
        return x_ != other.x_;
    }

  private:
    int x_;
};

class CollidingHashableInteger : public HashableIntegerBase<0>
{
  public:
    CollidingHashableInteger() : HashableIntegerBase()
    {
    }

    CollidingHashableInteger(int x) : HashableIntegerBase(x)
    {
    }

    size_t hash() const
    {
        return 1;
    }
};

class HashableInteger : public HashableIntegerBase<0>
{
  public:
    HashableInteger() : HashableIntegerBase()
    {
    }

    HashableInteger(int x) : HashableIntegerBase(x)
    {
    }
};

class ModuloTenHashableInteger : public HashableIntegerBase<10>
{
  public:
    ModuloTenHashableInteger() : HashableIntegerBase()
    {
    }

    ModuloTenHashableInteger(int x) : HashableIntegerBase(x)
    {
    }
};

TEST(PedigreeHashTable, NoOpRemoval)
{
    HashTable<HashableInteger, int> hashtable(-1);

    HashableInteger key(0);
    EXPECT_EQ(hashtable.lookup(key), -1);

    hashtable.remove(key);

    EXPECT_EQ(hashtable.lookup(key), -1);
}

TEST(PedigreeHashTable, AnotherNoOpRemoval)
{
    HashTable<HashableInteger, int> hashtable(-1);

    HashableInteger key(3);
    EXPECT_EQ(hashtable.lookup(key), -1);

    hashtable.remove(key);

    EXPECT_EQ(hashtable.lookup(key), -1);
}

TEST(PedigreeHashTable, RemoveInserted)
{
    HashTable<HashableInteger, int> hashtable;

    HashableInteger key(3);
    hashtable.insert(key, 5);

    EXPECT_EQ(hashtable.lookup(key), 5);

    hashtable.remove(key);

    EXPECT_EQ(hashtable.lookup(key), 0);
}

TEST(PedigreeHashTable, InsertedAlready)
{
    HashTable<HashableInteger, int> hashtable;

    HashableInteger key(0);

    hashtable.insert(key, 5);
    hashtable.insert(key, 6);
    EXPECT_EQ(hashtable.lookup(key), 5);
}

TEST(PedigreeHashTable, CollidingHashes)
{
    HashTable<CollidingHashableInteger, int> hashtable;

    CollidingHashableInteger key1(0), key2(1);

    hashtable.insert(key1, 5);
    hashtable.insert(key2, 6);
    EXPECT_EQ(hashtable.lookup(key1), 5);
    EXPECT_EQ(hashtable.lookup(key2), 6);
}

TEST(PedigreeHashTable, InsertionNoChains)
{
    HashTable<HashableInteger, int> hashtable;

    for (size_t i = 0; i < 10; ++i)
    {
        HashableInteger key(i);
        hashtable.insert(key, 5 + i);
    }

    for (size_t i = 0; i < 10; ++i)
    {
        HashableInteger key(i);
        EXPECT_EQ(hashtable.lookup(key), 5 + i);
    }
}

TEST(PedigreeHashTable, InsertionWithChains)
{
    HashTable<ModuloTenHashableInteger, int> hashtable;

    for (size_t i = 0; i < 20; ++i)
    {
        ModuloTenHashableInteger key(i);
        EXPECT_TRUE(hashtable.insert(key, 5 + i));
    }

    for (size_t i = 0; i < 20; ++i)
    {
        ModuloTenHashableInteger key(i);
        EXPECT_EQ(hashtable.lookup(key), 5 + i);
    }
}

TEST(PedigreeHashTable, RemoveChained)
{
    HashTable<CollidingHashableInteger, int, 4> hashtable(-1);

    CollidingHashableInteger key1(0), key2(1), key3(2);

    EXPECT_TRUE(hashtable.insert(key1, 1));
    EXPECT_TRUE(hashtable.insert(key2, 2));
    EXPECT_TRUE(hashtable.insert(key3, 3));

    hashtable.remove(key2);

    EXPECT_EQ(hashtable.lookup(key1), 1);
    EXPECT_EQ(hashtable.lookup(key2), -1);
    EXPECT_EQ(hashtable.lookup(key3), 3);
}

TEST(PedigreeHashTable, RemoveFirstInChain)
{
    HashTable<CollidingHashableInteger, int> hashtable;

    CollidingHashableInteger key1(0), key2(1);

    hashtable.insert(key1, 1);
    hashtable.insert(key2, 2);

    hashtable.remove(key1);

    EXPECT_EQ(hashtable.lookup(key1), 0);
    EXPECT_EQ(hashtable.lookup(key2), 2);
}

TEST(PedigreeHashTable, ForwardIteration)
{
    HashTable<CollidingHashableInteger, int> hashtable(1234);

    CollidingHashableInteger key1(0), key2(1), key3(2), key4(3);

    EXPECT_TRUE(hashtable.insert(key1, 1));
    EXPECT_TRUE(hashtable.insert(key2, 2));
    EXPECT_TRUE(hashtable.insert(key3, 3));
    EXPECT_TRUE(hashtable.insert(key4, 4));

    std::list<int> results;
    std::list<int> expected = {1, 2, 3, 4};
    for (auto it = hashtable.begin(); it != hashtable.end(); ++it)
    {
        results.push_back(*it);
    }
    results.sort();

    EXPECT_EQ(results, expected);
}
