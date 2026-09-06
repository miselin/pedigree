/* Copyright (c) 2026, Pedigree Developers. */
#include "fanotify-queue.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/utilities/utility.h"

namespace {
constexpr uint64_t Overflow = 0x4000;
struct Metadata {
  uint32_t eventLength;
  uint8_t version;
  uint8_t reserved;
  uint16_t metadataLength;
  uint64_t mask;
  int32_t fd;
  int32_t pid;
};
struct FidInfo {
  uint8_t type;
  uint8_t pad;
  uint16_t length;
  FileSystemId fsid;
  uint32_t handleLength;
  int32_t handleType;
};
static_assert(sizeof(Metadata) == 24 && sizeof(FidInfo) == 20, "Linux fanotify record layout");
}  // namespace

size_t FanotifyRecord::encodedSize() const {
  return sizeof(Metadata) + (mask == Overflow ? 0 : ((sizeof(FidInfo) + handle.length + 3) & ~3U));
}

void FanotifyRecord::encode(void* buffer) const {
  const size_t length = encodedSize();
  Metadata metadata = {static_cast<uint32_t>(length), 3, 0, sizeof(Metadata), mask, -1,
                       static_cast<int32_t>(producer)};
  ByteSet(buffer, 0, length);
  MemoryCopy(buffer, &metadata, sizeof(metadata));
  if (mask == Overflow)
    return;
  FidInfo info = {
      1, 0, static_cast<uint16_t>(length - sizeof(Metadata)), fsid, handle.length, handle.type};
  auto* bytes = static_cast<uint8_t*>(buffer);
  MemoryCopy(bytes + sizeof(metadata), &info, sizeof(info));
  MemoryCopy(bytes + sizeof(metadata) + sizeof(info), handle.bytes, handle.length);
}

bool FanotifyRecord::sameTarget(const FanotifyRecord& other) const {
  if (mask == Overflow || other.mask == Overflow || mountId != other.mountId ||
      producer != other.producer || handle.type != other.handle.type ||
      handle.length != other.handle.length || handle.length > sizeof(handle.bytes))
    return false;
  for (size_t index = 0; index < handle.length; ++index) {
    if (handle.bytes[index] != other.handle.bytes[index])
      return false;
  }
  return true;
}

FanotifyQueue::FanotifyQueue()
    : m_Records(UniqueArray<FanotifyRecord>::allocate(MaximumEvents + 1)) {}
FanotifyQueue::~FanotifyQueue() {
  close();
}
bool FanotifyQueue::valid() const {
  return static_cast<bool>(m_Records);
}

void FanotifyQueue::enqueue(const FanotifyRecord& record) {
  bool readable = false;
  {
    LockGuard<Mutex> guard(m_Lock);
    if (m_Closed || !m_Records || record.handle.length > sizeof(record.handle.bytes))
      return;
    if (m_Count) {
      auto& last = m_Records.get()[(m_Head + m_Count - 1) % (MaximumEvents + 1)];
      if (last.sameTarget(record)) {
        last.mask |= record.mask;
        return;
      }
    }
    if (m_Count >= MaximumEvents && m_OverflowQueued)
      return;
    auto& destination = m_Records.get()[(m_Head + m_Count) % (MaximumEvents + 1)];
    if (m_Count >= MaximumEvents) {
      destination = FanotifyRecord();
      destination.mask = Overflow;
      m_OverflowQueued = true;
    } else {
      destination = record;
    }
    readable = m_Count++ == 0;
    if (readable)
      ++m_Generations.read;
  }
  if (readable) {
    m_Readers.broadcast();
    notifyReadiness(ReadyRead);
  }
}

FanotifyQueue::Take FanotifyQueue::take(FanotifyRecord& record, size_t capacity, bool canBlock) {
  m_Lock.acquire();
  while (!m_Closed && !m_Count) {
    if (!canBlock) {
      m_Lock.release();
      return Take::Empty;
    }
    ConditionVariable::Error error = ConditionVariable::NoError;
    if (!m_Readers.wait(m_Lock, error)) {
      if (ConditionVariable::mutexAcquired(error))
        m_Lock.release();
      return error == ConditionVariable::Interrupted ||
                     error == ConditionVariable::TerminationDeferred
                 ? Take::Interrupted
                 : Take::Closed;
    }
  }
  if (m_Closed) {
    m_Lock.release();
    return Take::Closed;
  }
  if (m_Records.get()[m_Head].encodedSize() > capacity) {
    m_Lock.release();
    return Take::TooSmall;
  }
  record = m_Records.get()[m_Head];
  m_Head = (m_Head + 1) % (MaximumEvents + 1);
  --m_Count;
  if (record.mask == Overflow)
    m_OverflowQueued = false;
  m_Lock.release();
  return Take::Ready;
}

void FanotifyQueue::close() {
  {
    LockGuard<Mutex> guard(m_Lock);
    if (m_Closed)
      return;
    m_Closed = true;
    m_Count = 0;
    m_OverflowQueued = false;
  }
  m_Readers.broadcast();
  closeReadiness(ReadyInvalid | ReadyHangup);
}

ReadyMask FanotifyQueue::queryReady() {
  LockGuard<Mutex> guard(m_Lock);
  return m_Closed ? ReadyInvalid | ReadyHangup : (m_Count ? ReadyRead : ReadyNone);
}
ReadinessGenerations FanotifyQueue::readinessGenerations() {
  LockGuard<Mutex> guard(m_Lock);
  return m_Generations;
}
int FanotifyQueue::queuedMetadataBytes() {
  LockGuard<Mutex> guard(m_Lock);
  // Linux reports the fixed metadata contribution, excluding FID information.
  return static_cast<int>(m_Count * sizeof(Metadata));
}
