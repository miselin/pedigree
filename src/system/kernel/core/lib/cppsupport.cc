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

#include "pedigree/kernel/core/cppsupport.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/core/SlamAllocator.h"
#include "pedigree/kernel/machine/Trace.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/utilities/MemoryTracing.h"
#include "pedigree/kernel/utilities/utility.h"

/// If the debug allocator is enabled, this switches it into underflow detection
/// mode.
#define DEBUG_ALLOCATOR_CHECK_UNDERFLOWS 1

// We need to use __builtin_frame_* with non-zero arguments in some cases here.
#pragma GCC diagnostic ignored "-Wframe-address"

// Required for G++ to link static init/destructors.
#if !HOSTED
extern "C" void *__dso_handle;
#endif

// Defined in the linker.
extern uintptr_t start_kernel_ctors;
extern uintptr_t end_kernel_ctors;
extern uintptr_t start_kernel_dtors;
extern uintptr_t end_kernel_dtors;

/// Calls the constructors for all global objects.
/// Call this before using any global objects.
void initialiseConstructors()
{
    // Constructor list is defined in the linker script.
    // The .ctors section is just an array of function pointers.
    // iterate through, calling each in turn.
    uintptr_t *iterator = &start_kernel_ctors;
    while (iterator < &end_kernel_ctors)
    {
        void (*fp)(void) = reinterpret_cast<void (*)(void)>(*iterator);
        fp();
        iterator++;
    }
}

void runKernelDestructors()
{
    uintptr_t *iterator = &start_kernel_dtors;
    while (iterator < &end_kernel_dtors)
    {
        void (*fp)(void) = reinterpret_cast<void (*)(void)>(*iterator);
        fp();
        iterator++;
    }
}

/// Memory tracing defaults to being enabled if enabled in CMake
static bool traceAllocations = MEMORY_TRACING;

void startTracingAllocations()
{
    if constexpr (MEMORY_TRACING)
    {
        traceAllocations = true;
    }
}

void stopTracingAllocations()
{
    if constexpr (MEMORY_TRACING)
    {
        traceAllocations = false;
    }
}

void toggleTracingAllocations()
{
    if constexpr (MEMORY_TRACING)
    {
        traceAllocations = !traceAllocations;
    }
}

void traceAllocation(
    void *ptr, MemoryTracing::AllocationTrace type, size_t size)
{
    // Don't trace if the feature is completely disabled.
    if constexpr (!MEMORY_TRACING)
    {
        return;
    }

    // Don't trace if we're not allowed to.
    if (!traceAllocations)
        return;

    // Ignore physical allocations just for now.
    switch (type)
    {
        case MemoryTracing::Allocation:
        case MemoryTracing::Free:
        case MemoryTracing::Metadata:
            break;
        default:
            return;  // ignore
    }

    VirtualAddressSpace &va = VirtualAddressSpace::getKernelAddressSpace();

    MemoryTracing::AllocationTraceEntry entry;
    entry.data.type = type;
    entry.data.sz = size & 0xFFFFFFFFU;
    entry.data.ptr = reinterpret_cast<uintptr_t>(ptr);
    for (size_t i = 0; i < MemoryTracing::num_backtrace_entries; ++i)
    {
        entry.data.bt[i] = 0;
    }

#define BT_FRAME(M, N)                                                 \
    do                                                                 \
    {                                                                  \
        if (M && !entry.data.bt[M - 1])                                \
            break;                                                     \
        void *frame_addr = __builtin_frame_address(N);                 \
        if (!(frame_addr && va.isMapped(frame_addr)))                  \
        {                                                              \
            entry.data.bt[M] = 0;                                      \
            break;                                                     \
        }                                                              \
        entry.data.bt[M] =                                             \
            reinterpret_cast<uintptr_t>(__builtin_return_address(N)) & \
            0xFFFFFFFFU;                                               \
    } while (0)

    // we want to skip the allocate()/free() call and get a little bit of
    // context
    if (MemoryTracing::num_backtrace_entries >= 1)
        BT_FRAME(0, 1);
    if (MemoryTracing::num_backtrace_entries >= 2)
        BT_FRAME(1, 2);
    if (MemoryTracing::num_backtrace_entries >= 3)
        BT_FRAME(2, 3);
    if (MemoryTracing::num_backtrace_entries >= 4)
        BT_FRAME(3, 4);
    if (MemoryTracing::num_backtrace_entries >= 5)
        BT_FRAME(4, 5);

    __asm__ __volatile__("pushfq; cli" ::: "memory");

    for (size_t i = 0; i < sizeof entry.buf; ++i)
    {
        __asm__ __volatile__(
            "outb %%al, %%dx" ::"Nd"(0x2E8), "a"(entry.buf[i]));
    }

    __asm__ __volatile__("popf" ::: "memory");
}

/**
 * Adds a metadata field to the memory trace.
 *
 * This is typically used to define the region in which a module has been
 * loaded, so the correct debug symbols can be loaded and used.
 */
void traceMetadata(NormalStaticString str, void *p1, void *p2)
{
    // Removed for now - this can be provided by scripts/addr2line.py now
}

/// Required for G++ to compile code.
extern "C" EXPORTED_PUBLIC void atexit(void (*f)(void *), void *p, void *d);
void atexit(void (*f)(void *), void *p, void *d)
{
}

extern "C" EXPORTED_PUBLIC void abort(void);
void abort()
{
    FATAL_NOLOCK("abort");
}

/// Called by G++ if a pure virtual function is called. Bad Thing, should never
/// happen!
extern "C" EXPORTED_PUBLIC void __cxa_pure_virtual() NORETURN;
void __cxa_pure_virtual()
{
    /// \todo if FATAL etc don't work we need to still make this evident
    TRACE("Pure virtual function call made");
    FATAL_NOLOCK("Pure virtual function call made");
}

/// Called by G++ if function local statics are initialised for the first time
#if !HAS_THREAD_SANITIZER
extern "C" EXPORTED_PUBLIC int __cxa_guard_acquire();
extern "C" EXPORTED_PUBLIC void __cxa_guard_release();

int __cxa_guard_acquire()
{
    return 1;
}
void __cxa_guard_release()
{
    // TODO
}
#endif

#ifndef HOSTED_SYSTEM_MALLOC
#define HOSTED_SYSTEM_MALLOC 0
#endif

#if HOSTED

#if HOSTED_SYSTEM_MALLOC
// already using the system malloc so just define our versions as hosted_*
#define MALLOC hosted_malloc
#define CALLOC hosted_calloc 
#define FREE hosted_free
#define REALLOC hosted_realloc
#else
#define MALLOC _malloc
#define CALLOC _calloc 
#define FREE _free
#define REALLOC _realloc
#endif  // HOSTED_SYSTEM_MALLOC != 0

#else
#define MALLOC malloc
#define CALLOC calloc
#define FREE free
#define REALLOC realloc
#endif  // HOSTED

extern "C" void *MALLOC(size_t sz)
{
    return reinterpret_cast<void *>(new uint8_t[sz]);
}

extern "C" void *CALLOC(size_t num, size_t sz)
{
    void *result = reinterpret_cast<void *>(new uint8_t[num * sz]);
    ByteSet(result, 0, num * sz);
    return result;
}

extern "C" void FREE(void *p)
{
    if (p == 0)
        return;
    // SlamAllocator::instance().free(reinterpret_cast<uintptr_t>(p));
    delete[] reinterpret_cast<uint8_t *>(p);
}

extern "C" void *REALLOC(void *p, size_t sz)
{
    if (p == 0)
        return MALLOC(sz);
    if (sz == 0)
    {
        FREE(p);
        return 0;
    }

    // Don't attempt to read past the end of the source buffer if we can help it
    size_t copySz =
        SlamAllocator::instance().allocSize(reinterpret_cast<uintptr_t>(p)) -
        sizeof(SlamAllocator::AllocFooter);
    if (copySz > sz)
        copySz = sz;

    /// \note If sz > p's original size, this may fail.
    void *tmp = MALLOC(sz);
    MemoryCopy(tmp, p, copySz);
    FREE(p);

    return tmp;
}

#if !HOSTED_SYSTEM_MALLOC
namespace std
{
    enum class align_val_t : size_t {};
}

void *operator new(size_t size) noexcept
{
    void *ret =
        reinterpret_cast<void *>(SlamAllocator::instance().allocate(size));
    return ret;
}
void *operator new[](size_t size) noexcept
{
    void *ret =
        reinterpret_cast<void *>(SlamAllocator::instance().allocate(size));
    return ret;
}
void *operator new(size_t size, void *memory) noexcept
{
    return memory;
}
void *operator new(size_t size, std::align_val_t align)
{
    /// \todo manage alignment
    void *ret =
        reinterpret_cast<void *>(SlamAllocator::instance().allocate(size));
    return ret;
}
void *operator new[](size_t size, void *memory) noexcept
{
    return memory;
}
static void delete_shared(void *p) noexcept
{
    if (p == 0)
        return;
    uintptr_t mem = reinterpret_cast<uintptr_t>(p);
    // We want to attempt to delete even if this is not a valid pointer if
    // allocations are being traced, so we can catch the bad free and get a
    // backtrace for it.
    if (traceAllocations || SlamAllocator::instance().isPointerValid(mem))
    {
        SlamAllocator::instance().free(mem);
    }
    else
    {
        if (SlamAllocator::instance().isWithinHeap(mem))
        {
            FATAL("delete_shared failed as pointer was invalid: " << p);
        }
        else
        {
            // less critical - still annoying
            PEDANTRY(
                "delete_shared failed as pointer was not in the kernel heap: "
                << p);
        }
    }
}
void operator delete(void *p) noexcept
{
    delete_shared(p);
}
void operator delete[](void *p) noexcept
{
    delete_shared(p);
}
void operator delete(void *p, size_t sz) noexcept
{
    delete_shared(p);
}
void operator delete(void* p, std::align_val_t align) noexcept
{
    delete_shared(p);
}
void operator delete(void* p, size_t sz, std::align_val_t align) noexcept
{
    delete_shared(p);
}
void operator delete[](void *p, size_t sz) noexcept
{
    delete_shared(p);
}
void operator delete(void *p, void *q) noexcept
{
    // no-op
}
void operator delete[](void *p, void *q) noexcept
{
    // no-op
}
#endif

#if HOSTED
extern "C" {
void *__wrap_malloc(size_t sz)
{
    return _malloc(sz);
}

void *__wrap_realloc(void *p, size_t sz)
{
    return _realloc(p, sz);
}

void __wrap_free(void *p)
{
    return _free(p);
}
}
#endif
