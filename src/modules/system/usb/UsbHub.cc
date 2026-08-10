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
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/panic.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/TerminationDeferral.h"

#include "modules/system/usb/Usb.h"
#include "modules/system/usb/UsbDevice.h"
#include "modules/system/usb/UsbPnP.h"
#if (HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS) || PEDIGREE_CONCURRENCY_SMOKE_TESTS
#include "pedigree/kernel/process/OperationBarrier.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/time/Time.h"
#endif
#include "pedigree/kernel/utilities/ExtensibleBitmap.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/new"

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
namespace {
Atomic<size_t> g_HostedSyncParamDestructions(0);
void (*g_HostedSyncTimeoutHook)() = nullptr;
UsbHub::ConnectionChangePendingHook g_ConnectionChangePendingHook = nullptr;
UsbHub::ConnectionChangeReplayWaitHook g_ConnectionChangeReplayWaitHook = nullptr;
}  // namespace
#endif

UsbInterruptInHandle::UsbInterruptInHandle()
    : m_Owner(nullptr),
      m_Resetting(false),
      m_Token{static_cast<uintptr_t>(-1), 0},
      m_Callback(nullptr),
      m_Parameter(0),
      m_CancellationStarted(false) {}

UsbInterruptInHandle::UsbInterruptInHandle(UsbInterruptInHandle&& other)
    : m_Owner(static_cast<UsbHub*>(other.m_Owner)),
      m_Resetting(false),
      m_Token(other.m_Token),
      m_Callback(other.m_Callback),
      m_Parameter(other.m_Parameter),
      m_CancellationStarted(other.m_CancellationStarted) {
  assert(!other.m_Resetting);
  other.m_Owner = nullptr;
  other.m_Token = {static_cast<uintptr_t>(-1), 0};
  other.m_Callback = nullptr;
  other.m_Parameter = 0;
  other.m_CancellationStarted = false;
}

UsbInterruptInHandle::~UsbInterruptInHandle() {
  if (!reset())
    panic("USB interrupt-IN handle destroyed from its running callback");
}

UsbInterruptInHandle& UsbInterruptInHandle::operator=(UsbInterruptInHandle&& other) {
  if (this != &other) {
    if (!reset())
      panic("USB interrupt-IN handle move attempted from its running callback");
    assert(!other.m_Resetting);
    m_Owner = static_cast<UsbHub*>(other.m_Owner);
    m_Token = other.m_Token;
    m_Callback = other.m_Callback;
    m_Parameter = other.m_Parameter;
    m_CancellationStarted = other.m_CancellationStarted;
    other.m_Owner = nullptr;
    other.m_Token = {static_cast<uintptr_t>(-1), 0};
    other.m_Callback = nullptr;
    other.m_Parameter = 0;
    other.m_CancellationStarted = false;
  }
  return *this;
}

bool UsbInterruptInHandle::reset() {
  while (!m_Resetting.compareAndSwap(false, true)) {
    UsbHub* owner = m_Owner;
    if (owner && owner->inInterruptCallbackContext())
      return false;
    Scheduler::instance().yield();
  }

  UsbHub* owner = m_Owner;
  if (!owner) {
    m_Resetting = false;
    return true;
  }

  if (!owner->cancelInterruptInAndDrain(m_Token, m_Callback, m_Parameter, m_CancellationStarted)) {
    m_CancellationStarted = true;
    m_Resetting = false;
    return false;
  }

  m_Owner = nullptr;
  m_Token = {static_cast<uintptr_t>(-1), 0};
  m_Callback = nullptr;
  m_Parameter = 0;
  m_CancellationStarted = false;
  m_Resetting = false;
  return true;
}

void UsbInterruptInHandle::adopt(UsbHub* owner, const UsbInterruptInToken& token,
                                 void (*callback)(uintptr_t, ssize_t), uintptr_t parameter) {
  assert(!static_cast<UsbHub*>(m_Owner));
  assert(!m_Resetting);
  m_Token = token;
  m_Callback = callback;
  m_Parameter = parameter;
  m_CancellationStarted = false;
  // m_Owner is the publication sentinel observed by reset and destruction.
  m_Owner = owner;
}

UsbHub::ConnectionChangeSuppression::ConnectionChangeSuppression() : m_Hub(nullptr), m_Port(0) {}

UsbHub::ConnectionChangeSuppression::ConnectionChangeSuppression(UsbHub* hub, size_t port)
    : m_Hub(hub), m_Port(port) {}

UsbHub::ConnectionChangeSuppression::ConnectionChangeSuppression(
    ConnectionChangeSuppression&& other)
    : m_Hub(other.m_Hub), m_Port(other.m_Port) {
  other.m_Hub = nullptr;
  other.m_Port = 0;
}

UsbHub::ConnectionChangeSuppression::~ConnectionChangeSuppression() {
  reset();
}

UsbHub::ConnectionChangeSuppression& UsbHub::ConnectionChangeSuppression::operator=(
    ConnectionChangeSuppression&& other) {
  if (this != &other) {
    reset();
    m_Hub = other.m_Hub;
    m_Port = other.m_Port;
    other.m_Hub = nullptr;
    other.m_Port = 0;
  }
  return *this;
}

void UsbHub::ConnectionChangeSuppression::reset() {
  if (m_Hub) {
    UsbHub* hub = m_Hub;
    const size_t port = m_Port;
    m_Hub = nullptr;
    m_Port = 0;
    hub->releaseConnectionChangeSuppression(port);
  }
}

UsbHub::UsbHub()
    : m_RootHub(nullptr), m_IsRootHub(false), m_RootPort(0xff), m_RootPortGeneration(0) {
  m_UsedAddresses.set(0);
  m_AddressReferences[0] = 1;
}

UsbHub::UsbHub(Device* p)
    : Device(p), m_RootHub(this), m_IsRootHub(true), m_RootPort(0xff), m_RootPortGeneration(0) {
  m_UsedAddresses.set(0);
  m_AddressReferences[0] = 1;
}

UsbHub::~UsbHub() {}

UsbHub::RootConnection UsbHub::rootConnectionForChild(uint8_t childPort) const {
  if (m_IsRootHub)
    return {childPort, currentRootPortGeneration(childPort)};
  return {m_RootPort, m_RootPortGeneration};
}

bool UsbHub::publishInterruptInHandle(UsbInterruptInHandle& handle,
                                      const UsbInterruptInToken& token,
                                      void (*callback)(uintptr_t, ssize_t), uintptr_t parameter) {
  if (handle || !callback || token.transaction == static_cast<uintptr_t>(-1) || !token.generation)
    return false;

  handle.adopt(this, token, callback, parameter);
  return true;
}

void UsbHub::disconnectAllDevices() {
  for (size_t i = 0; i < m_Children.count();) {
    Device* child = m_Children[i];
    if (!child || child->getType() != Device::UsbContainer) {
      ++i;
      continue;
    }

    auto* container = static_cast<UsbDeviceContainer*>(child);
    UsbDevice* device = container->getUsbDevice();
    if (device) {
      deviceDisconnected(device->getPort());
    } else {
      {
        Device::TreeLockGuard treeGuard;
        container->closeProbeAdmission();
        removeChild(i);
        child->setParent(nullptr);
      }
      container->waitForProbes();
      delete container;
    }
  }
}

void UsbHub::retainDisconnectedAddressesUntilControllerTeardown() {
  m_RetainDisconnectedAddresses = true;
}

Device::Type UsbHub::getType() {
  return UsbController;
}

void UsbHub::attachToUpstreamHub(UsbHub* upstream, const RootConnection& connection) {
  if (m_IsRootHub || !upstream || !upstream->m_RootHub || connection.port == 0xff) {
    ERROR("USB: downstream hub has no root-controller association");
    m_RootHub = nullptr;
    m_RootPort = 0xff;
    m_RootPortGeneration = 0;
    return;
  }

  m_RootHub = upstream->m_RootHub;
  m_RootPort = connection.port;
  m_RootPortGeneration = connection.generation;
}

bool UsbHub::suppressConnectionChanges(size_t port, ConnectionChangeSuppression& suppression) {
  if (port >= ConnectionChangePortCount) {
    suppression = ConnectionChangeSuppression();
    ERROR(
        "USB: root-port connection-change suppression was requested for "
        "invalid port "
        << Dec << port << Hex);
    return false;
  }

  Atomic<size_t>& state = m_ConnectionChangeStates[port];
  while (true) {
    const size_t observed = state;
    const size_t count = observed & ConnectionChangeCountMask;
    if (!count && (observed & ConnectionChangePending)) {
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
      if (g_ConnectionChangeReplayWaitHook) {
        g_ConnectionChangeReplayWaitHook(this, port);
      }
#endif
      Scheduler::instance().yield();
      continue;
    }
    if (count == ConnectionChangeCountMask) {
      suppression = ConnectionChangeSuppression();
      ERROR(
          "USB: root-port connection-change suppression count "
          "overflowed on port "
          << Dec << port << Hex);
      return false;
    }

    if (state.compareAndSwap(observed, observed + 1)) {
      suppression = ConnectionChangeSuppression(this, port);
      return true;
    }
  }
}

bool UsbHub::deferConnectionChangeIfSuppressed(size_t port) {
  if (port >= ConnectionChangePortCount) {
    return false;
  }

  Atomic<size_t>& state = m_ConnectionChangeStates[port];
  while (true) {
    const size_t observed = state;
    if (!(observed & ConnectionChangeCountMask)) {
      return false;
    }

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
    if (g_ConnectionChangePendingHook) {
      g_ConnectionChangePendingHook(this, port, observed);
    }
#endif

    // Even an already-pending observation performs the CAS. A final
    // release which wins this boundary changes the state to zero, making
    // us retry and process the new hardware event normally instead of
    // acknowledging it behind an already-issued replay.
    if (state.compareAndSwap(observed, observed | ConnectionChangePending)) {
      return true;
    }
  }
}

void UsbHub::releaseConnectionChangeSuppression(size_t port) {
  if (port >= ConnectionChangePortCount) {
    ERROR("USB: invalid root-port connection-change suppression release");
    return;
  }

  Atomic<size_t>& state = m_ConnectionChangeStates[port];
  bool replay = false;
  while (true) {
    const size_t observed = state;
    const size_t count = observed & ConnectionChangeCountMask;
    if (!count) {
      ERROR(
          "USB: unbalanced root-port connection-change suppression "
          "release on port "
          << Dec << port << Hex);
      return;
    }

    const bool finalRelease = count == 1;
    // Pending with a zero count is a transient replay-in-progress state.
    // A new enumeration scope cannot overtake publication of the change
    // retained by the scope which is ending here.
    const size_t replacement =
        finalRelease ? ((observed & ConnectionChangePending) ? ConnectionChangePending : 0)
                     : observed - 1;
    if (state.compareAndSwap(observed, replacement)) {
      replay = finalRelease && (observed & ConnectionChangePending);
      break;
    }
  }

  if (replay) {
    replaySuppressedConnectionChange(port);
    const bool published = state.compareAndSwap(ConnectionChangePending, 0);
    assert(published);
    (void)published;
  }
}

void UsbHub::replaySuppressedConnectionChange(size_t port) {
  ERROR(
      "USB: root-port change replay has no controller implementation for "
      "port "
      << Dec << port << Hex);
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
void UsbHub::setConnectionChangePendingHookForTest(ConnectionChangePendingHook hook) {
  g_ConnectionChangePendingHook = hook;
}

void UsbHub::setConnectionChangeReplayWaitHookForTest(ConnectionChangeReplayWaitHook hook) {
  g_ConnectionChangeReplayWaitHook = hook;
}
#endif

bool UsbHub::deviceConnected(uint8_t nPort, UsbSpeed speed) {
  NOTICE("USB: Adding device on port " << Dec << nPort << Hex);

  UsbHub* pRootHub = m_RootHub;
  if (!pRootHub) {
    ERROR("USB: cannot enumerate a hub without a root controller");
    return false;
  }

  // Only an HCD invocation names a root port. A downstream hub's local port
  // number must never suppress an unrelated port on the root controller.
  ConnectionChangeSuppression changeSuppression;
  if (m_IsRootHub && !suppressConnectionChanges(nPort, changeSuppression)) {
    return false;
  }

  struct PendingProbe {
    explicit PendingProbe(UsbDeviceContainer* container) : container(container), probe() {}

    UsbDeviceContainer* container;
    OperationBarrier::Lease probe;
  };

  List<PendingProbe*> pendingProbes;
  {
    LockGuard<Mutex> topologyGuard(m_TopologyLock);

    // A repeated connected observation replaces the prior port generation.
    // The per-hub topology lock keeps another observation from publishing a
    // successor between retirement and generic-container publication.
    bool retiredAddresses[128] = {};
    bool pinnedSubtreeAddresses[128] = {};
    retirePortContainersLocked(nPort, retiredAddresses, pinnedSubtreeAddresses);

    {
      // Every device shares address zero until SET_ADDRESS completes. Keeping
      // this root-owned lock through generic-container publication also
      // prevents an address from being reused before its tree owner is visible.
      LockGuard<Mutex> enumerationGuard(pRootHub->m_EnumerationLock);
      releaseRetiredAddressesLocked(nPort, retiredAddresses);
      if (!portReset(nPort)) {
        // Subtree pins keep every old address reserved because a descendant may
        // still answer. Controller teardown is the safe release point.
        NOTICE("USB: Port reset failed before enumeration (port " << nPort << ")");
        return false;
      }
      pRootHub->releaseAddressesLocked(pinnedSubtreeAddresses);

      size_t nRetry = 0;
      uint8_t nAddress = 0;
      UsbDevice* pDevice = nullptr;

      // Try twice, releasing every failed attempt before the next allocation.
      while (nRetry < 2) {
        nAddress = pRootHub->m_UsedAddresses.getFirstClear();
        if (nAddress > 127) {
          ERROR("USB: HUB: Out of addresses!");
          return false;
        }

        pRootHub->m_UsedAddresses.set(nAddress);
        assert(!pRootHub->m_AddressReferences[nAddress]);
        pRootHub->m_AddressReferences[nAddress] = 1;
        NOTICE("USB: Allocated device on port " << Dec << nPort << Hex << " address " << nAddress);

        pDevice = new UsbDevice(this, nPort, speed);
        pDevice->initialise(nAddress);

        if (pDevice->getUsbState() != UsbDevice::Configured) {
          NOTICE(
              "USB: Device initialisation ended up not giving a configured "
              "device [retry "
              << nRetry << " of 2].");

          delete pDevice;
          pDevice = nullptr;

          NOTICE("USB: Performing a port reset on port " << nPort);
          if (!portReset(nPort, true)) {
            // SET_ADDRESS can take effect even when its completion was lost. If
            // reset cannot prove the device returned to address zero, retaining
            // this reservation is safer than assigning it to another device.
            NOTICE("USB: Port reset failed (port " << nPort << ")");
            return false;
          }
          pRootHub->releaseAddressLocked(nAddress);
        } else {
          NOTICE("USB: Device on port " << Dec << nPort << Hex << " accepted address " << nAddress);
          break;
        }

        nRetry++;
      }

      if (nRetry == 2) {
        NOTICE("Device initialisation couldn't configure the device.");
        return false;
      }

      UsbDevice::DeviceDescriptor* pDescriptor = pDevice->getDescriptor();
      Vector<UsbDevice::Interface*> interfaceList = pDevice->getConfiguration()->interfaceList;
      for (size_t i = 0; i < interfaceList.count(); i++) {
        UsbDevice::Interface* pInterface = interfaceList[i];
        if (pInterface->nAlternateSetting)
          continue;

        UsbDevice* pInterfaceDevice = new UsbDevice(pDevice);
        pInterfaceDevice->useInterface(i);

        UsbDeviceContainer* pContainer = new UsbDeviceContainer(pInterfaceDevice);
        auto* pending = new PendingProbe(pContainer);
        if (!pContainer->tryAcquireProbe(pending->probe))
          panic("USB: new device container rejected its initial probe");
        {
          Device::TreeLockGuard treeGuard;
          pContainer->setParent(this);
          addChild(pContainer);
        }
        pendingProbes.pushBack(pending);

        NOTICE("USB: Device (address " << nAddress << "): " << pDescriptor->sVendor << " "
                                       << pDescriptor->sProduct << ", class " << Dec
                                       << pInterface->nClass << ":" << pInterface->nSubclass << ":"
                                       << pInterface->nProtocol << Hex);
      }

      if (!pendingProbes.count()) {
        delete pDevice;
        pDevice = nullptr;
        WARNING("USB: configured device exposed no default interface");
        if (portReset(nPort, true))
          pRootHub->releaseAddressLocked(nAddress);
        return false;
      }
      delete pDevice;
    }
  }

  changeSuppression.reset();

  while (pendingProbes.count()) {
    PendingProbe* pending = pendingProbes.popFront();
    UsbPnP::instance().probeDeviceAdmitted(pending->container, pending->probe);
    delete pending;
  }
  return true;
}

void UsbHub::deviceDisconnected(uint8_t nPort) {
  LockGuard<Mutex> topologyGuard(m_TopologyLock);
  deviceDisconnectedLocked(nPort);
}

void UsbHub::deviceDisconnectedLocked(uint8_t nPort) {
  bool retiredAddresses[128] = {};
  bool pinnedSubtreeAddresses[128] = {};
  retirePortContainersLocked(nPort, retiredAddresses,
                             m_RetainDisconnectedAddresses ? pinnedSubtreeAddresses : nullptr);

  UsbHub* addressOwner = m_RootHub ? m_RootHub : this;
  LockGuard<Mutex> enumerationGuard(addressOwner->m_EnumerationLock);
  releaseRetiredAddressesLocked(nPort, retiredAddresses);
  // Driver-only hub unbind leaves physical downstream ports configured. The
  // subtree pins therefore remain until the root HCD itself is destroyed.
}

void UsbHub::retirePortContainersLocked(uint8_t nPort, bool* retiredAddresses,
                                        bool* pinnedSubtreeAddresses) {
  List<UsbDeviceContainer*> retiredContainers;
  {
    Device::TreeLockGuard treeGuard;
    for (size_t i = 0; i < m_Children.count();) {
      Device* child = m_Children[i];
      if (!child || child->getType() != Device::UsbContainer) {
        ++i;
        continue;
      }

      auto* container = static_cast<UsbDeviceContainer*>(child);
      UsbDevice* pDevice = container->getUsbDevice();
      if (!pDevice || pDevice->getPort() != nPort) {
        ++i;
        continue;
      }

      const uint8_t address = pDevice->getAddress();
      if (address && address < 128)
        retiredAddresses[address] = true;

      container->closeProbeAdmission();
      removeChild(i);
      child->setParent(nullptr);
      retiredContainers.pushBack(container);
    }
  }

  for (List<UsbDeviceContainer*>::Iterator it = retiredContainers.begin();
       it != retiredContainers.end(); ++it) {
    drainSubtreeProbeAdmissions(*it);
  }

  if (pinnedSubtreeAddresses) {
    UsbHub* addressOwner = m_RootHub ? m_RootHub : this;
    LockGuard<Mutex> enumerationGuard(addressOwner->m_EnumerationLock);
    Device::TreeLockGuard treeGuard;
    for (List<UsbDeviceContainer*>::Iterator it = retiredContainers.begin();
         it != retiredContainers.end(); ++it) {
      collectUsbAddresses(*it, pinnedSubtreeAddresses);
    }
    addressOwner->retainAddressesLocked(pinnedSubtreeAddresses);
  }

  while (retiredContainers.count()) {
    UsbDeviceContainer* container = retiredContainers.popFront();
    delete container;
  }
}

void UsbHub::collectUsbAddresses(Device* device, bool* addresses) {
  if (!device || !addresses)
    return;

  if (device->getType() == Device::UsbContainer) {
    auto* container = static_cast<UsbDeviceContainer*>(device);
    UsbDevice* usbDevice = container->getUsbDevice();
    if (usbDevice) {
      const uint8_t address = usbDevice->getAddress();
      if (address && address < 128)
        addresses[address] = true;
    }
  }

  for (size_t i = 0; i < device->getNumChildren(); ++i)
    collectUsbAddresses(device->getChild(i), addresses);
}

void UsbHub::collectImmediateUsbContainers(Device* device, List<UsbDeviceContainer*>& containers) {
  if (!device)
    return;

  for (size_t i = 0; i < device->getNumChildren(); ++i) {
    Device* child = device->getChild(i);
    if (child->getType() == Device::UsbContainer) {
      containers.pushBack(static_cast<UsbDeviceContainer*>(child));
    } else {
      collectImmediateUsbContainers(child, containers);
    }
  }
}

void UsbHub::drainSubtreeProbeAdmissions(UsbDeviceContainer* container) {
  if (!container)
    return;

  // A container's probe can replace its complete device subtree. Drain the
  // parent before retaining child pointers so replacement cannot free a
  // descendant while teardown is waiting on its admission barrier.
  container->closeProbeAdmission();
  container->waitForProbes();

  List<UsbDeviceContainer*> children;
  {
    Device::TreeLockGuard treeGuard;
    collectImmediateUsbContainers(container, children);
  }
  for (List<UsbDeviceContainer*>::Iterator it = children.begin(); it != children.end(); ++it)
    drainSubtreeProbeAdmissions(*it);
}

void UsbHub::retainAddressLocked(size_t address) {
  assert(address && address < 128);
  assert(m_UsedAddresses.test(address));
  assert(m_AddressReferences[address]);
  ++m_AddressReferences[address];
}

void UsbHub::releaseAddressLocked(size_t address) {
  assert(address && address < 128);
  assert(m_UsedAddresses.test(address));
  assert(m_AddressReferences[address]);
  if (!--m_AddressReferences[address])
    m_UsedAddresses.clear(address);
}

void UsbHub::retainAddressesLocked(const bool* addresses) {
  for (size_t address = 1; address < 128; ++address) {
    if (addresses[address])
      retainAddressLocked(address);
  }
}

void UsbHub::releaseAddressesLocked(const bool* addresses) {
  for (size_t address = 1; address < 128; ++address) {
    if (addresses[address])
      releaseAddressLocked(address);
  }
}

void UsbHub::releaseRetiredAddressesLocked(uint8_t nPort, const bool* retiredAddresses) {
  bool retiredAny = false;
  for (size_t address = 1; address < 128; ++address) {
    if (!retiredAddresses[address])
      continue;
    retiredAny = true;
    if (m_RootHub) {
      m_RootHub->releaseAddressLocked(address);
      NOTICE("USB: Retired device on port " << Dec << nPort << " address " << address << Hex);
    }
  }
  if (retiredAny && !m_RootHub)
    ERROR("USB: cannot release an address without a root controller");
}

void UsbHub::syncCallback(uintptr_t pParam, ssize_t nResult) {
  if (!pParam)
    return;
  SyncParam* pSyncParam = reinterpret_cast<SyncParam*>(pParam);
  pSyncParam->nResult = nResult;
  pSyncParam->semaphore.release();
  pSyncParam->releaseOwner();
}

UsbHub::SyncParam::~SyncParam() {
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  g_HostedSyncParamDestructions += 1;
#endif
}

void UsbHub::SyncParam::releaseOwner() {
  if ((owners -= 1) == 0) {
    delete this;
  }
}

ssize_t UsbHub::doSync(uintptr_t nTransaction, uint32_t timeout) {
  // Terminal cancellation must return through this scope so the caller's
  // asynchronous-transaction reference is always released.
  TerminationDeferral transactionLifetime;

  // Create a structure to hold the semaphore and the result
  SyncParam* pSyncParam = new SyncParam();

  // Send the async request. A rejected request has no callback owner.
  const uintptr_t callbackParam = reinterpret_cast<uintptr_t>(pSyncParam);
  if (!doAsync(nTransaction, syncCallback, callbackParam)) {
    pSyncParam->releaseOwner();
    pSyncParam->releaseOwner();
    return -TransactionError;
  }
  // The caller and callback own one reference each. A timeout relinquishes
  // only the caller's reference; controllers must still complete every
  // accepted asynchronous transaction exactly once.
  Semaphore::SemaphoreError waitError = Semaphore::NoError;
  const bool completed =
      pSyncParam->semaphore.acquireWithError(1, timeout / 1000, (timeout % 1000) * 1000, waitError);
  const bool bTimeout = !completed && waitError == Semaphore::TimedOut;

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  if (bTimeout && g_HostedSyncTimeoutHook) {
    g_HostedSyncTimeoutHook();
  }
#endif

  if (!completed) {
    // Cancellation is also the hardware ownership barrier. It either
    // completes this callback itself or waits for a completion which won
    // the race, so no caller-owned transfer buffer can outlive this scope.
    cancelAsyncAndDrain(nTransaction, syncCallback, callbackParam);
    const bool callbackDrained = pSyncParam->semaphore.acquireForCompletion();
    (void)callbackDrained;
  }

  const ssize_t result = completed ? pSyncParam->nResult : -TransactionError;
  if (bTimeout) {
    WARNING("USB: a transaction timed out.");
  }
  pSyncParam->releaseOwner();
  return result;
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
namespace {
struct HostedSyncCallback {
  HostedSyncCallback()
      : callback(nullptr),
        parameter(0),
        start(0),
        finished(0),
        thread(nullptr),
        completionState(0),
        callbackCount(0),
        bufferOwned(false) {}

  void (*callback)(uintptr_t, ssize_t);
  uintptr_t parameter;
  Semaphore start;
  Semaphore finished;
  Thread* thread;
  Atomic<size_t> completionState;
  Atomic<size_t> callbackCount;
  Atomic<bool> bufferOwned;
};

int hostedSyncCallbackThread(void* parameter) {
  HostedSyncCallback* context = reinterpret_cast<HostedSyncCallback*>(parameter);
  const bool started = context->start.acquireForCompletion();
  (void)started;
  if (context->completionState.compareAndSwap(1, 2)) {
    context->bufferOwned = false;
    context->callbackCount += 1;
    context->callback(context->parameter, 0x55);
  }
  context->finished.release();
  return 0;
}

class HostedSyncHub : public UsbHub {
 public:
  explicit HostedSyncHub(bool accept = true) : UsbHub(), m_Callback(), m_Accept(accept) {}

  ~HostedSyncHub() override {
    if (m_Callback.thread) {
      m_Callback.start.release();
      m_Callback.thread->joinForCompletion();
      m_Callback.thread = nullptr;
    }
  }

  void addTransferToTransaction(uintptr_t, bool, UsbPid, uintptr_t, size_t) override {}

  uintptr_t createTransaction(UsbEndpoint) override {
    return 1;
  }

  bool doAsync(uintptr_t, void (*callback)(uintptr_t, ssize_t), uintptr_t parameter) override {
    if (!m_Accept)
      return false;

    m_Callback.callback = callback;
    m_Callback.parameter = parameter;
    m_Callback.completionState = 1;
    m_Callback.bufferOwned = true;
    m_Callback.thread = new Thread(Scheduler::instance().getKernelProcess(),
                                   hostedSyncCallbackThread, &m_Callback, nullptr, false, true);
    m_Callback.thread->setName("hosted USB timeout callback");
    return true;
  }

  void cancelAsyncAndDrain(uintptr_t, void (*callback)(uintptr_t, ssize_t),
                           uintptr_t parameter) override {
    if (m_Callback.callback == callback && m_Callback.parameter == parameter &&
        m_Callback.completionState.compareAndSwap(1, 2)) {
      // This transition is the fake controller's deterministic DMA
      // release boundary.
      m_Callback.bufferOwned = false;
      m_Callback.callbackCount += 1;
      callback(parameter, -TransactionError);
    }

    if (m_Callback.thread) {
      m_Callback.start.release();
      const bool finished = m_Callback.finished.acquireForCompletion();
      (void)finished;
      m_Callback.thread->joinForCompletion();
      m_Callback.thread = nullptr;
    }
  }

  bool addInterruptInHandler(UsbEndpoint, uintptr_t, uint16_t, void (*)(uintptr_t, ssize_t),
                             UsbInterruptInHandle&, uintptr_t) override {
    return false;
  }

  bool portReset(uint8_t, bool) override {
    return true;
  }

  void completeInsideTimeoutHandoff() {
    m_Callback.start.release();
    const bool finished = m_Callback.finished.acquireForCompletion();
    (void)finished;
    m_Callback.thread->joinForCompletion();
    m_Callback.thread = nullptr;
  }

  bool completedExactlyOnce() const {
    return static_cast<size_t>(m_Callback.callbackCount) == 1 &&
           !static_cast<bool>(m_Callback.bufferOwned);
  }

 private:
  bool cancelInterruptInAndDrain(const UsbInterruptInToken&, void (*)(uintptr_t, ssize_t),
                                 uintptr_t, bool) override {
    panic("hosted synchronous USB hub unexpectedly owned an interrupt subscription");
    return false;
  }

  HostedSyncCallback m_Callback;
  bool m_Accept;
};

HostedSyncHub* g_pHostedSyncHub = nullptr;

void completeHostedSyncTimeout() {
  g_pHostedSyncHub->completeInsideTimeoutHandoff();
}
}  // namespace

bool UsbHub::runHostedSyncOwnershipRegression() {
  const size_t destructionsBefore = g_HostedSyncParamDestructions;
  bool rejectionPassed = false;
  {
    HostedSyncHub hub(false);
    rejectionPassed = hub.doSync(1, 1) == -TransactionError;
  }
  if (rejectionPassed) {
    NOTICE("HOSTED-WAIT-TEST: PASS usb-sync-rejected-no-callback");
  } else {
    ERROR(
        "HOSTED-WAIT-TEST: FAIL usb-sync-rejected-no-callback: rejected "
        "submission retained a callback obligation");
  }

  bool cancelPassed = false;
  {
    HostedSyncHub hub;
    const ssize_t result = hub.doSync(1, 1);
    cancelPassed = result == -TransactionError && hub.completedExactlyOnce();
  }
  if (cancelPassed) {
    NOTICE("HOSTED-WAIT-TEST: PASS usb-sync-timeout-cancel-drain");
  } else {
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
    handoffPassed = result == -TransactionError && hub.completedExactlyOnce();
  }
  if (handoffPassed) {
    NOTICE("HOSTED-WAIT-TEST: PASS usb-sync-timeout-completion-handoff");
  } else {
    ERROR(
        "HOSTED-WAIT-TEST: FAIL usb-sync-timeout-completion-handoff: "
        "completion/cancellation race delivered more than once");
  }

  return rejectionPassed && cancelPassed && handoffPassed &&
         g_HostedSyncParamDestructions == (destructionsBefore + 3);
}

EXPORTED_PUBLIC bool runHostedUsbSyncOwnershipRegression() {
  return UsbHub::runHostedSyncOwnershipRegression();
}
#endif

#if (HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS) || PEDIGREE_CONCURRENCY_SMOKE_TESTS
namespace {
constexpr bool PinInterruptTestThreads = HOSTED;

void* interruptRegressionRunner() {
  ProcessorInformation& information = Processor::information();
  Thread* thread = information.getCurrentThread();
  return thread ? static_cast<void*>(thread) : static_cast<void*>(&information);
}

class InterruptOwnershipHub : public UsbHub {
 public:
  InterruptOwnershipHub()
      : UsbHub(),
        m_Callbacks(),
        m_Callback(nullptr),
        m_Parameter(0),
        m_CallbackRunner(nullptr),
        m_CancelCalls(0) {}

  ~InterruptOwnershipHub() override {
    if (m_Callbacks.isOpen())
      m_Callbacks.closeAndWait();
  }

  void addTransferToTransaction(uintptr_t, bool, UsbPid, uintptr_t, size_t) override {}

  uintptr_t createTransaction(UsbEndpoint) override {
    return static_cast<uintptr_t>(-1);
  }

  bool doAsync(uintptr_t, void (*)(uintptr_t, ssize_t), uintptr_t) override {
    return false;
  }

  void cancelAsyncAndDrain(uintptr_t, void (*)(uintptr_t, ssize_t), uintptr_t) override {
    panic("interrupt ownership regression unexpectedly cancelled an async transfer");
  }

  bool addInterruptInHandler(UsbEndpoint, uintptr_t, uint16_t, void (*callback)(uintptr_t, ssize_t),
                             UsbInterruptInHandle& handle, uintptr_t parameter) override {
    if (m_Callback || !callback || handle || !m_Callbacks.isOpen())
      return false;

    const UsbInterruptInToken token = {0x494e5452, 1};
    if (!publishInterruptInHandle(handle, token, callback, parameter))
      return false;

    m_Callback = callback;
    m_Parameter = parameter;
    return true;
  }

  bool portReset(uint8_t, bool) override {
    return true;
  }

  bool fire() {
    OperationBarrier::Lease callbackLease;
    if (!m_Callback || !m_Callbacks.tryAcquire(callbackLease))
      return false;

    m_CallbackRunner = interruptRegressionRunner();
    m_Callback(m_Parameter, 1);
    m_CallbackRunner = nullptr;
    return true;
  }

  size_t cancelCalls() const {
    return m_CancelCalls;
  }

 private:
  bool inInterruptCallbackContext() const override {
    return m_CallbackRunner == interruptRegressionRunner();
  }

  bool cancelInterruptInAndDrain(const UsbInterruptInToken& token,
                                 void (*callback)(uintptr_t, ssize_t), uintptr_t parameter,
                                 bool producerAlreadyStopped) override {
    if (token.transaction != 0x494e5452 || token.generation != 1 || callback != m_Callback ||
        parameter != m_Parameter) {
      panic("interrupt ownership regression received a stale cancellation token");
    }

    if (!producerAlreadyStopped) {
      m_CancelCalls += 1;
      m_Callbacks.close();
    }

    if (m_CallbackRunner == interruptRegressionRunner())
      return false;

    m_Callbacks.wait();
    return true;
  }

  OperationBarrier m_Callbacks;
  void (*m_Callback)(uintptr_t, ssize_t);
  uintptr_t m_Parameter;
  Atomic<void*> m_CallbackRunner;
  size_t m_CancelCalls;
};

class ProbeLeafUsbDevice : public UsbDevice {
 public:
  ProbeLeafUsbDevice(UsbHub* hub, uint8_t port, uint8_t address) : UsbDevice(hub, port, FullSpeed) {
    m_nAddress = address;
  }
};

class ProbeSubtreeUsbDevice : public Device, public UsbDevice {
 public:
  ProbeSubtreeUsbDevice(UsbHub* hub, uint8_t port, uint8_t address)
      : Device(), UsbDevice(hub, port, FullSpeed) {
    m_nAddress = address;
  }

  explicit ProbeSubtreeUsbDevice(UsbDevice* device) : Device(), UsbDevice(device) {}

  bool hasSubtree() const override {
    return true;
  }

  Device* getDevice() override {
    return this;
  }
};

struct DescendantProbePublication {
  explicit DescendantProbePublication(UsbDeviceContainer* container)
      : container(container), probe(), entered(0), publish(0), published(0) {}

  UsbDeviceContainer* container;
  OperationBarrier::Lease probe;
  Semaphore entered;
  Semaphore publish;
  Atomic<size_t> published;
};

int publishDescendantProbe(void* parameter) {
  auto* context = reinterpret_cast<DescendantProbePublication*>(parameter);
  context->entered.release();
  const bool released = context->publish.acquireForCompletion();
  (void)released;

  UsbDevice* generic = context->container->getUsbDevice();
  auto* replacement = new ProbeSubtreeUsbDevice(generic);
  auto* grandchild = new UsbDeviceContainer(new ProbeLeafUsbDevice(nullptr, 3, 7));
  {
    Device::TreeLockGuard treeGuard;
    grandchild->setParent(replacement);
    replacement->addChild(grandchild);
  }
  if (context->container->replaceUsbDevice(replacement))
    context->published += 1;
  context->probe = OperationBarrier::Lease();
  return 0;
}

struct ProbeSubtreeRetirement {
  ProbeSubtreeRetirement(UsbHub* hub, uint8_t port)
      : hub(hub), port(port), started(0), finished(0), retired{}, pinned{} {}

  UsbHub* hub;
  uint8_t port;
  Atomic<size_t> started;
  Atomic<size_t> finished;
  bool retired[128];
  bool pinned[128];
};

struct ExternalInterruptCancellation {
  ExternalInterruptCancellation(InterruptOwnershipHub* hub, UsbInterruptInHandle* handle)
      : hub(hub),
        handle(handle),
        callbackEntered(0),
        releaseCallback(0),
        callbackCalls(0),
        fireReturned(0),
        resetStarted(0),
        resetReturned(0),
        resetSucceeded(0),
        callbackResetRejected(0),
        callbackProcessor(static_cast<size_t>(-1)),
        resetProcessor(static_cast<size_t>(-1)) {}

  InterruptOwnershipHub* hub;
  UsbInterruptInHandle* handle;
  Semaphore callbackEntered;
  Semaphore releaseCallback;
  Atomic<size_t> callbackCalls;
  Atomic<size_t> fireReturned;
  Atomic<size_t> resetStarted;
  Atomic<size_t> resetReturned;
  Atomic<size_t> resetSucceeded;
  Atomic<size_t> callbackResetRejected;
  Atomic<size_t> callbackProcessor;
  Atomic<size_t> resetProcessor;
};

void blockingInterruptCallback(uintptr_t parameter, ssize_t) {
  auto* context = reinterpret_cast<ExternalInterruptCancellation*>(parameter);
  context->callbackProcessor = Processor::id();
  context->callbackCalls += 1;
  context->callbackEntered.release();
  const bool released = context->releaseCallback.acquireForCompletion();
  (void)released;
  if (!context->handle->reset())
    context->callbackResetRejected += 1;
}

int fireInterruptCallback(void* parameter) {
  auto* context = reinterpret_cast<ExternalInterruptCancellation*>(parameter);
  if (context->hub->fire())
    context->fireReturned += 1;
  return 0;
}

int resetInterruptHandle(void* parameter) {
  auto* context = reinterpret_cast<ExternalInterruptCancellation*>(parameter);
  context->resetProcessor = Processor::id();
  context->resetStarted += 1;
  if (context->handle->reset())
    context->resetSucceeded += 1;
  context->resetReturned += 1;
  return 0;
}

struct SelfInterruptCancellation {
  explicit SelfInterruptCancellation(UsbInterruptInHandle* handle)
      : handle(handle), callbacks(0), resetRejected(0), ownershipRetained(0) {}

  UsbInterruptInHandle* handle;
  Atomic<size_t> callbacks;
  Atomic<size_t> resetRejected;
  Atomic<size_t> ownershipRetained;
};

void selfCancellingInterruptCallback(uintptr_t parameter, ssize_t) {
  auto* context = reinterpret_cast<SelfInterruptCancellation*>(parameter);
  context->callbacks += 1;
  if (!context->handle->reset())
    context->resetRejected += 1;
  if (*context->handle)
    context->ownershipRetained += 1;
}

bool waitForInterruptValue(Atomic<size_t>& value, size_t expected) {
  const Time::Timestamp deadline = Time::getTicks() + (500 * Time::Multiplier::Millisecond);
  while (value != expected && Time::getTicks() < deadline)
    Scheduler::instance().yield();
  return value == expected;
}

bool waitForInterruptDrain(Thread* thread) {
  const Time::Timestamp deadline = Time::getTicks() + (500 * Time::Multiplier::Millisecond);
  while (thread->getStatus() != Thread::Sleeping && Time::getTicks() < deadline)
    Scheduler::instance().yield();
  return thread->getStatus() == Thread::Sleeping;
}
}  // namespace

int UsbHub::retireProbeSubtreeForTest(void* parameter) {
  auto* context = reinterpret_cast<ProbeSubtreeRetirement*>(parameter);
  context->started += 1;
  context->hub->retirePortContainersLocked(context->port, context->retired, context->pinned);
  context->finished += 1;
  return 0;
}

bool UsbHub::runInterruptOwnershipRegression() {
  bool subtreeAddressPinPassed = false;
  {
    InterruptOwnershipHub hub;
    constexpr size_t ParentAddress = 5;
    constexpr size_t DescendantAddress = 6;
    hub.m_UsedAddresses.set(ParentAddress);
    hub.m_UsedAddresses.set(DescendantAddress);
    hub.m_AddressReferences[ParentAddress] = 1;
    hub.m_AddressReferences[DescendantAddress] = 1;

    bool subtreeAddresses[128] = {};
    subtreeAddresses[ParentAddress] = true;
    subtreeAddresses[DescendantAddress] = true;
    hub.retainAddressesLocked(subtreeAddresses);

    // Recursive driver teardown releases both live owners, but the repeated
    // connection's subtree pins must keep both addresses unavailable until
    // the parent reset establishes a new generation boundary.
    hub.releaseAddressLocked(ParentAddress);
    hub.releaseAddressLocked(DescendantAddress);
    const bool heldThroughDrain =
        hub.m_UsedAddresses.test(ParentAddress) && hub.m_UsedAddresses.test(DescendantAddress);
    hub.releaseAddressesLocked(subtreeAddresses);
    subtreeAddressPinPassed = heldThroughDrain && !hub.m_UsedAddresses.test(ParentAddress) &&
                              !hub.m_UsedAddresses.test(DescendantAddress);
  }

  bool driverUnbindPinPassed = false;
  {
    InterruptOwnershipHub hub;
    constexpr size_t DescendantAddress = 7;
    hub.m_UsedAddresses.set(DescendantAddress);
    hub.m_AddressReferences[DescendantAddress] = 1;

    bool descendantAddresses[128] = {};
    descendantAddresses[DescendantAddress] = true;
    hub.retainAddressesLocked(descendantAddresses);
    hub.releaseAddressLocked(DescendantAddress);

    // Unbinding a hub driver does not reset physical downstream ports, so the
    // retained reference must deliberately survive the software subtree.
    driverUnbindPinPassed = hub.m_UsedAddresses.test(DescendantAddress) &&
                            hub.m_AddressReferences[DescendantAddress] == 1;
  }

  bool subtreeProbeFixedPointPassed = false;
  {
    InterruptOwnershipHub hub;
    constexpr size_t ParentAddress = 5;
    constexpr size_t DescendantAddress = 6;
    constexpr size_t GrandchildAddress = 7;
    constexpr size_t ReplacedGrandchildAddress = 8;
    for (size_t address = ParentAddress; address <= ReplacedGrandchildAddress; ++address) {
      hub.m_UsedAddresses.set(address);
      hub.m_AddressReferences[address] = 1;
    }

    auto* parentDevice = new ProbeSubtreeUsbDevice(&hub, 1, ParentAddress);
    auto* parentContainer = new UsbDeviceContainer(parentDevice);
    auto* descendantDevice = new ProbeSubtreeUsbDevice(&hub, 2, DescendantAddress);
    auto* descendantContainer = new UsbDeviceContainer(descendantDevice);
    auto* replacedGrandchild =
        new UsbDeviceContainer(new ProbeLeafUsbDevice(&hub, 4, ReplacedGrandchildAddress));
    {
      Device::TreeLockGuard treeGuard;
      replacedGrandchild->setParent(descendantDevice);
      descendantDevice->addChild(replacedGrandchild);
      descendantContainer->setParent(parentDevice);
      parentDevice->addChild(descendantContainer);
      parentContainer->setParent(&hub);
      hub.addChild(parentContainer);
    }

    DescendantProbePublication publication(descendantContainer);
    const bool probeAdmitted = descendantContainer->tryAcquireProbe(publication.probe);
    Process* process = Scheduler::instance().getKernelProcess();
    Thread* publisher = nullptr;
    Thread* retiree = nullptr;
    bool publisherEntered = false;
    bool retirementBlocked = false;
    ProbeSubtreeRetirement retirement(&hub, 1);
    if (probeAdmitted) {
      publisher = new Thread(process, publishDescendantProbe, &publication, nullptr, false,
                             PinInterruptTestThreads);
      publisher->setName("USB descendant probe publisher");
      publisherEntered = publication.entered.acquireForCompletion();
    }
    if (publisherEntered) {
      retiree = new Thread(process, retireProbeSubtreeForTest, &retirement, nullptr, false,
                           PinInterruptTestThreads);
      retiree->setName("USB subtree retirement");
      const bool retirementStarted = waitForInterruptValue(retirement.started, 1);
      retirementBlocked =
          retirementStarted && waitForInterruptDrain(retiree) && !retirement.finished;
    }

    publication.publish.release();
    const bool publisherJoined = !publisher || publisher->joinForCompletion();
    const bool retireeJoined = !retiree || retiree->joinForCompletion();
    subtreeProbeFixedPointPassed =
        probeAdmitted && publisherEntered && retirementBlocked && publisherJoined &&
        retireeJoined && publication.published == static_cast<size_t>(1) &&
        retirement.finished == static_cast<size_t>(1) && retirement.retired[ParentAddress] &&
        retirement.pinned[ParentAddress] && retirement.pinned[DescendantAddress] &&
        retirement.pinned[GrandchildAddress] && !retirement.pinned[ReplacedGrandchildAddress] &&
        hub.m_AddressReferences[GrandchildAddress] == 2;

    hub.releaseAddressesLocked(retirement.pinned);
    for (size_t address = ParentAddress; address <= ReplacedGrandchildAddress; ++address)
      hub.releaseAddressLocked(address);
  }

  bool externalPassed = false;
  size_t callbackProcessor = static_cast<size_t>(-1);
  size_t resetProcessor = static_cast<size_t>(-1);
#if PEDIGREE_CONCURRENCY_SMOKE_TESTS
  constexpr size_t MaxExternalAttempts = 16;
#else
  constexpr size_t MaxExternalAttempts = 1;
#endif
  size_t externalAttempts = 0;
  while (externalAttempts < MaxExternalAttempts && !externalPassed) {
    ++externalAttempts;
    InterruptOwnershipHub hub;
    UsbInterruptInHandle handle;
    ExternalInterruptCancellation context(&hub, &handle);
    UsbEndpoint endpoint(1, 1, 1, FullSpeed, 8);
    const bool submitted = hub.addInterruptInHandler(endpoint, 0x1000, 8, blockingInterruptCallback,
                                                     handle, reinterpret_cast<uintptr_t>(&context));

    Process* process = Scheduler::instance().getKernelProcess();
    Thread* delivery = nullptr;
    Thread* cancellation = nullptr;
    bool callbackEntered = false;
    bool resetStarted = false;
    bool resetBlocked = false;
    if (submitted) {
      delivery = new Thread(process, fireInterruptCallback, &context, nullptr, false,
                            PinInterruptTestThreads);
      delivery->setName("USB interrupt ownership callback");
      callbackEntered = context.callbackEntered.acquireForCompletion();
    }
    if (callbackEntered) {
      cancellation = new Thread(process, resetInterruptHandle, &context, nullptr, false,
                                PinInterruptTestThreads);
      cancellation->setName("USB interrupt ownership cancellation");
      resetStarted = waitForInterruptValue(context.resetStarted, 1);
      resetBlocked = resetStarted && waitForInterruptDrain(cancellation) && !context.resetReturned;
    }

    context.releaseCallback.release();
    const bool deliveryJoined = !delivery || delivery->joinForCompletion();
    const bool cancellationJoined = !cancellation || cancellation->joinForCompletion();
    bool cleanupSucceeded = true;
    if (handle)
      cleanupSucceeded = handle.reset();

    callbackProcessor = context.callbackProcessor;
    resetProcessor = context.resetProcessor;
    const bool semanticsPassed = submitted && callbackEntered && resetBlocked && deliveryJoined &&
                                 cancellationJoined && cleanupSucceeded &&
                                 context.callbackCalls == static_cast<size_t>(1) &&
                                 context.fireReturned == static_cast<size_t>(1) &&
                                 context.resetReturned == static_cast<size_t>(1) &&
                                 context.resetSucceeded == static_cast<size_t>(1) && !handle &&
                                 context.callbackResetRejected == static_cast<size_t>(1) &&
                                 hub.cancelCalls() == 1 && !hub.fire();
    if (!semanticsPassed)
      break;
#if PEDIGREE_CONCURRENCY_SMOKE_TESTS
    externalPassed = callbackProcessor != resetProcessor;
#else
    externalPassed = true;
#endif
  }

  bool selfPassed = false;
  {
    InterruptOwnershipHub hub;
    UsbInterruptInHandle handle;
    SelfInterruptCancellation context(&handle);
    UsbEndpoint endpoint(1, 1, 1, FullSpeed, 8);
    const bool submitted =
        hub.addInterruptInHandler(endpoint, 0x1000, 8, selfCancellingInterruptCallback, handle,
                                  reinterpret_cast<uintptr_t>(&context));
    const bool fired = submitted && hub.fire();
    const bool retained = static_cast<bool>(handle);
    const bool retried = retained && handle.reset();
    selfPassed = submitted && fired && context.callbacks == static_cast<size_t>(1) &&
                 context.resetRejected == static_cast<size_t>(1) &&
                 context.ownershipRetained == static_cast<size_t>(1) && retained && retried &&
                 !handle && hub.cancelCalls() == 1 && !hub.fire();
  }

#if PEDIGREE_CONCURRENCY_SMOKE_TESTS
  NOTICE("QEMU-CONCURRENCY-TEST: usb-interrupt-cancel cpus="
         << Dec << callbackProcessor << "/" << resetProcessor << " attempts=" << externalAttempts);
#endif
  return subtreeAddressPinPassed && driverUnbindPinPassed && subtreeProbeFixedPointPassed &&
         externalPassed && selfPassed;
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
bool UsbHub::runHostedInterruptOwnershipRegression() {
  const bool passed = runInterruptOwnershipRegression();
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS usb-interrupt-handle-cancel-drain");
  } else {
    ERROR(
        "HOSTED-WAIT-TEST: FAIL usb-interrupt-handle-cancel-drain: "
        "subtree address pin, external drain, or callback-context retry failed");
  }
  return passed;
}

EXPORTED_PUBLIC bool runHostedUsbInterruptOwnershipRegression() {
  return UsbHub::runHostedInterruptOwnershipRegression();
}
#endif

#if PEDIGREE_CONCURRENCY_SMOKE_TESTS
bool UsbHub::runQemuInterruptOwnershipRegression() {
  const bool passed = runInterruptOwnershipRegression();
  if (passed) {
    NOTICE("QEMU-CONCURRENCY-TEST: PASS usb-interrupt-cancel-drain-smp");
  } else {
    ERROR(
        "QEMU-CONCURRENCY-TEST: FAIL usb-interrupt-cancel-drain-smp: "
        "subtree address pin, external drain, callback-context retry, or CPU spread failed");
  }
  return passed;
}
#endif
#endif
