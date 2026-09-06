/* Copyright (c) 2026, Pedigree Developers. */
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/PhysicalMemoryManager.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/utilities/Vector.h"

#include "File.h"
#include "MemoryMappedFile.h"

MemoryMappedObject* AnonymousMemoryMap::stageSlice(uintptr_t source, size_t sourceLength,
                                                   uintptr_t destination,
                                                   size_t destinationLength) {
  auto* result = new AnonymousMemoryMap(destination, destinationLength, m_Permissions);
  if (!result)
    return nullptr;
  result->m_OwnsMappings = false;
  result->m_bCopyOnWrite = m_bCopyOnWrite;
  result->m_MaximumPermissions = m_MaximumPermissions;
  result->m_Attachment = m_Attachment;
  const size_t preserved = sourceLength < destinationLength ? sourceLength : destinationLength;
  for (void* mapping : m_Mappings) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(mapping);
    if (address >= source && address - source < preserved &&
        !result->m_Mappings.tryPushBack(reinterpret_cast<void*>(destination + address - source))) {
      delete result;
      return nullptr;
    }
  }
  return result;
}

bool MemoryMappedFile::backingRangeValid(uintptr_t source, size_t length) const {
  return source >= m_Address && source - m_Address <= ~size_t(0) - m_Offset &&
         length <= ~size_t(0) - (m_Offset + (source - m_Address));
}

MemoryMappedObject* MemoryMappedFile::stageSlice(uintptr_t source, size_t sourceLength,
                                                 uintptr_t destination, size_t destinationLength) {
  if (!backingRangeValid(source, destinationLength))
    return nullptr;
  auto* result = new MemoryMappedFile(destination, destinationLength,
                                      m_Offset + (source - m_Address), m_pBacking, m_bCopyOnWrite,
                                      m_Permissions, m_MaximumPermissions, m_Attachment);
  if (!result)
    return nullptr;
  result->m_OwnsMappings = false;
  const size_t preserved = sourceLength < destinationLength ? sourceLength : destinationLength;
  for (auto it = m_Mappings.begin(); it != m_Mappings.end(); ++it) {
    if (it.key() >= source && it.key() - source < preserved &&
        !result->m_Mappings.tryInsert(destination + (it.key() - source), it.value())) {
      delete result;
      return nullptr;
    }
  }
  return result;
}

namespace {
using Snapshot = Process::UserReservationSnapshot;
using Status = MemoryMapManager::VmStatus;
using PageStatus = VirtualAddressSpace::RemapStatus;

uintptr_t roundedEnd(MemoryMappedObject* object, size_t pageMask) {
  return (object->address() + object->length() + pageMask) & ~pageMask;
}
bool overlaps(uintptr_t a, size_t aLength, uintptr_t b, size_t bLength) {
  return a < b + bLength && b < a + aLength;
}
MemoryAllocator* allocatorFor(Snapshot& snapshot, VirtualAddressSpace& space, uintptr_t base,
                              size_t length) {
  if (length > ~uintptr_t(0) - base)
    return nullptr;
  const uintptr_t end = base + length;
  if (space.getDynamicStart() && base >= space.getDynamicStart() && end <= space.getDynamicEnd())
    return &snapshot.dynamic;
  if (base >= space.getUserStart() && end <= space.getUserReservedStart())
    return &snapshot.normal;
  return nullptr;
}
bool allocateAnywhere(Snapshot& snapshot, size_t length, size_t pageMask, uintptr_t& address) {
  MemoryAllocator* allocators[] = {&snapshot.dynamic, &snapshot.normal};
  for (MemoryAllocator* allocator : allocators) {
    uintptr_t allocation = 0;
    if (!allocator->allocate(length + pageMask, allocation))
      continue;
    address = (allocation + pageMask) & ~pageMask;
    if (address != allocation && !allocator->tryFree(allocation, address - allocation))
      return false;
    const uintptr_t tail = address + length;
    if (tail < allocation + length + pageMask &&
        !allocator->tryFree(tail, allocation + length + pageMask - tail))
      return false;
    return true;
  }
  return false;
}
Status translate(PageStatus status) {
  switch (status) {
    case PageStatus::Success:
      return Status::Success;
    case PageStatus::InvalidRange:
      return Status::InvalidRange;
    case PageStatus::Unsupported:
      return Status::Unsupported;
    default:
      return Status::NoMemory;
  }
}

struct MetadataPlan {
  List<MemoryMappedObject*> replacement;
  Vector<MemoryMappedObject*> staged;
  Vector<MemoryMappedObject*> retired;
  bool committed = false;
  ~MetadataPlan() {
    for (auto* object : committed ? retired : staged)
      delete object;
  }
  bool appendSlice(MemoryMappedObject* owner, uintptr_t from, size_t length, uintptr_t destination,
                   size_t newLength) {
    MemoryMappedObject* object = owner->stageSlice(from, length, destination, newLength);
    if (!object)
      return false;
    staged.pushBack(object);
    return replacement.tryPushBack(object);
  }
};
}  // namespace

MemoryMapManager::VmStatus MemoryMapManager::remap(const RemapRequest& request, uintptr_t& result) {
  OperationGuard operation(*this);
  const size_t pageSize = PhysicalMemoryManager::getPageSize();
  const size_t pageMask = pageSize - 1;
  if (!request.oldLength)
    return request.mayMove ? VmStatus::Unsupported : VmStatus::InvalidRange;
  if (!request.newLength || ((request.source | request.oldLength | request.newLength) & pageMask) ||
      request.oldLength > ~uintptr_t(0) - request.source ||
      request.newLength > ~size_t(0) - pageMask ||
      (request.fixed &&
       (!request.mayMove || (request.destination & pageMask) ||
        request.newLength > ~uintptr_t(0) - request.destination ||
        overlaps(request.source, request.oldLength, request.destination, request.newLength))))
    return VmStatus::InvalidRange;
  VirtualAddressSpace& space = Processor::information().getVirtualAddressSpace();
  Process* process = Processor::information().getCurrentThread()->getParent();
  MmObjectList* objects = m_MmObjectLists.lookup(&space);
  MemoryMappedObject* source = nullptr;
  if (objects) {
    for (auto* object : *objects) {
      if (object->address() <= request.source && request.source < roundedEnd(object, pageMask) &&
          request.oldLength <= roundedEnd(object, pageMask) - request.source) {
        source = object;
        break;
      }
    }
  }
  if (!source)
    return !contains(request.source, request.oldLength) &&
                   space.isMapped(reinterpret_cast<void*>(request.source))
               ? VmStatus::Unsupported
               : VmStatus::Unmapped;
  if (source->backingFile() && source->backingFile()->isDirectPhysicalMapping())
    return VmStatus::Unsupported;
  if (!source->backingRangeValid(request.source, request.newLength))
    return VmStatus::InvalidRange;
  auto attachment = source->m_Attachment;
  if (attachment) {
    if (request.source != source->address() ||
        request.oldLength != roundedEnd(source, pageMask) - source->address() ||
        request.newLength > request.oldLength)
      return VmStatus::Unsupported;
    for (auto* object : *objects)
      if (object != source && object->m_Attachment.get() == attachment.get())
        return VmStatus::Unsupported;
  }
  if (!request.fixed && request.oldLength == request.newLength) {
    result = request.source;
    return VmStatus::Success;
  }
  // Bound both leaf preparation and the worst-case tracking/list metadata.
  // The additional object cap includes pooled list storage for all fragments.
  constexpr size_t MaximumObjects = 4096;
  if (request.oldLength / pageSize > VirtualAddressSpace::MaximumRemapPages ||
      request.newLength / pageSize > VirtualAddressSpace::MaximumRemapPages ||
      objects->count() > MaximumObjects)
    return VmStatus::NoMemory;
  for (size_t attempt = 0; attempt < 32; ++attempt) {
    Snapshot snapshot;
    if (!process->snapshotUserReservations(snapshot))
      return VmStatus::NoMemory;
    uintptr_t destination = request.fixed ? request.destination : request.source;
    if (!request.fixed && request.newLength > request.oldLength) {
      const uintptr_t extension = request.source + request.oldLength;
      MemoryAllocator* allocator =
          allocatorFor(snapshot, space, extension, request.newLength - request.oldLength);
      const bool inPlace =
          extension == roundedEnd(source, pageMask) && allocator &&
          allocator->allocateSpecific(extension, request.newLength - request.oldLength);
      if (!inPlace) {
        if (!request.mayMove ||
            !allocateAnywhere(snapshot, request.newLength, pageMask, destination))
          return VmStatus::NoMemory;
      }
    }
    MemoryAllocator* destinationAllocator =
        allocatorFor(snapshot, space, destination, request.newLength);
    if (!destinationAllocator)
      return VmStatus::InvalidRange;

    Vector<VirtualAddressSpace::RemapRange> victims;
    MetadataPlan metadata;
    if (!victims.tryReserve(objects->count()) || !metadata.retired.tryReserve(objects->count()) ||
        !metadata.staged.tryReserve(objects->count() * 3 + 1))
      return VmStatus::NoMemory;
    size_t metadataPages = request.newLength / pageSize;
    for (auto* object : *objects) {
      const size_t length = roundedEnd(object, pageMask) - object->address();
      const bool victim = destination != request.source &&
                          overlaps(destination, request.newLength, object->address(), length);
      if (victim) {
        if (!request.fixed)
          return VmStatus::NoMemory;
        if (object->backingFile() && object->backingFile()->isDirectPhysicalMapping())
          return VmStatus::Unsupported;
        const uintptr_t first = object->address() > destination ? object->address() : destination;
        const uintptr_t last = object->address() + length < destination + request.newLength
                                   ? object->address() + length
                                   : destination + request.newLength;
        victims.pushBack({first, last - first});
      }
      if (object == source || victim) {
        if (length / pageSize > VirtualAddressSpace::MaximumRemapPages - metadataPages)
          return VmStatus::NoMemory;
        metadataPages += length / pageSize;
        metadata.retired.pushBack(object);
      }
    }
    if (request.fixed) {
      uintptr_t cursor = destination;
      const uintptr_t end = destination + request.newLength;
      while (cursor < end) {
        uintptr_t coveredEnd = cursor, next = end;
        for (const auto& victim : victims) {
          if (victim.base <= cursor && cursor < victim.base + victim.length)
            coveredEnd = victim.base + victim.length;
          else if (victim.base > cursor && victim.base < next)
            next = victim.base;
        }
        if (coveredEnd > cursor)
          cursor = coveredEnd < end ? coveredEnd : end;
        else {
          if (!destinationAllocator->allocateSpecific(cursor, next - cursor))
            return VmStatus::NoMemory;
          cursor = next;
        }
      }
    }
    uintptr_t released = request.source;
    size_t releasedLength = request.oldLength;
    if (destination == request.source) {
      released += request.newLength;
      releasedLength =
          request.oldLength > request.newLength ? request.oldLength - request.newLength : 0;
    }
    if (releasedLength) {
      auto* allocator = allocatorFor(snapshot, space, released, releasedLength);
      if (!allocator || !allocator->tryFree(released, releasedLength))
        return VmStatus::NoMemory;
    }
    for (auto* object : *objects) {
      bool replaced = false;
      for (auto* retired : metadata.retired)
        replaced = replaced || retired == object;
      if (!replaced) {
        if (!metadata.replacement.tryPushBack(object))
          return VmStatus::NoMemory;
        continue;
      }
      VirtualAddressSpace::RemapRange cuts[2];
      size_t cutCount = 0;
      if (object == source)
        cuts[cutCount++] = {request.source, request.oldLength};
      if (destination != request.source)
        cuts[cutCount++] = {destination, request.newLength};
      if (cutCount == 2 && cuts[1].base < cuts[0].base) {
        const auto first = cuts[0];
        cuts[0] = cuts[1];
        cuts[1] = first;
      }
      uintptr_t cursor = object->address();
      const uintptr_t end = roundedEnd(object, pageMask);
      for (size_t i = 0; i < cutCount; ++i) {
        const uintptr_t first = cuts[i].base > cursor ? cuts[i].base : cursor;
        const uintptr_t last =
            cuts[i].base + cuts[i].length < end ? cuts[i].base + cuts[i].length : end;
        if (first >= end || last <= cursor)
          continue;
        if (first > cursor &&
            !metadata.appendSlice(object, cursor, first - cursor, cursor, first - cursor))
          return VmStatus::NoMemory;
        cursor = last;
      }
      if (cursor < end && !metadata.appendSlice(object, cursor, end - cursor, cursor, end - cursor))
        return VmStatus::NoMemory;
    }
    if (!metadata.appendSlice(source, request.source, request.oldLength, destination,
                              request.newLength))
      return VmStatus::NoMemory;
    // A complete replacement list owns its own nodes; publication replaces an
    // existing tree value without transferring nodes between ObjectPools.
    auto* replacement = new MmObjectList;
    if (!replacement)
      return VmStatus::NoMemory;
    for (auto* object : metadata.replacement) {
      if (!replacement->tryPushBack(object)) {
        delete replacement;
        return VmStatus::NoMemory;
      }
    }
    const VirtualAddressSpace::PageRemapRequest pages{
        request.source,    destination,   request.oldLength,
        request.newLength, request.fixed, victims.count() ? &victims[0] : nullptr,
        victims.count()};
    UniquePointer<VirtualAddressSpace::PreparedPageRemap> prepared;
    PageStatus status = space.prepareRemap(pages, prepared);
    if (status == PageStatus::Success) {
      struct Admission {
        Process* process;
        Snapshot* snapshot;
      } admission{process, &snapshot};
      status = prepared.get()->commit(
          [](void* opaque) {
            auto* state = static_cast<Admission*>(opaque);
            return state->process->commitUserReservations(state->snapshot->generation,
                                                          *state->snapshot);
          },
          &admission);
    }
    if (status != PageStatus::Success) {
      delete replacement;
      if (status == PageStatus::Retry)
        continue;
      return translate(status);
    }
    for (auto* object : metadata.retired)
      object->setMappingOwnership(false);
    for (auto* object : metadata.staged)
      object->setMappingOwnership(true);
    {
      LockGuard<Spinlock> guard(m_Lock);
      assert(m_MmObjectLists.contains(&space));
      m_MmObjectLists.insert(&space, replacement);
    }
    if (attachment)
      attachment->relocate(destination);
    metadata.committed = true;
    for (size_t i = 0; i < prepared.get()->detachedPageCount(); ++i) {
      const auto& page = prepared.get()->detachedPages()[i];
      for (auto* owner : metadata.retired) {
        if (owner->address() <= page.address && page.address < roundedEnd(owner, pageMask)) {
          owner->releaseDetachedPage(page.address, page);
          break;
        }
      }
    }
    delete objects;
    result = destination;
    return VmStatus::Success;
  }
  return VmStatus::NoMemory;
}
