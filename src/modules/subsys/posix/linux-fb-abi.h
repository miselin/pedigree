/* Copyright (c) 2026, Pedigree Developers. */
#ifndef LINUX_FB_ABI_H
#define LINUX_FB_ABI_H

#include <stdint.h>

enum LinuxFramebufferCommand {
  LinuxFbGetVariableInfo = 0x4600,
  LinuxFbPutVariableInfo = 0x4601,
  LinuxFbGetFixedInfo = 0x4602,
  LinuxFbGetColorMap = 0x4604,
  LinuxFbPutColorMap = 0x4605,
  LinuxFbPanDisplay = 0x4606,
  LinuxFbBlank = 0x4611,
};

enum LinuxFramebufferType {
  LinuxFbPackedPixels = 0,
};

enum LinuxFramebufferVisual {
  LinuxFbTrueColor = 2,
  LinuxFbPseudoColor = 3,
};

enum LinuxFramebufferActivation {
  LinuxFbActivateNow = 0,
  LinuxFbActivateTest = 2,
  LinuxFbActivateMask = 15,
};

struct LinuxFbBitfield {
  uint32_t offset;
  uint32_t length;
  uint32_t msbRight;
};

struct LinuxFbFixedInfo {
  char id[16];
  uintptr_t memoryStart;
  uint32_t memoryLength;
  uint32_t type;
  uint32_t typeAux;
  uint32_t visual;
  uint16_t xPanStep;
  uint16_t yPanStep;
  uint16_t yWrapStep;
  uint32_t lineLength;
  uintptr_t mmioStart;
  uint32_t mmioLength;
  uint32_t acceleration;
  uint16_t capabilities;
  uint16_t reserved[2];
};

struct LinuxFbVariableInfo {
  uint32_t xResolution;
  uint32_t yResolution;
  uint32_t virtualXResolution;
  uint32_t virtualYResolution;
  uint32_t xOffset;
  uint32_t yOffset;
  uint32_t bitsPerPixel;
  uint32_t grayscale;
  LinuxFbBitfield red;
  LinuxFbBitfield green;
  LinuxFbBitfield blue;
  LinuxFbBitfield transparency;
  uint32_t nonstandard;
  uint32_t activate;
  uint32_t heightMillimeters;
  uint32_t widthMillimeters;
  uint32_t accelerationFlags;
  uint32_t pixelClock;
  uint32_t leftMargin;
  uint32_t rightMargin;
  uint32_t upperMargin;
  uint32_t lowerMargin;
  uint32_t horizontalSyncLength;
  uint32_t verticalSyncLength;
  uint32_t sync;
  uint32_t mode;
  uint32_t rotate;
  uint32_t colorSpace;
  uint32_t reserved[4];
};

struct LinuxFbColorMap {
  uint32_t start;
  uint32_t length;
  uint16_t* red;
  uint16_t* green;
  uint16_t* blue;
  uint16_t* transparency;
};

static_assert(sizeof(LinuxFbVariableInfo) == 160,
              "Linux framebuffer variable information ABI changed");
#if UINTPTR_MAX == UINT64_MAX
static_assert(sizeof(LinuxFbFixedInfo) == 80, "Linux framebuffer fixed information ABI changed");
static_assert(sizeof(LinuxFbColorMap) == 40, "Linux framebuffer colour map ABI changed");
#else
static_assert(sizeof(LinuxFbFixedInfo) == 68, "Linux framebuffer fixed information ABI changed");
static_assert(sizeof(LinuxFbColorMap) == 24, "Linux framebuffer colour map ABI changed");
#endif

#endif
