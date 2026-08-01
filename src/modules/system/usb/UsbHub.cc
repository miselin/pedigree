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

#include "modules/system/usb/UsbHub.h"
#include "modules/system/usb/Usb.h"
#include "modules/system/usb/UsbDevice.h"
#include "modules/system/usb/UsbPnP.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#endif
#include "pedigree/kernel/utilities/ExtensibleBitmap.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/new"

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
namespace
{
Atomic<size_t> g_HostedSyncParamDestructions(0);
void (*g_HostedSyncTimeoutHook)() = nullptr;
}
#endif

UsbHub::UsbHub()
{
    m_UsedAddresses.set(0);
}

UsbHub::UsbHub(Device *p) : Device(p)
{
}

UsbHub::~UsbHub()
{
}

Device::Type UsbHub::getType()
{
    return UsbController;
}

bool UsbHub::deviceConnected(uint8_t nPort, UsbSpeed speed)
{
    NOTICE("USB: Adding device on port " << Dec << nPort << Hex);

    // Find the root hub
    UsbHub *pRootHub = this;
    while (pRootHub->getParent()->getType() == Device::UsbController)
        pRootHub = static_cast<UsbHub *>(pRootHub->getParent());

    size_t nRetry = 0;
    uint8_t lastAddress = 0, nAddress = 0;

    pRootHub->ignoreConnectionChanges(nPort);

    // Try twice with two different addresses
    UsbDevice *pDevice = 0;
    while (nRetry < 2)
    {
        // Get first unused address and check it
        nAddress = pRootHub->m_UsedAddresses.getFirstClear();
        if (nAddress > 127)
        {
            ERROR("USB: HUB: Out of addresses!");
            return false;
        }

        // This address is now used
        pRootHub->m_UsedAddresses.set(nAddress);
        if (lastAddress)
            pRootHub->m_UsedAddresses.clear(lastAddress);
        NOTICE(
            "USB: Allocated device on port " << Dec << nPort << Hex
                                             << " address " << nAddress);

        // Create the UsbDevice instance and set us as parent
        pDevice = new UsbDevice(this, nPort, speed);

        // Initialise the device - it basically sets the address and gets the
        // descriptors
        pDevice->initialise(nAddress);

        // Check for initialisation failures
        if (pDevice->getUsbState() != UsbDevice::Configured)
        {
            NOTICE(
                "USB: Device initialisation ended up not giving a configured "
                "device [retry "
                << nRetry << " of 2].");

            // Cleanup descriptors
            if (pDevice->getUsbState() >= UsbDevice::HasDescriptors)
                delete pDevice->getDescriptor();

            delete pDevice;

            // Reset the port that this device is attached to.
            NOTICE("USB: Performing a port reset on port " << nPort);
            if ((!pRootHub->portReset(nPort, true)) && (nRetry < 1))
            {
                // Give up completely
                NOTICE("USB: Port reset failed (port " << nPort << ")");
                pRootHub->ignoreConnectionChanges(nPort, false);
                pRootHub->m_UsedAddresses.clear(nAddress);
                return false;
            }
        }
        else
        {
            NOTICE(
                "USB: Device on port " << Dec << nPort << Hex
                                       << " accepted address " << nAddress);
            break;
        }

        lastAddress = nAddress;
        nRetry++;
    }

    pRootHub->ignoreConnectionChanges(nPort, false);

    if (nRetry == 2)
    {
        NOTICE("Device initialisation couldn't configure the device.");
        return false;
    }

    // Get the device descriptor
    UsbDevice::DeviceDescriptor *pDescriptor = pDevice->getDescriptor();

    // Iterate all interfaces
    Vector<UsbDevice::Interface *> interfaceList =
        pDevice->getConfiguration()->interfaceList;
    for (size_t i = 0; i < interfaceList.count(); i++)
    {
        UsbDevice::Interface *pInterface = interfaceList[i];

        // Skip alternate settings
        if (pInterface->nAlternateSetting)
            continue;

        // If we're not at the first interface, we have to clone the UsbDevice
        if (i)
            pDevice = new UsbDevice(pDevice);

        // Set the right interface
        pDevice->useInterface(i);

        // Add the device as a child
        UsbDeviceContainer *pContainer = new UsbDeviceContainer(pDevice);
        addChild(pContainer);
        pContainer->setParent(this);

        NOTICE(
            "USB: Device (address "
            << nAddress << "): " << pDescriptor->sVendor << " "
            << pDescriptor->sProduct << ", class " << Dec << pInterface->nClass
            << ":" << pInterface->nSubclass << ":" << pInterface->nProtocol
            << Hex);

        // Send it to the USB PnP manager
        NOTICE("pnp instance is: " << &UsbPnP::instance() << ".");
        UsbPnP::instance().probeDevice(pContainer);
    }
    return true;
}

void UsbHub::deviceDisconnected(uint8_t nPort)
{
    uint8_t nAddress = 0;
    UsbDevice::DeviceDescriptor *pDescriptor = 0;

    for (size_t i = 0; i < m_Children.count(); i++)
    {
        UsbDevice *pDevice =
            static_cast<UsbDeviceContainer *>(m_Children[i])->getUsbDevice();

        if (!pDevice)
            continue;

        if (pDevice->getPort() != nPort)
            continue;

        if (!nAddress)
            nAddress = pDevice->getAddress();
        else if (nAddress != pDevice->getAddress())
            ERROR("USB: HUB: Found devices on the same port with different "
                  "addresses");

        if (pDevice->getUsbState() >= UsbDevice::HasDescriptors)
        {
            if (!pDescriptor)
                pDescriptor = pDevice->getDescriptor();
            else if (pDescriptor != pDevice->getDescriptor())
                ERROR("USB: HUB: Found devices on the same port with different "
                      "device descriptors");
        }

        delete pDevice;
    }

    if (pDescriptor)
        delete pDescriptor;

    if (!nAddress)
        return;

    // Find the root hub
    UsbHub *pRootHub = this;
    while (pRootHub->getParent()->getType() == Device::UsbController)
        pRootHub = static_cast<UsbHub *>(pRootHub->getParent());

    // This address is now free
    pRootHub->m_UsedAddresses.clear(nAddress);
}

void UsbHub::syncCallback(uintptr_t pParam, ssize_t nResult)
{
    if (!pParam)
        return;
    SyncParam *pSyncParam = reinterpret_cast<SyncParam *>(pParam);
    pSyncParam->nResult = nResult;
    pSyncParam->semaphore.release();
    pSyncParam->releaseOwner();
}

UsbHub::SyncParam::~SyncParam()
{
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    g_HostedSyncParamDestructions += 1;
#endif
}

void UsbHub::SyncParam::releaseOwner()
{
    if ((owners -= 1) == 0)
    {
        delete this;
    }
}

ssize_t UsbHub::doSync(uintptr_t nTransaction, uint32_t timeout)
{
    // Terminal cancellation must return through this scope so the caller's
    // asynchronous-transaction reference is always released.
    TerminationDeferral transactionLifetime;

    // Create a structure to hold the semaphore and the result
    SyncParam *pSyncParam = new SyncParam();

    // Send the async request. A rejected request has no callback owner.
    const uintptr_t callbackParam = reinterpret_cast<uintptr_t>(pSyncParam);
    if (!doAsync(nTransaction, syncCallback, callbackParam))
    {
        pSyncParam->releaseOwner();
        pSyncParam->releaseOwner();
        return -TransactionError;
    }
    // The caller and callback own one reference each. A timeout relinquishes
    // only the caller's reference; controllers must still complete every
    // accepted asynchronous transaction exactly once.
    Semaphore::SemaphoreError waitError = Semaphore::NoError;
    const bool completed = pSyncParam->semaphore.acquireWithError(
        1, timeout / 1000, (timeout % 1000) * 1000, waitError);
    const bool bTimeout =
        !completed && waitError == Semaphore::TimedOut;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    if (bTimeout && g_HostedSyncTimeoutHook)
    {
        g_HostedSyncTimeoutHook();
    }
#endif

    if (!completed)
    {
        // Cancellation is also the hardware ownership barrier. It either
        // completes this callback itself or waits for a completion which won
        // the race, so no caller-owned transfer buffer can outlive this scope.
        cancelAsyncAndDrain(nTransaction, syncCallback, callbackParam);
        const bool callbackDrained =
            pSyncParam->semaphore.acquireForCompletion();
        (void) callbackDrained;
    }

    const ssize_t result =
        completed ? pSyncParam->nResult : -TransactionError;
    if (bTimeout)
    {
        WARNING("USB: a transaction timed out.");
    }
    pSyncParam->releaseOwner();
    return result;
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
namespace
{
struct HostedSyncCallback
{
    HostedSyncCallback()
        : callback(nullptr), parameter(0), start(0), finished(0),
          thread(nullptr), completionState(0), callbackCount(0),
          bufferOwned(false)
    {
    }

    void (*callback)(uintptr_t, ssize_t);
    uintptr_t parameter;
    Semaphore start;
    Semaphore finished;
    Thread *thread;
    Atomic<size_t> completionState;
    Atomic<size_t> callbackCount;
    Atomic<bool> bufferOwned;
};

int hostedSyncCallbackThread(void *parameter)
{
    HostedSyncCallback *context =
        reinterpret_cast<HostedSyncCallback *>(parameter);
    const bool started = context->start.acquireForCompletion();
    (void) started;
    if (context->completionState.compareAndSwap(1, 2))
    {
        context->bufferOwned = false;
        context->callbackCount += 1;
        context->callback(context->parameter, 0x55);
    }
    context->finished.release();
    return 0;
}

class HostedSyncHub : public UsbHub
{
  public:
    explicit HostedSyncHub(bool accept = true)
        : UsbHub(), m_Callback(), m_Accept(accept)
    {
    }

    ~HostedSyncHub() override
    {
        if (m_Callback.thread)
        {
            m_Callback.start.release();
            m_Callback.thread->joinForCompletion();
            m_Callback.thread = nullptr;
        }
    }

    void addTransferToTransaction(
        uintptr_t, bool, UsbPid, uintptr_t, size_t) override
    {
    }

    uintptr_t createTransaction(UsbEndpoint) override
    {
        return 1;
    }

    bool doAsync(
        uintptr_t, void (*callback)(uintptr_t, ssize_t),
        uintptr_t parameter) override
    {
        if (!m_Accept)
            return false;

        m_Callback.callback = callback;
        m_Callback.parameter = parameter;
        m_Callback.completionState = 1;
        m_Callback.bufferOwned = true;
        m_Callback.thread = new Thread(
            Scheduler::instance().getKernelProcess(),
            hostedSyncCallbackThread, &m_Callback, nullptr, false, true);
        m_Callback.thread->setName("hosted USB timeout callback");
        return true;
    }

    void cancelAsyncAndDrain(
        uintptr_t, void (*callback)(uintptr_t, ssize_t),
        uintptr_t parameter) override
    {
        if (m_Callback.callback == callback &&
            m_Callback.parameter == parameter &&
            m_Callback.completionState.compareAndSwap(1, 2))
        {
            // This transition is the fake controller's deterministic DMA
            // release boundary.
            m_Callback.bufferOwned = false;
            m_Callback.callbackCount += 1;
            callback(parameter, -TransactionError);
        }

        if (m_Callback.thread)
        {
            m_Callback.start.release();
            const bool finished =
                m_Callback.finished.acquireForCompletion();
            (void) finished;
            m_Callback.thread->joinForCompletion();
            m_Callback.thread = nullptr;
        }
    }

    void addInterruptInHandler(
        UsbEndpoint, uintptr_t, uint16_t,
        void (*)(uintptr_t, ssize_t), uintptr_t) override
    {
    }

    bool portReset(uint8_t, bool) override
    {
        return true;
    }

    void completeInsideTimeoutHandoff()
    {
        m_Callback.start.release();
        const bool finished = m_Callback.finished.acquireForCompletion();
        (void) finished;
        m_Callback.thread->joinForCompletion();
        m_Callback.thread = nullptr;
    }

    bool completedExactlyOnce() const
    {
        return static_cast<size_t>(m_Callback.callbackCount) == 1 &&
               !static_cast<bool>(m_Callback.bufferOwned);
    }

  private:
    HostedSyncCallback m_Callback;
    bool m_Accept;
};

HostedSyncHub *g_pHostedSyncHub = nullptr;

void completeHostedSyncTimeout()
{
    g_pHostedSyncHub->completeInsideTimeoutHandoff();
}
}  // namespace

bool UsbHub::runHostedSyncOwnershipRegression()
{
    const size_t destructionsBefore = g_HostedSyncParamDestructions;
    bool rejectionPassed = false;
    {
        HostedSyncHub hub(false);
        rejectionPassed = hub.doSync(1, 1) == -TransactionError;
    }
    if (rejectionPassed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS usb-sync-rejected-no-callback");
    }
    else
    {
        ERROR(
            "HOSTED-WAIT-TEST: FAIL usb-sync-rejected-no-callback: rejected "
            "submission retained a callback obligation");
    }

    bool cancelPassed = false;
    {
        HostedSyncHub hub;
        const ssize_t result = hub.doSync(1, 1);
        cancelPassed =
            result == -TransactionError && hub.completedExactlyOnce();
    }
    if (cancelPassed)
    {
        NOTICE("HOSTED-WAIT-TEST: PASS usb-sync-timeout-cancel-drain");
    }
    else
    {
        ERROR(
            "HOSTED-WAIT-TEST: FAIL usb-sync-timeout-cancel-drain: doSync "
            "returned before cancellation released the fake DMA buffer");
    }

    bool handoffPassed = false;
    {
        HostedSyncHub hub;
        g_pHostedSyncHub = &hub;
        g_HostedSyncTimeoutHook = completeHostedSyncTimeout;
        const ssize_t result = hub.doSync(1, 1);
        g_HostedSyncTimeoutHook = nullptr;
        g_pHostedSyncHub = nullptr;
        handoffPassed =
            result == -TransactionError && hub.completedExactlyOnce();
    }
    if (handoffPassed)
    {
        NOTICE(
            "HOSTED-WAIT-TEST: PASS usb-sync-timeout-completion-handoff");
    }
    else
    {
        ERROR(
            "HOSTED-WAIT-TEST: FAIL usb-sync-timeout-completion-handoff: "
            "completion/cancellation race delivered more than once");
    }

    return rejectionPassed && cancelPassed && handoffPassed &&
           g_HostedSyncParamDestructions == (destructionsBefore + 3);
}

EXPORTED_PUBLIC bool runHostedUsbSyncOwnershipRegression()
{
    return UsbHub::runHostedSyncOwnershipRegression();
}
#endif
