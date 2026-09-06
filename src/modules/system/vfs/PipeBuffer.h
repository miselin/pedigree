/* Copyright (c) 2026, Pedigree Developers. */
#ifndef VFS_PIPE_BUFFER_H
#define VFS_PIPE_BUFFER_H

#include "pedigree/kernel/Atomic.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/ConditionVariable.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/processor/types.h"

class EXPORTED_PUBLIC PipeBuffer {
 public:
  static constexpr size_t Capacity = 4096;
  using ChangeCallback = void (*)(void*);
  enum class Status { Ready, Eof, WouldBlock, Interrupted, Closed, Invalid };
  struct Result {
    Status status;
    size_t count;
  };

  class EXPORTED_PUBLIC ReadReservation {
   public:
    ReadReservation();
    ~ReadReservation();
    ReadReservation(const ReadReservation&) = delete;
    ReadReservation& operator=(const ReadReservation&) = delete;
    size_t size() const;
    void copyTo(uint8_t* destination, size_t count) const;
    void consume(size_t accepted);
    void cancel();

   private:
    friend class PipeBuffer;
    TerminationDeferral m_Termination;
    PipeBuffer* m_Buffer = nullptr;
    size_t m_Size = 0;
  };

  class EXPORTED_PUBLIC WriteReservation {
   public:
    WriteReservation();
    ~WriteReservation();
    WriteReservation(const WriteReservation&) = delete;
    WriteReservation& operator=(const WriteReservation&) = delete;
    size_t size() const;
    Result commit(const uint8_t* source, size_t accepted);
    void cancel();

   private:
    friend class PipeBuffer;
    TerminationDeferral m_Termination;
    PipeBuffer* m_Buffer = nullptr;
    size_t m_Size = 0;
  };

  explicit PipeBuffer(ChangeCallback changed = nullptr, void* context = nullptr);
  ~PipeBuffer();
  PipeBuffer(const PipeBuffer&) = delete;
  PipeBuffer& operator=(const PipeBuffer&) = delete;

  size_t read(uint8_t* destination, size_t count, bool block = true);
  size_t write(const uint8_t* source, size_t count, bool block = true);
  size_t writeAtomic(const uint8_t* source, size_t count, bool block = true);
  bool canRead(bool block);
  bool canWrite(bool block);
  uint64_t readableGeneration() const;
  uint64_t writableGeneration() const;
  size_t getDataSize();

  // These run under Pipe's endpoint lock; Pipe publishes after unlocking.
  void disableReads();
  void disableWrites();
  bool enableReads();
  bool enableWrites(bool resetIfPreviouslyDisabled = false);
  void wipe();
  void close();

  Result reserveRead(size_t maximum, bool block, ReadReservation& reservation);
  Result reserveWrite(size_t maximum, bool block, WriteReservation& reservation);
  Result waitTransfer(bool writing, bool block);
  Result transferTo(PipeBuffer& output, size_t maximum, bool consume, bool block);

 private:
  friend class PipeBufferTestPeer;
  struct PairWaiter;
  struct PairLink;
  class ActiveOperation {
   public:
    explicit ActiveOperation(PipeBuffer& buffer);
    ~ActiveOperation();
    explicit operator bool() const;
    void detach();

   private:
    ActiveOperation(const ActiveOperation&) = delete;
    ActiveOperation& operator=(const ActiveOperation&) = delete;
    TerminationDeferral m_Termination;
    PipeBuffer* m_Buffer;
  };

  static void lock(Mutex& mutex);
  static bool wait(ConditionVariable& condition, Mutex& mutex);
  static bool interrupted();
  bool beginOperation();
  void endOperation();
  bool readableLocked() const;
  bool writableLocked() const;
  Result readyLocked(bool writing, size_t minimum = 1) const;
  Result waitLocked(bool writing, size_t minimum, bool block);
  void changedLocked();
  void resetLocked();
  void finishResetLocked();
  void copyOutLocked(uint8_t* destination, size_t count) const;
  void appendLocked(const uint8_t* source, size_t count);
  void consumeLocked(size_t count);
  void publish();
  void wakePairsLocked();
  void linkPairLocked(PairLink& link);
  void unlinkPairLocked(PairLink& link);

  uint8_t m_Data[Capacity];
  size_t m_Head = 0;
  size_t m_Size = 0;
  bool m_ReadEnabled = true;
  bool m_WriteEnabled = true;
  bool m_Closing = false;
  bool m_ResetPending = false;
  bool m_ReadReserved = false;
  bool m_WriteReserved = false;
  bool m_WasReadable = false;
  bool m_WasWritable = true;
  size_t m_ActiveOperations = 0;
  Atomic<uint64_t> m_ReadGeneration;
  Atomic<uint64_t> m_WriteGeneration;
  Mutex m_Lock;
  ConditionVariable m_ReadCondition;
  ConditionVariable m_WriteCondition;
  ConditionVariable m_DrainCondition;
  PairLink* m_PairWaiters = nullptr;
  const ChangeCallback m_Changed;
  void* const m_Context;
};

#endif
