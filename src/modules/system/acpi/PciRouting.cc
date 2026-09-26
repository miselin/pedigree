/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#include "PciRouting.h"

#include <config.h>

#if (X86 || X64) && MULTIPROCESSOR && ACPI
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/utilities/Vector.h"

#include "system/kernel/machine/mach_pc/Acpi.h"
#include "system/kernel/machine/mach_pc/Pic.h"
#include <uacpi/acpi.h>
#include <uacpi/resources.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

namespace {
struct Root {
  uacpi_namespace_node* node;
  uint8_t firstBus;
  uint8_t lastBus;
};

struct Link {
  uacpi_namespace_node* node;
  uint32_t index;
  AcpiPciRouting::Route route;
  bool valid = false;
};

Mutex g_Lock;
Vector<Root> g_Roots;
Vector<Link> g_Links;
bool g_Ready = false;

struct ResourceLookup {
  uint32_t wanted;
  uint32_t index = 0;
  uacpi_resource* found = nullptr;
  bool dependent = false;
};

uacpi_iteration_decision findResource(void* user, uacpi_resource* resource) {
  auto& lookup = *static_cast<ResourceLookup*>(user);
  if (resource->type == UACPI_RESOURCE_TYPE_START_DEPENDENT ||
      resource->type == UACPI_RESOURCE_TYPE_END_DEPENDENT) {
    lookup.dependent = true;
  }
  if (resource->type != UACPI_RESOURCE_TYPE_END_TAG) {
    if (lookup.index == lookup.wanted) {
      lookup.found = resource;
    }
    ++lookup.index;
  }
  return UACPI_ITERATION_DECISION_CONTINUE;
}

bool interruptResource(uacpi_resource* resource, size_t& count, bool& activeLow) {
  if (!resource) {
    return false;
  }
  uint8_t trigger = 0, polarity = 0, sharing = 0;
  if (resource->type == UACPI_RESOURCE_TYPE_IRQ) {
    const auto& irq = resource->irq;
    count = irq.num_irqs;
    trigger = irq.triggering;
    polarity = irq.polarity;
    sharing = irq.sharing;
  } else if (resource->type == UACPI_RESOURCE_TYPE_EXTENDED_IRQ) {
    const auto& irq = resource->extended_irq;
    if (irq.direction != UACPI_CONSUMER || irq.source.length) {
      return false;
    }
    count = irq.num_irqs;
    trigger = irq.triggering;
    polarity = irq.polarity;
    sharing = irq.sharing;
  } else {
    return false;
  }
  activeLow = polarity == UACPI_POLARITY_ACTIVE_LOW;
  return trigger == UACPI_TRIGGERING_LEVEL && polarity <= UACPI_POLARITY_ACTIVE_LOW &&
         sharing == UACPI_SHARED;
}

uint32_t interruptNumber(uacpi_resource* resource, size_t index) {
  return resource->type == UACPI_RESOURCE_TYPE_IRQ ? resource->irq.irqs[index]
                                                   : resource->extended_irq.irqs[index];
}

bool readCurrent(uacpi_namespace_node* node, uint32_t index, AcpiPciRouting::Route& route) {
  uacpi_resources* resources = nullptr;
  if (uacpi_get_current_resources(node, &resources) != UACPI_STATUS_OK || !resources) {
    return false;
  }
  ResourceLookup lookup{index};
  size_t count = 0;
  const bool valid = uacpi_for_each_resource(resources, findResource, &lookup) == UACPI_STATUS_OK &&
                     !lookup.dependent && interruptResource(lookup.found, count, route.activeLow) &&
                     count == 1;
  if (valid) {
    route.gsi = interruptNumber(lookup.found, 0);
  }
  uacpi_free_resources(resources);
  return valid;
}

bool resolveLink(uacpi_namespace_node* node, uint32_t index, AcpiPciRouting::Route& route) {
  for (const Link& link : g_Links) {
    if (link.node == node && link.index == index) {
      route = link.route;
      return link.valid;
    }
  }
  uint32_t status = 0;
  if (uacpi_eval_sta(node, &status) != UACPI_STATUS_OK ||
      !(status & ACPI_STA_RESULT_DEVICE_PRESENT)) {
    return false;
  }
  if (status & ACPI_STA_RESULT_DEVICE_ENABLED) {
    if (!readCurrent(node, index, route) ||
        !Pic::instance().reservePciGsi(route.gsi, route.activeLow)) {
      return false;
    }
    g_Links.pushBack({node, index, route, true});
    return true;
  }

  // Only select from a simple interrupt link template. Dependent alternatives
  // and multi-resource reconfiguration require a general ACPI resource allocator.
  uacpi_resources* possible = nullptr;
  if (index || uacpi_get_possible_resources(node, &possible) != UACPI_STATUS_OK || !possible) {
    return false;
  }
  uacpi_resources* previous = nullptr;
  ResourceLookup previousLookup{0};
  if (uacpi_get_current_resources(node, &previous) != UACPI_STATUS_OK || !previous ||
      uacpi_for_each_resource(previous, findResource, &previousLookup) != UACPI_STATUS_OK ||
      previousLookup.dependent || previousLookup.index != 1) {
    uacpi_free_resources(previous);
    uacpi_free_resources(possible);
    return false;
  }
  ResourceLookup lookup{0};
  size_t count = 0;
  bool selected = false;
  if (uacpi_for_each_resource(possible, findResource, &lookup) == UACPI_STATUS_OK &&
      !lookup.dependent && lookup.index == 1 && lookup.found->type == previousLookup.found->type &&
      (lookup.found->type == UACPI_RESOURCE_TYPE_IRQ
           ? previousLookup.found->irq.length_kind == UACPI_RESOURCE_LENGTH_KIND_FULL
           : lookup.found->type == UACPI_RESOURCE_TYPE_EXTENDED_IRQ &&
                 previousLookup.found->extended_irq.num_irqs == 1 &&
                 !previousLookup.found->extended_irq.source.length) &&
      interruptResource(lookup.found, count, route.activeLow)) {
    for (size_t i = 0; i < count; ++i) {
      route.gsi = interruptNumber(lookup.found, i);
      if (Pic::instance().reservePciGsi(route.gsi, route.activeLow)) {
        selected = true;
        break;
      }
    }
  }
  if (!selected) {
    uacpi_free_resources(previous);
    uacpi_free_resources(possible);
    return false;
  }
  // _SRS must retain the descriptor layout and encoded size reported by _CRS.
  if (lookup.found->type == UACPI_RESOURCE_TYPE_IRQ) {
    lookup.found->irq.length_kind = previousLookup.found->irq.length_kind;
    lookup.found->irq.num_irqs = 1;
    lookup.found->irq.irqs[0] = route.gsi;
  } else {
    lookup.found->extended_irq.num_irqs = 1;
    lookup.found->extended_irq.irqs[0] = route.gsi;
  }
  uacpi_free_resources(previous);

  // A failed _SRS may already have changed hardware. Never retry or expose that
  // link to another caller until its complete configuration is known.
  const size_t slot = g_Links.size();
  g_Links.pushBack({node, index, route, false});
  const uacpi_status result = uacpi_set_resources(node, possible);
  uacpi_free_resources(possible);
  AcpiPciRouting::Route current{};
  if (result != UACPI_STATUS_OK || uacpi_eval_sta(node, &status) != UACPI_STATUS_OK ||
      !(status & ACPI_STA_RESULT_DEVICE_PRESENT) || !(status & ACPI_STA_RESULT_DEVICE_ENABLED) ||
      !readCurrent(node, index, current) || current.gsi != route.gsi ||
      current.activeLow != route.activeLow) {
    WARNING("PCI INTx: interrupt-link configuration could not be verified");
    return false;
  }
  g_Links[slot].valid = true;
  NOTICE("PCI INTx: configured ACPI interrupt link on GSI " << Dec << route.gsi);
  return true;
}

struct ChildLookup {
  uint32_t address;
  uacpi_namespace_node* node = nullptr;
  bool valid = true;
};

uacpi_iteration_decision findChild(void* user, uacpi_namespace_node* node, uacpi_u32) {
  auto& lookup = *static_cast<ChildLookup*>(user);
  uint64_t address = 0;
  const auto status = uacpi_eval_simple_integer(node, "_ADR", &address);
  if (status == UACPI_STATUS_OK && address == lookup.address) {
    if (lookup.node) {
      lookup.valid = false;
      return UACPI_ITERATION_DECISION_BREAK;
    }
    lookup.node = node;
  }
  return UACPI_ITERATION_DECISION_CONTINUE;
}

bool routeInterrupt(Device* device, uint8_t pin, AcpiPciRouting::Route& route) {
  LockGuard<Mutex> guard(g_Lock);
  if (!g_Ready) {
    return false;
  }
  const Root* root = nullptr;
  for (const Root& candidate : g_Roots) {
    if (device->getPciBusPosition() >= candidate.firstBus &&
        device->getPciBusPosition() <= candidate.lastBus) {
      root = &candidate;
      break;
    }
  }
  if (!root) {
    return false;
  }
  Vector<Device*> path;
  Device* current = device;
  while (true) {
    path.pushBack(current);
    if (current->getPciBusPosition() == root->firstBus) {
      break;
    }
    Device* bus = current->getParent();
    current = bus ? bus->getParent() : nullptr;
    if (!current || current->getPciClassCode() != 6 || current->getPciSubclassCode() != 4 ||
        path.size() == 256) {
      return false;
    }
  }

  Vector<uacpi_namespace_node*> scopes;
  uacpi_namespace_node* scope = root->node;
  scopes.pushBack(scope);
  for (size_t i = path.size(); i > 1; --i) {
    if (scope) {
      Device* bridge = path[i - 1];
      ChildLookup lookup{uint32_t(bridge->getPciDevicePosition()) << 16 |
                         bridge->getPciFunctionNumber()};
      if (uacpi_namespace_for_each_child(scope, findChild, nullptr, UACPI_OBJECT_DEVICE_BIT, 1,
                                         &lookup) != UACPI_STATUS_OK ||
          !lookup.valid) {
        return false;
      }
      scope = lookup.node;
    }
    scopes.pushBack(scope);
  }

  for (size_t level = 0; level < path.size(); ++level) {
    current = path[level];
    scope = scopes[path.size() - 1 - level];
    uacpi_pci_routing_table* table = nullptr;
    const uacpi_status result =
        scope ? uacpi_get_pci_routing_table(scope, &table) : UACPI_STATUS_NOT_FOUND;
    if (result == UACPI_STATUS_OK && table) {
      const uacpi_pci_routing_table_entry* match = nullptr;
      bool valid = true;
      for (size_t i = 0; i < table->num_entries; ++i) {
        const auto& entry = table->entries[i];
        const uint32_t slot = entry.address >> 16;
        const uint32_t function = entry.address & 0xffff;
        if (entry.pin > 3 || slot > 31 || (function > 7 && function != 0xffff)) {
          valid = false;
          break;
        }
        if (slot == current->getPciDevicePosition() && entry.pin == pin - 1 &&
            (function == 0xffff || function == current->getPciFunctionNumber())) {
          if (match) {
            valid = false;
            break;
          }
          match = &entry;
        }
      }
      valid = valid && match;
      if (valid) {
        if (match->source) {
          valid = resolveLink(match->source, match->index, route);
        } else {
          route = {match->index, true};
          valid = Pic::instance().reservePciGsi(route.gsi, route.activeLow);
        }
      }
      uacpi_free_pci_routing_table(table);
      return valid;
    }
    if (result != UACPI_STATUS_NOT_FOUND) {
      return false;
    }
    pin = static_cast<uint8_t>(((pin - 1 + current->getPciDevicePosition()) & 3) + 1);
  }
  return false;
}
}  // namespace

void addPciRoutingRoot(uacpi_namespace_node* node, uint8_t firstBus, uint8_t lastBus) {
  g_Roots.pushBack({node, firstBus, lastBus});
}

void initialisePciRouting() {
  if (!Pic::instance().hasIoApic() || !g_Roots.size()) {
    return;
  }
  LockGuard<Mutex> guard(g_Lock);
  Acpi::instance().setPciInterruptRouter(routeInterrupt);
  if (uacpi_set_interrupt_model(UACPI_INTERRUPT_MODEL_IOAPIC) != UACPI_STATUS_OK) {
    WARNING("PCI INTx: firmware could not select APIC routing");
    return;
  }
  g_Ready = true;
  NOTICE("PCI INTx: AML routing enabled in APIC mode");
}
#else
void addPciRoutingRoot(uacpi_namespace_node*, uint8_t, uint8_t) {}
void initialisePciRouting() {}
#endif
