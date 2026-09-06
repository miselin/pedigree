/* Copyright (c) 2026, Pedigree Developers. */
#include "DevFs-block.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/StaticString.h"
#include "pedigree/kernel/utilities/utility.h"

#include "DevFs.h"
#include "modules/system/vfs/VFS.h"

namespace {
bool parseId(const StringView& name, size_t start, uint32_t& id) {
  if (name.length() <= start || name[start] == '0')
    return false;
  id = 0;
  for (size_t i = start; i < name.length(); ++i) {
    const char ch = name[i];
    if (ch < '0' || ch > '9' || id > (PosixBlock::MaximumMinor - (ch - '0')) / 10)
      return false;
    id = id * 10 + ch - '0';
  }
  return true;
}

class BlockFile final : public File {
 public:
  BlockFile(DevFs& filesystem, File* parent, const String& name, uint32_t id, uint64_t bytes,
            const VFS::MountIdentity& mount)
      : File(name, 0, 0, 0, filesystem.getNextInode(), &filesystem, bytes, parent),
        m_Id(id),
        m_Mount(mount) {
    setPermissionsOnly(FILE_UR | FILE_UW);
    setUidOnly(0);
    setGidOnly(0);
  }
  bool isBlockDevice() const override {
    return true;
  }
  uint64_t deviceNumber() const override {
    return PosixBlock::encode(m_Mount ? PosixBlock::MountedMajor : PosixBlock::PhysicalMajor, m_Id);
  }
  uint64_t readBytewise(uint64_t offset, uint64_t length, uintptr_t buffer,
                        bool canBlock = true) override {
    return transfer(false, offset, length, buffer, canBlock);
  }
  uint64_t writeBytewise(uint64_t offset, uint64_t length, uintptr_t buffer,
                         bool canBlock = true) override {
    return transfer(true, offset, length, buffer, canBlock);
  }

 private:
  bool isBytewise() const override {
    return true;
  }
  uint64_t transfer(bool write, uint64_t offset, uint64_t length, uintptr_t buffer, bool canBlock) {
    if (!length)
      return 0;
    if (!canBlock) {
      SYSCALL_ERROR(NoMoreProcesses);
      return ~uint64_t(0);
    }
    TerminationDeferral lifetime;
    VFS::FilesystemPin mounted;
    DiskUse use;
    Disk* disk = nullptr;
    if (m_Mount) {
      if (m_Mount.pin(mounted))
        disk = mounted.filesystem()->getDisk();
    } else if (DiskEndpoints::acquire(m_Id, use)) {
      disk = use.get();
    }
    if (!disk) {
      uint64_t bytes = 0;
      syscallError(!m_Mount && DiskEndpoints::describe(m_Id, bytes) ? Error::DeviceBusy
                                                                    : Error::NoSuchDevice);
      return ~uint64_t(0);
    }
    const uint64_t bytes = disk->getSize();
    if (offset >= bytes)
      return 0;
    if (length > bytes - offset)
      length = bytes - offset;
    uint64_t done = 0;
    while (done < length) {
      const uint64_t position = offset + done;
      const uint64_t aligned = position - position % 512;
      const size_t displacement = position - aligned;
      const BufferView view = disk->read(aligned);
      if (!view || view.size() <= displacement) {
        if (view)
          disk->unpin(aligned);
        SYSCALL_ERROR(IoError);
        return done ? done : ~uint64_t(0);
      }
      const size_t amount = min(static_cast<uint64_t>(view.size() - displacement), length - done);
      auto* page = reinterpret_cast<uint8_t*>(view.address()) + displacement;
      if (write)
        MemoryCopy(page, reinterpret_cast<void*>(buffer + done), amount);
      else
        MemoryCopy(reinterpret_cast<void*>(buffer + done), page, amount);
      const bool completed = !write || disk->sync(aligned, false);
      disk->unpin(aligned);
      if (!completed) {
        SYSCALL_ERROR(IoError);
        return done ? done : ~uint64_t(0);
      }
      done += amount;
    }
    return done;
  }
  uint32_t m_Id;
  VFS::MountIdentity m_Mount;
};

class BlockDirectory final : public DevFsDirectory {
 public:
  BlockDirectory(DevFs& filesystem, File* parent)
      : DevFsDirectory(String("block"), 0, 0, 0, filesystem.getNextInode(), &filesystem, 0,
                       parent) {
    setPermissionsOnly(FILE_UR | FILE_UX | FILE_GR | FILE_GX | FILE_OR | FILE_OX);
  }

 protected:
  bool cacheResolvedChildren() const override {
    return false;
  }
  LookupStatus resolveChild(const StringView& name, File*& child) override {
    child = nullptr;
    uint32_t id = 0;
    uint64_t bytes = 0;
    VFS::MountIdentity mount;
    const bool physical =
        name.length() > 4 && name[0] == 'd' && name[1] == 'i' && name[2] == 's' && name[3] == 'k';
    if (!parseId(name, physical ? 4 : 0, id))
      return LookupStatus::NotFound;
    if (physical) {
      // An active paging endpoint remains nameable for swapoff, even though
      // ordinary byte I/O is denied by its exclusive claim.
      if (!DiskEndpoints::describe(id, bytes))
        return LookupStatus::NotFound;
    } else {
      VFS::FilesystemPin pin;
      if (!VFS::instance().diskMount(id, mount) || !mount.pin(pin) || !pin.filesystem()->getDisk())
        return LookupStatus::NotFound;
      bytes = pin.filesystem()->getDisk()->getSize();
    }
    child =
        new BlockFile(*static_cast<DevFs*>(getFilesystem()), this, String(name), id, bytes, mount);
    return child ? LookupStatus::Found : LookupStatus::IoError;
  }
  ReadStatus readDirectory(uint64_t& cookie, DirectoryEntryEmitter emitter,
                           void* context) override {
    Vector<VFS::MountIdentity> mounts;
    if (!VFS::instance().snapshotDiskMounts(mounts))
      return ReadStatus::IoError;
    uint32_t physical[DiskEndpoints::Capacity];
    const size_t physicalCount = DiskEndpoints::snapshot(physical, DiskEndpoints::Capacity);
    while (true) {
      uint64_t next = ~uint64_t(0);
      for (const auto& mount : mounts) {
        if (mount.id() >= cookie && mount.id() < next)
          next = mount.id();
      }
      for (size_t i = 0; i < physicalCount; ++i) {
        const uint64_t key = static_cast<uint64_t>(PosixBlock::MaximumMinor) + 1 + physical[i];
        if (key >= cookie && key < next)
          next = key;
      }
      if (next == ~uint64_t(0))
        return ReadStatus::Complete;
      NormalStaticString name;
      if (next > PosixBlock::MaximumMinor) {
        name += "disk";
        name.append(next - PosixBlock::MaximumMinor - 1);
      } else {
        name.append(next);
      }
      const DirectoryEntryView entry{StringView(name, name.length()), static_cast<uintptr_t>(next),
                                     EntryType::BlockDevice, next, next + 1};
      if (!emitter(context, entry))
        return ReadStatus::Stopped;
      cookie = next + 1;
    }
  }
};
}  // namespace
File* posix_make_block_directory(DevFs& filesystem, File* parent) {
  return new BlockDirectory(filesystem, parent);
}
