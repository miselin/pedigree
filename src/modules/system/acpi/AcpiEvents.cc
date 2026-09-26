/* Copyright (c) 2026, Pedigree Developers. */

#include "AcpiEvents.h"
#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/IrqHandler.h"
#include "pedigree/kernel/machine/IrqManager.h"
#include "pedigree/kernel/machine/Machine.h"
#include "pedigree/kernel/process/ConditionVariable.h"
#include "pedigree/kernel/process/CpuAffinity.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/utilities/RequestQueue.h"
#include "pedigree/kernel/utilities/new"

#include <uacpi/kernel_api.h>

namespace {
static_assert(sizeof(uacpi_work_handler) <= sizeof(uint64_t));
static_assert(sizeof(uacpi_handle) <= sizeof(uint64_t));

class AcpiWorkQueue : public RequestQueue {
 public:
  AcpiWorkQueue() : RequestQueue(MakeConstantString("uACPI events")) {}

  ~AcpiWorkQueue() override {
    destroy();
  }

 protected:
  bool workerPlacement(ThreadPlacement& placement) const override {
    // GPE AML may enter firmware through SMI paths that assume the boot CPU.
    placement.allowed.set(0);
    return true;
  }

  uint64_t executeRequest(uint64_t p1, uint64_t p2, uint64_t, uint64_t, uint64_t, uint64_t,
                          uint64_t, uint64_t) override {
    auto handler = reinterpret_cast<uacpi_work_handler>(static_cast<uintptr_t>(p1));
    auto context = reinterpret_cast<uacpi_handle>(static_cast<uintptr_t>(p2));
    handler(context);
    return 0;
  }
};

class AcpiIrqHandler;

struct AcpiEventsState {
  Mutex mutex;
  ConditionVariable callbackDone;
  AcpiIrqHandler* handlers = nullptr;
  size_t activeCallbacks = 0;
  Atomic<size_t> ready{0};
  AcpiWorkQueue work;
};

AcpiEventsState g_Events;

class AcpiIrqHandler : public IrqHandler {
 public:
  AcpiIrqHandler(uacpi_interrupt_handler callback, uacpi_handle context)
      : callback(callback), context(context) {}

  ~AcpiIrqHandler() override = default;

  IrqDisposition irq(irq_id_t) override {
    {
      LockGuard<Mutex> guard(g_Events.mutex);
      ++g_Events.activeCallbacks;
    }

    const uacpi_interrupt_ret result = callback(context);

    {
      LockGuard<Mutex> guard(g_Events.mutex);
      --g_Events.activeCallbacks;
      if (!g_Events.activeCallbacks) {
        g_Events.callbackDone.broadcast();
      }
    }

    return result & UACPI_INTERRUPT_HANDLED ? IrqDisposition::Handled : IrqDisposition::NotHandled;
  }

  uacpi_interrupt_handler callback;
  uacpi_handle context;
  irq_id_t id = 0;
  AcpiIrqHandler* next = nullptr;
  bool removing = false;
};
}  // namespace

bool initialiseAcpiEvents() {
  if (g_Events.ready.value()) {
    return true;
  }

  g_Events.work.initialise();
  if (g_Events.work.getLifecycleState() != RequestQueue::LifecycleState::Accepting) {
    return false;
  }

  g_Events.ready = 1;
  return true;
}

extern "C" uacpi_status uacpi_kernel_install_interrupt_handler(uacpi_u32 irq,
                                                               uacpi_interrupt_handler callback,
                                                               uacpi_handle context,
                                                               uacpi_handle* out_irq_handle) {
  if (!callback || !out_irq_handle) {
    return UACPI_STATUS_INVALID_ARGUMENT;
  }
  *out_irq_handle = nullptr;

  // The current IRQ manager exposes ISA lines, not arbitrary GSI routing.
  if (irq > 15) {
    ERROR("uACPI: unsupported interrupt GSI " << Dec << irq);
    return UACPI_STATUS_UNIMPLEMENTED;
  }

  if (!g_Events.ready.value()) {
    return UACPI_STATUS_INIT_LEVEL_MISMATCH;
  }

  IrqManager* manager = Machine::instance().getIrqManager();
  if (!manager) {
    return UACPI_STATUS_UNIMPLEMENTED;
  }

  auto* handler = new AcpiIrqHandler(callback, context);
  if (!handler) {
    return UACPI_STATUS_OUT_OF_MEMORY;
  }

  {
    LockGuard<Mutex> guard(g_Events.mutex);
    handler->next = g_Events.handlers;
    g_Events.handlers = handler;
  }

  handler->id = manager->registerIsaIrqHandler(static_cast<uint8_t>(irq), handler,
                                               IrqPolicy::levelThreaded());
  if (!handler->id) {
    LockGuard<Mutex> guard(g_Events.mutex);
    AcpiIrqHandler** link = &g_Events.handlers;
    while (*link != handler) {
      link = &(*link)->next;
    }
    *link = handler->next;
    delete handler;
    return UACPI_STATUS_INTERNAL_ERROR;
  }

  *out_irq_handle = handler;
  return UACPI_STATUS_OK;
}

extern "C" uacpi_status uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler callback,
                                                                 uacpi_handle irq_handle) {
  AcpiIrqHandler* handler = nullptr;
  {
    LockGuard<Mutex> guard(g_Events.mutex);
    for (AcpiIrqHandler* current = g_Events.handlers; current; current = current->next) {
      if (current == irq_handle) {
        if (current->callback != callback || current->removing || !current->id) {
          return UACPI_STATUS_INVALID_ARGUMENT;
        }
        current->removing = true;
        handler = current;
        break;
      }
    }
  }
  if (!handler) {
    return UACPI_STATUS_INVALID_ARGUMENT;
  }

  IrqManager* manager = Machine::instance().getIrqManager();
  if (!manager || !manager->unregisterHandler(handler->id, handler)) {
    LockGuard<Mutex> guard(g_Events.mutex);
    handler->removing = false;
    return UACPI_STATUS_INTERNAL_ERROR;
  }

  {
    LockGuard<Mutex> guard(g_Events.mutex);
    AcpiIrqHandler** link = &g_Events.handlers;
    while (*link != handler) {
      link = &(*link)->next;
    }
    *link = handler->next;
  }
  delete handler;
  return UACPI_STATUS_OK;
}

extern "C" uacpi_status uacpi_kernel_schedule_work(uacpi_work_type type, uacpi_work_handler handler,
                                                   uacpi_handle context) {
  if ((type != UACPI_WORK_GPE_EXECUTION && type != UACPI_WORK_NOTIFICATION) || !handler) {
    return UACPI_STATUS_INVALID_ARGUMENT;
  }
  if (!g_Events.ready.value()) {
    return UACPI_STATUS_INIT_LEVEL_MISMATCH;
  }

  // The same worker preserves the notification-before-GPE-restore order.
  return g_Events.work.publishAsyncRequest(0, reinterpret_cast<uintptr_t>(handler),
                                           reinterpret_cast<uintptr_t>(context))
             ? UACPI_STATUS_OK
             : UACPI_STATUS_INTERNAL_ERROR;
}

extern "C" uacpi_status uacpi_kernel_wait_for_work_completion() {
  if (!g_Events.ready.value()) {
    return UACPI_STATUS_INIT_LEVEL_MISMATCH;
  }
  if (!g_Events.work.canWaitForCompletion()) {
    return UACPI_STATUS_INVALID_ARGUMENT;
  }

  {
    LockGuard<Mutex> guard(g_Events.mutex);
    while (g_Events.activeCallbacks) {
      g_Events.callbackDone.waitForCompletion(g_Events.mutex);
    }
  }

  return g_Events.work.drain() ? UACPI_STATUS_OK : UACPI_STATUS_INTERNAL_ERROR;
}
