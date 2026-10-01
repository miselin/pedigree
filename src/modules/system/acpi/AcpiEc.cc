/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "AcpiEc.h"

#if X86 || X64
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/time/Time.h"

#include <uacpi/namespace.h>
#include <uacpi/opregion.h>
#include <uacpi/resources.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

namespace {
constexpr size_t MaxControllers = 4;
constexpr uint8_t OutputFull = 1;
constexpr uint8_t InputFull = 2;
constexpr uint8_t ReadCommand = 0x80;
constexpr uint8_t WriteCommand = 0x81;
constexpr auto EcTimeout = 100 * Time::Multiplier::Millisecond;
constexpr const char* EcIds[] = {"PNP0C09", nullptr};

struct Ec {
  uacpi_namespace_node* node = nullptr;
  uint16_t dataPort = 0;
  uint16_t commandPort = 0;
  Mutex mutex;
};

Ec controllers[MaxControllers];
size_t controllerCount = 0;
bool installed = false;

uint8_t readPort(uint16_t port) {
  uint8_t value;
  asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
  return value;
}

void writePort(uint16_t port, uint8_t value) {
  asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

bool waitStatus(const Ec& ec, uint8_t mask, uint8_t expected) {
  const auto deadline = Time::getTicks() + EcTimeout;
  do {
    if ((readPort(ec.commandPort) & mask) == expected) {
      return true;
    }
    Processor::pause();
  } while (Time::getTicks() < deadline);
  return (readPort(ec.commandPort) & mask) == expected;
}

bool readByte(const Ec& ec, uint8_t offset, uint8_t& value) {
  if (!waitStatus(ec, InputFull | OutputFull, 0)) {
    return false;
  }
  writePort(ec.commandPort, ReadCommand);
  if (!waitStatus(ec, InputFull, 0)) {
    return false;
  }
  writePort(ec.dataPort, offset);
  if (!waitStatus(ec, OutputFull, OutputFull)) {
    return false;
  }
  value = readPort(ec.dataPort);
  return true;
}

bool writeByte(const Ec& ec, uint8_t offset, uint8_t value) {
  if (!waitStatus(ec, InputFull | OutputFull, 0)) {
    return false;
  }
  writePort(ec.commandPort, WriteCommand);
  if (!waitStatus(ec, InputFull, 0)) {
    return false;
  }
  writePort(ec.dataPort, offset);
  if (!waitStatus(ec, InputFull, 0)) {
    return false;
  }
  writePort(ec.dataPort, value);
  return waitStatus(ec, InputFull, 0);
}

uacpi_status regionOperation(uacpi_region_op operation, uacpi_handle opaque) {
  if (operation == UACPI_REGION_OP_ATTACH) {
    const auto* data = static_cast<uacpi_region_attach_data*>(opaque);
    return data->generic_info.base < 256 &&
                   data->generic_info.length <= 256 - data->generic_info.base
               ? UACPI_STATUS_OK
               : UACPI_STATUS_INVALID_ARGUMENT;
  }
  if (operation == UACPI_REGION_OP_DETACH) {
    return UACPI_STATUS_OK;
  }
  if (operation != UACPI_REGION_OP_READ && operation != UACPI_REGION_OP_WRITE) {
    return UACPI_STATUS_INVALID_ARGUMENT;
  }

  auto* data = static_cast<uacpi_region_rw_data*>(opaque);
  auto* ec = static_cast<Ec*>(data->handler_context);
  if (!ec || !data->byte_width || data->byte_width > 8 || data->offset > 255 ||
      data->byte_width > 256 - data->offset) {
    return UACPI_STATUS_INVALID_ARGUMENT;
  }

  LockGuard<Mutex> guard(ec->mutex);
  if (operation == UACPI_REGION_OP_READ) {
    data->value = 0;
  }
  for (uint8_t i = 0; i < data->byte_width; ++i) {
    const uint8_t address = static_cast<uint8_t>(data->offset + i);
    uint8_t value = 0;
    const bool ok = operation == UACPI_REGION_OP_READ
                        ? readByte(*ec, address, value)
                        : writeByte(*ec, address, static_cast<uint8_t>(data->value >> (i * 8)));
    if (!ok) {
      ERROR("ACPI EC: timed out at offset " << Hex << static_cast<unsigned>(address));
      return UACPI_STATUS_HARDWARE_TIMEOUT;
    }
    if (operation == UACPI_REGION_OP_READ) {
      data->value |= static_cast<uint64_t>(value) << (i * 8);
    }
  }
  return UACPI_STATUS_OK;
}

uacpi_iteration_decision findController(void*, uacpi_namespace_node* node, uacpi_u32) {
  if (controllerCount < MaxControllers && uacpi_device_matches_pnp_id(node, EcIds)) {
    controllers[controllerCount++].node = node;
  }
  return UACPI_ITERATION_DECISION_CONTINUE;
}

uacpi_iteration_decision findPort(void* opaque, uacpi_resource* resource) {
  auto* ec = static_cast<Ec*>(opaque);
  uint16_t port = 0;
  if (resource->type == UACPI_RESOURCE_TYPE_IO) {
    if (resource->io.minimum != resource->io.maximum || resource->io.length != 1) {
      return UACPI_ITERATION_DECISION_CONTINUE;
    }
    port = resource->io.minimum;
  } else if (resource->type == UACPI_RESOURCE_TYPE_FIXED_IO) {
    if (resource->fixed_io.length != 1) {
      return UACPI_ITERATION_DECISION_CONTINUE;
    }
    port = resource->fixed_io.address;
  }
  if (port) {
    if (!ec->dataPort) {
      ec->dataPort = port;
    } else if (!ec->commandPort) {
      ec->commandPort = port;
    }
  }
  return UACPI_ITERATION_DECISION_CONTINUE;
}
}  // namespace

void installAcpiEcHandlersForShutdown() {
  if (installed) {
    return;
  }
  installed = true;
  const auto status =
      uacpi_namespace_for_each_child(uacpi_namespace_root(), findController, nullptr,
                                     UACPI_OBJECT_DEVICE_BIT, UACPI_MAX_DEPTH_ANY, nullptr);
  if (status != UACPI_STATUS_OK) {
    WARNING("ACPI EC: device discovery failed: " << uacpi_status_to_string(status));
    return;
  }
  if (!controllerCount) {
    WARNING("ACPI EC: no PNP0C09 controller found");
  }

  for (size_t i = 0; i < controllerCount; ++i) {
    Ec& ec = controllers[i];
    uacpi_resources* resources = nullptr;
    const auto resourceStatus = uacpi_get_current_resources(ec.node, &resources);
    if (resourceStatus != UACPI_STATUS_OK || !resources) {
      if (resources) {
        uacpi_free_resources(resources);
      }
      WARNING("ACPI EC: current resources unavailable: " << uacpi_status_to_string(resourceStatus));
      continue;
    }
    const auto parseStatus = uacpi_for_each_resource(resources, findPort, &ec);
    uacpi_free_resources(resources);
    if (parseStatus != UACPI_STATUS_OK || !ec.dataPort || !ec.commandPort ||
        ec.dataPort == ec.commandPort) {
      WARNING("ACPI EC: invalid data and command ports");
      continue;
    }

    uint64_t globalLock = 0;
    const auto lockStatus = uacpi_eval_simple_integer(ec.node, "_GLK", &globalLock);
    if ((lockStatus != UACPI_STATUS_OK && lockStatus != UACPI_STATUS_NOT_FOUND) || globalLock) {
      WARNING("ACPI EC: shared controller needs global-lock support");
      continue;
    }

    // Firmware may place an EC operation region outside the EC device's
    // namespace subtree. The first controller is the system-wide default;
    // local handlers below override it for any additional controllers.
    const auto installStatus = uacpi_install_address_space_handler(
        i ? ec.node : uacpi_namespace_root(), UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER,
        regionOperation, &ec);
    if (installStatus == UACPI_STATUS_OK) {
      NOTICE("ACPI EC: shutdown handler on data port " << Hex << ec.dataPort << ", command port "
                                                       << ec.commandPort);
    } else {
      WARNING("ACPI EC: handler installation failed: " << uacpi_status_to_string(installStatus));
    }
  }
}
#else
void installAcpiEcHandlersForShutdown() {}
#endif
