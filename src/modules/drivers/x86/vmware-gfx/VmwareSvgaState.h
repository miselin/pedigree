/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef VMWARE_SVGA_STATE_H
#define VMWARE_SVGA_STATE_H

#include "pedigree/kernel/processor/types.h"

namespace VmwareSvgaState
{
constexpr uint64_t FifoSyncTimeout = 1000000000ULL;
constexpr size_t FifoSyncPollLimit = 10000000;

class PollBudget
{
  public:
    explicit PollBudget(
        uint64_t started, uint64_t timeout = FifoSyncTimeout,
        size_t pollLimit = FifoSyncPollLimit)
        : m_Started(started), m_Timeout(timeout), m_PollLimit(pollLimit),
          m_Polls(0)
    {
    }

    bool keepPolling(uint64_t now)
    {
        if (m_Polls >= m_PollLimit)
            return false;

        ++m_Polls;
        return (m_Polls < m_PollLimit) && ((now - m_Started) < m_Timeout);
    }

    size_t polls() const
    {
        return m_Polls;
    }

  private:
    uint64_t m_Started;
    uint64_t m_Timeout;
    size_t m_PollLimit;
    size_t m_Polls;
};

struct FifoLayout
{
    uint32_t min;
    uint32_t max;
    uint32_t next;
    uint32_t stop;
};

inline bool valid(const FifoLayout &layout)
{
    constexpr uint32_t alignmentMask = sizeof(uint32_t) - 1;
    return !(layout.min & alignmentMask) && !(layout.max & alignmentMask) &&
           !(layout.next & alignmentMask) && !(layout.stop & alignmentMask) &&
           (layout.max > layout.min) &&
           ((layout.max - layout.min) >= (2 * sizeof(uint32_t))) &&
           (layout.next >= layout.min) && (layout.next < layout.max) &&
           (layout.stop >= layout.min) && (layout.stop < layout.max);
}

inline bool
valid(const FifoLayout &layout, size_t headerFloor, size_t apertureBytes)
{
    return valid(layout) && (layout.min >= headerFloor) &&
           (layout.max <= apertureBytes);
}

inline size_t freeBytes(const FifoLayout &layout)
{
    if (!valid(layout))
        return 0;

    if (layout.next >= layout.stop)
    {
        return (layout.max - layout.next) + (layout.stop - layout.min) -
               sizeof(uint32_t);
    }

    return layout.stop - layout.next - sizeof(uint32_t);
}

inline bool canFit(const FifoLayout &layout, size_t words)
{
    if (!words || (words > (~static_cast<size_t>(0) / sizeof(uint32_t))))
        return false;

    return (words * sizeof(uint32_t)) <= freeBytes(layout);
}
}  // namespace VmwareSvgaState

#endif
