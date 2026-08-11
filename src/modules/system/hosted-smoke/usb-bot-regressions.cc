/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/utilities/utility.h"

#include "modules/drivers/common/usb-mass-storage/UsbMassStorageDevice.h"
#include "modules/system/usb/UsbConstants.h"
#include "modules/system/usb/UsbDescriptors.h"
#include "modules/system/usb/UsbHub.h"

class UsbMassStorageBotTestAccess {
 public:
  static void bind(UsbMassStorageDevice& device, UsbDevice::Endpoint* in,
                   UsbDevice::Endpoint* out) {
    device.m_nUnits = 4;
    device.m_pInEndpoint = in;
    device.m_pOutEndpoint = out;
  }

  static void setUnits(UsbMassStorageDevice& device, size_t units) {
    device.m_nUnits = units;
  }
};

namespace {
constexpr size_t DataBytes = 512;
constexpr size_t MaxTransactions = 64;
constexpr size_t MaxTransfers = 8;
constexpr size_t MaxCbws = 16;
constexpr int AnyToggle = -1;
constexpr uint32_t CbwSignature = HOST_TO_LITTLE32(0x43425355);
constexpr uint32_t CswSignature = HOST_TO_LITTLE32(0x53425355);

constexpr uint8_t WriteCdb[10] = {0x2a, 0, 0, 0, 0, 8, 0, 0, 1, 0};

struct WireCbw {
  uint32_t signature;
  uint32_t tag;
  uint32_t dataBytes;
  uint8_t flags;
  uint8_t lun;
  uint8_t commandSize;
  uint8_t command[16];
} PACKED;

struct WireCsw {
  uint32_t signature;
  uint32_t tag;
  uint32_t residue;
  uint8_t status;
} PACKED;

static_assert(sizeof(WireCbw) == 31, "BOT CBW wire size changed");
static_assert(sizeof(WireCsw) == 13, "BOT CSW wire size changed");

enum class StepKind { Cbw, DataOut, Csw, Reset, ClearIn, ClearOut };
enum class TagReply { Match, Wrong };

struct Step {
  Step()
      : kind(StepKind::Cbw),
        result(0),
        firstToggle(AnyToggle),
        expectedData(nullptr),
        expectedBytes(0),
        lun(0),
        commandSize(0),
        command(),
        signature(CswSignature),
        tagReply(TagReply::Match),
        residue(0),
        status(0) {}

  StepKind kind;
  ssize_t result;
  int firstToggle;
  const uint8_t* expectedData;
  size_t expectedBytes;
  uint8_t lun;
  uint8_t commandSize;
  uint8_t command[16];
  uint32_t signature;
  TagReply tagReply;
  uint32_t residue;
  uint8_t status;
};

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

class ScriptedBotHub final : public UsbHub {
 public:
  ScriptedBotHub()
      : UsbHub(),
        m_Steps(),
        m_StepCount(0),
        m_Transactions(),
        m_TransactionCount(0),
        m_Cbws(),
        m_CbwCount(0),
        m_LastTag(0),
        m_Callbacks(0),
        m_Cancellations(0),
        m_Valid(true) {}

  void expectCbw(uint8_t lun, const uint8_t* command, uint8_t commandSize, size_t dataBytes,
                 int firstToggle, ssize_t result = 31) {
    Step* step = append(StepKind::Cbw, result, firstToggle);
    if (!step)
      return;
    step->lun = lun;
    step->commandSize = commandSize;
    step->expectedBytes = dataBytes;
    MemoryCopy(step->command, command, commandSize);
  }

  void expectDataOut(const uint8_t* data, size_t bytes, int firstToggle, ssize_t result) {
    Step* step = append(StepKind::DataOut, result, firstToggle);
    if (!step)
      return;
    step->expectedData = data;
    step->expectedBytes = bytes;
  }

  void expectCsw(ssize_t result, uint8_t status = 0, uint32_t residue = 0,
                 TagReply tagReply = TagReply::Match, uint32_t signature = CswSignature,
                 int firstToggle = AnyToggle) {
    Step* step = append(StepKind::Csw, result, firstToggle);
    if (!step)
      return;
    step->signature = signature;
    step->tagReply = tagReply;
    step->residue = residue;
    step->status = status;
  }

  void expectControl(StepKind kind, ssize_t result = sizeof(UsbDevice::Setup)) {
    append(kind, result, AnyToggle);
  }

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

    const size_t index = transaction - 1;
    Step* step = index < m_StepCount ? &m_Steps[index] : nullptr;
    if (!step) {
      m_Valid = false;
    }

    // Keep old-code reds defined even when a missing recovery step shifts the
    // script: every 13-byte Bulk-IN receives a complete, benign CSW.
    seedCsw(*recorded, step && step->kind == StepKind::Csw ? step : nullptr);
    if (step && !validate(*recorded, *step))
      m_Valid = false;

    recorded->completed = true;
    for (size_t i = 0; i < recorded->transferCount; ++i)
      recorded->transfers[i].buffer = 0;

    ++m_Callbacks;
    callback(parameter, step ? step->result : -TransactionError);
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

  bool complete() const {
    if (!m_Valid || m_TransactionCount != m_StepCount || m_Callbacks != m_TransactionCount ||
        m_Cancellations)
      return false;
    for (size_t i = 0; i < m_TransactionCount; ++i) {
      if (!m_Transactions[i].completed)
        return false;
      for (size_t j = 0; j < m_Transactions[i].transferCount; ++j) {
        if (m_Transactions[i].transfers[j].buffer)
          return false;
      }
    }
    return true;
  }

  bool tagsMonotonic() const {
    if (!m_CbwCount)
      return false;
    uint32_t previous = LITTLE_TO_HOST32(m_Cbws[0].tag);
    if (!previous)
      return false;
    for (size_t i = 1; i < m_CbwCount; ++i) {
      const uint32_t tag = LITTLE_TO_HOST32(m_Cbws[i].tag);
      if (tag != previous + 1)
        return false;
      previous = tag;
    }
    return true;
  }

  bool firstTagIs(uint32_t tag) const {
    return m_CbwCount && LITTLE_TO_HOST32(m_Cbws[0].tag) == tag;
  }

  size_t transactionCount() const {
    return m_TransactionCount;
  }

 protected:
  bool cancelInterruptInAndDrain(const UsbInterruptInToken&, void (*)(uintptr_t, ssize_t),
                                 uintptr_t, bool) override {
    m_Valid = false;
    return false;
  }

 private:
  Step* append(StepKind kind, ssize_t result, int firstToggle) {
    if (m_StepCount >= MaxTransactions) {
      m_Valid = false;
      return nullptr;
    }
    Step& step = m_Steps[m_StepCount++];
    step.kind = kind;
    step.result = result;
    step.firstToggle = firstToggle;
    return &step;
  }

  RecordedTransaction* transactionFor(uintptr_t transaction) {
    if (!transaction || transaction > m_TransactionCount)
      return nullptr;
    return &m_Transactions[transaction - 1];
  }

  bool bulkShape(const RecordedTransaction& transaction, uint8_t endpoint, UsbPid pid, size_t bytes,
                 int firstToggle) const {
    if (transaction.endpoint.nEndpoint != endpoint || !transaction.transferCount)
      return false;
    size_t total = 0;
    bool toggle = firstToggle > 0;
    for (size_t i = 0; i < transaction.transferCount; ++i) {
      const RecordedTransfer& transfer = transaction.transfers[i];
      if (transfer.pid != pid || (firstToggle != AnyToggle && transfer.toggle != toggle))
        return false;
      total += transfer.bytes;
      toggle = !toggle;
    }
    return total == bytes;
  }

  bool validateCbw(const RecordedTransaction& transaction, const Step& step) {
    if (!bulkShape(transaction, 2, UsbPidOut, sizeof(WireCbw), step.firstToggle) ||
        transaction.transferCount != 1 || !transaction.transfers[0].buffer || m_CbwCount >= MaxCbws)
      return false;

    WireCbw& cbw = m_Cbws[m_CbwCount++];
    MemoryCopy(&cbw, reinterpret_cast<void*>(transaction.transfers[0].buffer), sizeof(cbw));
    m_LastTag = cbw.tag;
    if (cbw.signature != CbwSignature || cbw.dataBytes != HOST_TO_LITTLE32(step.expectedBytes) ||
        cbw.flags != 0 || cbw.lun != step.lun || cbw.commandSize != step.commandSize ||
        MemoryCompare(cbw.command, step.command, step.commandSize))
      return false;
    for (size_t i = step.commandSize; i < sizeof(cbw.command); ++i) {
      if (cbw.command[i])
        return false;
    }
    return true;
  }

  bool validateData(const RecordedTransaction& transaction, const Step& step) const {
    if (!bulkShape(transaction, 2, UsbPidOut, step.expectedBytes, step.firstToggle))
      return false;
    size_t offset = 0;
    for (size_t i = 0; i < transaction.transferCount; ++i) {
      const RecordedTransfer& transfer = transaction.transfers[i];
      if (!transfer.buffer || MemoryCompare(reinterpret_cast<void*>(transfer.buffer),
                                            step.expectedData + offset, transfer.bytes))
        return false;
      offset += transfer.bytes;
    }
    return true;
  }

  bool validateCsw(const RecordedTransaction& transaction, const Step& step) const {
    return bulkShape(transaction, 3, UsbPidIn, sizeof(WireCsw), step.firstToggle) &&
           transaction.transferCount == 1 && transaction.transfers[0].buffer;
  }

  bool validateControl(const RecordedTransaction& transaction, const Step& step) const {
    if (transaction.endpoint.nEndpoint != 0 || transaction.transferCount != 2 ||
        transaction.transfers[0].toggle || transaction.transfers[0].pid != UsbPidSetup ||
        transaction.transfers[0].bytes != sizeof(UsbDevice::Setup) ||
        !transaction.transfers[0].buffer || !transaction.transfers[1].toggle ||
        transaction.transfers[1].pid != UsbPidIn || transaction.transfers[1].bytes)
      return false;

    UsbDevice::Setup setup(0, 0, 0, 0, 0);
    MemoryCopy(&setup, reinterpret_cast<void*>(transaction.transfers[0].buffer), sizeof(setup));
    if (step.kind == StepKind::Reset) {
      return setup.nRequestType == (UsbRequestType::Class | UsbRequestRecipient::Interface) &&
             setup.nRequest == 0xff && setup.nValue == 0 && setup.nIndex == 4 && setup.nLength == 0;
    }

    const uint16_t endpoint = step.kind == StepKind::ClearIn ? 0x83 : 0x02;
    return setup.nRequestType == UsbRequestRecipient::Endpoint &&
           setup.nRequest == UsbRequest::ClearFeature && setup.nValue == 0 &&
           setup.nIndex == endpoint && setup.nLength == 0;
  }

  bool validate(const RecordedTransaction& transaction, const Step& step) {
    switch (step.kind) {
      case StepKind::Cbw:
        return validateCbw(transaction, step);
      case StepKind::DataOut:
        return validateData(transaction, step);
      case StepKind::Csw:
        return validateCsw(transaction, step);
      case StepKind::Reset:
      case StepKind::ClearIn:
      case StepKind::ClearOut:
        return validateControl(transaction, step);
    }
    return false;
  }

  void seedCsw(RecordedTransaction& transaction, const Step* step) {
    if (transaction.endpoint.nEndpoint != 3 || transaction.transferCount != 1 ||
        transaction.transfers[0].pid != UsbPidIn ||
        transaction.transfers[0].bytes != sizeof(WireCsw) || !transaction.transfers[0].buffer)
      return;

    WireCsw csw = {};
    csw.signature = step ? step->signature : CswSignature;
    csw.tag = m_LastTag;
    if (step && step->tagReply == TagReply::Wrong)
      csw.tag = HOST_TO_LITTLE32(LITTLE_TO_HOST32(m_LastTag) + 1);
    csw.residue = HOST_TO_LITTLE32(step ? step->residue : 0);
    csw.status = step ? step->status : 0;
    MemoryCopy(reinterpret_cast<void*>(transaction.transfers[0].buffer), &csw, sizeof(csw));
  }

  Step m_Steps[MaxTransactions];
  size_t m_StepCount;
  RecordedTransaction m_Transactions[MaxTransactions];
  size_t m_TransactionCount;
  WireCbw m_Cbws[MaxCbws];
  size_t m_CbwCount;
  uint32_t m_LastTag;
  size_t m_Callbacks;
  size_t m_Cancellations;
  bool m_Valid;
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

UsbInterfaceDescriptor interfaceDescriptor() {
  UsbInterfaceDescriptor descriptor;
  ByteSet(&descriptor, 0, sizeof(descriptor));
  descriptor.nLength = sizeof(descriptor);
  descriptor.nType = UsbDescriptor::Interface;
  descriptor.nInterface = 4;
  return descriptor;
}

class BotTestDevice final : public UsbMassStorageDevice {
 public:
  explicit BotTestDevice(UsbDevice* device) : UsbMassStorageDevice(device) {}

  void bindInterface(Interface* interface) {
    m_pInterface = interface;
  }
};

struct BotFixture {
  BotFixture()
      : hub(),
        base(&hub, 1, HighSpeed),
        interfaceDesc(interfaceDescriptor()),
        interface(&interfaceDesc),
        outDesc(endpointDescriptor(2, false)),
        out(&outDesc, HighSpeed),
        inDesc(endpointDescriptor(3, true)),
        in(&inDesc, HighSpeed),
        device(&base),
        payload() {
    device.bindInterface(&interface);
    UsbMassStorageBotTestAccess::bind(device, &in, &out);
    for (size_t i = 0; i < sizeof(payload); ++i)
      payload[i] = static_cast<uint8_t>((i * 17) ^ 0x5a);
  }

  ScriptedBotHub hub;
  UsbDevice base;
  UsbInterfaceDescriptor interfaceDesc;
  UsbDevice::Interface interface;
  UsbEndpointDescriptor outDesc;
  UsbDevice::Endpoint out;
  UsbEndpointDescriptor inDesc;
  UsbDevice::Endpoint in;
  BotTestDevice device;
  alignas(16) uint8_t payload[DataBytes];
};

void expectWrite(BotFixture& fixture, int cbwToggle, int dataToggle,
                 ssize_t dataResult = DataBytes) {
  fixture.hub.expectCbw(3, WriteCdb, sizeof(WriteCdb), DataBytes, cbwToggle);
  fixture.hub.expectDataOut(fixture.payload, DataBytes, dataToggle, dataResult);
}

bool sendWrite(BotFixture& fixture) {
  return fixture.device.sendCommand(3, reinterpret_cast<uintptr_t>(WriteCdb), sizeof(WriteCdb),
                                    reinterpret_cast<uintptr_t>(fixture.payload), DataBytes, true);
}

bool completeDataOut() {
  BotFixture fixture;
  expectWrite(fixture, 0, 1);
  fixture.hub.expectCsw(13, 0, 0, TagReply::Match, CswSignature, 0);
  expectWrite(fixture, 1, 0);
  fixture.hub.expectCsw(13, 0, 0, TagReply::Match, CswSignature, 1);

  const bool first = sendWrite(fixture);
  const bool second = sendWrite(fixture);
  const bool passed = first && second && fixture.hub.complete() && fixture.hub.tagsMonotonic();
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS usb-bot-data-out-complete");
  } else {
    ERROR("HOSTED-WAIT-TEST: FAIL usb-bot-data-out-complete: exact CBW/data/CSW or tags failed");
  }
  return passed;
}

bool shortDataOutResets() {
  BotFixture fixture;
  expectWrite(fixture, 0, 1, DataBytes / 2);
  fixture.hub.expectControl(StepKind::Reset);
  fixture.hub.expectControl(StepKind::ClearIn);
  fixture.hub.expectControl(StepKind::ClearOut);

  const bool result = sendWrite(fixture);
  const bool passed =
      !result && fixture.hub.complete() && !fixture.in.bDataToggle && !fixture.out.bDataToggle;
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS usb-bot-data-out-short-reset");
  } else {
    ERROR(
        "HOSTED-WAIT-TEST: FAIL usb-bot-data-out-short-reset: short OUT was accepted or recovery "
        "was incomplete");
  }
  return passed;
}

bool invalidCbwCase(ssize_t result, bool followWithValidCommand) {
  BotFixture fixture;
  fixture.hub.expectCbw(3, WriteCdb, sizeof(WriteCdb), DataBytes, 0, result);
  fixture.hub.expectControl(StepKind::Reset);
  fixture.hub.expectControl(StepKind::ClearIn);
  fixture.hub.expectControl(StepKind::ClearOut);
  if (followWithValidCommand) {
    expectWrite(fixture, 0, 1);
    fixture.hub.expectCsw(13, 0, 0, TagReply::Match, CswSignature, 0);
  }

  const bool failed = !sendWrite(fixture);
  const bool followUp = !followWithValidCommand || sendWrite(fixture);
  return failed && followUp && fixture.hub.complete() &&
         (!followWithValidCommand || (fixture.hub.firstTagIs(1) && fixture.hub.tagsMonotonic()));
}

bool invalidCbwResets() {
  const bool shortResult = invalidCbwCase(sizeof(WireCbw) - 1, true);
  const bool stalledResult = invalidCbwCase(-Stall, false);
  const bool passed = shortResult && stalledResult;
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS usb-bot-cbw-exact-reset");
  } else {
    ERROR(
        "HOSTED-WAIT-TEST: FAIL usb-bot-cbw-exact-reset: short or stalled CBW did not perform "
        "full reset recovery");
  }
  return passed;
}

bool stalledDataOutUsesCsw() {
  BotFixture fixture;
  expectWrite(fixture, 0, 1, -Stall);
  fixture.hub.expectControl(StepKind::ClearOut);
  fixture.hub.expectCsw(13, 0, 0, TagReply::Match, CswSignature, 0);

  const bool result = sendWrite(fixture);
  const bool passed = result && fixture.hub.complete() && !fixture.out.bDataToggle;
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS usb-bot-data-out-stall-csw");
  } else {
    ERROR(
        "HOSTED-WAIT-TEST: FAIL usb-bot-data-out-stall-csw: recovered OUT STALL did not use the "
        "authoritative CSW");
  }
  return passed;
}

bool stalledDataOutCswCase(uint8_t status, uint32_t residue) {
  BotFixture fixture;
  expectWrite(fixture, 0, 1, -Stall);
  fixture.hub.expectControl(StepKind::ClearOut);
  fixture.hub.expectCsw(13, status, residue, TagReply::Match, CswSignature, 0);
  return !sendWrite(fixture) && fixture.hub.complete();
}

bool stalledDataOutHonoursCswFailure() {
  const bool statusResult = stalledDataOutCswCase(1, 0);
  const bool residueResult = stalledDataOutCswCase(0, 1);
  const bool passed = statusResult && residueResult;
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS usb-bot-data-out-stall-csw-failure");
  } else {
    ERROR(
        "HOSTED-WAIT-TEST: FAIL usb-bot-data-out-stall-csw-failure: recovered OUT STALL "
        "ignored CSW failure or residue");
  }
  return passed;
}

bool stalledCswRetriesOnce() {
  BotFixture fixture;
  expectWrite(fixture, 0, 1);
  fixture.hub.expectCsw(-Stall, 0, 0, TagReply::Match, CswSignature, 0);
  fixture.hub.expectControl(StepKind::ClearIn);
  fixture.hub.expectCsw(13, 0, 0, TagReply::Match, CswSignature, 0);

  const bool result = sendWrite(fixture);
  const bool passed = result && fixture.hub.complete() && fixture.in.bDataToggle;
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS usb-bot-csw-stall-retry");
  } else {
    ERROR(
        "HOSTED-WAIT-TEST: FAIL usb-bot-csw-stall-retry: CSW STALL was not cleared and retried "
        "once at DATA0");
  }
  return passed;
}

bool secondCswStallResets() {
  BotFixture fixture;
  expectWrite(fixture, 0, 1);
  fixture.hub.expectCsw(-Stall, 0, 0, TagReply::Match, CswSignature, 0);
  fixture.hub.expectControl(StepKind::ClearIn);
  fixture.hub.expectCsw(-Stall, 0, 0, TagReply::Match, CswSignature, 0);
  fixture.hub.expectControl(StepKind::Reset);
  fixture.hub.expectControl(StepKind::ClearIn);
  fixture.hub.expectControl(StepKind::ClearOut);

  const bool result = sendWrite(fixture);
  const bool passed = !result && fixture.hub.complete();
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS usb-bot-csw-second-stall-reset");
  } else {
    ERROR(
        "HOSTED-WAIT-TEST: FAIL usb-bot-csw-second-stall-reset: CSW was retried more than once "
        "or reset recovery was incomplete");
  }
  return passed;
}

bool cswTruthCase(ssize_t length, uint8_t status, uint32_t residue, TagReply tagReply,
                  uint32_t signature, bool recovery) {
  BotFixture fixture;
  expectWrite(fixture, 0, 1);
  fixture.hub.expectCsw(length, status, residue, tagReply, signature, 0);
  if (recovery) {
    fixture.hub.expectControl(StepKind::Reset);
    fixture.hub.expectControl(StepKind::ClearIn);
    fixture.hub.expectControl(StepKind::ClearOut);
  }
  return !sendWrite(fixture) && fixture.hub.complete();
}

bool cswTruth() {
  const bool length = cswTruthCase(12, 0, 0, TagReply::Match, CswSignature, true);
  const bool signature =
      cswTruthCase(13, 0, 0, TagReply::Match, HOST_TO_LITTLE32(0x12345678), true);
  const bool tag = cswTruthCase(13, 0, 0, TagReply::Wrong, CswSignature, true);
  const bool passedResidue = cswTruthCase(13, 0, 1, TagReply::Match, CswSignature, false);
  const bool failedStatus = cswTruthCase(13, 1, 0, TagReply::Match, CswSignature, false);
  const bool phaseError = cswTruthCase(13, 2, 0, TagReply::Match, CswSignature, true);
  const bool invalidStatus = cswTruthCase(13, 3, 0, TagReply::Match, CswSignature, true);
  const bool excessResidue =
      cswTruthCase(13, 0, DataBytes + 1, TagReply::Match, CswSignature, true);
  const bool passed = length && signature && tag && passedResidue && failedStatus && phaseError &&
                      invalidStatus && excessResidue;
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS usb-bot-csw-truth");
  } else {
    ERROR(
        "HOSTED-WAIT-TEST: FAIL usb-bot-csw-truth: invalid CSW length/signature/tag/status/residue "
        "was accepted or mis-recovered");
  }
  return passed;
}

bool failedRecoveryLatches() {
  BotFixture fixture;
  expectWrite(fixture, 0, 1);
  fixture.hub.expectCsw(13, 0, 0, TagReply::Wrong, CswSignature, 0);
  fixture.hub.expectControl(StepKind::Reset);
  fixture.hub.expectControl(StepKind::ClearIn, -TransactionError);
  fixture.hub.expectControl(StepKind::ClearOut);

  fixture.hub.expectControl(StepKind::Reset);
  fixture.hub.expectControl(StepKind::ClearIn);
  fixture.hub.expectControl(StepKind::ClearOut);
  expectWrite(fixture, 0, 1);
  fixture.hub.expectCsw(13, 0, 0, TagReply::Match, CswSignature, 0);

  const bool first = sendWrite(fixture);
  const bool second = sendWrite(fixture);
  const bool passed = !first && second && fixture.hub.complete() && fixture.hub.tagsMonotonic();
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS usb-bot-recovery-latch");
  } else {
    ERROR(
        "HOSTED-WAIT-TEST: FAIL usb-bot-recovery-latch: failed recovery did not block the next CBW "
        "until all three steps succeeded");
  }
  return passed;
}

bool commandBoundsDoNotReachUsb() {
  alignas(16) uint8_t command[17] = {};
  for (size_t i = 0; i < sizeof(command); ++i)
    command[i] = static_cast<uint8_t>(0xa0 + i);

  BotFixture fixture;
  UsbMassStorageBotTestAccess::setUnits(fixture.device, 17);
  const bool unencodableLun =
      fixture.device.sendCommand(16, reinterpret_cast<uintptr_t>(command), 16,
                                 reinterpret_cast<uintptr_t>(fixture.payload), DataBytes, true);
  const bool wideLunRejectedWithoutIo = !unencodableLun && fixture.hub.transactionCount() == 0;
  UsbMassStorageBotTestAccess::setUnits(fixture.device, 4);

  const bool zeroSize =
      fixture.device.sendCommand(3, reinterpret_cast<uintptr_t>(command), 0,
                                 reinterpret_cast<uintptr_t>(fixture.payload), DataBytes, true);
  const bool nullCommand = fixture.device.sendCommand(
      3, 0, 16, reinterpret_cast<uintptr_t>(fixture.payload), DataBytes, true);
  const bool oversized =
      fixture.device.sendCommand(3, reinterpret_cast<uintptr_t>(command), sizeof(command),
                                 reinterpret_cast<uintptr_t>(fixture.payload), DataBytes, true);
  const bool unavailableLun =
      fixture.device.sendCommand(4, reinterpret_cast<uintptr_t>(command), 16,
                                 reinterpret_cast<uintptr_t>(fixture.payload), DataBytes, true);
  const bool nullPayload =
      fixture.device.sendCommand(3, reinterpret_cast<uintptr_t>(command), 16, 0, DataBytes, true);
  const bool rejectedWithoutIo = wideLunRejectedWithoutIo && !zeroSize && !nullCommand &&
                                 !oversized && !unavailableLun && !nullPayload &&
                                 fixture.hub.transactionCount() == 0;

  fixture.hub.expectCbw(3, command, 16, DataBytes, 0);
  fixture.hub.expectDataOut(fixture.payload, DataBytes, 1, DataBytes);
  fixture.hub.expectCsw(13, 0, 0, TagReply::Match, CswSignature, 0);
  const bool valid =
      fixture.device.sendCommand(3, reinterpret_cast<uintptr_t>(command), 16,
                                 reinterpret_cast<uintptr_t>(fixture.payload), DataBytes, true);
  const bool complete = fixture.hub.complete();
  const bool firstTag = fixture.hub.firstTagIs(1);
  const bool passed = rejectedWithoutIo && valid && complete && firstTag;
  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS usb-bot-cdb-bounds");
  } else {
    ERROR(
        "HOSTED-WAIT-TEST: FAIL usb-bot-cdb-bounds: invalid CDB/LUN/payload reached USB or "
        "consumed the first tag; rejected="
        << rejectedWithoutIo << ", valid=" << valid << ", complete=" << complete
        << ", first-tag=" << firstTag);
  }
  return passed;
}
}  // namespace

EXPORTED_PUBLIC bool runHostedUsbBotRegressions() {
  const bool complete = completeDataOut();
  const bool cbwExact = invalidCbwResets();
  const bool shortReset = shortDataOutResets();
  const bool stalledData = stalledDataOutUsesCsw();
  const bool stalledDataFailure = stalledDataOutHonoursCswFailure();
  const bool stalledCsw = stalledCswRetriesOnce();
  const bool secondCswStall = secondCswStallResets();
  const bool truth = cswTruth();
  const bool recoveryLatch = failedRecoveryLatches();
  const bool commandBounds = commandBoundsDoNotReachUsb();
  return complete && cbwExact && shortReset && stalledData && stalledDataFailure && stalledCsw &&
         secondCswStall && truth && recoveryLatch && commandBounds;
}
