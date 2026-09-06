#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/utilities/Pointers.h"

#include "VirtualAddressSpace.h"
#include "utils.h"

#if PEDIGREE_VM_REMAP_TESTS
namespace {
constexpr size_t PageSize = 4096;
using Status = VirtualAddressSpace::RemapStatus;

bool reserveAligned(Process& process, size_t length, uintptr_t& result) {
  for (;;) {
    Process::UserReservationSnapshot snapshot;
    uintptr_t raw = 0;
    if (!process.snapshotUserReservations(snapshot) ||
        !snapshot.dynamic.allocate(length + PageSize - 1, raw)) {
      return false;
    }
    const uintptr_t aligned = (raw + PageSize - 1) & ~(PageSize - 1);
    const size_t prefix = aligned - raw;
    const size_t suffix = PageSize - 1 - prefix;
    if ((prefix && !snapshot.dynamic.tryFree(raw, prefix)) ||
        (suffix && !snapshot.dynamic.tryFree(aligned + length, suffix))) {
      return false;
    }
    if (process.commitUserReservations(snapshot.generation, snapshot)) {
      result = aligned;
      return true;
    }
  }
}

struct Admission {
  Process* process;
  Process::UserReservationSnapshot* snapshot;
  bool called = false;
};

bool admit(void* context) {
  auto& admission = *static_cast<Admission*>(context);
  admission.called = true;
  return admission.process->commitUserReservations(admission.snapshot->generation,
                                                   *admission.snapshot);
}

class RemapFixture {
 public:
  explicit RemapFixture(Process& process)
      : process(process),
        space(),
        source(0),
        destination(0),
        extra(0),
        originalCow(0),
        sourceReserved(false),
        destinationReserved(false),
        committed(false),
        plan() {}
  ~RemapFixture() {
    if (committed) {
      for (size_t i = 0; i < plan.get()->detachedPageCount(); ++i) {
        const auto& page = plan.get()->detachedPages()[i];
        if (page.mapped) {
          PhysicalMemoryManager::instance().freePage(page.physical);
        }
      }
    }
    if (space) {
      for (size_t i = 0; i < 5; ++i) {
        const uintptr_t address = i < 3 ? source + i * PageSize : destination + (i - 3) * PageSize;
        physical_uintptr_t physical = 0;
        size_t flags = 0;
        if (address &&
            space.get()->detachMapping(reinterpret_cast<void*>(address), physical, flags)) {
          PhysicalMemoryManager::instance().freePage(physical);
        }
      }
    }
    plan.reset();
    space.reset();
    if (originalCow) {
      PhysicalMemoryManager::instance().freePage(originalCow);
    }
    if (extra) {
      process.freeUserRange(Process::UserRegion::Dynamic, extra, PageSize);
    }
    if (sourceReserved) {
      process.freeUserRange(Process::UserRegion::Dynamic, source, 3 * PageSize);
    }
    if (destinationReserved) {
      process.freeUserRange(Process::UserRegion::Dynamic, destination, 2 * PageSize);
    }
  }

  bool initialise() {
    if (!reserveAligned(process, 3 * PageSize, source)) {
      return false;
    }
    sourceReserved = true;
    if (!reserveAligned(process, 2 * PageSize, destination)) {
      return false;
    }
    destinationReserved = true;
    space = UniquePointer<VirtualAddressSpace>::adopt(VirtualAddressSpace::create());
    if (!space) {
      return false;
    }
    for (size_t i = 0; i < 5; ++i) {
      const auto physical = PhysicalMemoryManager::instance().allocatePage();
      if (!physical) {
        return false;
      }
      const uintptr_t address = i < 3 ? source + i * PageSize : destination + (i - 3) * PageSize;
      size_t flags = VirtualAddressSpace::Write;
      if (i == 0) {
        flags = VirtualAddressSpace::CopyOnWrite | VirtualAddressSpace::Execute;
      } else if (i == 1) {
        flags = VirtualAddressSpace::NoAccess | VirtualAddressSpace::WriteProtected;
      }
      ByteSet(physicalAddress(reinterpret_cast<void*>(physical)), 0x31 + i, PageSize);
      if (!space.get()->map(physical, reinterpret_cast<void*>(address), flags)) {
        PhysicalMemoryManager::instance().freePage(physical);
        return false;
      }
      physicalPages[i] = physical;
      space.get()->getMapping(reinterpret_cast<void*>(address), physicalPages[i], pageFlags[i]);
      if (!i) {
        // The first pin counts the source mapping; the second retains the fixture's reference.
        PhysicalMemoryManager::instance().pin(physical);
        PhysicalMemoryManager::instance().pin(physical);
        originalCow = physical;
      }
    }
    return true;
  }

  bool unchanged() {
    for (size_t i = 0; i < 5; ++i) {
      const uintptr_t address = i < 3 ? source + i * PageSize : destination + (i - 3) * PageSize;
      if (!space.get()->isMapped(reinterpret_cast<void*>(address))) {
        return false;
      }
      physical_uintptr_t physical = 0;
      size_t flags = 0;
      space.get()->getMapping(reinterpret_cast<void*>(address), physical, flags);
      if (physical != physicalPages[i] || flags != pageFlags[i]) {
        return false;
      }
      const auto* bytes = physicalAddress(reinterpret_cast<unsigned char*>(physical));
      for (size_t offset = 0; offset < PageSize; ++offset) {
        if (bytes[offset] != (offset ? 0x31 + i : firstBytes[i])) {
          return false;
        }
      }
    }
    Process::UserReservationSnapshot snapshot;
    return process.snapshotUserReservations(snapshot) &&
           !snapshot.dynamic.allocateSpecific(source, 3 * PageSize) &&
           !snapshot.dynamic.allocateSpecific(destination, 2 * PageSize);
  }

  Process& process;
  UniquePointer<VirtualAddressSpace> space;
  uintptr_t source, destination, extra;
  physical_uintptr_t originalCow;
  physical_uintptr_t physicalPages[5] = {};
  size_t pageFlags[5] = {};
  unsigned char firstBytes[5] = {0x31, 0x32, 0x33, 0x34, 0x35};
  bool sourceReserved, destinationReserved, committed;
  UniquePointer<VirtualAddressSpace::PreparedPageRemap> plan;
};
}  // namespace

extern "C" EXPORTED_PUBLIC bool x64RemapCoreRegression() {
  Thread* thread = Processor::information().getCurrentThread();
  if (!thread || !thread->getParent()) {
    return false;
  }
  RemapFixture fixture(*thread->getParent());
  if (!fixture.initialise()) {
    return false;
  }
  VirtualAddressSpace::RemapRange victim{fixture.destination, 2 * PageSize};
  VirtualAddressSpace::PageRemapRequest request{
      fixture.source, fixture.destination, 3 * PageSize, 2 * PageSize, true, &victim, 1};
  size_t failedStages = 0;
  bool prepared = false;
  for (ssize_t stage = 0; stage < 32; ++stage) {
    fixture.space.get()->setRemapPreparationFailureForTest(stage);
    const auto status = fixture.space.get()->prepareRemap(request, fixture.plan);
    if (!fixture.unchanged()) {
      return false;
    }
    if (status == Status::Success) {
      prepared = true;
      break;
    }
    if (status != Status::NoMemory || fixture.plan) {
      return false;
    }
    ++failedStages;
  }
  if (!prepared || failedStages < 5) {
    return false;
  }

  Process::UserReservationSnapshot reservations;
  if (!fixture.process.snapshotUserReservations(reservations) ||
      !reservations.dynamic.tryFree(fixture.source, 3 * PageSize) ||
      !reserveAligned(fixture.process, PageSize, fixture.extra)) {
    return false;
  }
  Admission stale{&fixture.process, &reservations};
  if (fixture.plan.get()->commit(admit, &stale) != Status::Retry || !stale.called ||
      fixture.plan.get()->detachedPageCount() || !fixture.unchanged()) {
    return false;
  }

  fixture.plan.reset();
  if (fixture.space.get()->prepareRemap(request, fixture.plan) != Status::Success ||
      !fixture.space.get()->handleCopyOnWriteFault(reinterpret_cast<void*>(fixture.source), true)) {
    return false;
  }
  physical_uintptr_t currentCow = 0;
  size_t currentFlags = 0;
  fixture.space.get()->getMapping(reinterpret_cast<void*>(fixture.source), currentCow,
                                  currentFlags);
  if (currentCow == fixture.originalCow) {
    return false;
  }
  *physicalAddress(reinterpret_cast<unsigned char*>(currentCow)) = 0x7B;
  fixture.physicalPages[0] = currentCow;
  fixture.pageFlags[0] = currentFlags;
  fixture.firstBytes[0] = 0x7B;
  for (size_t attempt = 0; attempt < 32; ++attempt) {
    if (!fixture.process.snapshotUserReservations(reservations) ||
        !reservations.dynamic.tryFree(fixture.source, 3 * PageSize)) {
      return false;
    }
    Admission fresh{&fixture.process, &reservations};
    const auto status = fixture.plan.get()->commit(admit, &fresh);
    if (status == Status::Success) {
      fixture.committed = true;
      fixture.sourceReserved = false;
      if (!fresh.called) {
        return false;
      }
      break;
    }
    if (status != Status::Retry || !fresh.called || fixture.plan.get()->detachedPageCount() ||
        fixture.plan.get()->detachedPages() || !fixture.unchanged()) {
      return false;
    }
  }
  if (!fixture.committed || fixture.plan.get()->detachedPageCount() != 3) {
    return false;
  }
  for (size_t i = 0; i < 3; ++i) {
    const auto& retired = fixture.plan.get()->detachedPages()[i];
    const size_t original = i ? i + 2 : 2;
    if (!retired.mapped || retired.physical != fixture.physicalPages[original] ||
        retired.flags != fixture.pageFlags[original] ||
        fixture.space.get()->isMapped(reinterpret_cast<void*>(fixture.source + i * PageSize))) {
      return false;
    }
  }
  physical_uintptr_t moved = 0;
  size_t movedFlags = 0;
  fixture.space.get()->getMapping(reinterpret_cast<void*>(fixture.destination), moved, movedFlags);
  if (moved != currentCow || movedFlags != currentFlags ||
      *physicalAddress(reinterpret_cast<unsigned char*>(moved)) != 0x7B) {
    return false;
  }
  fixture.space.get()->getMapping(reinterpret_cast<void*>(fixture.destination + PageSize), moved,
                                  movedFlags);
  if (moved != fixture.physicalPages[1] || movedFlags != fixture.pageFlags[1]) {
    return false;
  }
  Process::UserReservationSnapshot after;
  return fixture.process.snapshotUserReservations(after) &&
         after.dynamic.allocateSpecific(fixture.source, 3 * PageSize) &&
         !after.dynamic.allocateSpecific(fixture.destination, 2 * PageSize);
}
#endif
