#ifndef KERNEL_PROCESSOR_ARM64_VIRTUALADDRESSSPACE_H
#define KERNEL_PROCESSOR_ARM64_VIRTUALADDRESSSPACE_H

#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/processor/VirtualAddressSpace.h"

constexpr uintptr_t ARM64_DIRECT_MAP_BASE = 0xffff000000000000ULL;
constexpr uintptr_t KERNEL_VIRTUAL_HEAP = 0xffff800000000000ULL;
constexpr uintptr_t KERNEL_VIRTUAL_HEAP_END = 0xffff800100000000ULL;
constexpr uintptr_t KERNEL_VIRTUAL_CACHE = 0xffff900000000000ULL;
constexpr uintptr_t KERNEL_VIRTUAL_CACHE_END = 0xffff901000000000ULL;
constexpr uintptr_t KERNEL_VIRTUAL_MEMORYREGION_ADDRESS = 0xffffa00000000000ULL;
constexpr uintptr_t KERNEL_VIRTUAL_MEMORYREGION_END = 0xffffa01000000000ULL;
constexpr uintptr_t KERNEL_VIRTUAL_MODULE_BASE = 0xffffb00000000000ULL;
constexpr uintptr_t KERNEL_VIRTUAL_MODULE_END = 0xffffb01000000000ULL;
constexpr uintptr_t KERNEL_VIRTUAL_EVENT_BASE = 0xffffc00000000000ULL;
constexpr uintptr_t KERNEL_VIRTUAL_INFO_BLOCK = 0xffffd00000000000ULL;
constexpr uintptr_t KERNEL_VIRTUAL_STACK = 0xffffe00000000000ULL;
constexpr size_t KERNEL_STACK_SIZE = 0x10000;
constexpr uintptr_t USERSPACE_VIRTUAL_START = 0x400000;
constexpr uintptr_t USERSPACE_DYNAMIC_LINKER_LOCATION = 0x4fa00000;
constexpr uintptr_t USERSPACE_VIRTUAL_HEAP = 0x50000000;
constexpr uintptr_t USERSPACE_DYNAMIC_START = 0x100000000ULL;
constexpr uintptr_t USERSPACE_DYNAMIC_END = 0x00007ff000000000ULL;
constexpr uintptr_t USERSPACE_VIRTUAL_STACK = 0x00007fffffff0000ULL;
constexpr size_t USERSPACE_VIRTUAL_STACK_SIZE = 0x100000;

class Arm64VirtualAddressSpace final : public VirtualAddressSpace {
  friend class ProcessorBase;
  friend VirtualAddressSpace& VirtualAddressSpace::getKernelAddressSpace();
  friend VirtualAddressSpace* VirtualAddressSpace::create();

 public:
  bool isAddressValid(void* address) override;
  bool isMapped(void* address) override;
  bool map(physical_uintptr_t physical, void* address, size_t flags) override;
  bool tryMapUserPage(physical_uintptr_t physical, void* address, size_t flags,
                      size_t* committedTablePages = nullptr) override;
  bool tryDetachUserPage(void* address, physical_uintptr_t expected) override;
  bool getMapping(void* address, physical_uintptr_t& physical, size_t& flags) override;
  bool handleCopyOnWriteFault(void* address, bool userMode) override;
  void setFlags(void* address, size_t flags) override;
  bool trySetFlags(void* address, size_t flags) override;
  void unmap(void* address) override;
  bool detachMapping(void* address, physical_uintptr_t& physical, size_t& flags,
                     size_t requiredFlags = 0) override;
  Stack* allocateStack() override;
  Stack* allocateStack(size_t bytes) override;
  void freeStack(Stack* stack) override;
  VirtualAddressSpace* clone(bool copyOnWrite = true) override;
  void revertToKernelAddressSpace() override;
  bool memIsInKernelHeap(void* address) override;
  bool memIsInHeap(void* address) override;
  void* getEndOfHeap() override;

  uintptr_t getKernelStart() const override {
    return ARM64_DIRECT_MAP_BASE;
  }
  uintptr_t getUserStart() const override {
    return USERSPACE_VIRTUAL_START;
  }
  uintptr_t getUserReservedStart() const override {
    return USERSPACE_DYNAMIC_LINKER_LOCATION;
  }
  uintptr_t getDynamicLinkerAddress() const override {
    return USERSPACE_DYNAMIC_LINKER_LOCATION;
  }
  uintptr_t getKernelHeapStart() const override {
    return KERNEL_VIRTUAL_HEAP;
  }
  uintptr_t getKernelHeapEnd() const override {
    return KERNEL_VIRTUAL_HEAP_END;
  }
  uintptr_t getKernelCacheStart() const override {
    return KERNEL_VIRTUAL_CACHE;
  }
  uintptr_t getKernelCacheEnd() const override {
    return KERNEL_VIRTUAL_CACHE_END;
  }
  uintptr_t getKernelEventBlockStart() const override {
    return KERNEL_VIRTUAL_EVENT_BASE;
  }
  uintptr_t getKernelModulesStart() const override {
    return KERNEL_VIRTUAL_MODULE_BASE;
  }
  uintptr_t getKernelModulesEnd() const override {
    return KERNEL_VIRTUAL_MODULE_END;
  }
  uintptr_t getGlobalInfoBlock() const override {
    return KERNEL_VIRTUAL_INFO_BLOCK;
  }
  uintptr_t getDynamicStart() const override {
    return USERSPACE_DYNAMIC_START;
  }
  uintptr_t getDynamicEnd() const override {
    return USERSPACE_DYNAMIC_END;
  }

  physical_uintptr_t root() const {
    return m_Root;
  }
  ~Arm64VirtualAddressSpace() override;

 private:
  explicit Arm64VirtualAddressSpace(bool kernel);
  uint64_t* findEntry(uintptr_t address, bool create, size_t* newTables = nullptr);
  uint64_t* findExistingEntry(uintptr_t address, size_t* level = nullptr) const;
  bool cloneTable(uint64_t* destination, const uint64_t* source, size_t level, bool copyOnWrite);
  void freeTable(uint64_t* table, size_t level, bool freeLeaves);
  static uint64_t pageDescriptor(physical_uintptr_t physical, size_t flags);
  static size_t descriptorFlags(uint64_t descriptor);

  static Arm64VirtualAddressSpace m_KernelSpace;
  physical_uintptr_t m_Root;
  uintptr_t m_StackTop;
  Spinlock m_Lock;
};

#endif
