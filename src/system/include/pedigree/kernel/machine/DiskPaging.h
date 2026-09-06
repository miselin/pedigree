/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */
#ifndef PEDIGREE_MACHINE_DISKPAGING_H
#define PEDIGREE_MACHINE_DISKPAGING_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"

class Disk;
struct DiskEndpoint;

/** Recoverable outcomes for the prepared block path. */
enum class PagingStatus { Success, Unsupported, Busy, NoMemory, Invalid, IoError, Closed };
enum class PagingOperation { Read, Write, Flush };
struct PagingRequest {
  PagingOperation operation;
  uint64_t offset;
  void* page;
  PagingStatus result;
};

/** A physical-device admission retained across a probe, mount, or ordinary I/O. */
class EXPORTED_PUBLIC DiskUse {
 public:
  DiskUse();
  DiskUse(DiskUse&& other) noexcept;
  ~DiskUse();
  DiskUse& operator=(DiskUse&& other) noexcept;
  void reset();
  Disk* get() const;
  explicit operator bool() const;

 private:
  friend class Disk;
  friend class DiskEndpoints;
  DiskUse(const DiskUse&) = delete;
  DiskUse& operator=(const DiskUse&) = delete;
  DiskEndpoint* m_Endpoint;
};

/** Backend storage is reserved before pressure; transfer must not allocate. */
class EXPORTED_PUBLIC PagingTransport {
 public:
  virtual PagingStatus transfer(PagingOperation operation, uint64_t offset, void* page) = 0;
  virtual void release() = 0;

 protected:
  virtual ~PagingTransport() = default;
};

/**
 * Exclusive physical-device ownership. All calls complete synchronously.
 * The pager must retire every swap reference before releasing this owner.
 * Destruction joins transfers, but does not substitute for an explicit flush.
 */
class EXPORTED_PUBLIC PagingChannel {
 public:
  static constexpr size_t PageBytes = 4096;
  PagingChannel();
  ~PagingChannel();
  PagingChannel(const PagingChannel&) = delete;
  PagingChannel& operator=(const PagingChannel&) = delete;

  PagingStatus readPage(uint64_t offset, void* page);
  PagingStatus writePage(uint64_t offset, const void* page);
  PagingStatus flush();
  void reset();
  uint32_t endpointId() const;
  uint64_t size() const;
  explicit operator bool() const;

 private:
  friend class DiskEndpoints;
  PagingStatus transfer(PagingOperation operation, uint64_t offset, void* page);
  DiskEndpoint* m_Endpoint;
};

/**
 * Numeric identities never repeat, including after removal. Registry storage
 * supports 64 simultaneous physical endpoints; exhaustion only disables new
 * physical selectors and paging, leaving ordinary driver I/O available.
 */
class EXPORTED_PUBLIC DiskEndpoints {
 public:
  static constexpr size_t Capacity = 64;
  static bool acquire(uint32_t id, DiskUse& use);
  static size_t snapshot(uint32_t* ids, size_t capacity);
  static bool describe(uint32_t id, uint64_t& bytes);
  static PagingStatus prepare(uint32_t id, PagingChannel& channel);

 private:
  friend class Disk;
  static DiskEndpoint* reserve(Disk* disk);
  static void publish(DiskEndpoint* endpoint);
  static bool acquire(DiskEndpoint* endpoint, Disk* disk, DiskUse& use);
  static uint32_t id(DiskEndpoint* endpoint, Disk* disk);
  static void retire(DiskEndpoint* endpoint, Disk* disk);
  static bool tryClose(DiskEndpoint* endpoint, Disk* disk);
  static void reopen(DiskEndpoint* endpoint, Disk* disk);
};

#endif
