/* Copyright (c) 2026, Pedigree Developers. */
#ifndef LINUX_INPUT_ABI_H
#define LINUX_INPUT_ABI_H

#include <stdint.h>

enum LinuxInputEventType : uint16_t {
  LinuxEvSyn = 0,
  LinuxEvKey = 1,
  LinuxEvRelative = 2,
};

enum LinuxInputRelativeAxis : uint16_t {
  LinuxRelX = 0,
  LinuxRelY = 1,
  LinuxRelWheel = 8,
};

enum LinuxInputButton : uint16_t {
  LinuxBtnLeft = 0x110,
  LinuxBtnRight = 0x111,
  LinuxBtnMiddle = 0x112,
  LinuxBtnSide = 0x113,
  LinuxBtnExtra = 0x114,
};

struct LinuxInputEvent {
  int64_t seconds;
  int64_t microseconds;
  uint16_t type;
  uint16_t code;
  int32_t value;
};

struct LinuxInputId {
  uint16_t busType;
  uint16_t vendor;
  uint16_t product;
  uint16_t version;
};

static_assert(sizeof(LinuxInputEvent) == 24, "Linux input event ABI changed");
static_assert(sizeof(LinuxInputId) == 8, "Linux input identifier ABI changed");

#endif
