/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"

namespace {
constexpr size_t PageCount = 128;

bool check(bool condition, const char* detail) {
  if (!condition) {
    ERROR("HOSTED-WAIT-TEST: FAIL hosted-vas-mapping-index: " << detail);
  }
  return condition;
}

bool mappingMatches(VirtualAddressSpace& space, uintptr_t address, physical_uintptr_t expected,
                    size_t expectedFlags) {
  if (!check(space.isMapped(reinterpret_cast<void*>(address)), "a mapping disappeared")) {
    return false;
  }
  physical_uintptr_t physical = 0;
  size_t flags = 0;
  space.getMapping(reinterpret_cast<void*>(address + sizeof(uint32_t)), physical, flags);
  return check(physical == expected && flags == expectedFlags,
               "mapping lookup returned different backing or permissions");
}

uint32_t contents(size_t owner, size_t page) {
  return 0x13570000U + static_cast<uint32_t>((owner * PageCount) + page);
}

bool checkUserPages(VirtualAddressSpace& space, uintptr_t base, physical_uintptr_t* pages,
                    size_t owner) {
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  bool passed = true;
  for (size_t i = 0; i < PageCount; ++i) {
    const uintptr_t address = base + (i * pageSize);
    size_t flags = VirtualAddressSpace::Write;
    if (owner == 1 && i == 0) {
      flags |= VirtualAddressSpace::WriteProtected;
    } else if (owner == 1 && i == 1) {
      flags = VirtualAddressSpace::NoAccess;
    } else if (owner == 1 && i == 2) {
      flags = 0;
    }
    const bool mapped = mappingMatches(space, address, pages[i], flags);
    passed &= mapped;
    if (mapped && !(flags & VirtualAddressSpace::NoAccess)) {
      auto* first = reinterpret_cast<volatile uint32_t*>(address);
      auto* last = reinterpret_cast<volatile uint32_t*>(address + pageSize - sizeof(uint32_t));
      passed &= check(*first == contents(owner, i) && *last == ~contents(owner, i),
                      "an address-space switch exposed another space's page contents");
    }
  }
  return passed;
}

bool checkKernelPages(VirtualAddressSpace& space, uintptr_t* addresses, physical_uintptr_t* pages) {
  bool passed = true;
  for (size_t i = 0; i < 2; ++i) {
    const bool mapped =
        mappingMatches(space, addresses[i], pages[i],
                       VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write);
    passed &= mapped;
    if (mapped) {
      passed &= check(*reinterpret_cast<volatile uint32_t*>(addresses[i]) == contents(2, i),
                      "an address-space switch changed a global kernel mapping");
    }
  }
  return passed;
}

bool releasePages(VirtualAddressSpace& space, uintptr_t base, physical_uintptr_t* pages,
                  bool* mapped) {
  Processor::switchAddressSpace(space);
  bool passed = true;
  for (size_t i = 0; i < PageCount; ++i) {
    if (mapped[i]) {
      physical_uintptr_t physical = 0;
      size_t flags = 0;
      const bool detached = space.detachMapping(
          reinterpret_cast<void*>(base + (i * PhysicalMemoryManager::getPageSize())), physical,
          flags);
      if (!check(detached && physical == pages[i], "cleanup could not detach the owned page")) {
        passed = false;
        continue;
      }
      mapped[i] = false;
    }
    if (!check(PhysicalMemoryManager::pageReferenceCountForTest(pages[i]) != 0,
               "cleanup found a physical page that was already released")) {
      passed = false;
      continue;
    }
    PhysicalMemoryManager::instance().freePage(pages[i]);
  }
  return passed;
}
}  // namespace

bool runHostedVasRegressions() {
  VirtualAddressSpace& original = Processor::information().getVirtualAddressSpace();
  VirtualAddressSpace& kernel = VirtualAddressSpace::getKernelAddressSpace();
  PhysicalMemoryManager& memory = PhysicalMemoryManager::instance();
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  VirtualAddressSpace* spaces[2] = {VirtualAddressSpace::create(), VirtualAddressSpace::create()};
  physical_uintptr_t pages[2][PageCount];
  bool mapped[2][PageCount] = {};
  physical_uintptr_t kernelPages[2] = {memory.allocatePage(), memory.allocatePage()};
  bool kernelMapped[2] = {};
  for (size_t owner = 0; owner < 2; ++owner) {
    for (size_t i = 0; i < PageCount; ++i) {
      pages[owner][i] = memory.allocatePage();
    }
  }

  // These temporary spaces have no Process. A timer must not schedule the
  // current Thread while its recorded address space differs from the live one.
  const bool interrupts = Processor::getInterrupts();
  Processor::setInterrupts(false);

  uintptr_t base = original.getDynamicStart() + (16 * 1024 * 1024);
  bool available = false;
  while (!available) {
    available = true;
    for (size_t i = 0; i < PageCount + 2; ++i) {
      if (original.isMapped(reinterpret_cast<void*>(base + (i * pageSize)))) {
        base += (PageCount + 2) * pageSize;
        available = false;
        break;
      }
    }
  }
  uintptr_t kernelAddresses[2] = {base + ((PageCount + 1) * pageSize),
                                  kernel.getKernelHeapEnd() + (16 * 1024 * 1024)};
  while (kernel.isMapped(reinterpret_cast<void*>(kernelAddresses[1]))) {
    kernelAddresses[1] += pageSize;
  }

  bool passed = true;
  for (size_t owner = 0; owner < 2; ++owner) {
    Processor::switchAddressSpace(*spaces[owner]);
    for (size_t i = 0; i < PageCount; ++i) {
      const uintptr_t address = base + (i * pageSize);
      mapped[owner][i] = spaces[owner]->map(pages[owner][i], reinterpret_cast<void*>(address),
                                            VirtualAddressSpace::Write);
      passed &= check(mapped[owner][i], "could not create the userspace fixture");
      if (mapped[owner][i]) {
        *reinterpret_cast<volatile uint32_t*>(address) = contents(owner, i);
        *reinterpret_cast<volatile uint32_t*>(address + pageSize - sizeof(uint32_t)) =
            ~contents(owner, i);
      }
    }
  }

  if (passed) {
    spaces[1]->setFlags(reinterpret_cast<void*>(base + sizeof(uint32_t)),
                        VirtualAddressSpace::Write | VirtualAddressSpace::WriteProtected);
    passed &= check(spaces[1]->trySetFlags(reinterpret_cast<void*>(base + pageSize),
                                           VirtualAddressSpace::NoAccess),
                    "trySetFlags could not protect an existing page");
    spaces[1]->setFlags(reinterpret_cast<void*>(base + (2 * pageSize)), 0);
    uint32_t value = 0;
    uint32_t expected = contents(1, 0);
    bool exchanged = false;
    passed &= check(spaces[1]->tryReadUser32(base, value) && value == contents(1, 0) &&
                        !spaces[1]->tryWriteUser32(base, 0) &&
                        !spaces[1]->tryCompareExchangeUser32(base, expected, 0, exchanged),
                    "a write-protected page did not retain read-only access");
    passed &= check(!spaces[1]->tryReadUser32(base + pageSize, value) &&
                        !spaces[1]->tryWriteUser32(base + pageSize, 0) &&
                        !spaces[1]->tryWriteUser32(base + (2 * pageSize), 0),
                    "NoAccess or read-only permissions allowed a user access");

    Processor::switchAddressSpace(*spaces[0]);
    for (size_t i = 0; i < 2; ++i) {
      kernelMapped[i] =
          spaces[0]->map(kernelPages[i], reinterpret_cast<void*>(kernelAddresses[i]),
                         VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write);
      passed &= check(kernelMapped[i], "could not create the kernel mapping fixture");
      if (kernelMapped[i]) {
        *reinterpret_cast<volatile uint32_t*>(kernelAddresses[i]) = contents(2, i);
      }
    }
  }

  if (passed) {
    VirtualAddressSpace* route[] = {spaces[0], &kernel, spaces[1], &kernel, spaces[0]};
    for (VirtualAddressSpace* space : route) {
      Processor::switchAddressSpace(*space);
      passed &= checkKernelPages(*space, kernelAddresses, kernelPages);
      if (space != &kernel) {
        const size_t owner = space == spaces[0] ? 0 : 1;
        passed &= checkUserPages(*space, base, pages[owner], owner);
      }
    }

    Processor::switchAddressSpace(*spaces[1]);
    passed &= check(
        !spaces[0]->map(pages[0][1], reinterpret_cast<void*>(base), VirtualAddressSpace::Write) &&
            *reinterpret_cast<volatile uint32_t*>(base) == contents(1, 0),
        "an inactive duplicate map replaced the current space's backing");
    Processor::switchAddressSpace(kernel);
    passed &= check(
        !spaces[0]->map(pages[1][0], reinterpret_cast<void*>(base), VirtualAddressSpace::Write),
        "an inactive duplicate map was accepted without a live host mapping");
    passed &= checkKernelPages(kernel, kernelAddresses, kernelPages);
    Processor::switchAddressSpace(*spaces[0]);
    passed &= checkUserPages(*spaces[0], base, pages[0], 0);

    uint32_t expected = contents(0, 0) + 1;
    bool exchanged = false;
    passed &=
        check(spaces[0]->tryWriteUser32(base, expected) &&
                  *reinterpret_cast<volatile uint32_t*>(base) == expected &&
                  spaces[0]->tryCompareExchangeUser32(base, expected, contents(0, 0), exchanged) &&
                  exchanged && *reinterpret_cast<volatile uint32_t*>(base) == contents(0, 0),
              "indexed writes or compare-exchange did not reach the mapped page");
    const uintptr_t pointerAddress = base + (2 * sizeof(uintptr_t));
    *reinterpret_cast<volatile uintptr_t*>(pointerAddress) = base + pageSize;
    uintptr_t pointer = 0;
    passed &=
        check(spaces[0]->tryReadUserPointer(pointerAddress, pointer) && pointer == base + pageSize,
              "indexed pointer read did not reach the mapped page");

    const uintptr_t absent = base + (PageCount * pageSize);
    passed &= check(
        !spaces[0]->isMapped(reinterpret_cast<void*>(absent)) &&
            !spaces[0]->trySetFlags(reinterpret_cast<void*>(absent), VirtualAddressSpace::Write),
        "an absent page was found in the mapping index");
    size_t tablePages = 99;
    passed &= check(!spaces[0]->tryMapUserPage(pages[1][0], reinterpret_cast<void*>(base),
                                               VirtualAddressSpace::Write, &tablePages) &&
                        tablePages == 0 &&
                        mappingMatches(*spaces[0], base, pages[0][0], VirtualAddressSpace::Write),
                    "duplicate insertion changed an existing mapping");

    for (size_t i = 0; i < PageCount; i += 2) {
      void* address = reinterpret_cast<void*>(base + (i * pageSize));
      spaces[0]->unmap(address);
      mapped[0][i] = false;
      passed &= check(!spaces[0]->isMapped(address), "unmap left a stale mapping index entry");
    }
    for (size_t remaining = PageCount; remaining; remaining -= 2) {
      const size_t i = remaining - 2;
      mapped[0][i] = spaces[0]->map(pages[0][i], reinterpret_cast<void*>(base + (i * pageSize)),
                                    VirtualAddressSpace::Write);
      passed &= check(mapped[0][i], "a deleted mapping index entry could not be reused");
    }
  }

  if (passed) {
    void* address = reinterpret_cast<void*>(base + (5 * pageSize));
    physical_uintptr_t detachedPhysical = 0;
    size_t detachedFlags = 0;
    passed &= check(!spaces[0]->detachMapping(address, detachedPhysical, detachedFlags,
                                              VirtualAddressSpace::KernelMode) &&
                        mappingMatches(*spaces[0], reinterpret_cast<uintptr_t>(address),
                                       pages[0][5], VirtualAddressSpace::Write),
                    "a rejected detach changed the mapping");
    const bool detached = spaces[0]->detachMapping(address, detachedPhysical, detachedFlags,
                                                   VirtualAddressSpace::Write);
    if (detached) {
      mapped[0][5] = false;
    }
    passed &=
        check(detached && detachedPhysical == pages[0][5] &&
                  detachedFlags == VirtualAddressSpace::Write && !spaces[0]->isMapped(address),
              "detach did not return and remove the original mapping");
    if (detached) {
      mapped[0][5] = spaces[0]->map(pages[0][5], address, VirtualAddressSpace::Write);
      passed &= check(mapped[0][5], "a detached mapping could not be reinserted");
    }

    address = reinterpret_cast<void*>(base + ((PageCount - 1) * pageSize));
    spaces[0]->setFlags(address, VirtualAddressSpace::NoAccess);
    passed &= check(!spaces[0]->tryDetachUserPage(address, pages[1][PageCount - 1]),
                    "tryDetachUserPage accepted the wrong physical page");
    const bool userDetached = spaces[0]->tryDetachUserPage(address, pages[0][PageCount - 1]);
    if (userDetached) {
      mapped[0][PageCount - 1] = false;
    }
    passed &= check(userDetached && !spaces[0]->isMapped(address),
                    "tryDetachUserPage left an inaccessible mapping indexed");
    if (userDetached) {
      mapped[0][PageCount - 1] =
          spaces[0]->tryMapUserPage(pages[0][PageCount - 1], address, VirtualAddressSpace::Write);
      passed &= check(mapped[0][PageCount - 1], "tryMapUserPage could not reuse a detached entry");
    }
  }

  VirtualAddressSpace* clone = nullptr;
  if (passed) {
    passed &= checkUserPages(*spaces[0], base, pages[0], 0);
    clone = spaces[0]->clone(false);
    passed &= check(clone != nullptr, "could not clone the populated mapping index");
    if (clone) {
      Processor::switchAddressSpace(*clone);
      passed &= checkUserPages(*clone, base, pages[0], 0);
      passed &= checkKernelPages(*clone, kernelAddresses, kernelPages);
      for (size_t i = 0; i < PageCount; ++i) {
        passed &= check(PhysicalMemoryManager::pageReferenceCountForTest(pages[0][i]) == 2,
                        "clone did not retain exactly two owners for a private page");
      }
      if (passed) {
        *reinterpret_cast<volatile uint32_t*>(base) = 0x87654321U;
        Processor::switchAddressSpace(kernel);
        Processor::switchAddressSpace(*spaces[0]);
        passed &= check(*reinterpret_cast<volatile uint32_t*>(base) == 0x87654321U,
                        "clone(false) did not preserve writable aliasing");
        *reinterpret_cast<volatile uint32_t*>(base) = contents(0, 0);
      }
      bool cloneMapped[PageCount];
      for (size_t i = 0; i < PageCount; ++i) {
        cloneMapped[i] = true;
      }
      passed &= releasePages(*clone, base, pages[0], cloneMapped);
      for (size_t i = 0; i < PageCount; ++i) {
        passed &= check(PhysicalMemoryManager::pageReferenceCountForTest(pages[0][i]) == 1,
                        "clone cleanup changed the source's physical page ownership");
      }
    }
  }

  if (passed) {
    Processor::switchAddressSpace(*spaces[0]);
    delete clone;
    clone = nullptr;
    // A retired alias leaves an explicit reference in the hosted PMM. Use
    // freshly allocated pages so this fixture tests one private clone lifetime.
    passed &= releasePages(*spaces[0], base, pages[0], mapped[0]);
    if (passed) {
      for (size_t i = 0; i < PageCount; ++i) {
        pages[0][i] = memory.allocatePage();
      }
      for (size_t i = 0; i < PageCount; ++i) {
        const uintptr_t address = base + (i * pageSize);
        mapped[0][i] = spaces[0]->map(pages[0][i], reinterpret_cast<void*>(address),
                                      VirtualAddressSpace::Write);
        passed &= check(mapped[0][i], "could not recreate fresh pages for copy-on-write");
        if (mapped[0][i]) {
          *reinterpret_cast<volatile uint32_t*>(address) = contents(0, i);
          *reinterpret_cast<volatile uint32_t*>(address + pageSize - sizeof(uint32_t)) =
              ~contents(0, i);
        }
      }
    }
  }

  if (passed) {
    spaces[0]->setFlags(reinterpret_cast<void*>(base + pageSize), 0);
    clone = spaces[0]->clone(true);
    passed &= check(clone != nullptr, "could not clone the mapping index with copy-on-write");
    if (clone) {
      Processor::switchAddressSpace(*clone);
      const bool writableCow =
          mappingMatches(*clone, base, pages[0][0], VirtualAddressSpace::CopyOnWrite);
      passed &= writableCow;
      passed &= mappingMatches(*spaces[0], base, pages[0][0], VirtualAddressSpace::CopyOnWrite);
      passed &=
          mappingMatches(*clone, base + pageSize, pages[0][1],
                         VirtualAddressSpace::CopyOnWrite | VirtualAddressSpace::WriteProtected);
      passed &=
          check(!clone->tryWriteUser32(base, 0) &&
                    !clone->handleCopyOnWriteFault(reinterpret_cast<void*>(base + pageSize), false),
                "copy-on-write bypassed a pending split or read-only protection");

      physical_uintptr_t replacement = 0;
      size_t replacementFlags = 0;
      const bool resolved =
          writableCow && clone->handleCopyOnWriteFault(reinterpret_cast<void*>(base), false);
      const bool replacementMapped = resolved && clone->isMapped(reinterpret_cast<void*>(base));
      passed &= check(replacementMapped,
                      "indexed copy-on-write lookup could not resolve a writable page");
      if (replacementMapped) {
        clone->getMapping(reinterpret_cast<void*>(base), replacement, replacementFlags);
        const bool privatePage =
            replacement != pages[0][0] && replacementFlags == VirtualAddressSpace::Write;
        passed &= check(privatePage, "copy-on-write did not publish an independent writable page");
        if (privatePage) {
          auto* bytes = reinterpret_cast<volatile uint32_t*>(base);
          passed &=
              check(*bytes == contents(0, 0), "copy-on-write lost the original page contents");
          *bytes = 0xABCDEF01U;
          Processor::switchAddressSpace(*spaces[0]);
          if (mappingMatches(*spaces[0], base, pages[0][0], VirtualAddressSpace::CopyOnWrite)) {
            passed &= check(*bytes == contents(0, 0), "the child write changed the source page");
          } else {
            passed = false;
          }
          Processor::switchAddressSpace(*clone);
          if (mappingMatches(*clone, base, replacement, VirtualAddressSpace::Write)) {
            passed &=
                check(*bytes == 0xABCDEF01U, "the source switch lost the child's private page");
          } else {
            passed = false;
          }
        }
      }

      clone->revertToKernelAddressSpace();
      uint32_t value = 0;
      passed &= check(!clone->tryReadUser32(base, value), "revert left an accessible index entry");
      for (size_t i = 0; i < PageCount; ++i) {
        passed &= check(!clone->isMapped(reinterpret_cast<void*>(base + (i * pageSize))) &&
                            PhysicalMemoryManager::pageReferenceCountForTest(pages[0][i]) == 1,
                        "revert retained a userspace mapping or changed source ownership");
      }
      if (replacement && replacement != pages[0][0]) {
        passed &= check(PhysicalMemoryManager::pageReferenceCountForTest(replacement) == 0,
                        "revert leaked the private copy-on-write page");
      }
      passed &= checkKernelPages(*clone, kernelAddresses, kernelPages);

      const physical_uintptr_t reused = memory.allocatePage();
      const bool remapped =
          clone->map(reused, reinterpret_cast<void*>(base), VirtualAddressSpace::Write);
      passed &= check(remapped, "revert left an index entry that blocked remapping");
      if (remapped) {
        passed &= mappingMatches(*clone, base, reused, VirtualAddressSpace::Write);
        passed &= check(clone->tryWriteUser32(base, contents(3, 0)) &&
                            clone->tryReadUser32(base, value) && value == contents(3, 0),
                        "a remapped page retained stale copy-on-write permissions or backing");
        clone->revertToKernelAddressSpace();
      } else {
        memory.freePage(reused);
      }
      passed &= check(PhysicalMemoryManager::pageReferenceCountForTest(reused) == 0,
                      "the remapped fixture leaked its physical page");
    }
  }

  for (size_t owner = 0; owner < 2; ++owner) {
    passed &= releasePages(*spaces[owner], base, pages[owner], mapped[owner]);
    for (size_t i = 0; i < PageCount; ++i) {
      passed &= check(PhysicalMemoryManager::pageReferenceCountForTest(pages[owner][i]) == 0,
                      "userspace fixture leaked a physical page");
    }
  }
  Processor::switchAddressSpace(kernel);
  for (size_t i = 0; i < 2; ++i) {
    if (kernelMapped[i]) {
      kernel.unmap(reinterpret_cast<void*>(kernelAddresses[i]));
    }
    memory.freePage(kernelPages[i]);
  }
  Processor::switchAddressSpace(original);
  delete clone;
  delete spaces[0];
  delete spaces[1];
  Processor::setInterrupts(interrupts);

  if (passed) {
    NOTICE("HOSTED-WAIT-TEST: PASS hosted-vas-mapping-index");
  }
  return passed;
}
