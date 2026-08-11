/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/utilities/utility.h"

#include "modules/system/usb/UsbConstants.h"
#include "modules/system/usb/UsbDescriptors.h"
#include "modules/system/usb/UsbDevice.h"
#include "modules/system/usb/UsbHub.h"

namespace {
constexpr size_t MaxTransactions = 2;
constexpr size_t MaxTransfers = 2;
constexpr size_t MaxTraceEvents = 5;

enum class TraceEvent { ControlSetup, ControlStatus, ControlComplete, BulkData, BulkComplete };

struct RecordedTransfer {
  RecordedTransfer() : toggle(false), pid(UsbPidOut), buffer(0), bytes(0) {}

  bool toggle;
  UsbPid pid;
  uintptr_t buffer;
  size_t bytes;
};

struct RecordedTransaction {
  RecordedTransaction() : endpoint(), transfers(), transferCount(0), completed(false) {}

  UsbEndpoint endpoint;
  RecordedTransfer transfers[MaxTransfers];
  size_t transferCount;
  bool completed;
};

class ScriptedEndpointHaltHub final : public UsbHub {
 public:
  explicit ScriptedEndpointHaltHub(ssize_t controlResult)
      : UsbHub(),
        m_Transactions(),
        m_TransactionCount(0),
        m_Trace(),
        m_TraceCount(0),
        m_Setup(0, 0, 0, 0, 0),
        m_ControlResult(controlResult),
        m_Callbacks(0),
        m_Cancellations(0),
        m_Valid(true) {}

  void addTransferToTransaction(uintptr_t transaction, bool toggle, UsbPid pid, uintptr_t buffer,
                                size_t bytes) override {
    RecordedTransaction* recorded = transactionFor(transaction);
    if (!recorded || recorded->transferCount >= MaxTransfers) {
      m_Valid = false;
      return;
    }

    RecordedTransfer& transfer = recorded->transfers[recorded->transferCount++];
    transfer.toggle = toggle;
    transfer.pid = pid;
    transfer.buffer = buffer;
    transfer.bytes = bytes;

    const size_t transactionIndex = transaction - 1;
    if (transactionIndex == 0 && recorded->transferCount == 1) {
      appendTrace(TraceEvent::ControlSetup);
    } else if (transactionIndex == 0 && recorded->transferCount == 2) {
      appendTrace(TraceEvent::ControlStatus);
    } else if (transactionIndex == 1 && recorded->transferCount == 1) {
      appendTrace(TraceEvent::BulkData);
    } else {
      m_Valid = false;
    }
  }

  uintptr_t createTransaction(UsbEndpoint endpoint) override {
    if (m_TransactionCount >= MaxTransactions) {
      m_Valid = false;
      return static_cast<uintptr_t>(-1);
    }

    RecordedTransaction& transaction = m_Transactions[m_TransactionCount];
    transaction.endpoint = endpoint;
    return ++m_TransactionCount;
  }

  bool doAsync(uintptr_t transaction, void (*callback)(uintptr_t, ssize_t),
               uintptr_t parameter) override {
    RecordedTransaction* recorded = transactionFor(transaction);
    if (!recorded || recorded->completed || !callback) {
      m_Valid = false;
      return false;
    }

    const size_t transactionIndex = transaction - 1;
    ssize_t result = 0;
    if (transactionIndex == 0) {
      captureControlTransaction(*recorded);
      result = m_ControlResult;
      appendTrace(TraceEvent::ControlComplete);
    } else if (transactionIndex == 1) {
      if (recorded->transferCount != 1) {
        m_Valid = false;
      } else {
        result = static_cast<ssize_t>(recorded->transfers[0].bytes);
      }
      appendTrace(TraceEvent::BulkComplete);
    } else {
      m_Valid = false;
    }

    recorded->completed = true;
    for (size_t i = 0; i < recorded->transferCount; ++i) {
      recorded->transfers[i].buffer = 0;
    }

    ++m_Callbacks;
    callback(parameter, result);
    return true;
  }

  void cancelAsyncAndDrain(uintptr_t, void (*)(uintptr_t, ssize_t), uintptr_t) override {
    ++m_Cancellations;
    m_Valid = false;
  }

  bool addInterruptInHandler(UsbEndpoint, uintptr_t, uint16_t, void (*)(uintptr_t, ssize_t),
                             UsbInterruptInHandle&, uintptr_t) override {
    m_Valid = false;
    return false;
  }

  bool portReset(uint8_t, bool) override {
    m_Valid = false;
    return false;
  }

  bool matches(uint16_t endpointIndex, uint8_t bulkEndpoint, UsbPid bulkPid,
               bool bulkToggle) const {
    if (!m_Valid || m_TransactionCount != MaxTransactions || m_Callbacks != MaxTransactions ||
        m_Cancellations || m_TraceCount != MaxTraceEvents) {
      return false;
    }

    const TraceEvent expectedTrace[MaxTraceEvents] = {
        TraceEvent::ControlSetup, TraceEvent::ControlStatus, TraceEvent::ControlComplete,
        TraceEvent::BulkData, TraceEvent::BulkComplete};
    for (size_t i = 0; i < MaxTraceEvents; ++i) {
      if (m_Trace[i] != expectedTrace[i]) {
        return false;
      }
    }

    const RecordedTransaction& control = m_Transactions[0];
    const RecordedTransaction& bulk = m_Transactions[1];
    if (!control.completed || control.endpoint.nEndpoint != 0 || control.transferCount != 2 ||
        control.transfers[0].toggle || control.transfers[0].pid != UsbPidSetup ||
        control.transfers[0].bytes != sizeof(UsbDevice::Setup) || !control.transfers[1].toggle ||
        control.transfers[1].pid != UsbPidIn || control.transfers[1].bytes != 0) {
      return false;
    }

    if (m_Setup.nRequestType != UsbRequestRecipient::Endpoint ||
        m_Setup.nRequest != UsbRequest::ClearFeature || m_Setup.nValue != 0 ||
        m_Setup.nIndex != endpointIndex || m_Setup.nLength != 0) {
      return false;
    }

    return bulk.completed && bulk.endpoint.nEndpoint == bulkEndpoint && bulk.transferCount == 1 &&
           bulk.transfers[0].toggle == bulkToggle && bulk.transfers[0].pid == bulkPid &&
           bulk.transfers[0].bytes == 1 && buffersRetired();
  }

 protected:
  bool cancelInterruptInAndDrain(const UsbInterruptInToken&, void (*)(uintptr_t, ssize_t),
                                 uintptr_t, bool) override {
    m_Valid = false;
    return false;
  }

 private:
  RecordedTransaction* transactionFor(uintptr_t transaction) {
    if (!transaction || transaction > m_TransactionCount) {
      return nullptr;
    }
    return &m_Transactions[transaction - 1];
  }

  void appendTrace(TraceEvent event) {
    if (m_TraceCount >= MaxTraceEvents) {
      m_Valid = false;
      return;
    }
    m_Trace[m_TraceCount++] = event;
  }

  void captureControlTransaction(RecordedTransaction& transaction) {
    if (transaction.transferCount != 2 || !transaction.transfers[0].buffer ||
        transaction.transfers[0].bytes != sizeof(UsbDevice::Setup)) {
      m_Valid = false;
      return;
    }
    MemoryCopy(&m_Setup, reinterpret_cast<void*>(transaction.transfers[0].buffer), sizeof(m_Setup));
  }

  bool buffersRetired() const {
    for (size_t i = 0; i < m_TransactionCount; ++i) {
      for (size_t j = 0; j < m_Transactions[i].transferCount; ++j) {
        if (m_Transactions[i].transfers[j].buffer) {
          return false;
        }
      }
    }
    return true;
  }

  RecordedTransaction m_Transactions[MaxTransactions];
  size_t m_TransactionCount;
  TraceEvent m_Trace[MaxTraceEvents];
  size_t m_TraceCount;
  UsbDevice::Setup m_Setup;
  ssize_t m_ControlResult;
  size_t m_Callbacks;
  size_t m_Cancellations;
  bool m_Valid;
};

class EndpointHaltTestDevice final : public UsbDevice {
 public:
  explicit EndpointHaltTestDevice(UsbHub* hub) : UsbDevice(hub, 1, HighSpeed) {}

  bool clearHalt(Endpoint* endpoint) {
    return clearEndpointHalt(endpoint);
  }

  ssize_t bulkIn(Endpoint* endpoint, uintptr_t buffer, size_t bytes) {
    return syncIn(endpoint, buffer, bytes);
  }

  ssize_t bulkOut(Endpoint* endpoint, uintptr_t buffer, size_t bytes) {
    return syncOut(endpoint, buffer, bytes);
  }
};

UsbEndpointDescriptor endpointDescriptor(uint8_t endpoint, bool in) {
  UsbEndpointDescriptor descriptor;
  ByteSet(&descriptor, 0, sizeof(descriptor));
  descriptor.nLength = sizeof(descriptor);
  descriptor.nType = UsbDescriptor::Endpoint;
  descriptor.nEndpoint = endpoint;
  descriptor.bDirection = in;
  descriptor.nTransferType = UsbDevice::Endpoint::Bulk;
  descriptor.nMaxPacketSize = 64;
  return descriptor;
}

bool outEndpointSuccess() {
  ScriptedEndpointHaltHub hub(8);
  EndpointHaltTestDevice device(&hub);
  UsbEndpointDescriptor descriptor = endpointDescriptor(2, false);
  UsbDevice::Endpoint endpoint(&descriptor, HighSpeed);
  endpoint.bDataToggle = true;
  alignas(16) uint8_t buffer[16] = {};

  const bool cleared = device.clearHalt(&endpoint);
  const ssize_t transferred = device.bulkOut(&endpoint, reinterpret_cast<uintptr_t>(buffer), 1);
  const bool passed = cleared && transferred == 1 && hub.matches(0x02, 2, UsbPidOut, false);
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS usb-clear-halt-out-endpoint-data0");
  } else {
    ERROR(
        "HOSTED-WAIT-TEST: FAIL usb-clear-halt-out-endpoint-data0: OUT clear did not use "
        "endpoint address 0x02 and restart bulk traffic at DATA0");
  }
  return passed;
}

bool inEndpointSuccess() {
  ScriptedEndpointHaltHub hub(8);
  EndpointHaltTestDevice device(&hub);
  UsbEndpointDescriptor descriptor = endpointDescriptor(3, true);
  UsbDevice::Endpoint endpoint(&descriptor, HighSpeed);
  endpoint.bDataToggle = true;
  alignas(16) uint8_t buffer[16] = {};

  const bool cleared = device.clearHalt(&endpoint);
  const ssize_t transferred = device.bulkIn(&endpoint, reinterpret_cast<uintptr_t>(buffer), 1);
  const bool passed = cleared && transferred == 1 && hub.matches(0x83, 3, UsbPidIn, false);
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS usb-clear-halt-in-endpoint-data0");
  } else {
    ERROR(
        "HOSTED-WAIT-TEST: FAIL usb-clear-halt-in-endpoint-data0: IN clear did not use endpoint "
        "address 0x83 and restart bulk traffic at DATA0");
  }
  return passed;
}

bool failedClearPreservesToggle() {
  ScriptedEndpointHaltHub hub(-TransactionError);
  EndpointHaltTestDevice device(&hub);
  UsbEndpointDescriptor descriptor = endpointDescriptor(2, false);
  UsbDevice::Endpoint endpoint(&descriptor, HighSpeed);
  endpoint.bDataToggle = true;
  alignas(16) uint8_t buffer[16] = {};

  const bool cleared = device.clearHalt(&endpoint);
  const bool preserved = endpoint.bDataToggle;
  const ssize_t transferred = device.bulkOut(&endpoint, reinterpret_cast<uintptr_t>(buffer), 1);
  const bool passed =
      !cleared && preserved && transferred == 1 && hub.matches(0x02, 2, UsbPidOut, true);
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS usb-clear-halt-failure-preserves-data1");
  } else {
    ERROR(
        "HOSTED-WAIT-TEST: FAIL usb-clear-halt-failure-preserves-data1: failed ClearFeature "
        "changed the endpoint toggle or reordered the next transfer");
  }
  return passed;
}
}  // namespace

EXPORTED_PUBLIC bool runHostedUsbEndpointHaltRegressions() {
  const bool outPassed = outEndpointSuccess();
  const bool inPassed = inEndpointSuccess();
  const bool failurePassed = failedClearPreservesToggle();
  return outPassed && inPassed && failurePassed;
}
