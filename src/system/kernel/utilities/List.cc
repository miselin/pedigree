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

#include <utilities/List.h>
#include <Log.h>

//
// List<void*> implementation
//

List<void*>::List()
    : m_Count(0), m_First(0), m_Last(0), m_Magic(0x1BADB002)
{
}
List<void*>::List(const List &x)
    : m_Count(0), m_First(0), m_Last(0), m_Magic(0x1BADB002)
{
  assign(x);
}
List<void*>::~List()
{
    if (m_Magic != 0x1BADB002)
    {
        FATAL("List: bad magic [" << m_Magic << "].");
    }
    clear();
}

List<void*> &List<void*>::operator = (const List &x)
{
  assign(x);
  return *this;
}

size_t List<void*>::size() const
{
  return m_Count;
}
size_t List<void*>::count() const
{
  return m_Count;
}
void List<void*>::pushBack(void *value)
{
  node_t *newNode = new node_t;
  newNode->m_Next = 0;
  newNode->m_Previous = m_Last;
  newNode->value = value;

  if (m_Last == 0)
    m_First = newNode;
  else
    m_Last->m_Next = newNode;

  m_Last = newNode;
  ++m_Count;
}
void *List<void*>::popBack()
{
  node_t *node = m_Last;
  m_Last = m_Last->m_Previous;
  if (m_Last != 0)
    m_Last->m_Next = 0;
  else
    m_First = 0;
  --m_Count;

  void *value = node->value;
  delete node;
  return value;
}
void List<void*>::pushFront(void *value)
{
  node_t *newNode = new node_t;
  newNode->m_Next = m_First;
  newNode->m_Previous = 0;
  newNode->value = value;

  if (m_First == 0)
    m_Last = newNode;
  else
    m_First->m_Previous = newNode;

  m_First = newNode;
  ++m_Count;
}
void *List<void*>::popFront()
{
  node_t *node = m_First;
  m_First = m_First->m_Next;
  if (m_First != 0)
    m_First->m_Previous = 0;
  else
    m_Last = 0;
  --m_Count;

  void *value = node->value;
  delete node;
  return value;
}
List<void*>::Iterator List<void*>::erase(Iterator &Iter)
{
  node_t *Node = Iter.__getNode();
  if (Node->m_Previous == 0)
    m_First = Node->m_Next;
  else
    Node->m_Previous->m_Next = Node->m_Next;
  if (Node->m_Next == 0)
    m_Last = Node->m_Previous;
  else
    Node->m_Next->m_Previous = Node->m_Previous;
  --m_Count;

  node_t *pNext = Node->m_Next;
  // If pNext is NULL, this will be the same as 'end()'.
  Iterator tmp(pNext);
  delete Node;
  return tmp;
}
List<void*>::ReverseIterator List<void*>::erase(ReverseIterator &Iter)
{
  node_t *Node = Iter.__getNode();
  if (Node->m_Next == 0)
    m_First = Node->m_Previous;
  else
    Node->m_Next->m_Previous = Node->m_Previous;
  if (Node->m_Previous == 0)
    m_Last = Node->m_Next;
  else
    Node->m_Previous->m_Next = Node->m_Next;
  --m_Count;

  node_t *pNext = Node->m_Previous;
  // If pNext is NULL, this will be the same as 'rend()'.
  ReverseIterator tmp(pNext);
  delete Node;
  return tmp;
}

void List<void*>::clear()
{
  node_t *cur = m_First;
  for (size_t i = 0;i < m_Count;i++)
  {
    node_t *tmp = cur;
    cur = cur->m_Next;
    delete tmp;
  }

  m_Count = 0;
  m_First = 0;
  m_Last = 0;
}
void List<void*>::assign(const List &x)
{
  if (m_Count != 0)clear();

  ConstIterator Cur(x.begin());
  ConstIterator End(x.end());
  for (;Cur != End;++Cur)
    pushBack(*Cur);
}

//
// Explicitly instantiate List<void*>
//
template class List<void*>;
template class List<uint64_t>;
