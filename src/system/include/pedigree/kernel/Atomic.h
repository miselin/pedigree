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

#ifndef KERNEL_ATOMIC_H
#define KERNEL_ATOMIC_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/utility.h"

/** @addtogroup kernel
 * @{ */

// NOTE: See http://gcc.gnu.org/onlinedocs/gcc/Atomic-Builtins.html
//       for more information about gcc's builtin atomic operations

template <
    typename T, bool bAllow = (is_integral<T>::value | is_pointer<T>::value)>
class Atomic;

/** Wrapper around gcc's builtin atomic operations */
template <typename T>
class EXPORTED_PUBLIC Atomic<T, true>
{
    friend class PerProcessorScheduler;

  public:
    /** The constructor
     *\param[in] value initial value */
    inline Atomic(T value = T()) : m_Atom(value)
    {
    }
    /** The copy-constructor
     *\param[in] x reference object */
    inline Atomic(const Atomic &x) : m_Atom(x.m_Atom)
    {
    }
    /** The assignment operator
     *\param[in] x reference object */
    inline Atomic &operator=(const Atomic &x)
    {
        m_Atom = x.m_Atom;
        return *this;
    }
    /** The destructor does nothing */
    virtual ~Atomic()
    {
    }

    /** Addition
     *\param[in] x value to add
     *\return the value after the addition */
    inline T operator+=(T x)
    {
#if !TARGET_HAS_NO_ATOMICS
        return __sync_add_and_fetch(&m_Atom, x);
#else
        m_Atom += x;
        return m_Atom;
#endif
    }
    /** Subtraction
     *\param[in] x value to subtract
     *\return the value after the subtraction */
    inline T operator-=(T x)
    {
#if !TARGET_HAS_NO_ATOMICS
        return __sync_sub_and_fetch(&m_Atom, x);
#else
        m_Atom -= x;
        return m_Atom;
#endif
    }
    /** Bitwise or
     *\param[in] x the operand
     *\return the value after the bitwise or */
    inline T operator|=(T x)
    {
#if !TARGET_HAS_NO_ATOMICS
        return __sync_or_and_fetch(&m_Atom, x);
#else
        m_Atom |= x;
        return m_Atom;
#endif
    }
    /** Bitwise and
     *\param[in] x the operand
     *\return the value after the bitwise and */
    inline T operator&=(T x)
    {
#if !TARGET_HAS_NO_ATOMICS
        return __sync_and_and_fetch(&m_Atom, x);
#else
        m_Atom &= x;
        return m_Atom;
#endif
    }
    /** Bitwise xor
     *\param[in] x the operand
     *\return the value after the bitwise xor */
    inline T operator^=(T x)
    {
#if !TARGET_HAS_NO_ATOMICS
        return __sync_xor_and_fetch(&m_Atom, x);
#else
        m_Atom ^= x;
        return m_Atom;
#endif
    }
    /** Compare and swap
     *\param[in] oldVal the comparision value
     *\param[in] newVal the new value
     *\return true, if the Atomic had the value oldVal and the value was changed
     *to newVal, false otherwise */
    inline bool compareAndSwap(T oldVal, T newVal)
    {
#if !TARGET_HAS_NO_ATOMICS
        return __sync_bool_compare_and_swap(&m_Atom, oldVal, newVal);
#else
        if (m_Atom == oldVal)
        {
            m_Atom = newVal;
            return true;
        }
        return false;
#endif
    }
    /** Get the value
     *\return the value of the Atomic */
    inline operator T() const
    {
        return m_Atom;
    }

    const volatile T &value() const
    {
        return m_Atom;
    }

    typedef T Type;

  private:
    /** The atomic value */
    volatile T m_Atom;
};

/** Wrapper around gcc's builtin atomic operations */
template <>
class EXPORTED_PUBLIC Atomic<bool, true> : public Atomic<processor_register_t>
{
  public:
    /** The constructor
     *\param[in] value initial value */
    inline Atomic(bool value = false) : Atomic<processor_register_t>((value) ? 1 : 0)
    {
    }
    /** The copy-constructor
     *\param[in] x reference object */
    inline Atomic(const Atomic &x) : Atomic<processor_register_t>(x)
    {
    }
    /** The assignment operator
     *\param[in] x reference object */
    inline Atomic &operator=(const Atomic &x)
    {
        Atomic<processor_register_t>::operator=(x);
        return *this;
    }
    /** The destructor does nothing */
    virtual ~Atomic();

    /** Bitwise or
     *\param[in] x the operand
     *\return the value after the bitwise or */
    inline bool operator|=(bool x)
    {
        return Atomic<processor_register_t>::operator|=(x);
    }
    /** Bitwise and
     *\param[in] x the operand
     *\return the value after the bitwise and */
    inline bool operator&=(bool x)
    {
        return Atomic<processor_register_t>::operator&=(x);
    }
    /** Bitwise xor
     *\param[in] x the operand
     *\return the value after the bitwise xor */
    inline bool operator^=(bool x)
    {
        return Atomic<processor_register_t>::operator^=(x);
    }
    /** Compare and swap
     *\param[in] oldVal the comparision value
     *\param[in] newVal the new value
     *\return true, if the Atomic had the value oldVal and the value was changed
     *to newVal, false otherwise */
    inline bool compareAndSwap(bool oldVal, bool newVal)
    {
        return Atomic<processor_register_t>::compareAndSwap(
            (oldVal) ? 1 : 0, (newVal) ? 1 : 0);
    }
    /** Get the value
     *\return the value of the Atomic */
    inline operator bool() const
    {
        return (Atomic<processor_register_t>::operator processor_register_t() == 1) ? true : false;
    }
};

/** @} */

#endif
