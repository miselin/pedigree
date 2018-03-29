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

#ifndef KERNEL_UTILITIES_RADIX_TREE_H
#define KERNEL_UTILITIES_RADIX_TREE_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/Iterator.h"
#include "pedigree/kernel/utilities/IteratorAdapter.h"
#include "pedigree/kernel/utilities/Result.h"
#include "pedigree/kernel/utilities/String.h"

/**\file  RadixTree.h
 *\author James Molloy <jamesm@osdev.org>
 *\date   Fri May  8 10:50:45 2009
 *\brief  Implements a Radix Tree, a kind of Trie with compressed keys. */

/** @addtogroup kernelutilities
 * @{ */

/** Dictionary class, aka Map for string keys. This is
 *  implemented as a Radix Tree - also known as a Patricia Trie.
 * \brief A key/value dictionary for string keys. */
template <class T>
class EXPORTED_PUBLIC RadixTree
{
  private:
    /** Tree node. */
    class Node
    {
      public:
        typedef Vector<Node *> childlist_t;
        enum MatchType
        {
            ExactMatch,    ///< Key matched node key exactly.
            NoMatch,       ///< Key didn't match node key at all.
            PartialMatch,  ///< A subset of key matched the node key.
            OverMatch      ///< Key matched node key, and had extra characters.
        };

        Node(bool bCaseSensitive)
            : m_Key(), value(T()), m_Children(), m_pParent(0),
              m_bCaseSensitive(bCaseSensitive), m_pParentTree(0),
              m_bHasValue(false)
        {
        }

        ~Node();

        // Clears all known children and returns them to the parent ObjectPool
        void returnAllChildren();

        /** Get the next data structure in the list
         *\return pointer to the next data structure in the list */
        Node *next()
        {
            return doNext();
        }
        /** Get the previous data structure in the list
         *\return pointer to the previous data structure in the list
         *\note Not implemented! */
        Node *previous()
        {
            return 0;
        }

        /** Locates a child of this node, given the key portion of key
            (lookahead on the first token) */
        Node *findChild(const char *cpKey) const;

        /** Adds a new child. */
        void addChild(Node *pNode);

        /** Replaces a currently existing child. */
        void replaceChild(Node *pNodeOld, Node *pNodeNew);

        /** Removes a child (doesn't delete it) */
        void removeChild(Node *pChild);

        /** Compares cpKey and this node's key, returning the type of match
            found. The offset out parameter provides the position where a
            partial match ceases. */
        MatchType matchKey(const char *cpKey, size_t &offset) const;

        /** Returns the first found child of the node. */
        Node *getFirstChild() const;

        /** Sets the node's key to the concatenation of \p cpKey and the
         *  current key.
         *\param cpKey Key to prepend to the current key. */
        void prependKey(const char *cpKey);

        void setKey(const char *cpKey);
        /** If you know the length of cpKey, this can be a small boost. */
        void setKey(const char *cpKey, size_t lengthHint);
        inline const char *getKey() const
        {
            return m_Key;
        }
        inline void setValue(const T &pV)
        {
            value = pV;
            m_bHasValue = true;
        }
        inline void removeValue()
        {
            value = T();
            m_bHasValue = false;
        }
        inline const T &getValue() const
        {
            return value;
        }
        inline void setParent(Node *pP)
        {
            m_pParent = pP;
        }
        inline Node *getParent() const
        {
            return m_pParent;
        }
        inline bool hasValue() const
        {
            return m_bHasValue;
        }

        void dump(void (*emit_line)(const char *s)) const;

        /** Node key, zero terminated. */
        String m_Key;
        /** Node value.
            \note Parting from coding standard because Iterator requires the
                  member be called 'value'. */
        T value;
        /** Array of 16 pointers to 16 nodes (256 total). */
        childlist_t m_Children;
        /** Parent node. */
        Node *m_pParent;
        /** Controls case-sensitive matching. */
        const bool m_bCaseSensitive;

        /** Link back to the node's RadixTree instance. */
        RadixTree *m_pParentTree;

        /** Do we have a value?
         * Some nodes are intermediates caused by a split but don't yet have a
         * value, and it is incorrect to return something for them.
         */
        bool m_bHasValue;

      private:
        Node(const Node &);
        Node &operator=(const Node &);

        /** Returns the next Node to look at during an in-order iteration. */
        Node *doNext() const;

        /** Returns the node's next sibling, by looking at its parent's
         * children. */
        Node *getNextSibling() const;
    };

  public:
    /** Type of the bidirectional iterator */
    typedef ::Iterator<T, Node> Iterator;
    /** Type of the constant bidirectional iterator */
    typedef typename Iterator::Const ConstIterator;
    typedef Result<T, bool> LookupType;

    /** The default constructor, does nothing */
    RadixTree();
    /** The copy-constructor
     *\param[in] x the reference object to copy */
    RadixTree(const RadixTree<T> &x);
    /** Constructor that offers case sensitivity adjustment. */
    RadixTree(bool bCaseSensitive);
    /** The destructor, deallocates memory */
    ~RadixTree();

    /** The assignment operator
     *\param[in] x the object that should be copied */
    RadixTree &operator=(const RadixTree &x);

    /** Get the number of elements in the Tree
     *\return the number of elements in the Tree */
    size_t count() const;
    /** Add an element to the Tree.
     *\param[in] key the key
     *\param[in] value the element */
    void insert(const String &key, const T &value);
    /** Attempts to find an element with the given key.
     *\return A Result that either has the found item, or an error if the item
     *        is not found */
    Result<T, bool> lookup(const String &key) const;
    /** Attempts to remove an element with the given key. */
    void remove(const String &key);

    /** Clear the tree. */
    void clear();

    /** Erase one Element */
    Iterator erase(Iterator iter)
    {
        Node *iterNode = iter.__getNode();
        Node *next = iterNode->next();
        remove(String(iterNode->getKey()));
        Iterator ret(next);
        return ret;
    }

    /** Get an iterator pointing to the beginning of the List
     *\return iterator pointing to the beginning of the List */
    inline Iterator begin()
    {
        if (!m_pRoot)
            return Iterator(0);
        Iterator it(m_pRoot->next());
        return it;
    }
    /** Get a constant iterator pointing to the beginning of the List
     *\return constant iterator pointing to the beginning of the List */
    inline ConstIterator begin() const
    {
        if (!m_pRoot)
            return ConstIterator(0);
        ConstIterator it(m_pRoot->next());
        return it;
    }
    /** Get an iterator pointing to the end of the List + 1
     *\return iterator pointing to the end of the List + 1 */
    inline Iterator end()
    {
        return Iterator(0);
    }
    /** Get a constant iterator pointing to the end of the List + 1
     *\return constant iterator pointing to the end of the List + 1 */
    inline ConstIterator end() const
    {
        return ConstIterator(0);
    }

    /** Dump the RadixTree in dot format. */
    void dump(void (*emit_line)(const char *s)) const;

  private:
    /** Internal function to create a copy of a subtree. */
    Node *cloneNode(Node *node, Node *parent);
    /** Obtain a new Node with the same case-sensitive flag. */
    Node *getNewNode()
    {
        Node *p = m_NodePool.allocate(m_bCaseSensitive);
        p->m_pParentTree = this;
        return p;
    }
    /** Return a Node so it can be allocated again. */
    void returnNode(Node *p)
    {
        if (p)
        {
            // wipe out info that shouldn't go back to the pool
            p->m_Key = String();
            p->value = T();
            p->m_bHasValue = false;
            m_NodePool.deallocate(p);
        }
    }

    /** Number of items in the tree. */
    size_t m_nItems;
    /** The tree's root. */
    Node *m_pRoot;
    /** Whether matches are case-sensitive or not. */
    const bool m_bCaseSensitive;
    /** Pool of node objects (to reduce impact of lots of node allocs/deallocs).
     */
    ObjectPool<Node> m_NodePool;
};

template <class T>
RadixTree<T>::RadixTree()
    : m_nItems(0), m_pRoot(0), m_bCaseSensitive(true), m_NodePool()
{
}

template <class T>
RadixTree<T>::RadixTree(bool bCaseSensitive)
    : m_nItems(0), m_pRoot(0), m_bCaseSensitive(bCaseSensitive), m_NodePool()
{
}

template <class T>
RadixTree<T>::~RadixTree()
{
    clear();
    returnNode(m_pRoot);
}

template <class T>
RadixTree<T>::RadixTree(const RadixTree &x)
    : m_nItems(0), m_pRoot(0), m_bCaseSensitive(x.m_bCaseSensitive),
      m_NodePool()
{
    clear();
    returnNode(m_pRoot);
    m_pRoot = cloneNode(x.m_pRoot, 0);
    m_nItems = x.m_nItems;
}

template <class T>
RadixTree<T> &RadixTree<T>::operator=(const RadixTree &x)
{
    clear();
    returnNode(m_pRoot);
    m_pRoot = cloneNode(x.m_pRoot, 0);
    m_nItems = x.m_nItems;
    /// \todo check for incompatible case-sensitivity?
    return *this;
}

template <class T>
size_t RadixTree<T>::count() const
{
    return m_nItems;
}

template <class T>
void RadixTree<T>::insert(const String &key, const T &value)
{
    if (!m_pRoot)
    {
        // The root node always exists and is a lambda transition node
        // (zero-length key). This removes the need for most special cases.
        m_pRoot = getNewNode();
        m_pRoot->setKey(0);
    }

    Node *pNode = m_pRoot;

    const char *cpKey = static_cast<const char *>(key);
    const char *cpKeyOrig = cpKey;
    size_t cpKeyLength = key.length();

    while (true)
    {
        size_t partialOffset = 0;
        switch (pNode->matchKey(cpKey, partialOffset))
        {
            case Node::ExactMatch:
            {
                if (!pNode->hasValue())
                {
                    m_nItems++;
                }

                pNode->setValue(value);
                return;
            }
            case Node::NoMatch:
            {
                FATAL("RadixTree: algorithmic error!");
                break;
            }
            case Node::PartialMatch:
            {
                // We need to create an intermediate node that contains the
                // partial match, then adjust the key of this node.

                // Find the common key prefix.
                size_t i = partialOffset;

                Node *pInter = getNewNode();

                // Intermediate node's key is the common prefix of both keys.
                pInter->m_Key.assign(cpKey, partialOffset);

                // Must do this before pNode's key is changed.
                pNode->getParent()->replaceChild(pNode, pInter);

                // pNode's new key is the uncommon postfix.
                size_t len = pNode->m_Key.length();

                // Note: this is guaranteed to not require an allocation,
                // because it's smaller than the current string in m_Key. We'll
                // not overwrite because we're copying from deeper in the
                // string. The null write will suffice.
                pNode->m_Key.assign(
                    &pNode->getKey()[partialOffset], len - partialOffset);

                // If the uncommon postfix of the key is non-zero length, we
                // have to create another node, a child of pInter.
                if (cpKey[partialOffset] != 0)
                {
                    Node *pChild = getNewNode();
                    pChild->setKey(
                        &cpKey[partialOffset],
                        (cpKeyLength - partialOffset - (cpKey - cpKeyOrig)));
                    pChild->setValue(value);
                    pChild->setParent(pInter);
                    pInter->addChild(pChild);
                }
                else
                {
                    pInter->setValue(value);
                }

                pInter->setParent(pNode->getParent());
                pInter->addChild(pNode);
                pNode->setParent(pInter);

                m_nItems++;
                return;
            }
            case Node::OverMatch:
            {
                cpKey += pNode->m_Key.length();

                Node *pChild = pNode->findChild(cpKey);
                if (pChild)
                {
                    pNode = pChild;
                    // Iterative case.
                    break;
                }
                else
                {
                    // No child - create a new one.
                    pChild = getNewNode();
                    pChild->setKey(cpKey, cpKeyLength - (cpKey - cpKeyOrig));
                    pChild->setValue(value);
                    pChild->setParent(pNode);
                    pNode->addChild(pChild);

                    m_nItems++;
                    return;
                }
            }
        }
    }
}

template <class T>
Result<T, bool> RadixTree<T>::lookup(const String &key) const
{
    if (!m_pRoot)
    {
        return LookupType::withError(true);
    }

    Node *pNode = m_pRoot;

    const char *cpKey = static_cast<const char *>(key);

    while (true)
    {
        size_t offset = 0;
        switch (pNode->matchKey(cpKey, offset))
        {
            case Node::ExactMatch:
                if (!pNode->hasValue())
                {
                    // No value here, exact match on key. This can happen in
                    // cases where we needed to create a node to split a key
                    // but nothing was attached to the split node.
                    return LookupType::withError(true);
                }
                return LookupType::withValue(pNode->getValue());
            case Node::NoMatch:
            case Node::PartialMatch:
                return LookupType::withError(true);
            case Node::OverMatch:
            {
                cpKey += pNode->m_Key.length();

                Node *pChild = pNode->findChild(cpKey);
                if (pChild)
                {
                    pNode = pChild;
                    // Iterative case.
                    break;
                }
                else
                {
                    return LookupType::withError(true);
                }
            }
        }
    }
}

template <class T>
void RadixTree<T>::remove(const String &key)
{
    if (!m_pRoot)
    {
        // The root node always exists and is a lambda transition node
        // (zero-length key). This removes the need for most special cases.
        m_pRoot = getNewNode();
        m_pRoot->setKey(0);
    }

    Node *pNode = m_pRoot;

    const char *cpKey = static_cast<const char *>(key);

    // Our invariant is that the root node always exists. Therefore we must
    // special case here so it doesn't get deleted.
    if (*cpKey == 0)
    {
        m_pRoot->removeValue();
        return;
    }

    while (true)
    {
        size_t offset = 0;
        switch (pNode->matchKey(cpKey, offset))
        {
            case Node::ExactMatch:
            {
                // Delete this node. If we set the value to zero, it is
                // effectively removed from the map. There are only certain
                // cases in which we can delete the node completely, however.
                pNode->removeValue();
                m_nItems--;

                // We have the invariant that the tree is always optimised. This
                // means that when we delete a node we only have to optimise the
                // local branch. There are two situations that need covering:
                //   (a) No children. This means it's a leaf node and can be
                //   deleted. We then need to consider its parent,
                //       which, if its value is zero and now has zero or one
                //       children can be optimised itself.
                //   (b) One child. This is a linear progression and the child
                //   node's key can be changed to be concatenation of
                //       pNode's and its. This doesn't affect anything farther
                //       up the tree and so no recursion is needed.

                Node *pParent = 0;
                if (pNode->m_Children.count() == 0)
                {
                    // Leaf node, can just delete.
                    pParent = pNode->getParent();
                    pParent->removeChild(pNode);
                    returnNode(pNode);

                    pNode = pParent;
                    // Optimise up the tree.
                    while (true)
                    {
                        if (pNode == m_pRoot)
                            return;

                        if (pNode->m_Children.count() == 1 && !pNode->hasValue())
                            // Break out of this loop and get caught in the next
                            // if(pNode->m_nChildren == 1)
                            break;

                        if (pNode->m_Children.count() == 0 && !pNode->hasValue())
                        {
                            // Leaf node, can just delete.
                            pParent = pNode->getParent();
                            pParent->removeChild(pNode);
                            returnNode(pNode);

                            pNode = pParent;
                            continue;
                        }
                        return;
                    }
                }

                if (pNode->m_Children.count() == 1)
                {
                    // Change the child's key to be the concatenation of ours
                    // and its.
                    Node *pChild = pNode->getFirstChild();
                    pParent = pNode->getParent();

                    // Must call this before delete, so pChild doesn't get
                    // deleted.
                    pNode->removeChild(pChild);

                    pChild->prependKey(pNode->getKey());
                    pChild->setParent(pParent);
                    pParent->removeChild(pNode);
                    pParent->addChild(pChild);

                    returnNode(pNode);
                }
                return;
            }
            case Node::NoMatch:
            case Node::PartialMatch:
                // Can't happen unless the key didn't actually exist.
                return;
            case Node::OverMatch:
            {
                cpKey += pNode->m_Key.length();

                Node *pChild = pNode->findChild(cpKey);
                if (pChild)
                {
                    pNode = pChild;
                    // Iterative case.
                    break;
                }
                else
                    return;
            }
        }
    }
}

template <class T>
typename RadixTree<T>::Node *RadixTree<T>::cloneNode(Node *pNode, Node *pParent)
{
    // Deal with the easy case first.
    if (!pNode)
        return 0;

    Node *n = getNewNode();
    n->setKey(pNode->m_Key);
    n->setValue(pNode->value);
    n->setParent(pParent);

    for (typename RadixTree<T>::Node::childlist_t::Iterator it =
             pNode->m_Children.begin();
         it != pNode->m_Children.end(); ++it)
    {
        n->addChild(cloneNode((*it), pParent));
    }

    return n;
}

template <class T>
void RadixTree<T>::clear()
{
    returnNode(m_pRoot);
    m_pRoot = getNewNode();
    m_pRoot->setKey(0);
    m_nItems = 0;
}

template <class T>
void RadixTree<T>::dump(void (*emit_line)(const char *s)) const
{
    if (m_pRoot)
    {
        m_pRoot->dump(emit_line);
    }
}

//
// RadixTree::Node implementation.
//

template <class T>
RadixTree<T>::Node::~Node()
{
    returnAllChildren();
}

template <class T>
void RadixTree<T>::Node::returnAllChildren()
{
    // Returns depth-first, which ensures we're not returning any children
    // while the ObjectPool lock is held.
    for (auto it : m_Children)
    {
        it->returnAllChildren();
        m_pParentTree->returnNode(it);
    }

    m_Children.clear();
}

template <class T>
typename RadixTree<T>::Node *
RadixTree<T>::Node::findChild(const char *cpKey) const
{
    for (auto it : m_Children)
    {
        size_t offset = 0;
        if (it->matchKey(cpKey, offset) != NoMatch)
        {
            return it;
        }
    }
    return 0;
}

template <class T>
void RadixTree<T>::Node::addChild(Node *pNode)
{
    if (pNode)
        m_Children.pushBack(pNode);
}

template <class T>
void RadixTree<T>::Node::replaceChild(Node *pNodeOld, Node *pNodeNew)
{
    for (auto it = m_Children.begin(); it != m_Children.end(); ++it)
    {
        if (*it == pNodeOld)
        {
            *it = pNodeNew;
            break;
        }
    }
}

template <class T>
void RadixTree<T>::Node::removeChild(Node *pChild)
{
    for (typename RadixTree<T>::Node::childlist_t::Iterator it =
             m_Children.begin();
         it != m_Children.end();)
    {
        if ((*it) == pChild)
        {
            it = m_Children.erase(it);
        }
        else
            ++it;
    }
}

template <class T>
typename RadixTree<T>::Node::MatchType
RadixTree<T>::Node::matchKey(const char *cpKey, size_t &offset) const
{
    // Cannot partially match (e.g. toast/toastier == PartialMatch if the
    // cpKey is longer (e.g. toastier/toast == OverMatch)).
    if (!m_Key.length())
    {
        return OverMatch;
    }

    const char *myKey = getKey();

    // Do some quick checks early.
    if ((m_bCaseSensitive && (cpKey[0] != myKey[0])) || ((!m_bCaseSensitive) && (toLower(cpKey[0]) != toLower(myKey[0]))))
    {
        // non-partial, first character didn't even match
        return NoMatch;
    }

    size_t i = 0;
    int r = StringCompareCase(
        cpKey, myKey, m_bCaseSensitive, m_Key.length() + 1, &i);

    if (r == 0)
    {
        // exact match, all characters & null terminators matched
        return ExactMatch;
    }
    else if (m_Key[i] == 0)
    {
        return OverMatch;
    }
    else
    {
        // partial, both strings share a common prefix
        offset = i;
        return PartialMatch;
    }
}

template <class T>
void RadixTree<T>::Node::setKey(const char *cpKey)
{
    m_Key.assign(cpKey);
}

template <class T>
void RadixTree<T>::Node::setKey(const char *cpKey, size_t lengthHint)
{
    m_Key.assign(cpKey, lengthHint);
}

template <class T>
typename RadixTree<T>::Node *RadixTree<T>::Node::getFirstChild() const
{
    return *(m_Children.begin());
}

template <class T>
void RadixTree<T>::Node::prependKey(const char *cpKey)
{
    String temp = m_Key;
    m_Key.assign(cpKey);
    m_Key += temp;
}

template <class T>
typename RadixTree<T>::Node *RadixTree<T>::Node::doNext() const
{
    // pNode needs to be settable, but not what it points to!
    Node const *pNode = this;
    while ((pNode == this) || (pNode && (!pNode->value)))
    {
        Node const *tmp;
        if (pNode->m_Children.count())
            pNode = pNode->getFirstChild();
        else
        {
            tmp = pNode;
            pNode = 0;
            while (tmp && tmp->m_pParent != 0 /* Root node */)
            {
                if ((pNode = tmp->getNextSibling()) != 0)
                    break;
                tmp = tmp->m_pParent;
            }
            if (tmp->m_pParent == 0)
                return 0;
        }
    }
    return const_cast<Node *>(pNode);
}

template <class T>
typename RadixTree<T>::Node *RadixTree<T>::Node::getNextSibling() const
{
    if (!m_pParent)
        return 0;

    bool b = false;
    for (typename RadixTree<T>::Node::childlist_t::Iterator it =
             m_pParent->m_Children.begin();
         it != m_pParent->m_Children.end(); ++it)
    {
        if (b)
            return (*it);
        if ((*it) == this)
            b = true;
    }

    return 0;
}

template <class T>
void RadixTree<T>::Node::dump(void (*emit_line)(const char *s)) const
{
    for (auto it : m_Children)
    {
        // depth-first dump the next node
        it->dump(emit_line);

        // dump this connection
        String s;
        s.Format(
            "  \"Node<%p: %s>\" -> \"Node<%p: %s>\";", it, it->getKey(), this,
            static_cast<const char *>(m_Key));
        emit_line(static_cast<const char *>(s));
    }
}

// Explicitly instantiate RadixTree<void*> early.
extern template class RadixTree<void *>;

/** @} */

#endif
