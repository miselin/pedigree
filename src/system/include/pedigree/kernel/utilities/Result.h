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

#ifndef KERNEL_UTILITIES_RESULT_H
#define KERNEL_UTILITIES_RESULT_H

#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/utility.h"

/**
 * Result encapsulates a typed result and an optional error in one object.
 *
 * Either is valid, but both cannot be valid at the same time.
 *
 * Result is useful for returning status in case of cases such as timeouts or
 * other failures where overloading a return value (e.g. a pointer) is not a
 * desirable action.
 */
template <class T, class E>
class Result
{
  public:
    static Result<T, E> withValue(const T &v)
    {
        return Result<T, E>(v);
    }

    static Result<T, E> withError(const E &e)
    {
        return Result<T, E>(e, true);
    }

    const T &value() const
    {
        assert(hasValue());
        return m_Value;
    }

    const E &error() const
    {
        assert(hasError());
        return m_Error;
    }

    bool hasError() const
    {
        return m_HasError;
    }

    bool hasValue() const
    {
        return !hasError();
    }

  private:
    Result() = delete;

    Result(const T &v) : m_Value(v), m_Error(), m_HasError(false)
    {
    }

    Result(const E &e, bool)
        : m_Value(m_DefaultValue), m_Error(e), m_HasError(true)
    {
    }

    T m_Value;
    E m_Error;
    bool m_HasError;

    typedef typename pedigree_std::remove_reference<T>::type BaseValueType;
    static const BaseValueType m_DefaultValue;
};

template <class T, class E>
const typename Result<T, E>::BaseValueType
    Result<T, E>::m_DefaultValue = Result<T, E>::BaseValueType();

#endif  // KERNEL_UTILITIES_RESULT_H
