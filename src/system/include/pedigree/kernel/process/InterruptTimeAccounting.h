/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_PROCESS_INTERRUPTTIMEACCOUNTING_H
#define PEDIGREE_KERNEL_PROCESS_INTERRUPTTIMEACCOUNTING_H

class Thread;

/**
 * Accounts one raw interrupt frame without invoking an arbitrary callback.
 *
 * Construction and destruction perform only a monotonic clock sample and the
 * fixed lock-free Thread/Process accounting publication path. Timer mutation
 * and signal delivery belong to the ordinary accounting worker.
 */
class InterruptTimeAccounting
{
  public:
    explicit InterruptTimeAccounting(bool fromUserspace);
    ~InterruptTimeAccounting();

    /** Charges the completed return tail and begins the next user slice. */
    static void finishUserReturn(Thread *thread);

  private:
    Thread *m_pThread;
    bool m_bFromUserspace;

    InterruptTimeAccounting(const InterruptTimeAccounting &) = delete;
    InterruptTimeAccounting &operator=(
        const InterruptTimeAccounting &) = delete;
};

#endif
