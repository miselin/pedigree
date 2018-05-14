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

#ifndef POINTERS_H
#define POINTERS_H

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/processor/types.h"

template <class T>
class UniqueCommon
{
  public:
    UniqueCommon() : m_Pointer(nullptr)
    {
    }

    virtual ~UniqueCommon()
    {
        reset();
    }

    T *operator*() const
    {
        return get();
    }

    operator void *() const
    {
        return get();
    }

    T *get() const
    {
        return m_Pointer;
    }

    void reset()
    {
        if (m_Pointer)
        {
            destroy();
            m_Pointer = 0;
        }
    }

    NOT_COPYABLE_OR_ASSIGNABLE(UniqueCommon<T>);

  protected:
    UniqueCommon(T *p) : m_Pointer(p)
    {
    }

    virtual void destroy()
    {
        delete m_Pointer;
    }

    void setPointer(T *p)
    {
        m_Pointer = p;
    }

    /** Stop tracking the memory but don't free it. */
    void release()
    {
        m_Pointer = nullptr;
    }

    T *m_Pointer;
};

/** Provides a wrapper around a single-use pointer. The copy constructor
 * will invalidate the reference in the object being copied from.
 */
template <class T>
class UniquePointer : public UniqueCommon<T>
{
  public:
    UniquePointer() = default;

    virtual ~UniquePointer()
    {
        this->reset();
    }

    // move constructor
    UniquePointer(UniquePointer<T> &&p)
    {
        move_from(pedigree_std::move(p));
    }

    // no copy construction permitted
    NOT_COPYABLE_OR_ASSIGNABLE(UniquePointer<T>);

    UniquePointer<T> &operator=(UniquePointer<T> &&p)
    {
        move_from(pedigree_std::move(p));
        return *this;
    }

    template <class... Args>
    static UniquePointer<T> allocate(Args &&... args)
    {
        return UniquePointer<T>(new T(args...));
    }

  private:
    UniquePointer(T *p) : UniqueCommon<T>(p)
    {
    }

    void move_from(UniquePointer<T> &&p)
    {
        T *ptr = p.get();
        p.release();

        this->reset();
        this->setPointer(ptr);
    }
};

/** Array version of UniquePointer that uses delete[] for deletion. */
template <class T>
class UniqueArray : public UniqueCommon<T>
{
  public:
    UniqueArray() = default;

    virtual ~UniqueArray()
    {
        this->reset();
    }

    // move constructor
    UniqueArray(UniqueArray<T> &&p)
    {
        move_from(pedigree_std::move(p));
    }

    // no copy construction permitted
    NOT_COPYABLE_OR_ASSIGNABLE(UniqueArray<T>);

    UniqueArray<T> &operator=(UniqueArray<T> &&p)
    {
        move_from(pedigree_std::move(p));
        return *this;
    }

    static UniqueArray<T> allocate(size_t count)
    {
        return UniqueArray<T>(new T[count]);
    }

  protected:
    virtual void destroy() override
    {
        T *ptr = this->get();
        delete[] ptr;
    }

  private:
    UniqueArray(T *p) : UniqueCommon<T>(p)
    {
    }

    void move_from(UniqueArray<T> &&p)
    {
        T *ptr = p.get();
        p.release();

        this->reset();
        this->setPointer(ptr);
    }
};

#endif
