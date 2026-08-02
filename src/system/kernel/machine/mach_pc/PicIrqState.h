/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#ifndef PEDIGREE_KERNEL_MACHINE_MACH_PC_PICIRQSTATE_H
#define PEDIGREE_KERNEL_MACHINE_MACH_PC_PICIRQSTATE_H

#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/assert.h"

/**
 * Software ownership state for the dual 8259 PIC.
 *
 * The caller serialises every operation with the PIC lock and writes the
 * returned master/slave masks to hardware. Registration counts are updated
 * independently of the callback registry's drain latency, so a new handler
 * cannot be hidden by the final accounting step of an older unregister.
 */
class PicIrqState
{
  public:
    static constexpr size_t LineCount = 16;

    enum class TriggerMode : uint8_t
    {
        Unconfigured,
        Level,
        Edge,
    };

    PicIrqState() : m_Mask(0)
    {
        for (size_t i = 0; i < LineCount; ++i)
        {
            m_TriggerModes[i] = TriggerMode::Unconfigured;
            m_ControllerAck[i] = IrqControllerAck::AfterHardStage;
            m_LineRelease[i] = IrqLineRelease::AfterHardStage;
            m_HandlerCounts[i] = 0;
            m_DispatchGenerations[i] = 0;
            m_AcknowledgedGenerations[i] = 0;
            m_AcknowledgementPending[i] = false;
            m_ThreadedPending[i] = false;
            m_RequestedEnabled[i] = true;
        }
    }

    bool canRegister(size_t irq, const IrqPolicy &policy) const
    {
        if (irq >= LineCount)
        {
            return false;
        }

        const TriggerMode requested =
            policy.trigger() == IrqTrigger::Edge ? TriggerMode::Edge :
                                                   TriggerMode::Level;
        return m_TriggerModes[irq] == TriggerMode::Unconfigured ||
               (m_TriggerModes[irq] == requested &&
                m_ControllerAck[irq] == policy.controllerAck() &&
                m_LineRelease[irq] == policy.lineRelease());
    }

    void handlerRegistered(size_t irq, const IrqPolicy &policy)
    {
        assert(canRegister(irq, policy));
        const bool firstHandler = m_HandlerCounts[irq] == 0;
        if (m_TriggerModes[irq] == TriggerMode::Unconfigured)
        {
            m_TriggerModes[irq] =
                policy.trigger() == IrqTrigger::Edge ? TriggerMode::Edge :
                                                       TriggerMode::Level;
            m_ControllerAck[irq] = policy.controllerAck();
            m_LineRelease[irq] = policy.lineRelease();
        }
        ++m_HandlerCounts[irq];
        if (firstHandler)
        {
            m_AcknowledgementPending[irq] = false;
            m_ThreadedPending[irq] = false;
            m_AcknowledgedGenerations[irq] = m_DispatchGenerations[irq];
            m_RequestedEnabled[irq] = true;
            rebuildMask();
        }
    }

    void handlerUnregistered(size_t irq)
    {
        assert(irq < LineCount);
        assert(m_HandlerCounts[irq]);
        --m_HandlerCounts[irq];
        if (!m_HandlerCounts[irq])
        {
            m_AcknowledgementPending[irq] = false;
            m_ThreadedPending[irq] = false;
            m_AcknowledgedGenerations[irq] = m_DispatchGenerations[irq];
            m_RequestedEnabled[irq] = false;
            m_TriggerModes[irq] = TriggerMode::Unconfigured;
            m_ControllerAck[irq] = IrqControllerAck::AfterHardStage;
            m_LineRelease[irq] = IrqLineRelease::AfterHardStage;
            rebuildMask();
        }
    }

    size_t beginDispatch(size_t irq)
    {
        assert(irq < LineCount);
        return ++m_DispatchGenerations[irq];
    }

    /**
     * Completes a dispatch without losing an acknowledgement which raced the
     * handler return. An acknowledgement covers every dispatch admitted
     * before it observed the line.
     */
    void completeDispatch(
        size_t irq, size_t dispatchGeneration, bool needsAcknowledgement)
    {
        assert(irq < LineCount);
        if (!needsAcknowledgement || !m_HandlerCounts[irq] ||
            generationReached(
                m_AcknowledgedGenerations[irq], dispatchGeneration))
        {
            return;
        }

        m_AcknowledgementPending[irq] = true;
        rebuildMask();
    }

    bool acknowledge(size_t irq)
    {
        assert(irq < LineCount);
        if (!m_HandlerCounts[irq])
        {
            return false;
        }

        m_AcknowledgedGenerations[irq] = m_DispatchGenerations[irq];
        if (m_AcknowledgementPending[irq])
        {
            m_AcknowledgementPending[irq] = false;
            rebuildMask();
        }
        return true;
    }

    /** Applies a policy-requested mask until the bottom half completes. */
    void beginThreadedDispatch(size_t irq)
    {
        assert(irq < LineCount);
        if (m_LineRelease[irq] == IrqLineRelease::AfterThreadedCompletion)
        {
            m_ThreadedPending[irq] = true;
            rebuildMask();
        }
    }

    /**
     * Completes the latest threaded batch. A stale batch cannot reopen a line
     * which has since delivered another occurrence.
     */
    bool completeThreadedDispatch(
        size_t irq, size_t dispatchGeneration, bool allowRearm)
    {
        assert(irq < LineCount);
        if (m_DispatchGenerations[irq] != dispatchGeneration)
        {
            return false;
        }

        if (m_LineRelease[irq] == IrqLineRelease::AfterThreadedCompletion &&
            (allowRearm || !m_HandlerCounts[irq]))
        {
            m_ThreadedPending[irq] = false;
            rebuildMask();
        }
        return true;
    }

    bool threadedPending(size_t irq) const
    {
        assert(irq < LineCount);
        return m_ThreadedPending[irq];
    }

    bool acknowledgementPending(size_t irq) const
    {
        assert(irq < LineCount);
        return m_AcknowledgementPending[irq];
    }

    size_t handlerCount(size_t irq) const
    {
        assert(irq < LineCount);
        return m_HandlerCounts[irq];
    }

    bool edgeTriggered(size_t irq) const
    {
        assert(irq < LineCount);
        return m_TriggerModes[irq] == TriggerMode::Edge;
    }

    IrqControllerAck controllerAck(size_t irq) const
    {
        assert(irq < LineCount);
        return m_ControllerAck[irq];
    }

    IrqLineRelease lineRelease(size_t irq) const
    {
        assert(irq < LineCount);
        return m_LineRelease[irq];
    }

    bool enabled(size_t irq) const
    {
        assert(irq < LineCount);
        return (m_Mask & bit(irq)) == 0;
    }

    void setEnabled(size_t irq, bool enabled)
    {
        assert(irq < LineCount);
        m_RequestedEnabled[irq] = enabled;
        rebuildMask();
    }

    void setAllEnabled(bool enabled)
    {
        for (size_t i = 0; i < LineCount; ++i)
        {
            m_RequestedEnabled[i] = enabled;
        }

        // IRQ2 is the cascade input and must remain available when device
        // lines are masked, or every slave IRQ becomes unreachable.
        if (!enabled)
        {
            m_RequestedEnabled[2] = true;
        }
        rebuildMask();
    }

    uint16_t mask() const
    {
        return m_Mask;
    }

    uint8_t masterMask() const
    {
        return static_cast<uint8_t>(m_Mask & 0xFF);
    }

    uint8_t slaveMask() const
    {
        return static_cast<uint8_t>(m_Mask >> 8);
    }

  private:
    static uint16_t bit(size_t irq)
    {
        return static_cast<uint16_t>(static_cast<uint16_t>(1U) << irq);
    }

    static bool generationReached(size_t current, size_t target)
    {
        return static_cast<intptr_t>(current - target) >= 0;
    }

    void rebuildMask()
    {
        uint16_t mask = 0;
        for (size_t i = 0; i < LineCount; ++i)
        {
            if (!m_RequestedEnabled[i] || m_AcknowledgementPending[i] ||
                m_ThreadedPending[i])
            {
                mask |= bit(i);
            }
        }

        // The master IRQ2 bit represents both a direct IRQ2 source and every
        // slave line, so a live slave always wins over a direct-line mask.
        if ((mask & static_cast<uint16_t>(0xFF00)) !=
            static_cast<uint16_t>(0xFF00))
        {
            mask &= static_cast<uint16_t>(~bit(2));
        }
        m_Mask = mask;
    }

    uint16_t m_Mask;
    TriggerMode m_TriggerModes[LineCount];
    IrqControllerAck m_ControllerAck[LineCount];
    IrqLineRelease m_LineRelease[LineCount];
    size_t m_HandlerCounts[LineCount];
    size_t m_DispatchGenerations[LineCount];
    size_t m_AcknowledgedGenerations[LineCount];
    bool m_AcknowledgementPending[LineCount];
    bool m_ThreadedPending[LineCount];
    bool m_RequestedEnabled[LineCount];
};

#endif
