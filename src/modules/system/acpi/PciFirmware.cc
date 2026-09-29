/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/PciFirmware.h"
#include "pedigree/kernel/process/Mutex.h"

#include "PciRouting.h"
#include <uacpi/acpi.h>
#include <uacpi/resources.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

#if ARM64 || ARMV7
#include "system/kernel/machine/mach_virt/DeviceTree.h"
#endif

namespace {
constexpr size_t MaxRoots = 8;
constexpr uint32_t OscSupport = (1U << 0) | (1U << 4);  // Extended config and MSI.
constexpr uint32_t NativeMask =
    PciFirmware::NativeHotplug | PciFirmware::NativeAer | PciFirmware::NativePcieCapability;
constexpr const char* RootIds[] = {"PNP0A03", "PNP0A08", nullptr};
constexpr uint8_t OscUuid[16] = {0x5b, 0x4d, 0xdb, 0x33, 0xf7, 0x1f, 0x1c, 0x40,
                                 0x96, 0x57, 0x74, 0x41, 0xc0, 0x3d, 0xd7, 0x66};

struct RootState {
  PciFirmware::Root root;
  uacpi_namespace_node* node = nullptr;
  uint32_t control = 0;
  bool oscFailed = false;
};

struct ResourceContext {
  PciFirmware::Root* root;
  uint8_t baseBus;
  bool sawBus = false;
  bool valid = true;
};

Mutex g_Lock;
RootState g_Roots[MaxRoots];
size_t g_RootCount = 0;
bool g_Ready = false;
bool g_Failed = false;

bool overlaps(uint64_t first, uint64_t firstSize, uint64_t second, uint64_t secondSize) {
  return first <= second + secondSize - 1 && second <= first + firstSize - 1;
}

bool translate(uint64_t address, uint64_t length, uint64_t offset, unsigned width,
               uint64_t& result) {
  const uint64_t mask = width == 64 ? ~uint64_t(0) : (uint64_t(1) << width) - 1;
  const uint64_t sign = uint64_t(1) << (width - 1);
  if (offset & sign) {
    const uint64_t magnitude = ((~offset) & mask) + 1;
    if (address < magnitude) {
      return false;
    }
    result = address - magnitude;
  } else {
    if (address > ~uint64_t(0) - offset) {
      return false;
    }
    result = address + offset;
  }
  return result <= ~uint64_t(0) - (length - 1);
}

bool addAddress(ResourceContext& context, const uacpi_resource_address_common& common,
                uint64_t minimum, uint64_t maximum, uint64_t offset, uint64_t length,
                unsigned width) {
  if (common.direction != UACPI_PRODUCER) {
    return true;
  }
  if (!length || minimum > maximum || length - 1 != maximum - minimum ||
      common.fixed_min_address != UACPI_ADDRESS_FIXED ||
      common.fixed_max_address != UACPI_ADDRESS_FIXED ||
      common.decode_type != UACPI_POSITIVE_DECODE) {
    return false;
  }
  if (common.type == UACPI_RANGE_BUS) {
    if (context.sawBus || offset || minimum != context.baseBus || maximum > 255) {
      return false;
    }
    context.root->firstBus = static_cast<uint8_t>(minimum);
    context.root->lastBus = static_cast<uint8_t>(maximum);
    context.sawBus = true;
    return true;
  }
  if (common.type != UACPI_RANGE_IO && common.type != UACPI_RANGE_MEMORY) {
    return false;
  }
  if (common.type == UACPI_RANGE_MEMORY &&
      common.attribute.memory.range_type != UACPI_RANGE_TYPE_MEMORY) {
    return true;
  }
  if (common.type == UACPI_RANGE_IO &&
      common.attribute.io.translation_type != UACPI_TRANSLATION_DENSE) {
    return false;
  }

  uint64_t cpuBase = 0;
  if (!translate(minimum, length, offset, width, cpuBase)) {
    return false;
  }
  const bool io = common.type == UACPI_RANGE_IO;
  if (io && (maximum > 0xffff || cpuBase + length - 1 > 0xffff)) {
    return false;
  }
  PciFirmware::Root& root = *context.root;
  if (root.windowCount == PciFirmware::MaxWindows) {
    return false;
  }
  for (size_t i = 0; i < root.windowCount; ++i) {
    const PciFirmware::Window& old = root.windows[i];
    if (old.io == io && (overlaps(minimum, length, old.pciBase, old.size) ||
                         overlaps(cpuBase, length, old.cpuBase, old.size))) {
      return false;
    }
  }
  PciFirmware::Window& window = root.windows[root.windowCount++];
  window.pciBase = minimum;
  window.cpuBase = cpuBase;
  window.size = length;
  window.io = io;
  window.prefetchable = !io && common.attribute.memory.caching == UACPI_PREFETCHABLE;
  return true;
}

uacpi_iteration_decision readResource(void* user, uacpi_resource* resource) {
  auto& context = *static_cast<ResourceContext*>(user);
  bool valid = true;
  switch (resource->type) {
    case UACPI_RESOURCE_TYPE_ADDRESS16: {
      const auto& address = resource->address16;
      valid = addAddress(context, address.common, address.minimum, address.maximum,
                         address.translation_offset, address.address_length, 16);
      break;
    }
    case UACPI_RESOURCE_TYPE_ADDRESS32: {
      const auto& address = resource->address32;
      valid = addAddress(context, address.common, address.minimum, address.maximum,
                         address.translation_offset, address.address_length, 32);
      break;
    }
    case UACPI_RESOURCE_TYPE_ADDRESS64: {
      const auto& address = resource->address64;
      valid = addAddress(context, address.common, address.minimum, address.maximum,
                         address.translation_offset, address.address_length, 64);
      break;
    }
    case UACPI_RESOURCE_TYPE_ADDRESS64_EXTENDED: {
      const auto& address = resource->address64_extended;
      valid = addAddress(context, address.common, address.minimum, address.maximum,
                         address.translation_offset, address.address_length, 64);
      break;
    }
    default:
      break;
  }
  context.valid = valid;
  return valid ? UACPI_ITERATION_DECISION_CONTINUE : UACPI_ITERATION_DECISION_BREAK;
}

bool readRoot(uacpi_namespace_node* node, RootState& result) {
  uint64_t segment = 0;
  uacpi_status status = uacpi_eval_simple_integer(node, "_SEG", &segment);
  if (status != UACPI_STATUS_OK && status != UACPI_STATUS_NOT_FOUND) {
    return false;
  }
  if (segment > 0xffff) {
    return false;
  }
  if (segment) {
    return true;
  }

  uint64_t baseBus = 0;
  status = uacpi_eval_simple_integer(node, "_BBN", &baseBus);
  if ((status != UACPI_STATUS_OK && status != UACPI_STATUS_NOT_FOUND) || baseBus > 255) {
    return false;
  }
  result.root.segment = 0;
  ResourceContext context{&result.root, static_cast<uint8_t>(baseBus)};
  uacpi_resources* resources = nullptr;
  status = uacpi_get_current_resources(node, &resources);
  if (status != UACPI_STATUS_OK || !resources) {
    if (resources) {
      uacpi_free_resources(resources);
    }
    return false;
  }
  status = uacpi_for_each_resource(resources, readResource, &context);
  uacpi_free_resources(resources);
  if (status != UACPI_STATUS_OK || !context.valid || !context.sawBus) {
    return false;
  }
  result.node = node;
  return true;
}

struct DiscoveryContext {
  bool failed = false;
};

uacpi_iteration_decision readDevice(void* user, uacpi_namespace_node* node, uacpi_u32) {
  auto& context = *static_cast<DiscoveryContext*>(user);
  if (!uacpi_device_matches_pnp_id(node, RootIds)) {
    return UACPI_ITERATION_DECISION_CONTINUE;
  }
  uacpi_u32 flags = 0;
  if (uacpi_eval_sta(node, &flags) != UACPI_STATUS_OK) {
    context.failed = true;
    return UACPI_ITERATION_DECISION_BREAK;
  }
  if (!(flags & (ACPI_STA_RESULT_DEVICE_PRESENT | ACPI_STA_RESULT_DEVICE_FUNCTIONING))) {
    return UACPI_ITERATION_DECISION_NEXT_PEER;
  }
  RootState candidate;
  if (!readRoot(node, candidate)) {
    context.failed = true;
    return UACPI_ITERATION_DECISION_BREAK;
  }
  if (!candidate.node) {
    return UACPI_ITERATION_DECISION_CONTINUE;
  }
  if (g_RootCount == MaxRoots) {
    context.failed = true;
    return UACPI_ITERATION_DECISION_BREAK;
  }
  for (size_t i = 0; i < g_RootCount; ++i) {
    const PciFirmware::Root& old = g_Roots[i].root;
    const PciFirmware::Root& current = candidate.root;
    if (current.firstBus <= old.lastBus && old.firstBus <= current.lastBus) {
      context.failed = true;
      return UACPI_ITERATION_DECISION_BREAK;
    }
    for (size_t a = 0; a < current.windowCount; ++a) {
      for (size_t b = 0; b < old.windowCount; ++b) {
        const auto& first = current.windows[a];
        const auto& second = old.windows[b];
        if (first.io == second.io &&
            overlaps(first.cpuBase, first.size, second.cpuBase, second.size)) {
          context.failed = true;
          return UACPI_ITERATION_DECISION_BREAK;
        }
      }
    }
  }
  g_Roots[g_RootCount++] = candidate;
  return UACPI_ITERATION_DECISION_CONTINUE;
}

RootState* findRoot(uint8_t bus) {
  for (size_t i = 0; i < g_RootCount; ++i) {
    PciFirmware::Root& root = g_Roots[i].root;
    if (root.firstBus <= bus && bus <= root.lastBus) {
      return &g_Roots[i];
    }
  }
  return nullptr;
}

void write32(uint8_t* bytes, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) {
    bytes[i] = static_cast<uint8_t>(value >> (i * 8));
  }
}

uint32_t read32(const uint8_t* bytes) {
  uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i) {
    value |= uint32_t(bytes[i]) << (i * 8);
  }
  return value;
}

bool evaluateOsc(uacpi_namespace_node* node, uint32_t control, bool query) {
  uint8_t capabilities[12] = {};
  write32(capabilities, query ? 1U : 0U);
  write32(capabilities + 4, OscSupport);
  write32(capabilities + 8, control);
  uacpi_data_view uuid{};
  uuid.const_bytes = OscUuid;
  uuid.length = sizeof(OscUuid);
  uacpi_data_view caps{};
  caps.const_bytes = capabilities;
  caps.length = sizeof(capabilities);
  uacpi_object* args[4] = {uacpi_object_create_buffer(uuid), uacpi_object_create_integer(1),
                           uacpi_object_create_integer(3), uacpi_object_create_buffer(caps)};
  bool complete = true;
  for (uacpi_object* arg : args) {
    if (!arg) {
      complete = false;
    }
  }
  uacpi_object* output = nullptr;
  if (complete) {
    uacpi_object_array array{args, 4};
    complete = uacpi_eval_buffer(node, "_OSC", &array, &output) == UACPI_STATUS_OK && output;
  }
  for (uacpi_object* arg : args) {
    if (arg) {
      uacpi_object_unref(arg);
    }
  }
  if (!complete) {
    if (output) {
      uacpi_object_unref(output);
    }
    return false;
  }
  uacpi_data_view response{};
  complete = uacpi_object_get_buffer(output, &response) == UACPI_STATUS_OK &&
             response.length == sizeof(capabilities) && response.const_bytes;
  if (complete) {
    const uint32_t status = read32(response.const_bytes);
    const uint32_t support = read32(response.const_bytes + 4);
    const uint32_t granted = read32(response.const_bytes + 8);
    complete = (status & ~1U) == 0 && bool(status & 1U) == query &&
               (support & OscSupport) == OscSupport && (granted & control) == control;
  }
  uacpi_object_unref(output);
  return complete;
}
}  // namespace

namespace PciFirmware {
bool discover() {
  LockGuard<Mutex> guard(g_Lock);
  if (g_Ready) {
    return true;
  }
  if (g_Failed) {
    return false;
  }
  if (uacpi_get_current_init_level() < UACPI_INIT_LEVEL_NAMESPACE_INITIALIZED) {
#if ARM64 || ARMV7
    VirtPciHost host{};
    if (!VirtDeviceTree::pciHost(host) || host.firstBus > host.lastBus || host.lastBus > 255) {
      return false;
    }
    Root& root = g_Roots[0].root;
    root.firstBus = host.firstBus;
    root.lastBus = host.lastBus;
    VirtPciWindow window{};
    for (size_t i = 0; VirtDeviceTree::pciWindow(i, window); ++i) {
      if (i == MaxWindows || !window.size || window.pciBase > ~uint64_t(0) - (window.size - 1) ||
          window.cpuBase > ~uint64_t(0) - (window.size - 1)) {
        root.windowCount = 0;
        return false;
      }
      root.windows[root.windowCount++] = {window.pciBase, window.cpuBase, window.size,
                                          window.space == 0x01000000, window.prefetchable};
    }
    g_RootCount = 1;
    g_Ready = true;
    NOTICE("PCI firmware: using device-tree root windows");
    return true;
#else
    return false;
#endif
  }
  DiscoveryContext context;
  const uacpi_status status =
      uacpi_namespace_for_each_child(uacpi_namespace_root(), readDevice, nullptr,
                                     UACPI_OBJECT_DEVICE_BIT, UACPI_MAX_DEPTH_ANY, &context);
  if (status != UACPI_STATUS_OK || context.failed || !g_RootCount) {
    g_RootCount = 0;
    g_Failed = true;
    WARNING("PCI firmware: root discovery failed");
    return false;
  }
  g_Ready = true;
  NOTICE("PCI firmware: discovered " << Dec << g_RootCount << " segment-zero root bridges");
  for (size_t i = 0; i < g_RootCount; ++i) {
    const RootState& root = g_Roots[i];
    addPciRoutingRoot(root.node, root.root.firstBus, root.root.lastBus);
  }
  return true;
}

const Root* rootForBus(uint8_t bus) {
  LockGuard<Mutex> guard(g_Lock);
  RootState* root = g_Ready ? findRoot(bus) : nullptr;
  return root ? &root->root : nullptr;
}

bool requestNativeControl(uint8_t bus, uint32_t bits) {
  if (!bits || (bits & ~NativeMask)) {
    return false;
  }
  LockGuard<Mutex> guard(g_Lock);
  RootState* root = g_Ready ? findRoot(bus) : nullptr;
  if (!root || !root->node || root->oscFailed) {
    return false;
  }
  const uint32_t requested = root->control | bits;
  if (requested == root->control) {
    return true;
  }
  if (!evaluateOsc(root->node, requested, true)) {
    return false;
  }
  if (!evaluateOsc(root->node, requested, false)) {
    root->oscFailed = true;
    return false;
  }
  root->control = requested;
  return true;
}

uint32_t nativeControl(uint8_t bus) {
  LockGuard<Mutex> guard(g_Lock);
  RootState* root = g_Ready ? findRoot(bus) : nullptr;
  return root ? root->control : 0;
}
}  // namespace PciFirmware
