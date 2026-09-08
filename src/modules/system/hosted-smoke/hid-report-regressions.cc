/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/InputManager.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/Semaphore.h"

#include "modules/drivers/common/hid/HidReport.h"
#include "modules/drivers/common/hid/HidUtils.h"

namespace {
struct Capture {
  Mutex lock;
  Semaphore received{0, false};
  int values[8]{};
  size_t count = 0;
};
void input(InputManager::InputNotification& notification) {
  auto* capture = static_cast<Capture*>(notification.meta);
  int value = 0;
  if (notification.type == InputManager::RawKey &&
      (notification.data.rawkey.scancode == 4 || notification.data.rawkey.scancode == 5))
    value = notification.data.rawkey.scancode * (notification.data.rawkey.keyUp ? -1 : 1);
  else if (notification.type == InputManager::Mouse && notification.data.pointy.relx == -1)
    value = 100;
  if (!value)
    return;
  LockGuard<Mutex> lock(capture->lock);
  if (capture->count < 8)
    capture->values[capture->count++] = value;
  capture->received.release();
}
}  // namespace

EXPORTED_PUBLIC bool runHostedHidReportRegressions() {
  uint8_t descriptor[] = {0x05, 1,    0x09, 6,    0xa1, 1,    0x85, 1,    0x05, 7,    0x09, 4,
                          0x15, 0,    0x25, 1,    0x75, 1,    0x95, 1,    0x81, 2,    0x75, 7,
                          0x81, 3,    0xc0, 0x05, 1,    0x09, 6,    0xa1, 1,    0x85, 2,    0x05,
                          7,    0x09, 5,    0x15, 0,    0x25, 1,    0x75, 1,    0x95, 1,    0x81,
                          2,    0x75, 7,    0x81, 3,    0xc0, 0x05, 1,    0x09, 2,    0xa1, 1,
                          0x85, 3,    0x09, 0x30, 0x16, 0,    0xf8, 0x26, 0xff, 7,    0x75, 12,
                          0x95, 1,    0x81, 6,    0x75, 4,    0x81, 3,    0xc0};
  HidReport report;
  report.parseDescriptor(descriptor, sizeof(descriptor));
  if (!report.valid())
    return false;
  Capture capture;
  InputManager::instance().installCallback(InputManager::RawKey | InputManager::Mouse, input,
                                           &capture);
  uint8_t packets[][3] = {{1, 1}, {2, 1}, {1}, {1, 0}, {2, 0}, {3, 0xff, 0x0f}};
  const size_t lengths[] = {2, 2, 1, 2, 2, 3};
  for (size_t i = 0; i < 6; ++i)
    report.feedInput(packets[i], nullptr, lengths[i]);
  const bool delivered = capture.received.acquireForCompletion(5, 2);
  InputManager::instance().removeCallback(input, &capture);
  const int expected[] = {4, 5, -4, -5, 100};
  bool valid = delivered && capture.count == 5;
  for (size_t i = 0; valid && i < 5; ++i)
    valid = capture.values[i] == expected[i];
  uint8_t truncated[] = {0x27, 0};
  HidReport malformed;
  malformed.parseDescriptor(truncated, sizeof(truncated));
  valid &= !malformed.valid();
  uint8_t nested[34];
  for (size_t i = 0; i < 34; i += 2) {
    nested[i] = 0xa1;
    nested[i + 1] = 1;
  }
  HidReport deep;
  deep.parseDescriptor(nested, sizeof(nested));
  valid &= !deep.valid();
  uint8_t field[] = {0xf0, 0x3f};
  valid &= HidUtils::getBufferField(field, 4, 10) == 0x3ff;
  if (valid)
    NOTICE("HOSTED-WAIT-TEST: PASS hid-report-ids-short-signed-and-bounds");
  else
    ERROR("HOSTED-WAIT-TEST: FAIL hid-report-ids-short-signed-and-bounds");
  return valid;
}
