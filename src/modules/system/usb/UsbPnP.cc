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

#include "modules/system/usb/UsbPnP.h"
#include "modules/system/usb/UsbDevice.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/process/OperationBarrier.h"
#include "pedigree/kernel/utilities/utility.h"

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/time/Time.h"
#endif

UsbPnP UsbPnP::m_Instance;

struct UsbPnP::CallbackItem
{
    CallbackItem(
        callback_t callback, uint16_t vendorId, uint16_t productId,
        uint8_t deviceClass, uint8_t subclass, uint8_t protocol)
        : callback(callback), nVendorId(vendorId), nProductId(productId),
          nClass(deviceClass), nSubclass(subclass), nProtocol(protocol),
          sequence(0), next(nullptr), operations()
    {
    }

    callback_t callback;
    uint16_t nVendorId;
    uint16_t nProductId;
    uint8_t nClass;
    uint8_t nSubclass;
    uint8_t nProtocol;
    size_t sequence;
    CallbackItem *next;
    OperationBarrier operations;
};

UsbPnP::Registration::Registration() : m_Owner(nullptr), m_Item(nullptr)
{
}

UsbPnP::Registration::Registration(Registration &&other)
    : m_Owner(other.m_Owner), m_Item(other.m_Item)
{
    other.m_Owner = nullptr;
    other.m_Item = nullptr;
}

UsbPnP::Registration::~Registration()
{
    reset();
}

UsbPnP::Registration &
UsbPnP::Registration::operator=(Registration &&other)
{
    if (this != &other)
    {
        reset();
        m_Owner = other.m_Owner;
        m_Item = other.m_Item;
        other.m_Owner = nullptr;
        other.m_Item = nullptr;
    }
    return *this;
}

void UsbPnP::Registration::reset()
{
    if (!m_Item)
    {
        return;
    }

    UsbPnP *owner = m_Owner;
    CallbackItem *item = m_Item;
    m_Owner = nullptr;
    m_Item = nullptr;
    owner->unregisterCallback(item);
}

void UsbPnP::Registration::adopt(UsbPnP *owner, CallbackItem *item)
{
    m_Owner = owner;
    m_Item = item;
}

UsbPnP::UsbPnP()
    : m_FirstCallback(nullptr), m_LastCallback(nullptr), m_CallbackCount(0),
      m_CallbackLock(), m_NextCallbackSequence(1)
{
}

UsbPnP::~UsbPnP()
{
    while (true)
    {
        CallbackItem *item = nullptr;
        {
            LockGuard<Spinlock> guard(m_CallbackLock);
            if (!m_FirstCallback)
            {
                break;
            }

            item = m_FirstCallback;
            m_FirstCallback = item->next;
            if (!m_FirstCallback)
            {
                m_LastCallback = nullptr;
            }
            --m_CallbackCount;
            item->operations.close();
        }

        item->operations.wait();
        delete item;
    }
}

bool UsbPnP::probeDevice(Device *pDeviceBase)
{
    Device *pResult = doProbe(pDeviceBase);
    return pResult == pDeviceBase;
}

Device *UsbPnP::doProbe(Device *pDeviceBase)
{
    // Sanity check.
    if (!(pDeviceBase->getType() == Device::UsbContainer))
    {
        return pDeviceBase;
    }

    UsbDevice *pDevice =
        static_cast<UsbDeviceContainer *>(pDeviceBase)->getUsbDevice();

    // Is this device already handled by a driver?
    if (pDevice->getUsbState() == UsbDevice::HasDriver)
    {
        return pDeviceBase;
    }

    size_t afterSequence = 0;
    while (true)
    {
        CallbackItem *item = nullptr;
        callback_t callback = nullptr;
        size_t sequence = 0;
        if (!acquireCallback(
                pDevice, afterSequence, item, callback, sequence))
        {
            break;
        }

        // Call the callback, which will give us (hopefully) a copy of pDevice,
        // in the form of a driver class
        UsbDevice *pNewDevice = callback(pDevice);
        afterSequence = sequence;

        // Was this device rejected by the driver?
        if (!pNewDevice)
        {
            item->operations.leave();
            continue;
        }

        // Initialise the driver
        pNewDevice->initialiseDriver();

        // Did the device go into the driver state?
        if (pNewDevice->getUsbState() == UsbDevice::HasDriver)
        {
            // Replace the old device with the new one
            UsbDeviceContainer *pNewContainer =
                new UsbDeviceContainer(pNewDevice);
            item->operations.leave();
            return pNewContainer;
        }
        else
        {
            delete pNewDevice;
            item->operations.leave();
        }
    }
    return pDeviceBase;
}

bool UsbPnP::acquireCallback(
    UsbDevice *device, size_t afterSequence, CallbackItem *&resultItem,
    callback_t &resultCallback, size_t &resultSequence)
{
    resultItem = nullptr;
    resultCallback = nullptr;
    resultSequence = 0;

    UsbDevice::DeviceDescriptor *descriptor = device->getDescriptor();
    UsbDevice::Interface *interface = device->getInterface();

    LockGuard<Spinlock> guard(m_CallbackLock);
    for (CallbackItem *item = m_FirstCallback; item; item = item->next)
    {
        if (item->sequence <= afterSequence)
        {
            continue;
        }

        if (
            (item->nVendorId != VendorIdNone) &&
            (item->nVendorId != descriptor->nVendorId))
        {
            continue;
        }
        if (
            (item->nProductId != ProductIdNone) &&
            (item->nProductId != descriptor->nProductId))
        {
            continue;
        }
        if (
            (item->nClass != ClassNone) &&
            (item->nClass != interface->nClass))
        {
            continue;
        }
        if (
            (item->nSubclass != SubclassNone) &&
            (item->nSubclass != interface->nSubclass))
        {
            continue;
        }
        if (
            (item->nProtocol != ProtocolNone) &&
            (item->nProtocol != interface->nProtocol))
        {
            continue;
        }
        if (!item->operations.tryEnter())
        {
            continue;
        }

        resultItem = item;
        resultCallback = item->callback;
        resultSequence = item->sequence;
        return true;
    }

    return false;
}

void UsbPnP::reprobeDevices(Device *pParent)
{
    auto performReprobe = [](Device *p) {
        if (p->getType() == Device::UsbContainer)
        {
            return UsbPnP::instance().doProbe(p);
        }

        // don't edit the tree - just iterating
        return p;
    };

    auto c = pedigree_std::make_callable(performReprobe);
    Device::foreach (c, pParent);
}

bool UsbPnP::registerCallbackItem(
    CallbackItem *item, Registration &registration, bool reprobe)
{
    if (!item || !item->callback || registration)
    {
        delete item;
        return false;
    }

    {
        LockGuard<Spinlock> guard(m_CallbackLock);
        item->sequence = m_NextCallbackSequence++;
        if (m_LastCallback)
        {
            m_LastCallback->next = item;
        }
        else
        {
            m_FirstCallback = item;
        }
        m_LastCallback = item;
        ++m_CallbackCount;
        registration.adopt(this, item);
    }

    if (reprobe)
    {
        reprobeDevices(nullptr);
    }
    return true;
}

bool UsbPnP::registerCallback(
    uint16_t nVendorId, uint16_t nProductId, callback_t callback,
    Registration &registration)
{
    return registerCallbackItem(
        new CallbackItem(
            callback, nVendorId, nProductId, ClassNone, SubclassNone,
            ProtocolNone),
        registration, true);
}

bool UsbPnP::registerCallback(
    uint8_t nClass, uint8_t nSubclass, uint8_t nProtocol, callback_t callback,
    Registration &registration)
{
    return registerCallbackItem(
        new CallbackItem(
            callback, VendorIdNone, ProductIdNone, nClass, nSubclass,
            nProtocol),
        registration, true);
}

void UsbPnP::unregisterCallback(CallbackItem *item)
{
    bool found = false;
    {
        LockGuard<Spinlock> guard(m_CallbackLock);
        CallbackItem *previous = nullptr;
        for (CallbackItem *current = m_FirstCallback; current;
             current = current->next)
        {
            if (current != item)
            {
                previous = current;
                continue;
            }

            item->operations.close();
            if (previous)
            {
                previous->next = item->next;
            }
            else
            {
                m_FirstCallback = item->next;
            }
            if (m_LastCallback == item)
            {
                m_LastCallback = previous;
            }
            item->next = nullptr;
            --m_CallbackCount;
            found = true;
            break;
        }
    }

    if (found)
    {
        item->operations.wait();
        delete item;
    }
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
namespace
{
struct HostedRegistrationContext
{
    HostedRegistrationContext(
        UsbPnP *registry, UsbPnP::Registration *registration)
        : registry(registry), registration(registration), releaseCallback(0),
          callbackEntered(0), callbackFinished(0), invocationFinished(0),
          unregisterStarted(0), unregisterFinished(0)
    {
    }

    UsbPnP *registry;
    UsbPnP::Registration *registration;
    Semaphore releaseCallback;
    Atomic<size_t> callbackEntered;
    Atomic<size_t> callbackFinished;
    Atomic<size_t> invocationFinished;
    Atomic<size_t> unregisterStarted;
    Atomic<size_t> unregisterFinished;
};

HostedRegistrationContext *g_HostedRegistrationContext = nullptr;

UsbDevice *hostedRegistrationCallback(UsbDevice *)
{
    HostedRegistrationContext *context = g_HostedRegistrationContext;
    context->callbackEntered += 1;
    const bool released = context->releaseCallback.acquireForCompletion();
    (void) released;
    context->callbackFinished += 1;
    return nullptr;
}

int invokeHostedRegistration(void *parameter)
{
    HostedRegistrationContext *context =
        reinterpret_cast<HostedRegistrationContext *>(parameter);
    if (context->registry->invokeCallbackForTest())
    {
        context->invocationFinished += 1;
    }
    return 0;
}

int unregisterHostedRegistration(void *parameter)
{
    HostedRegistrationContext *context =
        reinterpret_cast<HostedRegistrationContext *>(parameter);
    context->unregisterStarted += 1;
    context->registration->reset();
    context->unregisterFinished += 1;
    return 0;
}

bool waitForHostedValue(Atomic<size_t> &value, size_t expected)
{
    const Time::Timestamp deadline =
        Time::getTicks() + (500 * Time::Multiplier::Millisecond);
    while (value != expected && Time::getTicks() < deadline)
    {
        Scheduler::instance().yield();
    }
    return value == expected;
}
}  // namespace

bool UsbPnP::invokeCallbackForTest()
{
    CallbackItem *item = nullptr;
    callback_t callback = nullptr;
    {
        LockGuard<Spinlock> guard(m_CallbackLock);
        if (!m_FirstCallback)
        {
            return false;
        }

        item = m_FirstCallback;
        if (!item->operations.tryEnter())
        {
            return false;
        }
        callback = item->callback;
    }

    callback(nullptr);
    item->operations.leave();
    return true;
}

size_t UsbPnP::callbackCountForTest()
{
    LockGuard<Spinlock> guard(m_CallbackLock);
    return m_CallbackCount;
}

bool UsbPnP::runHostedRegistrationRegression()
{
    UsbPnP registry;
    Registration registration;
    HostedRegistrationContext context(&registry, &registration);
    g_HostedRegistrationContext = &context;

    const bool registered = registry.registerCallbackItem(
        new CallbackItem(
            hostedRegistrationCallback, VendorIdNone, ProductIdNone,
            ClassNone, SubclassNone, ProtocolNone),
        registration, false);

    Thread *invoker = nullptr;
    Thread *unregisterer = nullptr;
    bool callbackEntered = false;
    bool registrationRemoved = false;
    bool unregisterBlocked = false;
    bool lateInvocationRejected = false;

    if (registered)
    {
        Process *process = Scheduler::instance().getKernelProcess();
        invoker = new Thread(
            process, invokeHostedRegistration, &context, nullptr, false,
            true);
        invoker->setName("hosted USB PnP callback");
        callbackEntered = waitForHostedValue(context.callbackEntered, 1);

        if (callbackEntered)
        {
            unregisterer = new Thread(
                process, unregisterHostedRegistration, &context, nullptr,
                false, true);
            unregisterer->setName("hosted USB PnP unregister");

            const bool unregisterStarted =
                waitForHostedValue(context.unregisterStarted, 1);
            const Time::Timestamp deadline =
                Time::getTicks() + (500 * Time::Multiplier::Millisecond);
            while (
                unregisterStarted && registry.callbackCountForTest() &&
                Time::getTicks() < deadline)
            {
                Scheduler::instance().yield();
            }

            registrationRemoved = registry.callbackCountForTest() == 0;
            unregisterBlocked =
                registrationRemoved && context.unregisterFinished == 0;
            lateInvocationRejected =
                registrationRemoved && !registry.invokeCallbackForTest();
        }
    }

    context.releaseCallback.release();
    const bool invokerJoined = !invoker || invoker->joinForCompletion();
    const bool unregistererJoined =
        !unregisterer || unregisterer->joinForCompletion();
    if (registration)
    {
        registration.reset();
    }
    g_HostedRegistrationContext = nullptr;

    const bool passed =
        registered && callbackEntered && registrationRemoved &&
        unregisterBlocked && lateInvocationRejected && invokerJoined &&
        unregistererJoined && context.callbackFinished == 1 &&
        context.invocationFinished == 1 &&
        context.unregisterFinished == 1;
    if (passed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS usb-pnp-registration-drain");
    }
    else
    {
        ERROR(
            "HOSTED-WAIT-TEST: FAIL usb-pnp-registration-drain: "
            "unregister did not close admission and drain the pinned "
            "callback");
    }
    return passed;
}
#endif
