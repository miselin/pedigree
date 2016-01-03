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

#include <compiler.h>
#include <processor/types.h>
#include "cppsupport.h"
#include <panic.h>
#include <Log.h>

#include <utilities/Tree.h>

Tree<void*, void*> g_FreedPointers;

#include "SlamAllocator.h"

extern "C"
{
    void *malloc(size_t);
    void *realloc(void *, size_t);
    void free(void *);
}

/// If the debug allocator is enabled, this switches it into underflow detection
/// mode.
#define DEBUG_ALLOCATOR_CHECK_UNDERFLOWS

// Required for G++ to link static init/destructors.
#ifndef HOSTED
void *__dso_handle;
#endif

// Defined in the linker.
extern uintptr_t start_ctors;
extern uintptr_t end_ctors;
extern uintptr_t start_dtors;
extern uintptr_t end_dtors;

/// Calls the constructors for all global objects.
/// Call this before using any global objects.
void initialiseConstructors()
{
  // Constructor list is defined in the linker script.
  // The .ctors section is just an array of function pointers.
  // iterate through, calling each in turn.
  uintptr_t *iterator = &start_ctors;
  while (iterator < &end_ctors)
  {
    void (*fp)(void) = reinterpret_cast<void (*)(void)>(*iterator);
    fp();
    iterator++;
  }
}

void runKernelDestructors()
{
  uintptr_t *iterator = &start_dtors;
  while (iterator < &end_dtors)
  {
    void (*fp)(void) = reinterpret_cast<void (*)(void)>(*iterator);
    fp();
    iterator++;
  }
}

#ifdef MEMORY_TRACING
static bool traceAllocations = false;
void startTracingAllocations()
{
    traceAllocations = true;
}

void stopTracingAllocations()
{
    traceAllocations = false;
}

Spinlock traceLock;
void traceAllocation(void *ptr, MemoryTracing::AllocationTrace type, size_t size)
{
    // Don't trace if we're not allowed to.
    if (!traceAllocations)
        return;

    // Yes, this means we'll lose early init mallocs. Oh well...
    if(!Machine::instance().isInitialised())
        return;

    Serial *pSerial = Machine::instance().getSerial(1);
    if(!pSerial)
        return;

    LockGuard<Spinlock> guard(traceLock);

    char buf[255];

    size_t off = 0;

    // Allocation type.
    memcpy(&buf[off], &type, 1);
    ++off;

    // Resulting pointer.
    memcpy(&buf[off], &ptr, sizeof(void*));
    off += sizeof(void*);

    // Don't store size or backtrace for frees.
    if(type == MemoryTracing::Allocation || type == MemoryTracing::PageAlloc)
    {
        // Size of allocation.
        memcpy(&buf[off], &size, sizeof(size_t));
        off += sizeof(size_t);

        // Backtrace.
        do
        {
#define DO_BACKTRACE(v, level, buffer, offset) { \
                if(!__builtin_frame_address(level)) break; \
                v = __builtin_return_address(level); \
                memcpy(&buffer[offset], &v, sizeof(void*)); \
                offset += sizeof(void*); \
            }

            void *p;

            DO_BACKTRACE(p, 1, buf, off);
            DO_BACKTRACE(p, 2, buf, off);
            DO_BACKTRACE(p, 3, buf, off);
            DO_BACKTRACE(p, 4, buf, off);
            DO_BACKTRACE(p, 5, buf, off);
            DO_BACKTRACE(p, 6, buf, off);
        } while(0);

        // Ensure the trace always ends in a zero frame (ie, end of trace)
        uintptr_t endoftrace = 0;
        memcpy(&buf[off], &endoftrace, sizeof(uintptr_t));
        off += sizeof(uintptr_t);
    }

    for(size_t i = 0; i < off; ++i)
    {
        pSerial->write(buf[i]);
    }
}

/**
 * Adds a metadata field to the memory trace.
 *
 * This is typically used to define the region in which a module has been
 * loaded, so the correct debug symbols can be loaded and used.
 */
void traceMetadata(NormalStaticString str, void *p1, void *p2)
{
    LockGuard<Spinlock> guard(traceLock);

    // Yes, this means we'll lose early init mallocs. Oh well...
    if(!Machine::instance().isInitialised())
        return;

    Serial *pSerial = Machine::instance().getSerial(1);
    if(!pSerial)
        return;

    char buf[128];
    memset(buf, 0, 128);

    size_t off = 0;

    const MemoryTracing::AllocationTrace type = MemoryTracing::Metadata;

    memcpy(&buf[off], &type, 1);
    ++off;
    memcpy(&buf[off], static_cast<const char *>(str), str.length());
    off += 64; // Statically sized segment.
    memcpy(&buf[off], &p1, sizeof(void*));
    off += sizeof(void*);
    memcpy(&buf[off], &p2, sizeof(void*));
    off += sizeof(void*);

    for(size_t i = 0; i < off; ++i)
    {
        pSerial->write(buf[i]);
    }
}
#endif

#ifdef ARM_COMMON
#define ATEXIT __aeabi_atexit
#else
#define ATEXIT __cxa_atexit
#endif

/// Required for G++ to compile code.
extern "C" void ATEXIT(void (*f)(void *), void *p, void *d)
{
}

/// Called by G++ if a pure virtual function is called. Bad Thing, should never happen!
extern "C" void __cxa_pure_virtual() NORETURN;
void __cxa_pure_virtual()
{
    FATAL_NOLOCK("Pure virtual function call made");
}

/// Called by G++ if function local statics are initialised for the first time
#ifndef HAS_THREAD_SANITIZER
extern "C" int __cxa_guard_acquire()
{
  return 1;
}
extern "C" void __cxa_guard_release()
{
  // TODO
}
#endif

#if !(defined(HAS_ADDRESS_SANITIZER) || defined(HAS_THREAD_SANITIZER))
#ifdef HOSTED
extern "C" void *_malloc(size_t sz)
#else
extern "C" void *malloc(size_t sz)
#endif
{
    return reinterpret_cast<void *>(new uint8_t[sz]); //SlamAllocator::instance().allocate(sz));
}

#ifdef HOSTED
extern "C" void _free(void *p)
#else
extern "C" void free(void *p)
#endif
{
    if (p == 0)
        return;
    //SlamAllocator::instance().free(reinterpret_cast<uintptr_t>(p));
    delete [] reinterpret_cast<uint8_t*>(p);
}

#ifdef HOSTED
extern "C" void *_realloc(void *p, size_t sz)
#else
extern "C" void *realloc(void *p, size_t sz)
#endif
{
    if (p == 0)
        return malloc(sz);
    if (sz == 0)
    {
        free(p);
        return 0;
    }

    // Don't attempt to read past the end of the source buffer if we can help it
    size_t copySz = SlamAllocator::instance().allocSize(reinterpret_cast<uintptr_t>(p)) - sizeof(SlamAllocator::AllocFooter);
    if(copySz > sz)
        copySz = sz;
    
    /// \note If sz > p's original size, this may fail.
    void *tmp = malloc(sz);
    memcpy(tmp, p, copySz);
    free(p);

    return tmp;
}

void *operator new (size_t size) throw()
{
    void *ret = reinterpret_cast<void *>(SlamAllocator::instance().allocate(size));
    return ret;
}
void *operator new[] (size_t size) throw()
{
    void *ret = reinterpret_cast<void *>(SlamAllocator::instance().allocate(size));
    return ret;
}
void *operator new (size_t size, void* memory) throw()
{
  return memory;
}
void *operator new[] (size_t size, void* memory) throw()
{
  return memory;
}
void operator delete (void * p) throw()
{
    if (p == 0) return;
    if(SlamAllocator::instance().isPointerValid(reinterpret_cast<uintptr_t>(p)))
        SlamAllocator::instance().free(reinterpret_cast<uintptr_t>(p));
}
void operator delete[] (void * p) throw()
{
    if (p == 0) return;
    if(SlamAllocator::instance().isPointerValid(reinterpret_cast<uintptr_t>(p)))
        SlamAllocator::instance().free(reinterpret_cast<uintptr_t>(p));
}
void operator delete (void *p, void *q) throw()
{
  // TODO
  panic("Operator delete (placement) -implement");
}
void operator delete[] (void *p, void *q) throw()
{
  // TODO
  panic("Operator delete[] (placement) -implement");
}
#endif

#if defined(HOSTED) && (!(defined(HAS_ADDRESS_SANITIZER) || defined(HAS_THREAD_SANITIZER)))
extern "C"
{

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
