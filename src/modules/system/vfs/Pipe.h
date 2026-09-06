/*
 * Copyright (c) 2008-2014, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef PIPE_H
#define PIPE_H

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/ConditionVariable.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/time/Time.h"
#include "pedigree/kernel/utilities/String.h"

#include "File.h"
#include "PipeBuffer.h"

#define PIPE_BUF_MAX 4096

/** A first-in-first-out buffer node. */
class EXPORTED_PUBLIC Pipe : public File {
  friend class Filesystem;
  friend class ZombiePipe;

 public:
  /** Eases the pain of casting, and performs a sanity check. */
  static Pipe* fromFile(File* pF) {
    if (!(pF->isPipe() || pF->isFifo()))
      FATAL("Casting non-pipe/fifo File to Pipe!");
    return reinterpret_cast<Pipe*>(pF);
  }

  /** Constructor, creates an invalid file. */
  Pipe();

  /** Copy constructors are hidden - unused! */
  Pipe(const Pipe& file);

 private:
  Pipe& operator=(const Pipe&);

 public:
  /** Constructor, should be called only by a Filesystem. */
  Pipe(const String& name, Time::Timestamp accessedTime, Time::Timestamp modifiedTime,
       Time::Timestamp creationTime, uintptr_t inode, class Filesystem* pFs, size_t size,
       File* pParent, bool bIsAnonymous = false);
  /** Destructor - doesn't do anything. */
  virtual ~Pipe();

  /** select() */
  virtual int select(bool bWriting = false, int timeout = 0);

  ReadyMask queryReady(bool reading, bool writing) override;
  ReadinessGenerations readinessGenerations() override;

  bool supportsReadinessNotifications() const override {
    return true;
  }

  /** Reads from the file. */
  virtual uint64_t readBytewise(uint64_t location, uint64_t size, uintptr_t buffer,
                                bool bCanBlock = true);
  /** Writes to the file. */
  virtual uint64_t writeBytewise(uint64_t location, uint64_t size, uintptr_t buffer,
                                 bool bCanBlock = true);

  /** Pipes are anonymous (no name). */
  virtual bool isPipe() const;
  /** FIFOs are not anonymous (have a name). */
  virtual bool isFifo() const;

  virtual bool isSeekable() const {
    return false;
  }

  virtual void increaseRefCount(bool bIsWriter);

  /** Override decreaseRefCount so we can tell when all writers have hung up
      (and also when all readers have hung up so we can die). */
  virtual void decreaseRefCount(bool bIsWriter);

  /** Pin anonymous pipe storage independently of reader/writer presence. */
  bool retainVfsReference() override;

  /** Release a storage pin, retiring an unused anonymous pipe if needed. */
  void releaseVfsReference() override;

  /** Returns a locked diagnostic snapshot of the current reader count. */
  size_t getReaderCount();

  /** Returns a locked diagnostic snapshot of the current writer count. */
  size_t getWriterCount();

  /**
   * Atomically tests for a reader and, when permitted, waits for one.
   *
   * All writers waiting on the predicate are released when a reader arrives.
   * Returns false for a nonblocking miss or an interrupted blocking wait.
   */
  bool waitForReader(bool bCanBlock);

  class EXPORTED_PUBLIC ReadReservation {
   public:
    ReadReservation() = default;
    ~ReadReservation();
    ReadReservation(const ReadReservation&) = delete;
    ReadReservation& operator=(const ReadReservation&) = delete;
    size_t size() const;
    void copyTo(uint8_t* destination, size_t count) const;
    void consume(size_t accepted);
    void cancel();

   private:
    friend class Pipe;
    TerminationDeferral m_Termination;
    Pipe* m_Pipe = nullptr;
    PipeBuffer::ReadReservation m_Reservation;
  };

  class EXPORTED_PUBLIC WriteReservation {
   public:
    WriteReservation() = default;
    ~WriteReservation();
    WriteReservation(const WriteReservation&) = delete;
    WriteReservation& operator=(const WriteReservation&) = delete;
    size_t size() const;
    PipeBuffer::Result commit(const uint8_t* source, size_t accepted);
    void cancel();

   private:
    friend class Pipe;
    TerminationDeferral m_Termination;
    Pipe* m_Pipe = nullptr;
    PipeBuffer::WriteReservation m_Reservation;
  };

  PipeBuffer::Result reserveRead(size_t maximum, bool block, ReadReservation& reservation);
  PipeBuffer::Result reserveWrite(size_t maximum, bool block, WriteReservation& reservation);
  PipeBuffer::Result waitTransfer(bool writing, bool block);
  PipeBuffer::Result transferTo(Pipe& output, size_t maximum, bool consume, bool block);

 protected:
  /** If we're an anonymous pipe, we should delete ourselves when all
   * readers/writers have hung up. */
  bool m_bIsAnonymous;

  /** Have we reached EOF? */
  volatile bool m_bIsEOF;

  /** Internal pipe buffer. */
  PipeBuffer m_Buffer;

  /** Writers waiting for the protected m_nReaders predicate. */
  ConditionVariable m_ReaderCondition;

  uint64_t m_WriteGeneration;
  uint64_t m_ErrorGeneration;
  uint64_t m_HangupGeneration;

  /** OFD lifetime pins which do not count as live reader/writer endpoints. */
  size_t m_nLifetimePins;

  /** Ensures anonymous retirement is queued exactly once. */
  bool m_bRetirementQueued;

  bool shouldQueueRetirementLocked();

  static void bufferChanged(void* context);

  virtual bool isBytewise() const {
    return true;
  }
};

#endif
