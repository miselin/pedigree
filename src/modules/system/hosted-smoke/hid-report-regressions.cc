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
  int values[32]{};
  size_t count = 0;
};
void input(InputManager::InputNotification& notification) {
  auto* capture = static_cast<Capture*>(notification.meta);
  int value = 0;
  if (notification.type == InputManager::RawKey &&
      (notification.data.rawkey.scancode == 4 || notification.data.rawkey.scancode == 5 ||
       notification.data.rawkey.scancode == 0xe1)) {
    value = notification.data.rawkey.scancode * (notification.data.rawkey.keyUp ? -1 : 1);
  } else if (notification.type == InputManager::Mouse && notification.data.pointy.relx == -1) {
    value = 100;
  } else if (notification.type == InputManager::Mouse) {
    value = 200 + notification.data.pointy.buttons[0] + 2 * notification.data.pointy.buttons[1];
  }
  if (!value)
    return;
  LockGuard<Mutex> lock(capture->lock);
  if (capture->count < 32) {
    capture->values[capture->count++] = value;
  }
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
  bool delivered = capture.received.acquireForCompletion(5, 2);

  uint8_t hotplugDescriptor[] = {0x05, 1,    0x09, 6,    0xa1, 1,    0x85, 1,    0x05, 7,    0x09,
                                 4,    0x09, 0xe1, 0x15, 0,    0x25, 1,    0x75, 1,    0x95, 2,
                                 0x81, 2,    0x75, 6,    0x95, 1,    0x81, 3,    0xc0, 0x05, 1,
                                 0x09, 2,    0xa1, 1,    0x85, 2,    0x05, 9,    0x19, 1,    0x29,
                                 2,    0x15, 0,    0x25, 1,    0x75, 1,    0x95, 2,    0x81, 2,
                                 0x75, 6,    0x95, 1,    0x81, 3,    0xc0};
  auto* first = new HidReport;
  auto* second = new HidReport;
  first->parseDescriptor(hotplugDescriptor, sizeof(hotplugDescriptor));
  second->parseDescriptor(hotplugDescriptor, sizeof(hotplugDescriptor));
  uint8_t heldKeys[] = {1, 3};
  uint8_t left[] = {2, 1};
  uint8_t both[] = {2, 3};
  first->feedInput(heldKeys, nullptr, sizeof(heldKeys));
  delivered &= capture.received.acquireForCompletion(2, 2);
  first->feedInput(left, nullptr, sizeof(left));
  delivered &= capture.received.acquireForCompletion(1, 2);
  second->feedInput(heldKeys, nullptr, sizeof(heldKeys));
  second->feedInput(left, nullptr, sizeof(left));
  delivered &= capture.received.acquireForCompletion(1, 2);
  second->feedInput(both, nullptr, sizeof(both));
  delivered &= capture.received.acquireForCompletion(1, 2);
  delete first;
  delivered &= capture.received.acquireForCompletion(1, 2);
  uint8_t unshifted[] = {1, 1};
  uint8_t right[] = {2, 2};
  second->feedInput(unshifted, nullptr, sizeof(unshifted));
  delivered &= capture.received.acquireForCompletion(1, 2);
  second->feedInput(right, nullptr, sizeof(right));
  delivered &= capture.received.acquireForCompletion(1, 2);
  delete second;
  delivered &= capture.received.acquireForCompletion(2, 2);
  {
    HidReport reconnected;
    reconnected.parseDescriptor(hotplugDescriptor, sizeof(hotplugDescriptor));
    reconnected.feedInput(unshifted, nullptr, sizeof(unshifted));
  }
  const int expected[] = {4,   5,   -4,    -5,  100, 4,   0xe1, 201, 201,
                          203, 203, -0xe1, 202, -4,  200, 4,    -4};
  const size_t expectedCount = sizeof(expected) / sizeof(expected[0]);
  delivered &= capture.received.acquireForCompletion(2, 2);
  InputManager::instance().removeCallback(input, &capture);
  bool valid = delivered && capture.count == expectedCount;
  for (size_t i = 0; valid && i < expectedCount; ++i)
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
  if (valid) {
    NOTICE("HOSTED-WAIT-TEST: PASS hid-report-ids-short-signed-bounds-and-hotplug");
  } else {
    ERROR("HOSTED-WAIT-TEST: FAIL hid-report-ids-short-signed-bounds-and-hotplug");
  }
  return valid;
}
