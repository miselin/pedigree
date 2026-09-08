/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#ifndef XHCI_H
#define XHCI_H
#include "pedigree/kernel/machine/IrqHandler.h"
#include "pedigree/kernel/process/OperationBarrier.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/utilities/RequestQueue.h"

#include "CallbackDelivery.h"
#include "PortChangeRequest.h"
#include "TransferCompletion.h"
#include "XhciRing.h"
#include "modules/system/usb/UsbHub.h"
class IoBase;
class Thread;
class Xhci : public UsbHub, public IrqHandler, public RequestQueue {
 public:
  explicit Xhci(Device* device);
  ~Xhci() override;
  bool initialiseController();
  void getName(String& name) override {
    name.assign("xHCI");
  }
  bool controllerAssignsAddresses() const override {
    return true;
  }
  bool supportsHubDevices() const override {
    return false;
  }
  bool prepareDevice(uint8_t address, const UsbEndpoint& endpoint) override;
  bool addressDevice(uint8_t address, const UsbEndpoint& endpoint) override;
  bool resetEndpoint(const UsbEndpoint& endpoint) override;
  uintptr_t createTransaction(UsbEndpoint endpoint) override;
  void addTransferToTransaction(uintptr_t transaction, bool toggle, UsbPid pid, uintptr_t buffer,
                                size_t bytes) override;
  bool doAsync(uintptr_t transaction, void (*callback)(uintptr_t, ssize_t) = nullptr,
               uintptr_t parameter = 0) override;
  void cancelAsyncAndDrain(uintptr_t transaction, void (*callback)(uintptr_t, ssize_t),
                           uintptr_t parameter) override;
  bool addInterruptInHandler(UsbEndpoint endpoint, uintptr_t buffer, uint16_t bytes,
                             void (*callback)(uintptr_t, ssize_t), UsbInterruptInHandle& handle,
                             uintptr_t parameter = 0) override;
  bool portReset(uint8_t port, bool errorResponse = false) override;
  IrqDisposition irq(irq_id_t) override;

 protected:
  void releaseDeviceAddress(uint8_t address) override;
  bool cancelInterruptInAndDrain(const UsbInterruptInToken&, void (*)(uintptr_t, ssize_t),
                                 uintptr_t, bool producerAlreadyStopped) override;
  bool inInterruptCallbackContext() const override {
    return UsbHcd::CallbackDeliveryQueue::isInCallbackContext();
  }
  size_t currentRootPortGeneration(size_t port) const override;
  void replaySuppressedConnectionChange(size_t port) override;
  uint64_t executeRequest(uint64_t port, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                          uint64_t generation) override;
  void cancelRequest(const Request& request) override;

 private:
  struct Transaction;
  struct Endpoint {
    XhciHw::Ring ring;
    Transaction* active = nullptr;
    UsbEndpoint description;
    Atomic<bool> needsReset{false};
    uint64_t retiredAddresses[20]{};
    size_t retiredCount = 0;
  };
  struct Slot {
    Slot() : output("xHCI device context") {}
    MemoryRegion output;
    Endpoint* endpoints[32]{};
    uint8_t logical = 0;
    uint8_t port = 0;
    uint8_t speedId = 0;
    uint8_t contextEntries = 1;
    size_t generation = 0;
  };
  struct Transaction {
    Transaction() : bounce("xHCI transfer") {}
    MemoryRegion bounce;
    physical_uintptr_t pages[16]{};
    UsbEndpoint description;
    UsbHcd::TransferCompletion completion;
    Endpoint* endpoint = nullptr;
    uint8_t slot = 0, dci = 0;
    uintptr_t id = 0, client = 0;
    size_t bytes = 0, generation = 0;
    uint64_t setup = 0;
    bool input = false, setupPresent = false, statusPresent = false, bad = false;
    bool submitted = false, cancelling = false, periodic = false, captured = false;
    bool shortSeen = false;
    size_t actual = 0, trbCount = 0;
    uint64_t trbAddresses[20]{};
    size_t trbOffsets[20]{}, trbLengths[20]{};
    uint8_t trbTypes[20]{};
    void (*callback)(uintptr_t, ssize_t) = nullptr;
    uintptr_t parameter = 0;
  };
  struct Delivery {
    Xhci* controller;
    uintptr_t transaction;
    size_t generation;
    bool rearm;
  };
  struct Port {
    uint8_t major = 0, slotType = 0;
    UsbSpeed speeds[16]{};
    bool validSpeed[16]{};
    Atomic<bool> changePending{false};
    uint32_t resetChanges = 0;
  };
  uint32_t read(size_t offset) const;
  void write(size_t offset, uint32_t value);
  void write64(size_t offset, uint64_t value);
  bool wait(size_t offset, uint32_t mask, uint32_t expected, size_t milliseconds);
  bool parseCapabilities(uint32_t hcc);
  bool command(XhciHw::Trb trb, uint8_t* returnedSlot = nullptr);
  bool collectEventsLocked(bool fromInterrupt);
  bool transferEventLocked(const XhciHw::Trb& event);
  void notifyPortLocked(size_t port);
  bool halt();
  void failLocked();
  void shutdown();
  Endpoint* ensureEndpoint(uint8_t slot, const UsbEndpoint& description);
  uint8_t findSlot(const UsbEndpoint& endpoint) const;
  uint32_t* inputContext(size_t index);
  uint32_t* outputContext(uint8_t slot, size_t index);
  bool resetEndpointHardware(uint8_t slot, uint8_t dci, bool stop);
  bool submitLocked(Transaction& transaction);
  static bool decodeCompletion(Transaction&, size_t index, uint8_t code, size_t residual,
                               bool& complete, size_t& actual);
  static bool completionRegressions();
  bool accept(uintptr_t transaction, void (*callback)(uintptr_t, ssize_t), uintptr_t parameter,
              UsbInterruptInHandle* handle);
  void finishLocked(Transaction& transaction, ssize_t result, bool natural);
  void enqueueDeliveryLocked(UsbHcd::CallbackDeliveryQueue::Record* delivery);
  static void afterDelivery(void* context);
  static void destroyDelivery(void* context);
  static int deliveryWorker(void* context);
  void freeTransactionLocked(uintptr_t transaction);
  void cancelTransfer(uintptr_t transaction, size_t generation, bool recurring,
                      void (*callback)(uintptr_t, ssize_t), uintptr_t parameter);
  Device* m_Pci;
  IoBase* m_Registers = nullptr;
  size_t m_Op = 0, m_Runtime = 0, m_Doorbells = 0, m_ContextSize = 32;
  size_t m_PortCount = 0, m_SlotCount = 0;
  irq_id_t m_Irq = 0;
  bool m_HardwareOwned = false;
  Atomic<bool> m_Online{false}, m_Stopping{false}, m_Failed{false}, m_PortsClosing{false};
  bool m_Shutdown = false, m_DeliveryStopping = false, m_PolledInterrupt = false;
  bool m_ObservedInterruptCompletion = false;
  Mutex m_Lock, m_CommandLock, m_ConfigurationLock, m_CancelLock;
  OperationBarrier m_Submissions, m_Cancellations, m_Transfers;
  XhciHw::Ring m_Commands;
  MemoryRegion m_Events, m_Erst, m_Dcbaa, m_Input, m_ScratchPointers;
  MemoryRegion* m_Scratch[32]{};
  size_t m_EventHead = 0, m_EventWraps = 0;
  bool m_EventCycle = true;
  uint64_t m_CommandAddress = 0;
  uint8_t m_CommandCode = 0, m_CommandSlot = 0;
  Semaphore m_CommandDone{0, false};
  Slot m_Slots[XhciHw::MaxSlots + 1];
  uint8_t m_Addresses[128]{}, m_PortSlots[XhciHw::MaxPorts]{};
  Port m_Ports[XhciHw::MaxPorts];
  UsbHcd::PortChangeRequest m_PortChanges[XhciHw::MaxPorts];
  Transaction* m_Transactions[XhciHw::MaxTransactions]{};
  UsbHcd::CallbackDeliveryQueue m_Deliveries;
  List<UsbHcd::CallbackDeliveryQueue::Record*> m_ReadyDeliveries;
  Semaphore m_DeliveryReady{0, false};
  Thread* m_DeliveryThread = nullptr;
};
#endif
