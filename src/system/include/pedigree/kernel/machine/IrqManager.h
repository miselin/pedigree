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

#ifndef KERNEL_MACHINE_IRQMANAGER_H
#define KERNEL_MACHINE_IRQMANAGER_H

#include "pedigree/kernel/machine/types.h"
#include "pedigree/kernel/processor/types.h"

class Device;
class HardIrqHandler;
class IrqHandler;
class IrqHandlerBase;

/** @addtogroup kernelmachine
 * @{ */

/**
 * Source-declared trigger semantics used for safe controller behaviour.
 * Registration does not itself program platform electrical routing such as
 * the PC ELCR.
 */
enum class IrqTrigger : uint8_t
{
    Edge,
    Level,
    Synthetic,
};

/** Controller acknowledgement order relative to the bounded hard stage. */
enum class IrqControllerAck : uint8_t
{
    None,
    BeforeHardStage,
    AfterHardStage,
};

/** Point at which a controller-masked line may be made live again. */
enum class IrqLineRelease : uint8_t
{
    AfterHardStage,
    AfterThreadedCompletion,
};

/**
 * Orthogonal interrupt-line delivery policy.
 *
 * The hard stage is either a HardIrqHandler callback or publication to an
 * IrqHandler worker. Device acknowledgement remains the handler's
 * responsibility: a split hard callback must quiesce its source before it
 * defers, while a normal level-triggered worker runs with the controller line
 * masked until it reports completion.
 */
class IrqPolicy
{
  public:
    constexpr IrqPolicy(
        IrqTrigger trigger, IrqControllerAck controllerAck,
        IrqLineRelease lineRelease)
        : m_Trigger(trigger), m_ControllerAck(controllerAck),
          m_LineRelease(lineRelease)
    {
    }

    static constexpr IrqPolicy edgeHard()
    {
        return IrqPolicy(
            IrqTrigger::Edge, IrqControllerAck::BeforeHardStage,
            IrqLineRelease::AfterHardStage);
    }

    static constexpr IrqPolicy edgeThreaded()
    {
        return IrqPolicy(
            IrqTrigger::Edge, IrqControllerAck::AfterHardStage,
            IrqLineRelease::AfterHardStage);
    }

    static constexpr IrqPolicy levelHard()
    {
        return IrqPolicy(
            IrqTrigger::Level, IrqControllerAck::AfterHardStage,
            IrqLineRelease::AfterHardStage);
    }

    static constexpr IrqPolicy levelThreaded()
    {
        return IrqPolicy(
            IrqTrigger::Level, IrqControllerAck::AfterHardStage,
            IrqLineRelease::AfterThreadedCompletion);
    }

    static constexpr IrqPolicy pciIntxHard()
    {
        return levelHard();
    }

    static constexpr IrqPolicy pciIntxThreaded()
    {
        return levelThreaded();
    }

    static constexpr IrqPolicy syntheticHard()
    {
        return IrqPolicy(
            IrqTrigger::Synthetic, IrqControllerAck::None,
            IrqLineRelease::AfterHardStage);
    }

    static constexpr IrqPolicy syntheticThreaded()
    {
        return IrqPolicy(
            IrqTrigger::Synthetic, IrqControllerAck::None,
            IrqLineRelease::AfterHardStage);
    }

    constexpr IrqTrigger trigger() const
    {
        return m_Trigger;
    }

    constexpr IrqControllerAck controllerAck() const
    {
        return m_ControllerAck;
    }

    constexpr IrqLineRelease lineRelease() const
    {
        return m_LineRelease;
    }

    constexpr bool validForHard() const
    {
        return valid() && m_LineRelease == IrqLineRelease::AfterHardStage &&
               !(m_Trigger == IrqTrigger::Level &&
                 m_ControllerAck == IrqControllerAck::BeforeHardStage);
    }

    constexpr bool validForThreaded() const
    {
        return valid() &&
               ((m_Trigger == IrqTrigger::Level &&
                 m_LineRelease ==
                     IrqLineRelease::AfterThreadedCompletion) ||
                (m_Trigger != IrqTrigger::Level &&
                 m_LineRelease == IrqLineRelease::AfterHardStage));
    }

    constexpr bool operator==(const IrqPolicy &other) const
    {
        return m_Trigger == other.m_Trigger &&
               m_ControllerAck == other.m_ControllerAck &&
               m_LineRelease == other.m_LineRelease;
    }

    constexpr bool operator!=(const IrqPolicy &other) const
    {
        return !(*this == other);
    }

  private:
    constexpr bool valid() const
    {
        const bool synthetic = m_Trigger == IrqTrigger::Synthetic;
        if (synthetic != (m_ControllerAck == IrqControllerAck::None))
        {
            return false;
        }

        return m_LineRelease != IrqLineRelease::AfterThreadedCompletion ||
               m_Trigger == IrqTrigger::Level;
    }

    IrqTrigger m_Trigger;
    IrqControllerAck m_ControllerAck;
    IrqLineRelease m_LineRelease;
};

/** Delivery context currently configured for a physical interrupt line. */
enum class IrqDelivery : uint8_t
{
    None,
    Hard,
    Threaded,
};

/** Independent reasons an interrupt line is not currently live. */
enum IrqMaskReason : uint16_t
{
    IrqMaskNone = 0,
    IrqMaskNoHandler = 1U << 0,
    IrqMaskAdministrativelyDisabled = 1U << 1,
    IrqMaskAwaitingAcknowledgement = 1U << 2,
    IrqMaskAwaitingThreadedCompletion = 1U << 3,
    IrqMaskMitigated = 1U << 4,
    IrqMaskShuttingDown = 1U << 5,
};

/** Detached higher-level state for a threaded IRQ worker. */
enum class IrqWorkerDebugState : uint8_t
{
    Unavailable,
    None,
    SemaphoreWait,
    ConditionWait,
    Joining,
    FutexWait,
    EventWait,
    ProcessWait,
    CallbackDrain,
};

/** Detached wake state for a threaded IRQ worker's active WaitQueue record. */
enum class IrqWorkerWaitReason : uint8_t
{
    Unavailable,
    Waiting,
    Signalled,
    Event,
    Unwinding,
    Terminating,
    Spurious,
};

/**
 * Detached debugger-facing state for one physical interrupt line.
 *
 * This is deliberately a plain data object: consumers must not retain or
 * dereference any live kernel object while the debugger has other CPUs
 * stopped. The snapshot is best-effort while the machine is running, but a
 * controller must always leave its last complete publication readable if it
 * is interrupted partway through publishing a newer one.
 */
struct IrqLineDiagnosticSnapshot
{
    size_t snapshotGeneration;
    size_t observationTimestamp;
    size_t dispatchGeneration;
    size_t acknowledgedGeneration;
    size_t activeHardDispatchCount;
    size_t activeHardDispatchGeneration;
    size_t activeThreadedDispatchCount;
    size_t publicationCookie;
    size_t pendingCookie;
    size_t activeCookie;
    size_t completedCookie;
    size_t completedBatches;
    /** Independent best-effort samples; stopped-world snapshots are coherent. */
    size_t pendingSinceTimestamp;
    size_t activeCallbackStartedTimestamp;
    size_t lastWakeLatency;
    size_t maximumWakeLatency;
    size_t lastCallbackRuntime;
    size_t maximumCallbackRuntime;
    size_t interruptCount;
    size_t spuriousCount;
    size_t unhandledCount;
    size_t publicationFailures;
    size_t removalRejections;
    size_t diagnosticPublicationFailures;
    uintptr_t workerIdentity;
    uintptr_t activeThreadedHandlerIdentity;
    uintptr_t workerDebugAddress;
    uintptr_t workerWaitQueue;
    uintptr_t workerWaitChannelOwner;
    uintptr_t workerWaitChannelValue;
    size_t workerWaitStateLevel;
    size_t handlerCount;
    uint16_t maskReasons;
    uint8_t line;
    IrqDelivery delivery;
    IrqTrigger trigger;
    IrqControllerAck controllerAck;
    IrqLineRelease lineRelease;
    IrqWorkerDebugState workerDebugState;
    IrqWorkerWaitReason workerWaitReason;
    bool configured;
    bool effectiveMasked;
    bool requestedEnabled;
    bool acknowledgementPending;
    bool threadedPending;
    bool dispatcherInitialised;
    bool dispatcherActive;
    bool dispatcherClosed;
    bool hardStageActive;
    bool workerDiagnosticAvailable;
    bool workerWaitActive;
    bool workerWaitQueued;
};

/** This class handles IRQ (un)registration */
class IrqManager
{
  public:
    /** Control codes for the control function */
    enum ControlCode
    {
        MitigationThreshold, /** Controls the number of IRQs within a 1 ms
                              *  period that can occur before the IRQ is
                              *  mitigated. */
    };

    /** Register an ISA irq
     *\param[in] irq the ISA irq number (from 0 to 15)
     *\param[in] handler pointer to the IrqHandler class
     *\param[in] policy electrical, acknowledgement, and completion policy
     *\return the irq's identifier */
    virtual irq_id_t registerIsaIrqHandler(
        uint8_t irq, IrqHandler *handler, const IrqPolicy &policy) = 0;
    /** Register a PCI irq */
    virtual irq_id_t registerPciIrqHandler(
        IrqHandler *handler, Device *pDevice, const IrqPolicy &policy) = 0;

    /** Register an ISA handler which must run in hard IRQ context. */
    virtual irq_id_t registerHardIsaIrqHandler(
        uint8_t irq, HardIrqHandler *handler, const IrqPolicy &policy) = 0;

    /** Register a PCI handler which must run in hard IRQ context. */
    virtual irq_id_t registerHardPciIrqHandler(
        HardIrqHandler *handler, Device *pDevice,
        const IrqPolicy &policy) = 0;
    /**
     * Unregister a previously registered IrqHandler.
     *
     * A successful return is an ownership barrier: no callback can begin or
     * remain active for the handler. A callback cannot synchronously remove
     * itself, and an atomic context cannot wait for an active callback; those
     * requests return false.
     *
     *\param[in] Id the irq's identifier
     *\return true if removal completed synchronously
     */
    virtual bool unregisterHandler(irq_id_t Id, IrqHandlerBase *handler) = 0;

    virtual void enable(uint8_t irq, bool enable) = 0;

    /**
     * Copies up to `capacity` detached line diagnostics without taking an IRQ
     * manager or handler-registry lock. Unsupported managers return zero.
     */
    virtual size_t
    snapshotIrqLines(IrqLineDiagnosticSnapshot *out, size_t capacity) const;

    /** Called every millisecond, typically handles IRQ mitigation. */
    virtual void tick();

    /** Controls specific elements of a given IRQ */
    virtual bool control(uint8_t irq, ControlCode code, size_t argument);

  protected:
    /** The default constructor */
    IrqManager();
    /** The destructor */
    virtual ~IrqManager();

  private:
    /** The copy-constructor
     *\note NOT implemented */
    IrqManager(const IrqManager &);
    /** The assignment operator
     *\note NOT implemented */
    IrqManager &operator=(const IrqManager &);
};

/** @} */

#endif
