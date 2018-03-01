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

#ifndef KERNEL_UTILITIES_LAZYEVALUATE_H
#define KERNEL_UTILITIES_LAZYEVALUATE_H

/**
 * LazyEvaluate offers a way to defer potentially-expensive evaluation to the
 * time at which the result of the evaluation is needed. This can allow for
 * reduced memory usage and better performance, especially in cases where many
 * objects are available for evaluation but only a few actually become
 * evaluated.
 *
 * \param[in] T the result type of the evaluation
 * \param[in] M type for metadata to be passed to creation function
 * \param[in] create a function that performs evaluation based on the given
 *                   metadata
 * \param[in] destroy a function that cleans up an evaluation if needed
 */
template <class T, class M, T *(*create)(const M &), void (*destroy)(T *)>
class LazyEvaluate
{
    public:
        LazyEvaluate(const M &metadata) : m_Metadata(metadata), m_Field(nullptr) {}
        virtual ~LazyEvaluate()
        {
            reset();
        }

        bool active() const
        {
            return m_Field != nullptr;
        }

        void reset()
        {
            if (active())
            {
                destroy(m_Field);
                m_Field = nullptr;
            }
        }

        T *get()
        {
            if (!active())
            {
                m_Field = create(m_Metadata);
            }

            return m_Field;
        }

        T *operator ->()
        {
            return get();
        }

        T &operator *()
        {
            return *get();
        }

    private:
        M m_Metadata;
        T *m_Field;
};


#endif // KERNEL_UTILITIES_LAZYEVALUATE_H
