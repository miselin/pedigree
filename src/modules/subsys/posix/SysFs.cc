/* Copyright (c) 2026, Pedigree Developers. */
#define LWIP_DONT_PROVIDE_BYTEORDER_FUNCTIONS 1

#include "SysFs.h"
#include "pedigree/kernel/BootstrapInfo.h"
#include "pedigree/kernel/machine/Device.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/utilities/utility.h"

#include "DevFs-block.h"
#include "modules/system/vfs/Symlink.h"
#include "modules/system/lwip/include/lwip/netif.h"
#include "modules/system/lwip/include/lwip/tcpip.h"
#include "modules/system/network-stack/NetworkStack.h"
#include "modules/system/vfs/VFS.h"

namespace {
class AttributeFile final : public File {
 public:
  AttributeFile(const String& name, uintptr_t inode, Filesystem* filesystem, File* parent,
                const String& contents)
      : File(name, 0, 0, 0, inode, filesystem, contents.length(), parent), m_Contents(contents) {
    setPermissionsOnly(FILE_UR | FILE_GR | FILE_OR);
    setUidOnly(0);
    setGidOnly(0);
  }

  uint64_t readBytewise(uint64_t offset, uint64_t size, uintptr_t buffer, bool) override {
    if (offset >= m_Contents.length())
      return 0;
    size = min(size, m_Contents.length() - offset);
    MemoryCopy(reinterpret_cast<void*>(buffer), m_Contents.cstr() + offset, size);
    return size;
  }
  uint64_t writeBytewise(uint64_t, uint64_t, uintptr_t, bool) override {
    return 0;
  }
  size_t getSize() override {
    return m_Contents.length();
  }

 private:
  bool isBytewise() const override {
    return true;
  }
  String m_Contents;
};

class SysFsLink final : public Symlink {
 public:
  SysFsLink(const String& name, uintptr_t inode, Filesystem* filesystem, File* parent,
            const String& target)
      : Symlink(name, 0, 0, 0, inode, filesystem, target.length(), parent) {
    m_sTarget = target;
    setPermissions(FILE_UR | FILE_UW | FILE_UX | FILE_GR | FILE_GW | FILE_GX | FILE_OR | FILE_OW |
                   FILE_OX);
  }
};

struct NetworkSnapshot {
  struct netif* interface = nullptr;
  char name[2] = {};
  unsigned number = 0;
  unsigned mtu = 0;
  bool up = false;
  bool link = false;
  unsigned macLength = 0;
  unsigned char mac[NETIF_MAX_HWADDR_LEN] = {};
};

void snapshotNetwork(void* context) {
  auto& snapshot = *static_cast<NetworkSnapshot*>(context);
  auto* interface = snapshot.interface;
  MemoryCopy(snapshot.name, interface->name, sizeof(snapshot.name));
  snapshot.number = interface->num;
  snapshot.mtu = interface->mtu;
  snapshot.up = netif_is_up(interface);
  snapshot.link = netif_is_link_up(interface);
  snapshot.macLength = min(static_cast<unsigned>(interface->hwaddr_len),
                           static_cast<unsigned>(sizeof(snapshot.mac)));
  MemoryCopy(snapshot.mac, interface->hwaddr, snapshot.macLength);
}
}  // namespace

SysFs::~SysFs() {
  if (!m_Root)
    return;
  m_Root->emptyCache();
  if (!VFS::instance().untrackFile(m_Root))
    ERROR("SysFs: root directory did not get cleaned up");
}

const String& SysFs::getVolumeLabel() const {
  static String label("sysfs");
  return label;
}

SysFsDirectory* SysFs::directory(SysFsDirectory* parent, const char* name) {
  auto* result = new SysFsDirectory(String(name), allocateInode(), this, parent);
  if (!result)
    return nullptr;
  parent->addEntry(result->getName(), result);
  return result;
}

void SysFs::attribute(SysFsDirectory* parent, const char* name, const String& contents) {
  auto* file = new AttributeFile(String(name), allocateInode(), this, parent, contents);
  if (file)
    parent->addEntry(file->getName(), file);
}

void SysFs::symlink(SysFsDirectory* parent, const char* name, const String& target) {
  auto* link = new SysFsLink(String(name), allocateInode(), this, parent, target);
  if (link) {
    parent->addEntry(link->getName(), link);
  }
}

void SysFs::addPciDevices(SysFsDirectory* devices) {
  auto add = [this, devices](Device* device) -> Device* {
    const uint16_t vendor = device->getPciVendorId();
    if (!vendor || vendor == 0xffff)
      return device;
    String name;
    name.Format("0000:%02x:%02x.%x", device->getPciBusPosition(), device->getPciDevicePosition(),
                device->getPciFunctionNumber());
    auto* entry = directory(devices, name.cstr());
    if (!entry)
      return device;
    String value;
    value.Format("0x%04x\n", vendor);
    attribute(entry, "vendor", value);
    value.Format("0x%04x\n", device->getPciDeviceId());
    attribute(entry, "device", value);
    value.Format("0x%02x%02x%02x\n", device->getPciClassCode(), device->getPciSubclassCode(),
                 device->getPciProgInterface());
    attribute(entry, "class", value);
    value.Format("%lu\n", device->getInterruptNumber());
    attribute(entry, "irq", value);
    String resources;
    for (auto* address : device->addresses()) {
      const uintptr_t end =
          address->m_Size ? address->m_Address + address->m_Size - 1 : address->m_Address;
      String line;
      line.Format("0x%016lx 0x%016lx 0x%016lx\n", address->m_Address, end,
                  address->m_IsIoSpace ? 1UL : 0UL);
      resources += line;
    }
    attribute(entry, "resource", resources);
    return device;
  };
  auto callback = pedigree_std::make_callable(add);
  Device::foreach (callback, nullptr);
}

void SysFs::addBlockDevices(SysFsDirectory* devices) {
  auto add = [this, devices](Device* device) -> Device* {
    if (device->getType() != Device::Disk)
      return device;
    auto* disk = static_cast<Disk*>(device);
    const uint32_t id = disk->endpointId();
    if (!id)
      return device;
    String name;
    name.Format("disk%u", id);
    auto* entry = directory(devices, name.cstr());
    if (!entry)
      return device;
    String value;
    value.Format("%u:%u\n", PosixBlock::PhysicalMajor, id);
    attribute(entry, "dev", value);
    value.Format("%lu\n", disk->getSize() / 512);
    attribute(entry, "size", value);
    attribute(entry, "removable", String("0\n"));
    return device;
  };
  auto callback = pedigree_std::make_callable(add);
  Device::foreach (callback, nullptr);
}

void SysFs::addNetworkDevices(SysFsDirectory* devices) {
  auto* stack = NetworkStack::instanceIfAvailable();
  if (!stack)
    return;
  for (size_t index = 0;; ++index) {
    NetworkStack::DeviceLease device;
    if (!stack->acquireDevice(index, device))
      break;
    NetworkSnapshot snapshot;
    snapshot.interface = device.interface();
    if (tcpip_callback_wait(snapshotNetwork, &snapshot) != ERR_OK)
      continue;
    String name;
    name.Format("%c%c%u", snapshot.name[0], snapshot.name[1], snapshot.number);
    auto* entry = directory(devices, name.cstr());
    if (!entry)
      continue;
    String value;
    for (unsigned i = 0; i < snapshot.macLength; ++i) {
      String octet;
      octet.Format("%s%02x", i ? ":" : "", snapshot.mac[i]);
      value += octet;
    }
    value += "\n";
    attribute(entry, "address", value);
    value.Format("%u\n", snapshot.mtu);
    attribute(entry, "mtu", value);
    attribute(entry, "operstate", String(snapshot.up && snapshot.link ? "up\n" : "down\n"));
  }
}

bool SysFs::initialise(Disk*) {
  m_NextInode = 0;
  delete m_Root;
  m_Root = new SysFsDirectory(String(""), allocateInode(), this, nullptr);
  if (!m_Root)
    return false;
  VFS::instance().trackFile(m_Root);

  auto* classDirectory = directory(m_Root, "class");
  auto* block = classDirectory ? directory(classDirectory, "block") : nullptr;
  auto* network = classDirectory ? directory(classDirectory, "net") : nullptr;
  auto* graphics = classDirectory ? directory(classDirectory, "graphics") : nullptr;
  auto* framebuffer = graphics ? directory(graphics, "fb0") : nullptr;
  auto* framebufferDevice = framebuffer ? directory(framebuffer, "device") : nullptr;
  auto* bus = directory(m_Root, "bus");
  auto* pci = bus ? directory(bus, "pci") : nullptr;
  auto* pciDevices = pci ? directory(pci, "devices") : nullptr;
  auto* platform = bus ? directory(bus, "platform") : nullptr;
  auto* firmware = directory(m_Root, "firmware");
  if (!block || !network || !framebufferDevice || !pciDevices || !platform || !firmware)
    return false;

  symlink(framebufferDevice, "subsystem", String("/sys/bus/platform"));

  addBlockDevices(block);
  addNetworkDevices(network);
  addPciDevices(pciDevices);
  if (g_pBootstrapInfo && g_pBootstrapInfo->isUefi() && !directory(firmware, "efi"))
    return false;
  return true;
}
