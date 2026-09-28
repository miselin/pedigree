/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Scheduler.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/new"

#include "modules/system/lwip/include/lwip/err.h"
#include "modules/system/lwip/include/lwip/netif.h"
#include "modules/system/lwip/include/lwip/pbuf.h"
#include "modules/system/lwip/include/lwip/sys.h"
#include "modules/system/lwip/include/lwip/tcpip.h"
#include "modules/system/network-stack/NetworkStack.h"
#include "system/kernel/core/processor/DeviceHardIrqContext.h"

namespace {
class HostedNetworkDevice final : public Network {
 public:
  bool send(size_t, uintptr_t) override {
    return true;
  }

  const StationInfo& getStationInfo() override {
    return m_StationInfo;
  }
};

bool check(bool condition, const char* test, const char* detail) {
  if (condition) {
    return true;
  }

  ERROR("HOSTED-NETWORK-TEST: FAIL " << test << ": " << detail);
  return false;
}

bool isRegistered(NetworkStack& stack, Network* device) {
  NetworkStack::DeviceLease lease;
  return stack.acquireDevice(device, lease);
}

bool waitUntilQueued(Thread* thread, size_t debugState) {
  const Time::Timestamp deadline = Time::getTicks() + (500 * Time::Multiplier::Millisecond);
  while (Time::getTicks() < deadline) {
    Thread::WaitDebugInfo info = {};
    uintptr_t debugAddress = 0;
    if (thread->getWaitDebugInfo(info) && info.queue && info.queued &&
        thread->getDebugState(debugAddress) == debugState) {
      return true;
    }
    Scheduler::instance().yield();
  }
  return false;
}

struct DeviceDeregisterContext {
  DeviceDeregisterContext(NetworkStack* stack, Network* device)
      : stack(stack), device(device), entered(0), returned(0) {}

  NetworkStack* stack;
  Network* device;
  Atomic<size_t> entered;
  Atomic<size_t> returned;
};

int deregisterLeasedDevice(void* parameter) {
  DeviceDeregisterContext* context = reinterpret_cast<DeviceDeregisterContext*>(parameter);
  context->entered += 1;
  context->stack->deRegisterDevice(context->device);
  context->returned += 1;
  return 0;
}

bool deviceLeaseDeregisterDrain() {
  static const char* Test = "device-lease-deregister-drain";

  NetworkStack& stack = NetworkStack::instance();
  alignas(HostedNetworkDevice) uint8_t deviceStorage[sizeof(HostedNetworkDevice)];
  HostedNetworkDevice* original = new (deviceStorage) HostedNetworkDevice();
  stack.registerDevice(original);

  NetworkStack::DeviceLease held;
  const bool acquired = stack.acquireDevice(original, held);
  struct netif* originalInterface = acquired ? held.interface() : nullptr;
  DeviceDeregisterContext context(&stack, original);
  Thread* remover = nullptr;
  bool drainPublished = false;
  bool lateRejected = false;
  bool usableWhileDraining = false;

  if (acquired) {
    remover = new Thread(Scheduler::instance().getKernelProcess(), deregisterLeasedDevice, &context,
                         nullptr, false, true);
    remover->setName("hosted network device deregister");
    drainPublished = waitUntilQueued(remover, Thread::CallbackDrain);

    NetworkStack::DeviceLease late;
    lateRejected = !stack.acquireDevice(original, late);

    String name;
    held.device()->getName(name);
    const StationInfo& info = held.device()->getStationInfo();
    usableWhileDraining = held.device() == original && held.interface() == originalInterface &&
                          originalInterface && originalInterface->state == original &&
                          info.nPackets == 0;
    held = NetworkStack::DeviceLease();
  }

  const bool removerJoined = remover && remover->join();
  const bool unregistered = !isRegistered(stack, original);
  if (!remover) {
    stack.deRegisterDevice(original);
  }
  original->~HostedNetworkDevice();

  HostedNetworkDevice* replacement = new (deviceStorage) HostedNetworkDevice();
  stack.registerDevice(replacement);
  NetworkStack::DeviceLease replacementLease;
  const bool replacementRegistered = stack.acquireDevice(replacement, replacementLease);
  replacementLease = NetworkStack::DeviceLease();
  stack.deRegisterDevice(replacement);
  replacement->~HostedNetworkDevice();

  const bool passed = check(
      acquired && context.entered == 1 && drainPublished && context.returned == 1 && lateRejected &&
          usableWhileDraining && removerJoined && unregistered && replacementRegistered,
      Test, "deregistration did not unpublish, drain, and retire one held registration");
  if (passed) {
    NOTICE("HOSTED-NETWORK-TEST: PASS " << Test);
  }
  return passed;
}

bool mailboxTryPostRejectsHardIrq() {
  static const char* Test = "mailbox-hard-irq-trypost";

  sys_mbox_t mailbox = nullptr;
  if (sys_mbox_new(&mailbox, 1) != ERR_OK) {
    return check(false, Test, "mailbox creation failed");
  }

  int payload = 0;
  err_t result = ERR_OK;
  size_t previousDepth = 0;
  bool restorationArmed = false;
  {
    DeviceHardIrqContext hardIrq(previousDepth, restorationArmed);
    result = sys_mbox_trypost(&mailbox, &payload);
  }

  void* fetched = nullptr;
  const u32_t fetchResult = sys_arch_mbox_tryfetch(&mailbox, &fetched);
  sys_mbox_free(&mailbox);

  const bool passed =
      check(result == ERR_WOULDBLOCK && fetchResult == SYS_MBOX_EMPTY && !fetched, Test,
            "the ISR call enqueued data or entered a blocking notification path");
  if (passed) {
    NOTICE("HOSTED-NETWORK-TEST: PASS " << Test);
  }
  return passed;
}

struct ReceiveContext {
  ReceiveContext(NetworkStack* stack, Network* device, struct netif* interface)
      : stack(stack),
        device(device),
        interface(interface),
        removal(stack, device),
        inputEntered(0),
        allowInput(0),
        coreEntered(0),
        allowCore(0),
        admitInput(0),
        inputs(0),
        enqueued(0),
        delivered(0),
        failures(0) {}

  NetworkStack* stack;
  Network* device;
  struct netif* interface;
  DeviceDeregisterContext removal;
  Semaphore inputEntered;
  Semaphore allowInput;
  Semaphore coreEntered;
  Semaphore allowCore;
  Atomic<size_t> admitInput;
  Atomic<size_t> inputs;
  Atomic<size_t> enqueued;
  Atomic<size_t> delivered;
  Atomic<size_t> failures;
};

ReceiveContext* g_ReceiveContext = nullptr;

void holdTcpipWorker(void* parameter) {
  ReceiveContext* context = reinterpret_cast<ReceiveContext*>(parameter);
  context->coreEntered.release();
  if (!context->allowCore.acquireForCompletion()) {
    context->failures += 1;
  }
}

err_t receiveQueuedPacket(struct pbuf* packet, struct netif* interface) {
  ReceiveContext* context = g_ReceiveContext;
  if (context->removal.returned || interface != context->interface ||
      interface->state != context->device || packet->tot_len != 64 ||
      pbuf_get_at(packet, 0) != 0x5a || pbuf_get_at(packet, 63) != 0xa5) {
    context->failures += 1;
  }
  pbuf_free(packet);
  context->delivered += 1;
  return ERR_OK;
}

err_t holdReceiveInput(struct pbuf* packet, struct netif* interface) {
  ReceiveContext* context = g_ReceiveContext;
  if ((context->inputs += 1) != 1) {
    context->failures += 1;
    pbuf_free(packet);
    return ERR_OK;
  }

  context->inputEntered.release();
  if (!context->allowInput.acquireForCompletion()) {
    context->failures += 1;
  }
  if (!context->admitInput) {
    pbuf_free(packet);
    return ERR_OK;
  }

  const err_t result = tcpip_inpkt(packet, interface, receiveQueuedPacket);
  if (result == ERR_OK) {
    context->enqueued += 1;
  } else {
    context->failures += 1;
  }
  return result;
}

int receivePacket(void* parameter) {
  ReceiveContext* context = reinterpret_cast<ReceiveContext*>(parameter);
  uint8_t packet[68] = {};
  packet[4] = 0x5a;
  packet[67] = 0xa5;
  context->stack->receive(64, reinterpret_cast<uintptr_t>(packet), context->device, 4);
  return 0;
}

bool receiveInterfaceRetirement() {
  static const char* Test = "receive-interface-retirement";

  NetworkStack& stack = NetworkStack::instance();
  HostedNetworkDevice device;
  stack.registerDevice(&device);
  NetworkStack::DeviceLease setup;
  if (!stack.acquireDevice(&device, setup)) {
    stack.deRegisterDevice(&device);
    return check(false, Test, "could not acquire the registered interface");
  }

  ReceiveContext context(&stack, &device, setup.interface());
  setup.interface()->input = holdReceiveInput;
  setup = NetworkStack::DeviceLease();
  g_ReceiveContext = &context;

  const bool corePosted = tcpip_callback(holdTcpipWorker, &context) == ERR_OK;
  const bool coreHeld = corePosted && context.coreEntered.acquire(1, 2);
  Thread* receiver = nullptr;
  Thread* remover = nullptr;
  bool inputHeld = false;
  bool leaseDraining = false;
  bool lateRejected = false;
  bool receiverJoined = false;
  bool queueDraining = false;

  if (coreHeld) {
    receiver = new Thread(Scheduler::instance().getKernelProcess(), receivePacket, &context,
                          nullptr, false, true);
    receiver->setName("hosted network receive");
    inputHeld = context.inputEntered.acquire(1, 2);
    if (inputHeld) {
      remover = new Thread(Scheduler::instance().getKernelProcess(), deregisterLeasedDevice,
                           &context.removal, nullptr, false, true);
      remover->setName("hosted network receive retirement");
      leaseDraining = waitUntilQueued(remover, Thread::CallbackDrain);
      if (leaseDraining) {
        uint8_t latePacket[64] = {};
        stack.receive(sizeof(latePacket), reinterpret_cast<uintptr_t>(latePacket), &device, 0);
        lateRejected = !isRegistered(stack, &device) && context.inputs == 1;
        context.admitInput = 1;
      }
    }
  }

  // Release a late receiver as well, but only enqueue when retirement is
  // demonstrably waiting for its lease; a failed assertion must not use a
  // possibly retired interface.
  context.allowInput.release();
  receiverJoined = receiver && receiver->join();
  if (remover && leaseDraining) {
    queueDraining = waitUntilQueued(remover, Thread::SemWait) && !context.removal.returned &&
                    !context.delivered && context.enqueued == 1;
  }

  context.allowCore.release();
  const bool removerJoined = remover && remover->join();
  if (!remover) {
    stack.deRegisterDevice(&device);
  }
  g_ReceiveContext = nullptr;

  bool passed = true;
  passed &= check(coreHeld && inputHeld && leaseDraining && lateRejected && receiverJoined, Test,
                  "receive did not pin its interface through input admission");
  passed &= check(queueDraining && removerJoined && context.removal.returned == 1 &&
                      context.delivered == 1 && !context.failures && !isRegistered(stack, &device),
                  Test, "queued input did not finish before interface retirement");
  if (passed) {
    NOTICE("HOSTED-NETWORK-TEST: PASS " << Test);
  }
  return passed;
}
}  // namespace

bool runHostedNetworkStackRegressions() {
  bool passed = mailboxTryPostRejectsHardIrq();
  passed &= deviceLeaseDeregisterDrain();
  passed &= receiveInterfaceRetirement();
  return passed;
}
