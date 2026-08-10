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
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/process/OperationBarrier.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/utilities/List.h"
#include "pedigree/kernel/utilities/utility.h"

#include "modules/system/usb/UsbDevice.h"
#if (HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS) || PEDIGREE_CONCURRENCY_SMOKE_TESTS
#include "modules/system/usb/UsbConstants.h"
#include "modules/system/usb/UsbDescriptors.h"
#endif

#if (HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS) || PEDIGREE_CONCURRENCY_SMOKE_TESTS
#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/time/Time.h"
#endif

UsbPnP UsbPnP::m_Instance;

struct UsbPnP::CallbackItem {
  CallbackItem(callback_t callback, uint16_t vendorId, uint16_t productId, uint8_t deviceClass,
               uint8_t subclass, uint8_t protocol)
      : callback(callback),
        nVendorId(vendorId),
        nProductId(productId),
        nClass(deviceClass),
        nSubclass(subclass),
        nProtocol(protocol),
        sequence(0),
        next(nullptr),
        operations(),
        bindings(),
        firstBinding(nullptr),
        lastBinding(nullptr) {}

  ~CallbackItem() {
    if (operations.isOpen())
      operations.close();
    operations.wait();
    if (bindings.isOpen())
      bindings.close();
    bindings.wait();
    assert(!firstBinding && !lastBinding);
  }

  callback_t callback;
  uint16_t nVendorId;
  uint16_t nProductId;
  uint8_t nClass;
  uint8_t nSubclass;
  uint8_t nProtocol;
  size_t sequence;
  CallbackItem* next;
  OperationBarrier operations;
  OperationBarrier bindings;
  UsbDeviceContainer* firstBinding;
  UsbDeviceContainer* lastBinding;
};

struct UsbPnP::ActiveInvocation {
  void* owner;
  ActiveInvocation* next;
};

UsbPnP::Registration::Registration() : m_Owner(nullptr), m_Item(nullptr), m_Resetting(false) {}

UsbPnP::Registration::Registration(Registration&& other)
    : m_Owner(static_cast<UsbPnP*>(other.m_Owner)),
      m_Item(static_cast<CallbackItem*>(other.m_Item)),
      m_Resetting(false) {
  assert(!other.m_Resetting);
  other.m_Owner = nullptr;
  other.m_Item = nullptr;
}

UsbPnP::Registration::~Registration() {
  if (!reset()) {
    FATAL("Live UsbPnP registration could not be retired.");
  }
}

UsbPnP::Registration& UsbPnP::Registration::operator=(Registration&& other) {
  if (this != &other) {
    if (!reset()) {
      FATAL("UsbPnP registration move could not retire ownership.");
    }
    assert(!other.m_Resetting);
    m_Owner = static_cast<UsbPnP*>(other.m_Owner);
    m_Item = static_cast<CallbackItem*>(other.m_Item);
    other.m_Owner = nullptr;
    other.m_Item = nullptr;
  }
  return *this;
}

bool UsbPnP::Registration::reset() {
  while (!m_Resetting.compareAndSwap(false, true)) {
    UsbPnP* owner = m_Owner;
    if (owner && owner->inCurrentCallbackContext())
      return false;
    Scheduler::instance().yield();
  }

  UsbPnP* owner = m_Owner;
  CallbackItem* item = m_Item;
  if (!owner || !item) {
    m_Resetting = false;
    return true;
  }

  if (!owner->unregisterCallback(item)) {
    m_Resetting = false;
    return false;
  }

  m_Owner = nullptr;
  m_Item = nullptr;
  m_Resetting = false;
  return true;
}

void UsbPnP::Registration::adopt(UsbPnP* owner, CallbackItem* item) {
  assert(!static_cast<CallbackItem*>(m_Item));
  assert(!m_Resetting);
  m_Owner = owner;
  m_Item = item;
}

UsbPnP::UsbPnP()
    : m_FirstCallback(nullptr),
      m_LastCallback(nullptr),
      m_CallbackCount(0),
      m_CallbackLock(),
      m_BindingLock(),
      m_NextCallbackSequence(1),
      m_ActiveInvocations(nullptr) {}

UsbPnP::~UsbPnP() {
  {
    LockGuard<Spinlock> guard(m_CallbackLock);
    if (isCallbackContext(currentInvocationOwner())) {
      FATAL("UsbPnP cannot be destroyed from callback context.");
    }
  }

  while (true) {
    CallbackItem* item = nullptr;
    {
      LockGuard<Spinlock> guard(m_CallbackLock);
      if (!m_FirstCallback) {
        break;
      }

      item = m_FirstCallback;
      m_FirstCallback = item->next;
      if (!m_FirstCallback) {
        m_LastCallback = nullptr;
      }
      if (item->operations.isOpen()) {
        --m_CallbackCount;
      }
      item->operations.close();
    }

    item->operations.wait();
    retireBindings(item, false);
    delete item;
  }
}

bool UsbPnP::probeDevice(Device* pDeviceBase) {
  if (!pDeviceBase || pDeviceBase->getType() != Device::UsbContainer)
    return false;
  auto* container = static_cast<UsbDeviceContainer*>(pDeviceBase);
  OperationBarrier::Lease probe;
  if (!container->tryAcquireProbe(probe))
    return false;
  return probeDeviceAdmitted(container, probe);
}

bool UsbPnP::probeDeviceAdmitted(UsbDeviceContainer* container, OperationBarrier::Lease& probe) {
  if (!container || !probe)
    return false;

  LockGuard<Mutex> guard(container->m_ProbeLock);
  doProbe(container);
  UsbDevice* device = container->getUsbDevice();
  return device && device->getUsbState() == UsbDevice::HasDriver;
}

Device* UsbPnP::doProbe(Device* pDeviceBase) {
  // Sanity check.
  if (!(pDeviceBase->getType() == Device::UsbContainer)) {
    return pDeviceBase;
  }

  UsbDeviceContainer* pContainer = static_cast<UsbDeviceContainer*>(pDeviceBase);
  UsbDevice* pDevice = pContainer->getUsbDevice();

  // Is this device already handled by a driver?
  if (pDevice->getUsbState() == UsbDevice::HasDriver) {
    return pDeviceBase;
  }

  size_t afterSequence = 0;
  while (true) {
    CallbackItem* item = nullptr;
    callback_t callback = nullptr;
    size_t sequence = 0;
    ActiveInvocation invocation = {nullptr, nullptr};
    if (!acquireCallback(pDevice, afterSequence, item, callback, sequence, invocation)) {
      break;
    }

    // Call the callback, which will give us (hopefully) a copy of pDevice,
    // in the form of a driver class
    UsbDevice* pNewDevice = callback(pDevice);
    afterSequence = sequence;

    // Was this device rejected by the driver?
    if (!pNewDevice) {
      finishCallback(item, invocation);
      continue;
    }
    if (pNewDevice == pDevice) {
      ERROR("USB: PnP factories must return a distinct driver instance");
      finishCallback(item, invocation);
      continue;
    }

    // Initialise the driver
    pNewDevice->initialiseDriver();

    // Did the device go into the driver state?
    if (pNewDevice->getUsbState() == UsbDevice::HasDriver) {
      OperationBarrier::Lease binding;
      if (!item->bindings.tryAcquire(binding)) {
        delete pNewDevice;
        finishCallback(item, invocation);
        ERROR("USB: PnP registration closed before its binding was published");
        return pDeviceBase;
      }

      // Publish into the already-linked container before releasing callback
      // ownership, so registration teardown cannot miss the bound object.
      const bool replaced = pContainer->replaceUsbDevice(pNewDevice);
      if (replaced)
        publishBinding(item, pContainer, binding);
      finishCallback(item, invocation);
      if (!replaced) {
        delete pNewDevice;
        ERROR("USB: PnP could not publish a matched driver into its container");
      }
      return pDeviceBase;
    } else {
      delete pNewDevice;
      finishCallback(item, invocation);
    }
  }
  return pDeviceBase;
}

bool UsbPnP::acquireCallback(UsbDevice* device, size_t afterSequence, CallbackItem*& resultItem,
                             callback_t& resultCallback, size_t& resultSequence,
                             ActiveInvocation& invocation) {
  resultItem = nullptr;
  resultCallback = nullptr;
  resultSequence = 0;

  UsbDevice::DeviceDescriptor* descriptor = device->getDescriptor();
  UsbDevice::Interface* interface = device->getInterface();

  LockGuard<Spinlock> guard(m_CallbackLock);
  for (CallbackItem* item = m_FirstCallback; item; item = item->next) {
    if (item->sequence <= afterSequence) {
      continue;
    }

    if ((item->nVendorId != VendorIdNone) && (item->nVendorId != descriptor->nVendorId)) {
      continue;
    }
    if ((item->nProductId != ProductIdNone) && (item->nProductId != descriptor->nProductId)) {
      continue;
    }
    if ((item->nClass != ClassNone) && (item->nClass != interface->nClass)) {
      continue;
    }
    if ((item->nSubclass != SubclassNone) && (item->nSubclass != interface->nSubclass)) {
      continue;
    }
    if ((item->nProtocol != ProtocolNone) && (item->nProtocol != interface->nProtocol)) {
      continue;
    }
    if (!item->operations.tryEnter()) {
      continue;
    }

    resultItem = item;
    resultCallback = item->callback;
    resultSequence = item->sequence;
    invocation.owner = currentInvocationOwner();
    invocation.next = m_ActiveInvocations;
    m_ActiveInvocations = &invocation;
    return true;
  }

  return false;
}

void UsbPnP::finishCallback(CallbackItem* item, ActiveInvocation& invocation) {
  LockGuard<Spinlock> guard(m_CallbackLock);
  ActiveInvocation** invocationLink = &m_ActiveInvocations;
  while (*invocationLink && *invocationLink != &invocation) {
    invocationLink = &((*invocationLink)->next);
  }
  if (*invocationLink) {
    *invocationLink = invocation.next;
  } else {
    FATAL("UsbPnP lost an active callback invocation.");
  }

  item->operations.leave();
}

void* UsbPnP::currentInvocationOwner() {
  ProcessorInformation& information = Processor::information();
  Thread* thread = information.getCurrentThread();
  return thread ? static_cast<void*>(thread) : static_cast<void*>(&information);
}

bool UsbPnP::isCallbackContext(void* owner) const {
  for (ActiveInvocation* invocation = m_ActiveInvocations; invocation;
       invocation = invocation->next) {
    if (invocation->owner == owner) {
      return true;
    }
  }
  return false;
}

bool UsbPnP::inCurrentCallbackContext() {
  LockGuard<Spinlock> guard(m_CallbackLock);
  return isCallbackContext(currentInvocationOwner());
}

void UsbPnP::reprobeDevices(Device* pParent) {
  struct ProbeCandidate {
    explicit ProbeCandidate(UsbDeviceContainer* container) : container(container), probe() {}

    UsbDeviceContainer* container;
    OperationBarrier::Lease probe;
  };

  List<ProbeCandidate*> candidates;
  auto collectCandidate = [](Device* p, List<ProbeCandidate*>* candidates) -> Device* {
    if (p->getType() == Device::UsbContainer) {
      auto* candidate = new ProbeCandidate(static_cast<UsbDeviceContainer*>(p));
      if (candidate->container->tryAcquireProbe(candidate->probe))
        candidates->pushBack(candidate);
      else
        delete candidate;
    }
    return p;
  };

  auto collector = pedigree_std::make_callable(collectCandidate);
  Device::foreach (collector, pParent, &candidates);

  while (candidates.count()) {
    ProbeCandidate* candidate = candidates.popFront();
    probeDeviceAdmitted(candidate->container, candidate->probe);
    delete candidate;
  }
}

bool UsbPnP::registerCallbackItem(CallbackItem* item, Registration& registration, bool reprobe) {
  if (!item || !item->callback || registration) {
    delete item;
    return false;
  }

  {
    LockGuard<Spinlock> guard(m_CallbackLock);
    item->sequence = m_NextCallbackSequence++;
    if (m_LastCallback) {
      m_LastCallback->next = item;
    } else {
      m_FirstCallback = item;
    }
    m_LastCallback = item;
    ++m_CallbackCount;
    registration.adopt(this, item);
  }

  if (reprobe) {
    reprobeDevices(nullptr);
  }
  return true;
}

bool UsbPnP::registerCallback(uint16_t nVendorId, uint16_t nProductId, callback_t callback,
                              Registration& registration) {
  return registerCallbackItem(
      new CallbackItem(callback, nVendorId, nProductId, ClassNone, SubclassNone, ProtocolNone),
      registration, true);
}

bool UsbPnP::registerCallback(uint8_t nClass, uint8_t nSubclass, uint8_t nProtocol,
                              callback_t callback, Registration& registration) {
  return registerCallbackItem(
      new CallbackItem(callback, VendorIdNone, ProductIdNone, nClass, nSubclass, nProtocol),
      registration, true);
}

void UsbPnP::publishBinding(CallbackItem* item, UsbDeviceContainer* container,
                            OperationBarrier::Lease& binding) {
  assert(item && container && binding);
  LockGuard<Mutex> guard(m_BindingLock);
  assert(!container->m_BindingOwner && !container->m_PreviousBinding && !container->m_NextBinding);

  container->m_BindingRegistry = this;
  container->m_BindingOwner = item;
  container->m_PreviousBinding = item->lastBinding;
  if (item->lastBinding)
    item->lastBinding->m_NextBinding = container;
  else
    item->firstBinding = container;
  item->lastBinding = container;
  container->m_BindingLease = pedigree_std::move(binding);
}

void UsbPnP::unlinkBindingLocked(UsbDeviceContainer* container) {
  auto* item = static_cast<CallbackItem*>(container->m_BindingOwner);
  if (!item) {
    assert(!container->m_PreviousBinding && !container->m_NextBinding);
    return;
  }

  if (container->m_PreviousBinding)
    container->m_PreviousBinding->m_NextBinding = container->m_NextBinding;
  else
    item->firstBinding = container->m_NextBinding;
  if (container->m_NextBinding)
    container->m_NextBinding->m_PreviousBinding = container->m_PreviousBinding;
  else
    item->lastBinding = container->m_PreviousBinding;

  container->m_BindingOwner = nullptr;
  container->m_PreviousBinding = nullptr;
  container->m_NextBinding = nullptr;
  // Destruction treats this atomic pointer as the unlink-complete sentinel.
  // Publish it only after every other access to the container has finished.
  container->m_BindingRegistry = nullptr;
}

void UsbPnP::detachBinding(UsbDeviceContainer* container) {
  if (!container)
    return;
  LockGuard<Mutex> guard(m_BindingLock);
  unlinkBindingLocked(container);
}

void UsbPnP::retireBindings(CallbackItem* item, bool reprobe) {
  if (!item)
    return;
  if (item->bindings.isOpen())
    item->bindings.close();

  struct BoundCandidate {
    BoundCandidate() : container(nullptr), probe(), binding() {}

    UsbDeviceContainer* container;
    OperationBarrier::Lease probe;
    OperationBarrier::Lease binding;
  };

  while (true) {
    BoundCandidate candidate;
    {
      LockGuard<Mutex> bindingGuard(m_BindingLock);
      candidate.container = item->firstBinding;
      if (!candidate.container)
        break;

      const bool admitted = candidate.container->tryAcquireProbe(candidate.probe);
      unlinkBindingLocked(candidate.container);
      if (admitted)
        candidate.binding = pedigree_std::move(candidate.container->m_BindingLease);
    }

    if (!candidate.probe) {
      // Physical teardown already closed this container. It keeps the binding
      // lease until its driver has been destroyed; bindings.wait() below is
      // the retirement join.
      continue;
    }

    {
      // Keep the retiring registration alive through its driver's destructor,
      // then release it before another callback can bind the generic device.
      OperationBarrier::Lease binding = pedigree_std::move(candidate.binding);
      LockGuard<Mutex> probeGuard(candidate.container->m_ProbeLock);
      UsbDevice* bound = candidate.container->m_pUsbDevice;
      UsbDevice* generic = new UsbDevice(bound);
      generic->m_UsbState = UsbDevice::HasInterface;
      bound->prepareForDriverRetirement();
      const bool replaced = candidate.container->replaceUsbDevice(generic);
      assert(replaced);
      (void)replaced;
    }

    if (reprobe)
      probeDeviceAdmitted(candidate.container, candidate.probe);
  }

  item->bindings.wait();
}

bool UsbPnP::unregisterCallback(CallbackItem* item) {
  bool found = false;
  bool callbackContext = false;
  {
    LockGuard<Spinlock> guard(m_CallbackLock);
    CallbackItem* previous = nullptr;
    for (CallbackItem* current = m_FirstCallback; current; current = current->next) {
      if (current != item) {
        previous = current;
        continue;
      }

      if (item->operations.isOpen()) {
        item->operations.close();
        --m_CallbackCount;
      }
      callbackContext = isCallbackContext(currentInvocationOwner());
      if (!callbackContext) {
        if (previous) {
          previous->next = item->next;
        } else {
          m_FirstCallback = item->next;
        }
        if (m_LastCallback == item) {
          m_LastCallback = previous;
        }
        item->next = nullptr;
      }
      found = true;
      break;
    }
  }

  if (!found) {
    return true;
  }
  if (callbackContext) {
    return false;
  }
  item->operations.wait();
  retireBindings(item, true);
  delete item;
  return true;
}

#if (HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS) || PEDIGREE_CONCURRENCY_SMOKE_TESTS
namespace {
constexpr bool PinTestThreads = HOSTED;

struct HostedRegistrationContext {
  HostedRegistrationContext(UsbPnP* registry, UsbPnP::Registration* registration)
      : registry(registry),
        registration(registration),
        releaseCallback(0),
        callbackEntered(0),
        callbackFinished(0),
        callbackResetRejected(0),
        invocationFinished(0),
        unregisterStarted(0),
        unregisterFinished(0) {}

  UsbPnP* registry;
  UsbPnP::Registration* registration;
  Semaphore releaseCallback;
  Atomic<size_t> callbackEntered;
  Atomic<size_t> callbackFinished;
  Atomic<size_t> callbackResetRejected;
  Atomic<size_t> invocationFinished;
  Atomic<size_t> unregisterStarted;
  Atomic<size_t> unregisterFinished;
};

HostedRegistrationContext* g_HostedRegistrationContext = nullptr;

struct HostedSelfRemovalContext {
  explicit HostedSelfRemovalContext(UsbPnP::Registration* registration)
      : registration(registration), callbackEntered(0), resetRejected(0), resetFinished(0) {}

  UsbPnP::Registration* registration;
  Atomic<size_t> callbackEntered;
  Atomic<size_t> resetRejected;
  Atomic<size_t> resetFinished;
};

HostedSelfRemovalContext* g_HostedSelfRemovalContext = nullptr;

struct HostedReciprocalRemovalContext {
  HostedReciprocalRemovalContext(UsbPnP* registry, UsbPnP::Registration* first,
                                 UsbPnP::Registration* second)
      : registry(registry),
        first(first),
        second(second),
        beginReset(0),
        callbacksEntered(0),
        resetRejections(0),
        resetsFinished(0),
        invocationsFinished(0),
        firstProcessor(static_cast<size_t>(-1)),
        secondProcessor(static_cast<size_t>(-1)),
        failures(0) {}

  UsbPnP* registry;
  UsbPnP::Registration* first;
  UsbPnP::Registration* second;
  Semaphore beginReset;
  Atomic<size_t> callbacksEntered;
  Atomic<size_t> resetRejections;
  Atomic<size_t> resetsFinished;
  Atomic<size_t> invocationsFinished;
  Atomic<size_t> firstProcessor;
  Atomic<size_t> secondProcessor;
  Atomic<size_t> failures;
};

HostedReciprocalRemovalContext* g_HostedReciprocalRemovalContext = nullptr;

Atomic<size_t> g_BoundDriverDestructions(0);
Atomic<size_t> g_BoundDriverRetirements(0);
Atomic<size_t> g_UnrelatedTreeVisits(0);

class BindingTreeAccess : public Device {
 public:
  static Device& rootDevice() {
    return root();
  }
};

class HostedUnrelatedTreeDevice : public Device {
 public:
  Type getType() override {
    g_UnrelatedTreeVisits += 1;
    return Generic;
  }
};

class HostedBindingUsbDevice : public UsbDevice {
 public:
  HostedBindingUsbDevice() : UsbDevice(nullptr, 1, FullSpeed), m_IsDriver(false) {
    auto* rawDevice =
        reinterpret_cast<UsbDeviceDescriptor*>(new uint8_t[sizeof(UsbDeviceDescriptor)]);
    ByteSet(rawDevice, 0, sizeof(UsbDeviceDescriptor));
    rawDevice->nLength = sizeof(UsbDeviceDescriptor);
    rawDevice->nType = UsbDescriptor::Device;
    rawDevice->nConfigurations = 1;
    m_pDescriptor = new DeviceDescriptor(rawDevice);

    constexpr size_t ConfigBytes =
        sizeof(UsbConfigurationDescriptor) + sizeof(UsbInterfaceDescriptor);
    uint8_t* rawConfig = new uint8_t[ConfigBytes];
    ByteSet(rawConfig, 0, ConfigBytes);
    auto* config = reinterpret_cast<UsbConfigurationDescriptor*>(rawConfig);
    config->nLength = sizeof(UsbConfigurationDescriptor);
    config->nType = UsbDescriptor::Configuration;
    config->nTotalLength = ConfigBytes;
    config->nInterfaces = 1;
    config->nConfig = 1;
    auto* interface =
        reinterpret_cast<UsbInterfaceDescriptor*>(rawConfig + sizeof(UsbConfigurationDescriptor));
    interface->nLength = sizeof(UsbInterfaceDescriptor);
    interface->nType = UsbDescriptor::Interface;
    interface->nClass = 0xFE;

    m_pConfiguration = new ConfigDescriptor(rawConfig, ConfigBytes, FullSpeed);
    m_pDescriptor->configList.pushBack(m_pConfiguration);
    m_pInterface = m_pConfiguration->interfaceList[0];
    m_nAddress = 1;
    m_UsbState = HasInterface;
  }

  explicit HostedBindingUsbDevice(UsbDevice* device) : UsbDevice(device), m_IsDriver(true) {}

  ~HostedBindingUsbDevice() override {
    if (m_IsDriver)
      g_BoundDriverDestructions += 1;
  }

  void initialiseDriver() override {
    m_UsbState = HasDriver;
  }

  void prepareForDriverRetirement() override {
    if (m_IsDriver)
      g_BoundDriverRetirements += 1;
  }

 private:
  bool m_IsDriver;
};

UsbDevice* hostedBindingCallback(UsbDevice* device) {
  return new HostedBindingUsbDevice(device);
}

UsbDevice* hostedRegistrationCallback(UsbDevice*) {
  HostedRegistrationContext* context = g_HostedRegistrationContext;
  context->callbackEntered += 1;
  const bool released = context->releaseCallback.acquireForCompletion();
  (void)released;
  if (!context->registration->reset())
    context->callbackResetRejected += 1;
  context->callbackFinished += 1;
  return nullptr;
}

UsbDevice* hostedSelfRemovalCallback(UsbDevice*) {
  HostedSelfRemovalContext* context = g_HostedSelfRemovalContext;
  context->callbackEntered += 1;
  if (!context->registration->reset() && *context->registration) {
    context->resetRejected += 1;
  }
  context->resetFinished += 1;
  return nullptr;
}

UsbDevice* hostedFirstReciprocalRemovalCallback(UsbDevice*) {
  HostedReciprocalRemovalContext* context = g_HostedReciprocalRemovalContext;
  context->firstProcessor = Processor::id();
  context->callbacksEntered += 1;
  const bool released = context->beginReset.acquireForCompletion();
  (void)released;
  if (!context->second->reset() && *context->second) {
    context->resetRejections += 1;
  } else {
    context->failures += 1;
  }
  context->resetsFinished += 1;
  const Time::Timestamp deadline = Time::getTicks() + (500 * Time::Multiplier::Millisecond);
  while (context->resetsFinished != static_cast<size_t>(2) && Time::getTicks() < deadline) {
    Scheduler::instance().yield();
  }
  if (context->resetsFinished != static_cast<size_t>(2)) {
    context->failures += 1;
  }
  return nullptr;
}

UsbDevice* hostedSecondReciprocalRemovalCallback(UsbDevice*) {
  HostedReciprocalRemovalContext* context = g_HostedReciprocalRemovalContext;
  context->secondProcessor = Processor::id();
  context->callbacksEntered += 1;
  const bool released = context->beginReset.acquireForCompletion();
  (void)released;
  if (!context->first->reset() && *context->first) {
    context->resetRejections += 1;
  } else {
    context->failures += 1;
  }
  context->resetsFinished += 1;
  const Time::Timestamp deadline = Time::getTicks() + (500 * Time::Multiplier::Millisecond);
  while (context->resetsFinished != static_cast<size_t>(2) && Time::getTicks() < deadline) {
    Scheduler::instance().yield();
  }
  if (context->resetsFinished != static_cast<size_t>(2)) {
    context->failures += 1;
  }
  return nullptr;
}

int invokeHostedRegistration(void* parameter) {
  HostedRegistrationContext* context = reinterpret_cast<HostedRegistrationContext*>(parameter);
  if (context->registry->invokeCallbackForTest()) {
    context->invocationFinished += 1;
  }
  return 0;
}

int unregisterHostedRegistration(void* parameter) {
  HostedRegistrationContext* context = reinterpret_cast<HostedRegistrationContext*>(parameter);
  context->unregisterStarted += 1;
  context->registration->reset();
  context->unregisterFinished += 1;
  return 0;
}

int invokeFirstReciprocalRemoval(void* parameter) {
  HostedReciprocalRemovalContext* context =
      reinterpret_cast<HostedReciprocalRemovalContext*>(parameter);
  if (context->registry->invokeCallbackForTest(0)) {
    context->invocationsFinished += 1;
  }
  return 0;
}

int invokeSecondReciprocalRemoval(void* parameter) {
  HostedReciprocalRemovalContext* context =
      reinterpret_cast<HostedReciprocalRemovalContext*>(parameter);
  if (context->registry->invokeCallbackForTest(1)) {
    context->invocationsFinished += 1;
  }
  return 0;
}

bool waitForHostedValue(Atomic<size_t>& value, size_t expected) {
  const Time::Timestamp deadline = Time::getTicks() + (500 * Time::Multiplier::Millisecond);
  while (value != expected && Time::getTicks() < deadline) {
    Scheduler::instance().yield();
  }
  return value == expected;
}
}  // namespace

bool UsbPnP::invokeCallbackForTest(size_t callbackIndex) {
  CallbackItem* item = nullptr;
  callback_t callback = nullptr;
  ActiveInvocation invocation = {nullptr, nullptr};
  {
    LockGuard<Spinlock> guard(m_CallbackLock);
    item = m_FirstCallback;
    while (item && callbackIndex) {
      item = item->next;
      --callbackIndex;
    }
    if (!item || !item->operations.tryEnter()) {
      return false;
    }
    callback = item->callback;
    invocation.owner = currentInvocationOwner();
    invocation.next = m_ActiveInvocations;
    m_ActiveInvocations = &invocation;
  }

  callback(nullptr);
  finishCallback(item, invocation);
  return true;
}

size_t UsbPnP::callbackCountForTest() {
  LockGuard<Spinlock> guard(m_CallbackLock);
  return m_CallbackCount;
}

bool UsbPnP::callbackStorageEmptyForTest() {
  LockGuard<Spinlock> guard(m_CallbackLock);
  return !m_FirstCallback && !m_LastCallback && !m_CallbackCount;
}

bool UsbPnP::runRegistrationRegression() {
  UsbPnP registry;
  Registration registration;
  HostedRegistrationContext context(&registry, &registration);
  g_HostedRegistrationContext = &context;

  const bool registered = registry.registerCallbackItem(
      new CallbackItem(hostedRegistrationCallback, VendorIdNone, ProductIdNone, ClassNone,
                       SubclassNone, ProtocolNone),
      registration, false);

  Thread* invoker = nullptr;
  Thread* unregisterer = nullptr;
  bool callbackEntered = false;
  bool registrationRemoved = false;
  bool unregisterBlocked = false;
  bool lateInvocationRejected = false;

  if (registered) {
    Process* process = Scheduler::instance().getKernelProcess();
    invoker =
        new Thread(process, invokeHostedRegistration, &context, nullptr, false, PinTestThreads);
    invoker->setName("hosted USB PnP callback");
    callbackEntered = waitForHostedValue(context.callbackEntered, 1);

    if (callbackEntered) {
      unregisterer = new Thread(process, unregisterHostedRegistration, &context, nullptr, false,
                                PinTestThreads);
      unregisterer->setName("hosted USB PnP unregister");

      const bool unregisterStarted = waitForHostedValue(context.unregisterStarted, 1);
      const Time::Timestamp deadline = Time::getTicks() + (500 * Time::Multiplier::Millisecond);
      while (unregisterStarted && registry.callbackCountForTest() && Time::getTicks() < deadline) {
        Scheduler::instance().yield();
      }

      registrationRemoved = registry.callbackCountForTest() == 0;
      unregisterBlocked =
          registrationRemoved && context.unregisterFinished == static_cast<size_t>(0);
      lateInvocationRejected = registrationRemoved && !registry.invokeCallbackForTest();
    }
  }

  context.releaseCallback.release();
  const bool invokerJoined = !invoker || invoker->joinForCompletion();
  const bool unregistererJoined = !unregisterer || unregisterer->joinForCompletion();
  if (registration) {
    registration.reset();
  }
  g_HostedRegistrationContext = nullptr;

  const bool drainPassed = registered && callbackEntered && registrationRemoved &&
                           unregisterBlocked && lateInvocationRejected && invokerJoined &&
                           unregistererJoined &&
                           context.callbackFinished == static_cast<size_t>(1) &&
                           context.callbackResetRejected == static_cast<size_t>(1) &&
                           context.invocationFinished == static_cast<size_t>(1) &&
                           context.unregisterFinished == static_cast<size_t>(1);

  Registration selfRegistration;
  HostedSelfRemovalContext selfContext(&selfRegistration);
  g_HostedSelfRemovalContext = &selfContext;
  const bool selfRegistered = registry.registerCallbackItem(
      new CallbackItem(hostedSelfRemovalCallback, VendorIdNone, ProductIdNone, ClassNone,
                       SubclassNone, ProtocolNone),
      selfRegistration, false);
  const bool selfInvoked = selfRegistered && registry.invokeCallbackForTest();
  const bool selfOwnershipPreserved = selfInvoked && selfRegistration &&
                                      registry.callbackCountForTest() == 0 &&
                                      !registry.callbackStorageEmptyForTest() &&
                                      selfContext.callbackEntered == static_cast<size_t>(1) &&
                                      selfContext.resetRejected == static_cast<size_t>(1) &&
                                      selfContext.resetFinished == static_cast<size_t>(1);
  const bool selfRetired = selfRegistration && selfRegistration.reset();
  const bool selfRemovalPassed = selfOwnershipPreserved && selfRetired && !selfRegistration &&
                                 registry.callbackStorageEmptyForTest();
  g_HostedSelfRemovalContext = nullptr;

  Registration first;
  Registration second;
  HostedReciprocalRemovalContext reciprocalContext(&registry, &first, &second);
  g_HostedReciprocalRemovalContext = &reciprocalContext;
  const bool firstRegistered = registry.registerCallbackItem(
      new CallbackItem(hostedFirstReciprocalRemovalCallback, VendorIdNone, ProductIdNone, ClassNone,
                       SubclassNone, ProtocolNone),
      first, false);
  const bool secondRegistered = registry.registerCallbackItem(
      new CallbackItem(hostedSecondReciprocalRemovalCallback, VendorIdNone, ProductIdNone,
                       ClassNone, SubclassNone, ProtocolNone),
      second, false);

  Thread* firstInvoker = nullptr;
  Thread* secondInvoker = nullptr;
  bool bothEntered = false;
  if (firstRegistered && secondRegistered) {
    Process* process = Scheduler::instance().getKernelProcess();
    firstInvoker = new Thread(process, invokeFirstReciprocalRemoval, &reciprocalContext, nullptr,
                              false, PinTestThreads);
    firstInvoker->setName("hosted USB PnP reciprocal callback A");
    secondInvoker = new Thread(process, invokeSecondReciprocalRemoval, &reciprocalContext, nullptr,
                               false, PinTestThreads);
    secondInvoker->setName("hosted USB PnP reciprocal callback B");
    bothEntered = waitForHostedValue(reciprocalContext.callbacksEntered, 2);
  }

  reciprocalContext.beginReset.release(2);
  const bool firstJoined = !firstInvoker || firstInvoker->joinForCompletion();
  const bool secondJoined = !secondInvoker || secondInvoker->joinForCompletion();
  const bool registrationsPreserved = first && second;
  const bool admissionClosed = registry.callbackCountForTest() == 0 &&
                               !registry.callbackStorageEmptyForTest() &&
                               !registry.invokeCallbackForTest();
  const bool firstRetired = first && first.reset();
  const bool secondRetired = second && second.reset();
  const bool reciprocalRemovalPassed =
      firstRegistered && secondRegistered && bothEntered && firstJoined && secondJoined &&
      registrationsPreserved && admissionClosed && firstRetired && secondRetired && !first &&
      !second && registry.callbackStorageEmptyForTest() && !reciprocalContext.failures &&
      reciprocalContext.resetRejections == static_cast<size_t>(2) &&
      reciprocalContext.resetsFinished == static_cast<size_t>(2) &&
      reciprocalContext.invocationsFinished == static_cast<size_t>(2)
#if PEDIGREE_CONCURRENCY_SMOKE_TESTS
      && reciprocalContext.firstProcessor != reciprocalContext.secondProcessor
#endif
      ;
  g_HostedReciprocalRemovalContext = nullptr;

#if PEDIGREE_CONCURRENCY_SMOKE_TESTS
  NOTICE("QEMU-CONCURRENCY-TEST: usb-pnp reciprocal cpus="
         << Dec << static_cast<size_t>(reciprocalContext.firstProcessor) << "/"
         << static_cast<size_t>(reciprocalContext.secondProcessor));
#endif

  const size_t destructionsBefore = g_BoundDriverDestructions;
  const size_t retirementsBefore = g_BoundDriverRetirements;
  Registration bindingRegistration;
  const bool bindingRegistered = registry.registerCallbackItem(
      new CallbackItem(hostedBindingCallback, VendorIdNone, ProductIdNone, ClassNone, SubclassNone,
                       ProtocolNone),
      bindingRegistration, false);
  auto* bindingContainer = new UsbDeviceContainer(new HostedBindingUsbDevice);
  auto* unrelatedDevice = new HostedUnrelatedTreeDevice;
  {
    Device::TreeLockGuard treeGuard;
    bindingContainer->setParent(&BindingTreeAccess::rootDevice());
    BindingTreeAccess::rootDevice().addChild(bindingContainer);
    unrelatedDevice->setParent(&BindingTreeAccess::rootDevice());
    BindingTreeAccess::rootDevice().addChild(unrelatedDevice);
  }
  const bool bindingPublished =
      bindingRegistered && registry.probeDevice(bindingContainer) &&
      bindingContainer->getUsbDevice()->getUsbState() == UsbDevice::HasDriver;
  const size_t unrelatedVisitsBefore = g_UnrelatedTreeVisits;
  const bool bindingRetired = bindingPublished && bindingRegistration.reset();
  const bool genericRestored =
      bindingRetired && !bindingRegistration &&
      bindingContainer->getUsbDevice()->getUsbState() == UsbDevice::HasInterface &&
      g_BoundDriverRetirements == retirementsBefore + 1 &&
      g_BoundDriverDestructions == destructionsBefore + 1 &&
      g_UnrelatedTreeVisits == unrelatedVisitsBefore;
  {
    Device::TreeLockGuard treeGuard;
    bindingContainer->closeProbeAdmission();
    BindingTreeAccess::rootDevice().removeChild(bindingContainer);
    bindingContainer->setParent(nullptr);
    BindingTreeAccess::rootDevice().removeChild(unrelatedDevice);
    unrelatedDevice->setParent(nullptr);
  }
  bindingContainer->waitForProbes();
  delete bindingContainer;
  delete unrelatedDevice;
  const bool bindingRetirementPassed =
      bindingRegistered && bindingPublished && bindingRetired && genericRestored;

  return drainPassed && selfRemovalPassed && reciprocalRemovalPassed && bindingRetirementPassed;
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
bool UsbPnP::runHostedRegistrationRegression() {
  const bool passed = runRegistrationRegression();
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS usb-pnp-registration-drain");
  } else {
    ERROR(
        "HOSTED-WAIT-TEST: FAIL usb-pnp-registration-drain: "
        "callback drain, reciprocal removal, or bound-instance retirement failed");
  }
  return passed;
}
#endif

#if PEDIGREE_CONCURRENCY_SMOKE_TESTS
bool UsbPnP::runQemuRegistrationRegression() {
  const bool passed = runRegistrationRegression();
  if (passed) {
    NOTICE("QEMU-CONCURRENCY-TEST: PASS usb-pnp-reciprocal-unregister-smp");
  } else {
    ERROR(
        "QEMU-CONCURRENCY-TEST: FAIL usb-pnp-reciprocal-unregister-smp: "
        "callback drain, reciprocal removal, bound-instance retirement, or CPU spread failed");
  }
  return passed;
}
#endif

#endif
