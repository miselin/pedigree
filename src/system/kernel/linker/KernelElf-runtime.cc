/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/linker/KernelElf.h"
#include "pedigree/kernel/linker/ModuleImage.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"
#include "pedigree/kernel/utilities/StringView.h"

#if HOSTED
namespace __pedigree_hosted {
#include <dlfcn.h>
}
#endif

struct RuntimeModuleSlot {
  enum State { Unavailable, Free, Reserved, Live, Quarantined } state = Unavailable;
  Module module;
  ModuleImage plan;
  uint8_t source[ModuleImage::MaximumImageBytes];
  uintptr_t resolved[ModuleImage::MaximumSymbols];
  const char* dependencies[ModuleImage::MaximumDependencies + 1];
  const char* optionalDependencies[ModuleImage::MaximumDependencies + 1];
  uintptr_t base = 0;
  bool lifecycleStarted = false;
  bool lifecycleComplete = true;
};

namespace {
constexpr size_t SlotCount = 4;
// Bulky source buffers must not extend the early bootstrap image mapping.
RuntimeModuleSlot* slots[SlotCount] = {};
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
size_t failProtectionAfter = ~size_t{0};
#endif

bool protect(uintptr_t address, size_t flags) {
#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
  if (failProtectionAfter != ~size_t{0} && failProtectionAfter-- == 0) {
    failProtectionAfter = ~size_t{0};
    return false;
  }
#endif
  return VirtualAddressSpace::getKernelAddressSpace().trySetFlags(
      reinterpret_cast<void*>(address), flags | VirtualAddressSpace::KernelMode);
}

bool writable(RuntimeModuleSlot& slot) {
  bool result = true;
  for (size_t offset = 0; offset < ModuleImage::MaximumMappedBytes;
       offset += PhysicalMemoryManager::getPageSize()) {
    // Keep restoring subsequent pages after an error. A partially restored
    // slot stays quarantined and is never written or handed to another load.
    if (!protect(slot.base + offset, VirtualAddressSpace::Write))
      result = false;
  }
  return result;
}

bool seal(RuntimeModuleSlot& slot) {
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  for (size_t offset = 0; offset < ModuleImage::MaximumMappedBytes; offset += pageSize) {
    size_t flags = 0;
    for (size_t i = 0; i < slot.plan.segmentCount; ++i) {
      const auto& segment = slot.plan.segments[i];
      if (segment.memsz && offset >= (segment.vaddr & ~(pageSize - 1)) &&
          offset < segment.vaddr + segment.memsz) {
        if (segment.flags & PF_W)
          flags |= VirtualAddressSpace::Write;
        if (segment.flags & PF_X)
          flags |= VirtualAddressSpace::Execute;
      }
    }
    if (!protect(slot.base + offset, flags))
      return false;
  }
  return true;
}

bool addSigned(uintptr_t value, int64_t addend, uintptr_t& result) {
  if (addend < 0) {
    const uint64_t magnitude = uint64_t{0} - static_cast<uint64_t>(addend);
    if (value < magnitude)
      return false;
    result = value - magnitude;
  } else {
    if (value > ~uintptr_t{0} - static_cast<uint64_t>(addend))
      return false;
    result = value + addend;
  }
  return true;
}
}  // namespace

KernelElf::RuntimeLoad::RuntimeLoad() : lifetime(), slot(nullptr), length(0) {}
KernelElf::RuntimeLoad::~RuntimeLoad() {
  if (slot)
    KernelElf::instance().abandonRuntimeModuleLoad(*this);
}
uint8_t* KernelElf::RuntimeLoad::data() const {
  return slot ? slot->source : nullptr;
}

bool KernelElf::prepareRuntimeModules() {
#if STATIC_DRIVERS || !BITS_64 || (!X64 && !HOSTED)
  return false;
#endif
  if (!beginModuleLoad())
    return false;
  if (m_RuntimeModulesPrepared) {
    bool available = false;
    for (const auto* slot : slots) {
      if (slot &&
          (slot->state == RuntimeModuleSlot::Free || slot->state == RuntimeModuleSlot::Live))
        available = true;
    }
    finishModuleLoad();
    return available;
  }
  auto& space = VirtualAddressSpace::getKernelAddressSpace();
  if (!m_ModuleAllocatorInitialised) {
    const uintptr_t start = space.getKernelModulesStart();
    m_ModuleAllocator.free(start, space.getKernelModulesEnd() - start);
    m_ModuleAllocatorInitialised = true;
  }
  // These are trusted boot allocations. No syscall can grow this arena or
  // trigger the kernel's infallible heap/physical-page allocation paths.
  lockModules();
  m_Modules.reserve(m_Modules.count() + SlotCount + 1, true);
  unlockModules();
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  size_t available = 0;
  for (size_t index = 0; index < SlotCount; ++index) {
    RuntimeModuleSlot* prepared = new RuntimeModuleSlot;
    if (!prepared)
      break;
    auto& slot = *prepared;
    if (!m_ModuleAllocator.allocate(ModuleImage::MaximumMappedBytes, slot.base)) {
      delete prepared;
      break;
    }
    size_t mapped = 0;
    for (; mapped < ModuleImage::MaximumMappedBytes; mapped += pageSize) {
      const physical_uintptr_t page = PhysicalMemoryManager::instance().allocatePage();
      if (!space.map(page, reinterpret_cast<void*>(slot.base + mapped),
                     VirtualAddressSpace::KernelMode | VirtualAddressSpace::Write)) {
        PhysicalMemoryManager::instance().freePage(page);
        break;
      }
    }
    if (mapped != ModuleImage::MaximumMappedBytes) {
      for (size_t offset = 0; offset < mapped; offset += pageSize) {
        void* address = reinterpret_cast<void*>(slot.base + offset);
        physical_uintptr_t page = 0;
        size_t flags = 0;
        space.getMapping(address, page, flags);
        space.unmap(address);
        PhysicalMemoryManager::instance().freePage(page);
      }
      m_ModuleAllocator.free(slot.base, ModuleImage::MaximumMappedBytes);
      delete prepared;
      break;
    }
    slot.module.name.reserve(ModuleImage::NameBytes);
    slot.module.runtime = &slot;
    slot.module.status = Module::Unloaded;
    slot.module.unloadComplete = true;
    slot.state = writable(slot) ? RuntimeModuleSlot::Free : RuntimeModuleSlot::Quarantined;
    lockModules();
    slots[index] = prepared;
    m_Modules.pushBack(&slot.module);
    unlockModules();
    if (slot.state == RuntimeModuleSlot::Free)
      ++available;
  }
  lockModules();
  // Preparation runs only once, including partial boot allocation. Existing
  // live slots must never be remapped by a subsequent preparation request.
  m_RuntimeModulesPrepared = true;
  unlockModules();
  finishModuleLoad();
  return available != 0;
}

KernelElf::RuntimeLoadResult KernelElf::beginRuntimeModuleLoad(size_t length, RuntimeLoad& load) {
  if (load.slot)
    return RuntimeLoadResult::Busy;
  if (!length)
    return RuntimeLoadResult::InvalidImage;
  if (length > ModuleImage::MaximumImageBytes)
    return RuntimeLoadResult::ImageTooLarge;
#if STATIC_DRIVERS || !BITS_64 || (!X64 && !HOSTED)
  return RuntimeLoadResult::UnsupportedImage;
#endif
  lockModules();
  RuntimeLoadResult result = RuntimeLoadResult::NoMemory;
  if (m_ModuleShutdown)
    result = RuntimeLoadResult::Shutdown;
  else if (m_ModuleLoading || m_UnloadingModule || m_ModuleExecutions || m_ModuleExecutionPasses)
    result = RuntimeLoadResult::Busy;
  else if (m_RuntimeModulesPrepared) {
    for (auto* prepared : slots) {
      if (!prepared || prepared->state != RuntimeModuleSlot::Free)
        continue;
      auto& slot = *prepared;
      slot.state = RuntimeModuleSlot::Reserved;
      slot.lifecycleStarted = false;
      slot.lifecycleComplete = true;
      load.slot = &slot;
      load.length = length;
      m_ModuleLoading = true;
      result = RuntimeLoadResult::Ready;
      break;
    }
  }
  unlockModules();
  return result;
}

uintptr_t KernelElf::runtimeExportLocked(const char* name, Module* owner) const {
  uintptr_t weak = 0;
  for (const auto* prepared : slots) {
    if (!prepared)
      continue;
    const auto& slot = *prepared;
    if (!slot.module.isActive() || (owner && owner != &slot.module))
      continue;
    for (size_t i = 1; i < slot.plan.symbolCount; ++i) {
      ModuleImage::Symbol symbol;
      if (!slot.plan.exportedSymbol(i, symbol) || StringCompare(slot.plan.symbolName(symbol), name))
        continue;
      const uintptr_t address = slot.base + symbol.value;
      if (ST_BIND(symbol.info) == STB_GLOBAL)
        return address;
      if (!weak)
        weak = address;
    }
  }
  return weak;
}

const char* KernelElf::runtimeLookupSymbolLocked(uintptr_t addr, uintptr_t* startAddr) const {
  for (const auto* prepared : slots) {
    if (!prepared || !(prepared->module.isActive() || prepared->module.isExecuting())) {
      continue;
    }

    const auto& slot = *prepared;
    ModuleImage::Symbol symbol;
    for (size_t i = 1; slot.plan.symbol(i, symbol, false); ++i) {
      // Diagnostics use the retained full table, independently of exports.
      const unsigned type = ST_TYPE(symbol.info);
      if (!symbol.shndx || symbol.shndx >= 0xff00 || (type != STT_FUNC && type != STT_NOTYPE) ||
          ST_BIND(symbol.info) > STB_WEAK || symbol.value > ~uintptr_t{0} - slot.base ||
          !slot.plan.contains(symbol.value, symbol.size ? symbol.size : 1, PF_R | PF_X)) {
        continue;
      }

      const uintptr_t symbolAddress = slot.base + symbol.value;
      const size_t symbolSize = symbol.size ? symbol.size : 1;
      if (addr >= symbolAddress && addr - symbolAddress < symbolSize) {
        const char* name = slot.plan.symbolName(symbol, false);
        if (name && *name) {
          if (startAddr) {
            *startAddr = symbolAddress;
          }
          return name;
        }
      }
    }
  }
  return nullptr;
}

uintptr_t KernelElf::resolveRuntimeImport(const char* name, Module* consumer) {
  const HashedStringView symbol(name);
  uintptr_t result = m_SymbolTable.lookupOwned(symbol, this);
#if HOSTED
  if (!result)
    result = reinterpret_cast<uintptr_t>(__pedigree_hosted::dlsym(RTLD_DEFAULT, name));
#endif
  if (result)
    return result;
  for (auto provider : m_Modules) {
    if (provider == consumer || !provider->isActive() || !moduleDependsOn(consumer, provider))
      continue;
    result = provider->runtime ? runtimeExportLocked(name, provider)
                               : m_SymbolTable.lookupOwned(symbol, provider->elf);
    if (result)
      return result;
  }
  return 0;
}

KernelElf::RuntimeLoadResult KernelElf::loadModuleRuntime(RuntimeLoad& load) {
  if (!load.slot)
    return RuntimeLoadResult::Busy;
  auto& slot = *load.slot;
  auto& plan = slot.plan;
  auto& module = slot.module;
  const auto preflight = plan.preflight(slot.source, load.length);
  switch (preflight) {
    case ModuleImage::Result::Malformed:
      return RuntimeLoadResult::InvalidImage;
    case ModuleImage::Result::Unsupported:
      return RuntimeLoadResult::UnsupportedImage;
    case ModuleImage::Result::TooLarge:
      return RuntimeLoadResult::ImageTooLarge;
    case ModuleImage::Result::Valid:
      break;
  }
  lockModules();
  for (auto existing : m_Modules) {
    if (!existing->isUnloaded() && !StringCompare(existing->name.cstr(), plan.name)) {
      unlockModules();
      return RuntimeLoadResult::Duplicate;
    }
  }
  module.name.assign(plan.name);
  module.loadBase = slot.base;
  module.loadSize = plan.mappedBytes;
  module.depends = slot.dependencies;
  module.depends_opt = slot.optionalDependencies;
  for (size_t i = 0; i <= plan.dependencyCount; ++i)
    slot.dependencies[i] = i < plan.dependencyCount ? plan.dependencies[i] : nullptr;
  for (size_t i = 0; i <= plan.optionalDependencyCount; ++i)
    slot.optionalDependencies[i] =
        i < plan.optionalDependencyCount ? plan.optionalDependencies[i] : nullptr;
  unlockModules();
  for (size_t i = 0; i < plan.dependencyCount; ++i) {
    bool found = false;
    for (auto provider : m_Modules) {
      if (provider->isActive() && !StringCompare(provider->name.cstr(), plan.dependencies[i]))
        found = true;
    }
    if (!found) {
      return RuntimeLoadResult::MissingDependency;
    }
  }
  // The loader claim pins all providers until publication or rollback. The
  // copied dependency lists then provide the ordinary unload ordering barrier.
  for (size_t i = 0; i < plan.symbolCount; ++i) {
    ModuleImage::Symbol symbol;
    plan.symbol(i, symbol);
    if (symbol.shndx) {
      slot.resolved[i] = slot.base + symbol.value;
    } else if (i) {
      slot.resolved[i] = resolveRuntimeImport(plan.symbolName(symbol), &module);
      if (!slot.resolved[i] && ST_BIND(symbol.info) != STB_WEAK) {
        return RuntimeLoadResult::MissingDependency;
      }
    } else
      slot.resolved[i] = 0;
  }
  ByteSet(reinterpret_cast<void*>(slot.base), 0, ModuleImage::MaximumMappedBytes);
  for (size_t i = 0; i < plan.segmentCount; ++i) {
    const auto& segment = plan.segments[i];
    MemoryCopy(reinterpret_cast<void*>(slot.base + segment.vaddr), plan.bytes + segment.offset,
               segment.filesz);
  }
  for (size_t i = 0; i < plan.relocationCount; ++i) {
    ModuleImage::Relocation relocation;
    plan.relocation(i, relocation);
    uintptr_t value = 0;
    if (!addSigned(R_TYPE(relocation.info) == 8 ? slot.base : slot.resolved[R_SYM(relocation.info)],
                   relocation.addend, value))
      return RuntimeLoadResult::InvalidImage;
    MemoryCopy(reinterpret_cast<void*>(slot.base + relocation.offset), &value, sizeof(value));
  }
  if (!plan.validateMaterialized(slot.base))
    return RuntimeLoadResult::InvalidImage;
  if (!seal(slot))
    return RuntimeLoadResult::ProtectionFailed;
  lockModules();
  module.entry = reinterpret_cast<ModuleEntry>(slot.base + plan.entry);
  module.exit = reinterpret_cast<void (*)()>(slot.base + plan.exit);
  module.unloadComplete = false;
  module.status = Module::Executing;
  module.unloadable = true;
  module.runtimeUnloadable = true;
  slot.lifecycleStarted = true;
  slot.lifecycleComplete = false;
  unlockModules();
  for (size_t i = 0; i < plan.constructorCount; ++i)
    reinterpret_cast<void (*)()>(slot.base + plan.constructors[i])();
  if (!module.entry())
    return RuntimeLoadResult::EntryFailed;
  lockModules();
  module.status = Module::Active;
  slot.state = RuntimeModuleSlot::Live;
  load.slot = nullptr;
  m_ModuleLoading = false;
  unlockModules();
  return RuntimeLoadResult::Loaded;
}

bool KernelElf::retireRuntimeModule(Module* module, bool runLifecycle) {
  auto& slot = *module->runtime;
  if (runLifecycle && slot.lifecycleStarted && !slot.lifecycleComplete) {
    TerminalQuiesceHook hook = nullptr;
    lockModules();
    if (m_TerminalQuiesceOwner == module)
      hook = m_TerminalQuiesceHook;
    unlockModules();
    if (hook && !hook()) {
      slot.state = RuntimeModuleSlot::Quarantined;
      module->unloadable = false;
      module->runtimeUnloadable = false;
      return false;
    }
    lockModules();
    if (m_TerminalQuiesceOwner == module) {
      m_TerminalQuiesceOwner = nullptr;
      m_TerminalQuiesceHook = nullptr;
    }
    unlockModules();
    module->exit();
    for (size_t i = 0; i < slot.plan.destructorCount; ++i)
      reinterpret_cast<void (*)()>(slot.base + slot.plan.destructors[i])();
    slot.lifecycleComplete = true;
  }
  const bool restored = writable(slot);
  if (restored) {
    ByteSet(reinterpret_cast<void*>(slot.base), 0, ModuleImage::MaximumMappedBytes);
    ByteSet(slot.source, 0, sizeof(slot.source));
  }
  lockModules();
  slot.state = restored ? RuntimeModuleSlot::Free : RuntimeModuleSlot::Quarantined;
  module->entry = nullptr;
  module->exit = nullptr;
  module->unloadAdmission = nullptr;
  module->depends = nullptr;
  module->depends_opt = nullptr;
  unlockModules();
  return restored;
}

void KernelElf::abandonRuntimeModuleLoad(RuntimeLoad& load) {
  auto& module = load.slot->module;
  lockModules();
  module.status = Module::Failed;
  unlockModules();
  const bool admitted = !load.slot->lifecycleStarted || !module.unloadAdmission ||
                        module.unloadAdmission(false) == Module::UnloadAdmission::Ready;
  if (admitted) {
    retireRuntimeModule(&module, load.slot->lifecycleStarted);
  } else {
    load.slot->state = RuntimeModuleSlot::Quarantined;
    module.unloadable = false;
    module.runtimeUnloadable = false;
  }
  lockModules();
  module.status = Module::Failed;
  module.unloadComplete = load.slot->lifecycleComplete;
  load.slot = nullptr;
  m_ModuleLoading = false;
  unlockModules();
}

#if HOSTED && PEDIGREE_HOSTED_SMOKE_TESTS
void KernelElf::failRuntimeProtectionForTest(size_t operationsBeforeFailure) {
  failProtectionAfter = operationsBeforeFailure;
}
#endif
