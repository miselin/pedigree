#define PEDIGREE_EXTERNAL_SOURCE 1
#include "pedigree/kernel/machine/PciConfigAccess.h"

#include <array>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace {
struct Lock {
  std::mutex mutex;
  bool held = false;
  void acquire() {
    mutex.lock();
    held = true;
  }
  void release() {
    held = false;
    mutex.unlock();
  }
};
struct Ports {
  Lock& lock;
  std::array<uint8_t, 256> bytes{};
  uint32_t selected = 0;
  unsigned traffic = 0;
  bool refuse = false, mirrorAddress = false;
  std::vector<unsigned> writeWidths;
  uint32_t read(unsigned port, unsigned width) {
    EXPECT_TRUE(lock.held);
    ++traffic;
    if (mirrorAddress)
      return selected;
    uint32_t value = 0;
    for (unsigned i = 0; i < width; ++i)
      value |= uint32_t{bytes[(selected & 0xfc) + port - 4 + i]} << (8 * i);
    return value;
  }
  void write(uint32_t value, unsigned port, unsigned width) {
    EXPECT_TRUE(lock.held);
    ++traffic;
    if (!port) {
      selected = value;
      std::this_thread::yield();
      return;
    }
    writeWidths.push_back(width);
    if (refuse)
      return;
    for (unsigned i = 0; i < width; ++i) {
      const unsigned offset = (selected & 0xfc) + port - 4 + i;
      if (offset == 6 || offset == 7)
        bytes[offset] &= ~(value >> (8 * i));
      else
        bytes[offset] = value >> (8 * i);
    }
  }
  uint8_t read8(unsigned port) {
    return read(port, 1);
  }
  uint16_t read16(unsigned port) {
    return read(port, 2);
  }
  uint32_t read32(unsigned port) {
    return read(port, 4);
  }
  void write8(uint8_t value, unsigned port) {
    write(value, port, 1);
  }
  void write16(uint16_t value, unsigned port) {
    write(value, port, 2);
  }
  void write32(uint32_t value, unsigned port) {
    write(value, port, 4);
  }
};
}  // namespace
TEST(PciConfigAccess, RejectsInvalidAddressesBeforeAnyPortIo) {
  Lock lock;
  Ports ports{lock};
  PciConfigAccess<Ports, Lock> config(ports, lock);
  uint32_t value = 0x12345678;
  EXPECT_FALSE(config.read(0, 32, 0, 0, 4, value));
  EXPECT_FALSE(config.read(0, 0, 8, 0, 4, value));
  for (uint16_t offset : {2U, 255U, 256U, 1020U, 65535U}) {
    EXPECT_FALSE(config.read(0, 0, 0, offset, 4, value));
    EXPECT_FALSE(config.write(0, 0, 0, offset, 4, 0));
  }
  EXPECT_EQ(ports.traffic, 0U);
  EXPECT_EQ(value, 0x12345678U);
}
TEST(PciConfigAccess, CommandUpdatePreservesStatusAndVerifiesReadback) {
  Lock lock;
  Ports ports{lock};
  PciConfigAccess<Ports, Lock> config(ports, lock);
  ports.bytes[4] = 0x40;
  ports.bytes[5] = 4;
  ports.bytes[6] = 0xff;
  ports.bytes[7] = 0xff;
  EXPECT_TRUE(config.updateCommand(0, 2, 0, 0x400, 7));
  EXPECT_EQ(ports.bytes[4], 0x47);
  EXPECT_EQ(ports.bytes[5], 0);
  EXPECT_EQ(ports.bytes[6], 0xff);
  EXPECT_EQ(ports.bytes[7], 0xff);
  ASSERT_EQ(ports.writeWidths.size(), 1U);
  EXPECT_EQ(ports.writeWidths[0], 2U);
  ports.refuse = true;
  EXPECT_FALSE(config.updateCommand(0, 2, 0, 7, 0x400));
}
TEST(PciConfigAccess, UsesCorrectByteLanesAtConfigurationBoundary) {
  Lock lock;
  Ports ports{lock};
  PciConfigAccess<Ports, Lock> config(ports, lock);
  ASSERT_TRUE(config.write(0, 0, 0, 255, 1, 0xa5));
  ASSERT_TRUE(config.write(0, 0, 0, 252, 2, 0x1234));
  uint32_t value = 0;
  ASSERT_TRUE(config.read(0, 0, 0, 252, 4, value));
  EXPECT_EQ(value, 0xa5001234U);
  EXPECT_TRUE(config.write(0, 0, 0, 16, 4, 0xffffffffU));
  EXPECT_TRUE(config.read(0, 0, 0, 16, 4, value));
  EXPECT_EQ(value, 0xffffffffU);
}
TEST(PciConfigAccess, BarSizingCanMaskDecodeWithoutInterruptingFirmwareDma) {
  Lock lock;
  Ports ports{lock};
  PciConfigAccess<Ports, Lock> config(ports, lock);
  ports.bytes[4] = 7;
  EXPECT_TRUE(config.updateCommand(0, 2, 0, 3, 0));
  EXPECT_EQ(ports.bytes[4], 4);
  EXPECT_TRUE(config.updateCommand(0, 2, 0, 3, 3));
  EXPECT_EQ(ports.bytes[4], 7);
}
TEST(PciConfigAccess, SerializesAddressDataPairsAcrossFourCallers) {
  Lock lock;
  Ports ports{lock};
  ports.mirrorAddress = true;
  PciConfigAccess<Ports, Lock> config(ports, lock);
  std::atomic<unsigned> failures{0};
  std::vector<std::thread> threads;
  for (unsigned id = 0; id < 4; ++id)
    threads.emplace_back([&, id] {
      for (unsigned i = 0; i < 1000; ++i) {
        uint32_t value = 0;
        if (!config.read(7, id, 3, 0xfc, 4, value) || value != (0x800703fcU | (id << 11)))
          ++failures;
      }
    });
  for (auto& thread : threads)
    thread.join();
  EXPECT_EQ(failures.load(), 0U);
}
