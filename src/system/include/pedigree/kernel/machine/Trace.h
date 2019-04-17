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

#ifndef MACHINE_TRACE_H
#define MACHINE_TRACE_H

/**
 * TRACE provides an interface to tracing kernel startup progress.
 *
 * In release builds, tracing should be disabled to avoid the performance hit.
 *
 * Think of TRACE as a direct path to push messages to a debug log or other
 * target, which may or may not be interspersed with actual kernel log messages.
 *
 * Note: this must be completely functional even before constructors have run
 * as it cannot be guaranteed that tracing will not be needed before
 * constructors have been invoked. This differs to the kernel log, which
 * requires constructors to be run to be useful.
 */
#if TRACING
#define TRACE(msg) pedigree_trace::trace(("trace: " msg))
#else
#define TRACE(...)
#endif  // TRACING

namespace pedigree_trace
{
void trace(const char *msg);
}

#endif  // MACHINE_TRACE_H
