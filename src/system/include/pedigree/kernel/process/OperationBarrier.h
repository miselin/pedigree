/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_PROCESS_OPERATIONBARRIER_H
#define PEDIGREE_KERNEL_PROCESS_OPERATIONBARRIER_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/WaitQueue.h"

/**
 * Closes admission to asynchronous work and drains work already admitted.
 *
 * tryEnter() is safe in non-sleeping contexts. Every successful admission must
 * have exactly one matching leave() after its final owner has stopped touching
 * the protected object. Teardown closes admission before waiting, so late work
 * is rejected without racing the final active operation.
 */
class EXPORTED_PUBLIC OperationBarrier
{
  public:
    /** RAII admission for callbacks which complete in the current scope. */
    class EXPORTED_PUBLIC Lease
    {
      public:
        Lease();
        Lease(Lease &&other);
        ~Lease();

        Lease &operator=(Lease &&other);

        explicit operator bool() const
        {
            return m_Barrier != nullptr;
        }

      private:
        friend class OperationBarrier;

        explicit Lease(OperationBarrier *barrier);
        void reset();

        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;

        OperationBarrier *m_Barrier;
    };

    OperationBarrier();
    ~OperationBarrier();

    /** Attempts to admit one operation without blocking. */
    MUST_USE_RESULT bool tryEnter();

    /**
     * Attempts one scope-bound admission without blocking.
     *
     * The scalar/out shape is stable across the native hosted kernel and
     * Pedigree module compilers.
     */
    MUST_USE_RESULT bool tryAcquire(Lease &lease);

    /** Completes one successfully admitted operation. */
    void leave();

    /** Rejects future admissions without waiting for existing work. */
    void close();

    /** Waits for admitted work after close() has been called. */
    void wait();

    /** Atomically closes admission and waits for admitted work. */
    void closeAndWait();

    bool isOpen();
    bool isClosedAndDrained();

  private:
    NOT_COPYABLE_OR_ASSIGNABLE(OperationBarrier);

    WaitQueue m_Waiters;
    bool m_Open;
    size_t m_ActiveOperations;
};

#endif
