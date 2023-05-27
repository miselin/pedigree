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

#ifndef KERNEL_UTILITIES_VECTOR_H
#define KERNEL_UTILITIES_VECTOR_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/utility.h"

/** @addtogroup kernelutilities
 * @{ */

/** General Vector template class, aka dynamic array
 *\brief A vector / dynamic array */
template <class T>
class EXPORTED_PUBLIC Vector
{
  public:
    /** Random-access iterator for the Vector */
    typedef T *Iterator;
    /** Contant random-access iterator for the Vector */
    typedef T const *ConstIterator;
    /** Random-access reversed iterator for the Vector. */
    template <class T_>
    struct ReverseIteratorContainer
    {
        T_ value;

        ReverseIteratorContainer<T_> &operator++()
        {
            --value;
            return *this;
        }
        ReverseIteratorContainer<T_> &operator--()
        {
            ++value;
            return *this;
        }

        ReverseIteratorContainer<T_> operator++(int)
        {
            T_ origValue = value;
            --value;
            return ReverseIteratorContainer<T_>{.value = origValue};
        }
        ReverseIteratorContainer<T_> operator--(int)
        {
            T_ origValue = value;
            ++value;
            return ReverseIteratorContainer<T_>{.value = origValue};
        }

        operator T_() const
        {
            return value;
        }

        typename pedigree_std::remove_pointer<T_>::type operator*() const
        {
            return *value;
        }

        bool operator==(const ReverseIteratorContainer<T_> &other)
        {
            return value == other.value;
        }
    };
    typedef ReverseIteratorContainer<T *> ReverseIterator;
    typedef ReverseIteratorContainer<const T *> ConstReverseIterator;

    /** The default constructor, does nothing */
    Vector();
    /** Reserves space for size elements
     *\param[in] size the number of elements */
    explicit Vector(size_t size);
    /** The copy-constructor
     *\param[in] x the reference object to copy */
    Vector(const Vector &x);
    /** The destructor, deallocates memory */
    ~Vector();

    /** The assignment operator
     *\param[in] x the object that should be copied */
    Vector &operator=(const Vector &x);
    /** The [] operator
     *\param[in] index the index of the element that should be returned
     *\return the element at index index */
    T &operator[](size_t index) const;

    /** Get the number of elements that we have reserved space for
     *\return the number of elements that we have reserved space for */
    size_t size() const;
    /** Get the number of elements in the Vector
     *\return the number of elements in the Vector */
    size_t count() const;
    /** Add an element to the end of the Vector
     *\param[in] value the element */
    void pushBack(const T &value);
    /** Create an element in place at the end of the Vector */
    template <class... Args>
    void createBack(Args&&... args)
    {
        reserve(m_Count + 1, true);

        // If we've hit the end of the reserved space we can use, we need to move
        // the existing entries (rather than this happening in each reserve).
        if ((m_Start + m_Count + 1) > m_Size)
        {
            pedigree_std::copy(m_Data, m_Data + m_Start, m_Count);
            m_Start = 0;
        }

        T *loc = &m_Data[m_Start + m_Count++];
        new (loc) T(pedigree_std::forward<Args>(args)...);
    }
    /** Move a value to the end of the Vector
     *\param[in] value the value that should be added */
    void pushBack(T &&value);
    /** Remove the element from the back and return it
     *\return the removed element */
    T popBack();
    /** Add an element to the front of the Vector
     *\param[in] value the element */
    void pushFront(const T &value);
    /** Move a value to the end of the Vector
     *\param[in] value the value that should be added */
    void pushFront(T &&value);
    /** Remove the element from the front and return it
     *\return the removed element */
    T popFront();
    /** Set an element at the given index, if it exists. */
    void setAt(size_t idx, const T &value);
    /** Swap the two elements. */
    void swap(Iterator a, Iterator b);
    /** Insert into the vector, moving all items after the given position along.
     */
    void insert(size_t index, const T &value);

    /**
     * Clear the Vector.
     * Pass freeMem = true to also free the memory consumed by the Vector.
     * Pass freeMem = false to retain the memory for future Vector usage.
     */
    void clear(bool freeMem=false);
    /** Erase the element at the given index. */
    void erase(size_t index);
    /** Erase one Element */
    Iterator erase(Iterator iter);
    /** Erase one Element */
    ReverseIterator erase(ReverseIterator iter);

    /** Get an iterator pointing to the beginning of the Vector
     *\return iterator pointing to the beginning of the Vector */
    Iterator begin()
    {
        return m_Data + m_Start;
    }
    /** Get a constant iterator pointing to the beginning of the Vector
     *\return constant iterator pointing to the beginning of the Vector */
    ConstIterator begin() const
    {
        return m_Data + m_Start;
    }
    /** Get an iterator pointing to the last element + 1
     *\return iterator pointing to the last element + 1 */
    Iterator end()
    {
        return m_Data + m_Start + m_Count;
    }
    /** Get a constant iterator pointing to the last element + 1
     *\return constant iterator pointing to the last element + 1 */
    ConstIterator end() const
    {
        return m_Data + m_Start + m_Count;
    }

    ReverseIterator rbegin()
    {
        return ReverseIterator{.value = m_Data + m_Start + m_Count - 1};
    }
    ConstReverseIterator rbegin() const
    {
        return ConstReverseIterator{.value = m_Data + m_Start + m_Count - 1};
    }
    ReverseIterator rend()
    {
        return ReverseIterator{.value = m_Data + m_Start - 1};
    }
    ConstReverseIterator rend() const
    {
        return ConstReverseIterator{.value = m_Data + m_Start - 1};
    }
    /** Copy the content of a Vector into this Vector
     *\param[in] x the reference Vector */
    void assign(const Vector &x);
    /** Reserve space for size elements
     *\param[in] size the number of elements to reserve space for
     *\param[in] copy Should we copy the old contents over? */
    void reserve(size_t size, bool copy);

  private:
    /** Internal reserve() function.
     *\param[in] size the number of elements to reserve space for
     *\param[in] copy Should we copy the old contents over?
     *\param[in] free should we free the old buffer? */
    void reserve(size_t size, bool copy, bool free);
    /** The number of elements we have reserved space for */
    size_t m_Size;
    /** The number of elements in the Vector */
    size_t m_Count;
    /**
     * The current start index in the array.
     * This is used to reduce the need to keep copying the array contents.
     */
    size_t m_Start;
    /** Pointer to the Elements */
    T *m_Data;
    /** Factor to multiply by in reserve(). */
    static const int m_ReserveFactor = 2;
};

template <class T>
Vector<T>::Vector() : m_Size(0), m_Count(0), m_Start(0), m_Data(0)
{
}

template <class T>
Vector<T>::Vector(size_t size) : m_Size(0), m_Count(0), m_Start(0), m_Data(0)
{
    reserve(size, false);
}

template <class T>
Vector<T>::Vector(const Vector &x)
    : m_Size(0), m_Count(0), m_Start(0), m_Data(0)
{
    assign(x);
}

template <class T>
Vector<T>::~Vector()
{
    if (m_Data != 0)
        delete[] m_Data;
}

template <class T>
Vector<T> &Vector<T>::operator=(const Vector &x)
{
    assign(x);
    return *this;
}

template <class T>
T &Vector<T>::operator[](size_t index) const
{
    static T outofbounds = T();
    if (index > m_Count)
        return outofbounds;
    return m_Data[m_Start + index];
}

template <class T>
size_t Vector<T>::size() const
{
    return m_Size;
}

template <class T>
size_t Vector<T>::count() const
{
    return m_Count;
}

template <class T>
void Vector<T>::pushBack(const T &value)
{
    reserve(m_Count + 1, true);

    // If we've hit the end of the reserved space we can use, we need to move
    // the existing entries (rather than this happening in each reserve).
    if ((m_Start + m_Count + 1) > m_Size)
    {
        pedigree_std::copy(m_Data, m_Data + m_Start, m_Count);
        m_Start = 0;
    }

    m_Data[m_Start + m_Count++] = value;
}

template <class T>
void Vector<T>::pushBack(T &&value)
{
    reserve(m_Count + 1, true);

    // If we've hit the end of the reserved space we can use, we need to move
    // the existing entries (rather than this happening in each reserve).
    if ((m_Start + m_Count + 1) > m_Size)
    {
        pedigree_std::copy(m_Data, m_Data + m_Start, m_Count);
        m_Start = 0;
    }

    m_Data[m_Start + m_Count++] = pedigree_std::move(value);
}

template <class T>
T Vector<T>::popBack()
{
    m_Count--;
    return m_Data[m_Start + m_Count];
}

template <class T>
void Vector<T>::pushFront(const T &value)
{
    const T *oldData = m_Data;

    reserve(m_Count + 1, true, false);

    if (m_Start && (m_Data == oldData))
    {
        m_Start--;
        m_Data[m_Start] = value;
    }
    else
    {
        // We have a bigger buffer, copy items from the old buffer now.
        pedigree_std::copy(&m_Data[1], oldData, m_Count);
        m_Data[0] = value;
    }

    m_Count++;

    // All finished with the previous buffer now.
    if (m_Data != oldData)
    {
        delete[] oldData;
    }
}

template <class T>
void Vector<T>::pushFront(T &&value)
{
    const T *oldData = m_Data;

    reserve(m_Count + 1, true, false);

    if (m_Start && (m_Data == oldData))
    {
        m_Start--;
        m_Data[m_Start] = pedigree_std::move(value);
    }
    else
    {
        // We have a bigger buffer, copy items from the old buffer now.
        pedigree_std::copy(&m_Data[1], oldData, m_Count);
        m_Data[0] = pedigree_std::move(value);
    }

    m_Count++;

    // All finished with the previous buffer now.
    if (m_Data != oldData)
    {
        delete[] oldData;
    }
}

template <class T>
T Vector<T>::popFront()
{
    const T &ret = m_Data[m_Start];
    m_Count--;
    m_Start++;
    return ret;
}

template <class T>
void Vector<T>::setAt(size_t idx, const T &value)
{
    if (idx < m_Count)
        m_Data[m_Start + idx] = value;
}

template <class T>
void Vector<T>::clear(bool freeMem)
{
    m_Count = 0;
    m_Start = 0;
    if (freeMem)
    {
        m_Size = 0;
        delete[] m_Data;
        m_Data = 0;
    }
}

template <class T>
void Vector<T>::erase(size_t index)
{
    if (!m_Count)
    {
        return;
    }
    else if (index >= m_Count)
    {
        return;
    }

    T *base = m_Data + m_Start;
    pedigree_std::copy(base + index, base + index + 1, m_Count - index - 1);
    m_Count--;
}

template <class T>
typename Vector<T>::Iterator Vector<T>::erase(Iterator iter)
{
    erase(static_cast<size_t>(iter - begin()));
    return iter;
}

template <class T>
typename Vector<T>::ReverseIterator Vector<T>::erase(ReverseIterator iter)
{
    erase(static_cast<size_t>(iter.value - (m_Data + m_Start)));
    return iter;
}

template <class T>
void Vector<T>::assign(const Vector &x)
{
    reserve(x.size(), false);
    pedigree_std::copy(m_Data, x.m_Data, x.m_Count);
    m_Count = x.count();
    m_Start = x.m_Start;
}

template <class T>
void Vector<T>::reserve(size_t size, bool copy)
{
    reserve(size, copy, true);
}

template <class T>
void Vector<T>::reserve(size_t size, bool copy, bool free)
{
    if (size <= m_Size)
    {
        return;
    }
    else if (size < (m_Size * m_ReserveFactor))
    {
        // Grow exponentially.
        size = m_Size * m_ReserveFactor;
    }

    T *tmp = m_Data;
    m_Data = new T[size];
    if (tmp != 0)
    {
        if ((copy == true) && m_Count)
        {
            pedigree_std::copy(m_Data, tmp + m_Start, m_Count - m_Start);
            m_Start = 0;
        }
        if (free)
        {
            delete[] tmp;
        }
    }
    m_Size = size;
}

template <class T>
void Vector<T>::swap(Iterator a, Iterator b)
{
    if (a == b)
        return;
    else if (a < begin() || a >= end())
        return;
    else if (b < begin() || b >= end())
        return;

    // Perform the swap.
    T tmp = *a;
    *a = *b;
    *b = tmp;
}

template <class T>
void Vector<T>::insert(size_t index, const T &value)
{
    if (index >= m_Count)
    {
        pushBack(value);
        return;
    }
    else if (index == 0)
    {
        pushFront(value);
        return;
    }

    reserve(m_Count + 1, true);

    pedigree_std::copy(
        m_Data + m_Start + index + 1, m_Data + m_Start + index,
        m_Count - index);

    m_Data[m_Start + index] = value;
    ++m_Count;
}

extern template class Vector<void *>;    // IWYU pragma: keep
extern template class Vector<uint64_t>;  // IWYU pragma: keep
extern template class Vector<uint32_t>;  // IWYU pragma: keep
extern template class Vector<uint16_t>;  // IWYU pragma: keep
extern template class Vector<uint8_t>;   // IWYU pragma: keep
extern template class Vector<int64_t>;   // IWYU pragma: keep
extern template class Vector<int32_t>;   // IWYU pragma: keep
extern template class Vector<int16_t>;   // IWYU pragma: keep
extern template class Vector<int8_t>;    // IWYU pragma: keep

/** @} */

#endif
