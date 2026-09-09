/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#define LWIP_DONT_PROVIDE_BYTEORDER_FUNCTIONS 1

#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/utilities/utility.h"

#include "ProcFs.h"
#include "modules/system/lwip/include/lwip/dns.h"
#include "modules/system/lwip/include/lwip/tcpip.h"

namespace {
class ResolverFile final : public File {
 public:
  ResolverFile(size_t inode, Filesystem* filesystem, File* parent)
      : File(String("resolv.conf"), 0, 0, 0, inode, filesystem, 0, parent) {
    setPermissionsOnly(FILE_UR | FILE_GR | FILE_OR);
    setUidOnly(0);
    setGidOnly(0);
  }

  uint64_t readBytewise(uint64_t location, uint64_t size, uintptr_t buffer,
                        bool bCanBlock = true) override {
    if (!size)
      return 0;
    const String contents = generateString();
    if (location >= contents.length())
      return 0;
    const uint64_t remaining = contents.length() - location;
    if (size > remaining)
      size = remaining;
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
#if LWIP_DNS
    TerminationDeferral lifetime;
    ip_addr_t servers[DNS_MAX_SERVERS] = {};
    // DHCP updates this table on the TCP/IP thread. Copy there so formatting
    // never retains pointers into resolver state after the callback completes.
    const auto snapshot = [](void* context) {
      auto* addresses = static_cast<ip_addr_t*>(context);
      for (size_t i = 0; i < DNS_MAX_SERVERS; ++i)
        addresses[i] = *dns_getserver(static_cast<u8_t>(i));
    };
    if (tcpip_callback_wait(snapshot, servers) != ERR_OK)
      return contents;

    for (const auto& server : servers) {
      if (ip_addr_isany(&server))
        continue;
      char address[IPADDR_STRLEN_MAX];
      if (!ipaddr_ntoa_r(&server, address, sizeof(address)))
        continue;
      contents += "nameserver ";
      contents += address;
      contents += "\n";
    }
#endif
    return contents;
  }
};
}  // namespace

void ProcFs::initialiseResolverFile() {
  auto* resolver = new ResolverFile(getNextInode(), this, m_pRoot);
  m_pRoot->addEntry(resolver->getName(), resolver);
}
