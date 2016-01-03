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

#include "TcpManager.h"
#include "TcpMisc.h"
#include "TcpStateBlock.h"
#include <LockGuard.h>

int stateBlockFree(void* p)
{
  StateBlock* stateBlock = reinterpret_cast<StateBlock*>(p);
  TcpManager::instance().removeConn(stateBlock->connId);
  return 0;
}

size_t TcpBuffer::write(uintptr_t buffer, size_t nBytes)
{
    LockGuard<Mutex> guard(m_Lock);

    // Validation
    if(!m_Buffer || !m_BufferSize)
        return 0;

    // Is the buffer full?
    if(m_DataSize >= m_BufferSize)
        return 0;

    // If adding this data will cause an overflow, limit the write
    if((nBytes + m_DataSize) > m_BufferSize)
        nBytes = m_BufferSize - m_DataSize;

    // If there's still too many bytes, restrict further
    if(nBytes > m_BufferSize)
        nBytes = m_BufferSize;

    // If however there's no more room, we can't write
    if(!nBytes)
        return 0;

    // Can we just write the whole thing?
    if((m_Writer + nBytes) < m_BufferSize)
    {
        // Yes, so just copy directly into the buffer
        memcpy(reinterpret_cast<void*>(m_Buffer+m_Writer),
               reinterpret_cast<void*>(buffer),
               nBytes);
        m_Writer += nBytes;
        m_DataSize += nBytes;
        return nBytes;
    }
    else
    {
        // This write will overlap the buffer
        size_t numNormalBytes = m_BufferSize - m_Writer;
        size_t numOverlapBytes = (m_Writer + nBytes) % m_BufferSize;

        // Does the write overlap the reader position?
        if(numOverlapBytes >= m_Reader && m_Reader != 0)
            numOverlapBytes = m_Reader - 1;
        // Has the reader position progressed, at all?
        else if(m_Reader == 0 && m_DataSize == 0)
            numOverlapBytes = 0;

        // Copy the normal bytes
        if(numNormalBytes)
            memcpy(reinterpret_cast<void*>(m_Buffer+m_Writer),
                   reinterpret_cast<void*>(buffer),
                   numNormalBytes);
        if(numOverlapBytes)
            memcpy(reinterpret_cast<void*>(m_Buffer),
                   reinterpret_cast<void*>(buffer),
                   numOverlapBytes);

        // Update the writer position, if needed
        if(numOverlapBytes)
            m_Writer = numOverlapBytes;

        // Return the number of bytes written
        m_DataSize += numNormalBytes + numOverlapBytes;
        return numNormalBytes + numOverlapBytes;
    }
}

size_t TcpBuffer::read(uintptr_t buffer, size_t nBytes, bool bDoNotMove)
{
    LockGuard<Mutex> guard(m_Lock);

    // Verify that we will actually be able to read this data
    if(!m_Buffer || !m_BufferSize)
        return 0;

    // Do not read past the end of the allocated buffer
    if(nBytes > m_BufferSize)
        nBytes = m_BufferSize;

    // And do not read more than the data that is already in the buffer
    if(nBytes > m_DataSize)
        nBytes = m_DataSize;

    // If either of these checks cause nBytes to be zero, just return
    if(!nBytes)
        return 0;

    // Can we just read the whole thing?
    if((m_Reader + nBytes) < m_BufferSize)
    {
        // Limit the number of bytes to the writer position
        if(m_Writer == 0 && m_Reader == 0)
            return 0; // No data to read
        else if(m_Writer == m_Reader && m_DataSize == 0) // If there's data, the writer has wrapped around
            return 0; // Reader == Writer, no data
        else if(nBytes > m_DataSize)
            nBytes = m_DataSize;
        if(!nBytes)
            return 0; // No data?

        // Yes, so just copy directly into the buffer
        memcpy(reinterpret_cast<void*>(buffer),
               reinterpret_cast<void*>(m_Buffer+m_Reader),
               nBytes);
        if(!bDoNotMove)
        {
            m_Reader += nBytes;
            m_DataSize -= nBytes;
        }
        return nBytes;
    }
    else
    {
        // This read will wrap around to the beginning
        size_t numNormalBytes = m_BufferSize - m_Reader;
        size_t numOverlapBytes = (m_Reader + nBytes) % m_BufferSize;

        // Does the read overlap the write position?
        if(numOverlapBytes >= m_Writer && m_Writer != 0)
            numOverlapBytes = m_Writer - 1;
        // If the writer's sitting at position 0, don't overlap
        else if(m_Writer == 0)
            numOverlapBytes = 0;

        // Copy the normal bytes
        if(numNormalBytes)
            memcpy(reinterpret_cast<void*>(buffer),
                   reinterpret_cast<void*>(m_Buffer+m_Reader),
                   numNormalBytes);
        if(numOverlapBytes)
            memcpy(reinterpret_cast<void*>(buffer + numNormalBytes),
                   reinterpret_cast<void*>(m_Buffer),
                   numOverlapBytes);

        // Update the writer position, if needed
        if(numOverlapBytes)
            m_Reader = numOverlapBytes;

        // Return the number of bytes written
        if((numNormalBytes + numOverlapBytes) > m_DataSize)
            m_DataSize = 0;
        else
            m_DataSize -= numNormalBytes + numOverlapBytes;
        return numNormalBytes + numOverlapBytes;
    }
}

void TcpBuffer::setSize(size_t newBufferSize)
{
    LockGuard<Mutex> guard(m_Lock);

    if(m_Buffer)
        delete [] reinterpret_cast<uint8_t*>(m_Buffer);

    if(newBufferSize)
    {
        m_BufferSize = newBufferSize;
        m_Buffer = reinterpret_cast<uintptr_t>(new uint8_t[newBufferSize + 1]);
    }
}

//
// Tree<StateBlockHandle,void*> implementation.
//

Tree<StateBlockHandle,void*>::Tree() :
  root(0), nItems(0), m_Begin(0)
{
}

Tree<StateBlockHandle,void*>::~Tree()
{
  delete m_Begin;
}

void Tree<StateBlockHandle,void*>::traverseNode_Insert(Node *n)
{
  if (!n) return;
  insert(n->key, n->element);
  traverseNode_Insert(n->leftChild);
  traverseNode_Insert(n->rightChild);
}

void Tree<StateBlockHandle,void*>::traverseNode_Remove(Node *n)
{
  if (!n) return;
  traverseNode_Remove(n->leftChild);
  traverseNode_Remove(n->rightChild);
  delete n;
}

Tree<StateBlockHandle,void*>::Tree(const Tree &x) :
  root(0), nItems(0), m_Begin(0)
{
  // Traverse the tree, adding everything encountered.
  traverseNode_Insert(x.root);

  m_Begin = new IteratorNode(root, 0);
}

Tree<StateBlockHandle,void*> &Tree<StateBlockHandle,void*>::operator =(const Tree &x)
{
  clear();
  // Traverse the tree, adding everything encountered.
  traverseNode_Insert(x.root);

  m_Begin = new IteratorNode(root, 0);

  return *this;
}

size_t Tree<StateBlockHandle,void*>::count() const
{
  return nItems;
}

void Tree<StateBlockHandle,void*>::insert(StateBlockHandle key, void *value)
{
  Node *n = new Node;
  n->key = key;
  n->element = value;
  n->height = 0;
  n->parent = 0;
  n->leftChild = 0;
  n->rightChild = 0;

  bool inserted = false;

  if (lookup(key))
  {
      delete n;
      return; // Key already in tree.
  }

  if (root == 0)
  {
    root = n; // We are the root node.

    m_Begin = new IteratorNode(root, 0);
  }
  else
  {
    // Traverse the tree.
    Node *currentNode = root;

    while (!inserted)
    {
      if (key > currentNode->key)
      {
        if (currentNode->rightChild == 0) // We have found our insert point.
        {
          currentNode->rightChild = n;
          n->parent = currentNode;
          inserted = true;
        }
        else
          currentNode = currentNode->rightChild;
      }
      else
      {
        if (currentNode->leftChild == 0) // We have found our insert point.
        {
          currentNode->leftChild = n;
          n->parent = currentNode;
          inserted = true;
        }
        else
          currentNode = currentNode->leftChild;
      }
    }

    // The value has been inserted, but has that messed up the balance of the tree?
    while (currentNode)
    {
      int b = balanceFactor(currentNode);
      if ( (b<-1) || (b>1) )
        rebalanceNode(currentNode);
      currentNode = currentNode->parent;
    }
  }

  nItems++;
}

void *Tree<StateBlockHandle,void*>::lookup(StateBlockHandle key)
{
  Node *n = root;
  while (n != 0)
  {
    if (n->key == key)
    {
      return n->element;
    }
    else if (key > n->key)
    {
      n = n->rightChild;
    }
    else
    {
      n = n->leftChild;
    }
  }
  return 0;
}

void Tree<StateBlockHandle,void*>::remove(StateBlockHandle key)
{
  Node *n = root;
  while (n != 0)
  {
    if (n->key == key)
      break;
    else if (n->key > key)
      n = n->leftChild;
    else
      n = n->rightChild;
  }

  Node *orign = n;
  if (n == 0) return;

  while (n->leftChild || n->rightChild) // While n is not a leaf.
  {
    size_t hl = height(n->leftChild);
    size_t hr = height(n->rightChild);
    if (hl == 0)
      rotateLeft(n); // N is now a leaf.
    else if (hr == 0)
      rotateRight(n); // N is now a leaf.
    else if (hl <= hr)
    {
      rotateRight(n);
      rotateLeft(n); // These are NOT inverse operations - rotateRight changes n's position.
    }
    else
    {
      rotateLeft(n);
      rotateRight(n);
    }
  }

  // N is now a leaf, so can be easily pruned.
  if (n->parent == 0)
    root = 0;
  else
    if (n->parent->leftChild == n)
      n->parent->leftChild = 0;
    else
      n->parent->rightChild = 0;

  // Work our way up the path, balancing.
  while (n)
  {
    int b = balanceFactor(n);
    if ( (b < -1) || (b > 1) )
      rebalanceNode(n);
    n = n->parent;
  }

  delete orign;
  nItems--;
}

void Tree<StateBlockHandle,void*>::clear()
{
  traverseNode_Remove(root);
  root = 0;
  nItems = 0;

  delete m_Begin;
  m_Begin = 0;
}

Tree<StateBlockHandle,void*>::Iterator Tree<StateBlockHandle,void*>::erase(Iterator iter)
{
  return iter; // to avoid "control reaches end of non-void function" warning
}

void Tree<StateBlockHandle,void*>::rotateLeft(Node *n)
{
  // See Cormen,Lieserson,Rivest&Stein  pp-> 278 for pseudocode.
  Node *y = n->rightChild;            // Set Y.

  n->rightChild = y->leftChild;       // Turn Y's left subtree into N's right subtree.
  if (y->leftChild != 0)
    y->leftChild->parent = n;

  y->parent = n->parent;              // Link Y's parent to N's parent.
  if (n->parent == 0)
    root = y;
  else if (n == n->parent->leftChild)
    n->parent->leftChild = y;
  else
    n->parent->rightChild = y;
  y->leftChild = n;
  n->parent = y;
}

void Tree<StateBlockHandle,void*>::rotateRight(Node *n)
{
  Node *y = n->leftChild;

  n->leftChild = y->rightChild;
  if (y->rightChild != 0)
    y->rightChild->parent = n;

  y->parent = n->parent;
  if (n->parent == 0)
    root = y;
  else if (n == n->parent->leftChild)
    n->parent->leftChild = y;
  else
    n->parent->rightChild = y;

  y->rightChild = n;
  n->parent = y;
}

size_t Tree<StateBlockHandle,void*>::height(Node *n)
{
  // Assumes: n's children's heights are up to date. Will always be true if balanceFactor
  //          is called in a bottom-up fashion.
  if (n == 0) return 0;

  size_t tempL = 0;
  size_t tempR = 0;

  if (n->leftChild != 0)
    tempL = n->leftChild->height;
  if (n->rightChild != 0)
    tempR = n->rightChild->height;

  tempL++; // Account for the height increase stepping up to us, its parent.
  tempR++;

  if (tempL > tempR) // If one is actually bigger than the other, return that, else return the other.
  {
    n->height = tempL;
    return tempL;
  }
  else
  {
    n->height = tempR;
    return tempR;
  }
}

int Tree<StateBlockHandle,void*>::balanceFactor(Node *n)
{
  return static_cast<int>(height(n->rightChild)) - static_cast<int>(height(n->leftChild));
}

void Tree<StateBlockHandle,void*>::rebalanceNode(Node *n)
{
  // This way of choosing which rotation to do took me AGES to find...
  // See http://www.cmcrossroads.com/bradapp/ftp/src/libs/C++/AvlTrees.html
  int balance = balanceFactor(n);
  if (balance < -1) // If it's left imbalanced, we need a right rotation.
  {
    if (balanceFactor(n->leftChild) > 0) // If its left child is right heavy...
    {
      // We need a RL rotation - left rotate n's left child, then right rotate N.
      rotateLeft(n->leftChild);
      rotateRight(n);
    }
    else
    {
      // RR rotation will do.
      rotateRight(n);
    }
  }
  else if (balance > 1)
  {
    if (balanceFactor(n->rightChild) < 0) // If its right child is left heavy...
    {
      // We need a LR rotation; Right rotate N's right child, then left rotate N.
      rotateRight(n->rightChild);
      rotateLeft(n);
    }
    else
    {
      // LL rotation.
      rotateLeft(n);
    }
  }
}
