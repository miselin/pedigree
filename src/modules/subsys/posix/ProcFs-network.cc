/* Copyright (c) 2026, Pedigree Developers. */
#define LWIP_DONT_PROVIDE_BYTEORDER_FUNCTIONS 1

#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/utilities/utility.h"

#include "ProcFs.h"
#include "modules/system/lwip/include/lwip/netif.h"
#include "modules/system/lwip/include/lwip/tcpip.h"
#include "modules/system/network-stack/NetworkStack.h"

namespace {
class NetworkFile final : public File {
 public:
  NetworkFile(size_t inode, Filesystem* filesystem, File* parent)
      : File(String("interfaces"), 0, 0, 0, inode, filesystem, 0, parent) {
    setPermissionsOnly(FILE_UR | FILE_GR | FILE_OR);
    setUidOnly(0);
    setGidOnly(0);
  }

  uint64_t readBytewise(uint64_t location, uint64_t size, uintptr_t buffer, bool = true) override {
    if (!size)
      return 0;
    const String contents = generateString();
    if (location >= contents.length())
      return 0;
    if (size > contents.length() - location)
      size = contents.length() - location;
    MemoryCopy(reinterpret_cast<void*>(buffer), contents.cstr() + location, size);
    return size;
  }

  uint64_t writeBytewise(uint64_t, uint64_t, uintptr_t, bool = true) override {
    return 0;
  }

  size_t getSize() override {
    return generateString().length();
  }

 private:
  bool isBytewise() const override {
    return true;
  }

  static String generateString() {
    String contents;
    auto* stack = NetworkStack::instanceIfAvailable();
    if (!stack)
      return String("No network devices.\n");

    TerminationDeferral lifetime;
    for (size_t index = 0;; ++index) {
      NetworkStack::DeviceLease device;
      if (!stack->acquireDevice(index, device))
        break;

      struct Snapshot {
        struct netif* interface;
        char name[2];
        unsigned number;
        unsigned mtu;
        bool up;
        bool link;
        bool defaultRoute;
        unsigned macLength;
        unsigned char mac[NETIF_MAX_HWADDR_LEN];
        ip4_addr_t address, netmask, gateway;
        ip6_addr_t ipv6[LWIP_IPV6_NUM_ADDRESSES];
        bool ipv6Valid[LWIP_IPV6_NUM_ADDRESSES];
      } snapshot = {};
      snapshot.interface = device.interface();

      // The lease pins the device; the callback copies configuration where
      // DHCP and IPv6 address updates are serialized.
      const auto copy = [](void* context) {
        auto& result = *static_cast<Snapshot*>(context);
        auto* iface = result.interface;
        MemoryCopy(result.name, iface->name, sizeof(result.name));
        result.number = iface->num;
        result.mtu = iface->mtu;
        result.up = netif_is_up(iface);
        result.link = netif_is_link_up(iface);
        result.defaultRoute = iface == netif_default;
        result.macLength = iface->hwaddr_len;
        if (result.macLength > sizeof(result.mac))
          result.macLength = sizeof(result.mac);
        MemoryCopy(result.mac, iface->hwaddr, result.macLength);
        result.address = *netif_ip4_addr(iface);
        result.netmask = *netif_ip4_netmask(iface);
        result.gateway = *netif_ip4_gw(iface);
        for (size_t i = 0; i < LWIP_IPV6_NUM_ADDRESSES; ++i) {
          result.ipv6[i] = *netif_ip6_addr(iface, i);
          result.ipv6Valid[i] = ip6_addr_isvalid(netif_ip6_addr_state(iface, i));
        }
      };
      if (tcpip_callback_wait(copy, &snapshot) != ERR_OK) {
        contents += "Network status unavailable.\n";
        break;
      }

      String line;
      line.Format("%c%c%u: %s, link flag %s, mtu %u%s\n", snapshot.name[0], snapshot.name[1],
                  snapshot.number, snapshot.up ? "up" : "down", snapshot.link ? "up" : "down",
                  snapshot.mtu, snapshot.defaultRoute ? ", default route" : "");
      contents += line;
      String name;
      device.device()->getName(name);
      contents += "  Device: ";
      contents += name;
      contents += "\n  MAC:";
      for (unsigned i = 0; i < snapshot.macLength; ++i) {
        line.Format("%s%02x", i ? ":" : " ", snapshot.mac[i]);
        contents += line;
      }
      contents += "\n";
      char address[IP4ADDR_STRLEN_MAX], netmask[IP4ADDR_STRLEN_MAX], gateway[IP4ADDR_STRLEN_MAX];
      ip4addr_ntoa_r(&snapshot.address, address, sizeof(address));
      ip4addr_ntoa_r(&snapshot.netmask, netmask, sizeof(netmask));
      ip4addr_ntoa_r(&snapshot.gateway, gateway, sizeof(gateway));
      line.Format("  IPv4: %s  Netmask: %s  Gateway: %s\n", address, netmask, gateway);
      contents += line;
      for (size_t i = 0; i < LWIP_IPV6_NUM_ADDRESSES; ++i) {
        if (!snapshot.ipv6Valid[i])
          continue;
        char ipv6[IP6ADDR_STRLEN_MAX];
        if (ip6addr_ntoa_r(&snapshot.ipv6[i], ipv6, sizeof(ipv6))) {
          contents += "  IPv6: ";
          contents += ipv6;
          contents += "\n";
        }
      }
    }
    if (!contents.length())
      contents = String("No network devices.\n");
    return contents;
  }
};

class NetworkDevFile final : public File {
 public:
  NetworkDevFile(size_t inode, Filesystem* filesystem, File* parent)
      : File(String("dev"), 0, 0, 0, inode, filesystem, 0, parent) {
    setPermissionsOnly(FILE_UR | FILE_GR | FILE_OR);
    setUidOnly(0);
    setGidOnly(0);
  }

  uint64_t readBytewise(uint64_t location, uint64_t size, uintptr_t buffer, bool = true) override {
    String contents = generateString();
    if (location >= contents.length())
      return 0;
    size = min(size, contents.length() - location);
    MemoryCopy(reinterpret_cast<void*>(buffer), contents.cstr() + location, size);
    return size;
  }
  uint64_t writeBytewise(uint64_t, uint64_t, uintptr_t, bool = true) override {
    return 0;
  }
  size_t getSize() override {
    return generateString().length();
  }

 private:
  bool isBytewise() const override {
    return true;
  }

  static String generateString() {
    String contents(
        "Inter-|   Receive                                                |  Transmit\n"
        " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets "
        "errs drop fifo colls carrier compressed\n");
    auto* stack = NetworkStack::instanceIfAvailable();
    if (!stack)
      return contents;
    TerminationDeferral lifetime;
    for (size_t index = 0;; ++index) {
      NetworkStack::DeviceLease device;
      if (!stack->acquireDevice(index, device))
        break;
      struct Snapshot {
        struct netif* interface;
        char name[2];
        unsigned number;
      } snapshot = {device.interface(), {}, 0};
      const auto copy = [](void* context) {
        auto& result = *static_cast<Snapshot*>(context);
        MemoryCopy(result.name, result.interface->name, sizeof(result.name));
        result.number = result.interface->num;
      };
      if (tcpip_callback_wait(copy, &snapshot) != ERR_OK)
        continue;
      String line;
      line.Format("%c%c%u: 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n", snapshot.name[0], snapshot.name[1],
                  snapshot.number);
      contents += line;
    }
    return contents;
  }
};
}  // namespace

void ProcFs::initialiseNetworkFile() {
  auto* directory = new ProcFsDirectory(String("net"), 0, 0, 0, getNextInode(), this, 0, m_pRoot);
  directory->setPermissions(FILE_UR | FILE_UX | FILE_GR | FILE_GX | FILE_OR | FILE_OX);
  m_pRoot->addEntry(directory->getName(), directory);
  auto* interfaces = new NetworkFile(getNextInode(), this, directory);
  directory->addEntry(interfaces->getName(), interfaces);
  auto* devices = new NetworkDevFile(getNextInode(), this, directory);
  directory->addEntry(devices->getName(), devices);
}
