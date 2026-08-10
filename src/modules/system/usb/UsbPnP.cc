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
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/utilities/utility.h"

#include "modules/system/usb/UsbDevice.h"

#if (HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS) || PEDIGREE_CONCURRENCY_SMOKE_TESTS
#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/process/Scheduler.h"
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
        operations() {}

  callback_t callback;
  uint16_t nVendorId;
  uint16_t nProductId;
  uint8_t nClass;
  uint8_t nSubclass;
  uint8_t nProtocol;
  size_t sequence;
  CallbackItem* next;
  OperationBarrier operations;
};

struct UsbPnP::ActiveInvocation {
  void* owner;
  ActiveInvocation* next;
};

UsbPnP::Registration::Registration() : m_Owner(nullptr), m_Item(nullptr) {}

UsbPnP::Registration::Registration(Registration&& other)
    : m_Owner(other.m_Owner), m_Item(other.m_Item) {
  other.m_Owner = nullptr;
  other.m_Item = nullptr;
}

UsbPnP::Registration::~Registration() {
  if (m_Item && !reset()) {
    FATAL("Live UsbPnP registration could not be retired.");
  }
}

UsbPnP::Registration& UsbPnP::Registration::operator=(Registration&& other) {
  if (this != &other) {
    if (m_Item && !reset()) {
      FATAL("UsbPnP registration move could not retire ownership.");
    }
    m_Owner = other.m_Owner;
    m_Item = other.m_Item;
    other.m_Owner = nullptr;
    other.m_Item = nullptr;
  }
  return *this;
}

bool UsbPnP::Registration::reset() {
  if (!m_Item) {
    return true;
  }

  if (!m_Owner->unregisterCallback(m_Item)) {
    return false;
  }

  m_Owner = nullptr;
  m_Item = nullptr;
  return true;
}

void UsbPnP::Registration::adopt(UsbPnP* owner, CallbackItem* item) {
  m_Owner = owner;
  m_Item = item;
}

UsbPnP::UsbPnP()
    : m_FirstCallback(nullptr),
      m_LastCallback(nullptr),
      m_CallbackCount(0),
      m_CallbackLock(),
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
    delete item;
  }
}

bool UsbPnP::probeDevice(Device* pDeviceBase) {
  Device* pResult = doProbe(pDeviceBase);
  return pResult == pDeviceBase;
}

Device* UsbPnP::doProbe(Device* pDeviceBase) {
  // Sanity check.
  if (!(pDeviceBase->getType() == Device::UsbContainer)) {
    return pDeviceBase;
  }

  UsbDevice* pDevice = static_cast<UsbDeviceContainer*>(pDeviceBase)->getUsbDevice();

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

    // Initialise the driver
    pNewDevice->initialiseDriver();

    // Did the device go into the driver state?
    if (pNewDevice->getUsbState() == UsbDevice::HasDriver) {
      // Replace the old device with the new one
      UsbDeviceContainer* pNewContainer = new UsbDeviceContainer(pNewDevice);
      finishCallback(item, invocation);
      return pNewContainer;
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

void UsbPnP::reprobeDevices(Device* pParent) {
  auto performReprobe = [](Device* p) {
    if (p->getType() == Device::UsbContainer) {
      return UsbPnP::instance().doProbe(p);
    }

    // don't edit the tree - just iterating
    return p;
  };

  auto c = pedigree_std::make_callable(performReprobe);
  Device::foreach (c, pParent);
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

bool UsbPnP::unregisterCallback(CallbackItem* item) {
  bool found = false;
  bool callbackContext = false;
  bool drained = false;
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
      drained = item->operations.isClosedAndDrained();
      if (!callbackContext || drained) {
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
  if (callbackContext && !drained) {
    return false;
  }
  if (!callbackContext) {
    item->operations.wait();
  }
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
        invocationFinished(0),
        unregisterStarted(0),
        unregisterFinished(0) {}

  UsbPnP* registry;
  UsbPnP::Registration* registration;
  Semaphore releaseCallback;
  Atomic<size_t> callbackEntered;
  Atomic<size_t> callbackFinished;
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

UsbDevice* hostedRegistrationCallback(UsbDevice*) {
  HostedRegistrationContext* context = g_HostedRegistrationContext;
  context->callbackEntered += 1;
  const bool released = context->releaseCallback.acquireForCompletion();
  (void)released;
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

  return drainPassed && selfRemovalPassed && reciprocalRemovalPassed;
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
bool UsbPnP::runHostedRegistrationRegression() {
  const bool passed = runRegistrationRegression();
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS usb-pnp-registration-drain");
  } else {
    ERROR(
        "HOSTED-WAIT-TEST: FAIL usb-pnp-registration-drain: "
        "external drain, self-removal, or reciprocal removal failed");
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
        "external drain, self-removal, reciprocal removal, or CPU spread failed");
  }
  return passed;
}
#endif

#endif
