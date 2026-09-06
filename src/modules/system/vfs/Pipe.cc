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

#include "Pipe.h"
#include "pedigree/kernel/LockGuard.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/ProcessorInformation.h"
#include "pedigree/kernel/utilities/ZombieQueue.h"
#include "pedigree/kernel/utilities/new"

class Filesystem;

class ZombiePipe : public ZombieObject {
 public:
  ZombiePipe(Pipe* pPipe) : m_pPipe(pPipe) {}
  virtual ~ZombiePipe();

 private:
  Pipe* m_pPipe;
};

ZombiePipe::~ZombiePipe() {
  NOTICE("ZombiePipe: freeing " << m_pPipe);
  delete m_pPipe;
}

Pipe::Pipe()
    : File(),
      m_bIsAnonymous(true),
      m_bIsEOF(false),
      m_Buffer(bufferChanged, this),
      m_ReaderCondition(),
      m_WriteGeneration(0),
      m_ErrorGeneration(0),
      m_HangupGeneration(0),
      m_nLifetimePins(0),
      m_bRetirementQueued(false) {
#if VERBOSE_KERNEL
  NOTICE("Pipe: new anonymous pipe " << reinterpret_cast<uintptr_t>(this));
#endif
}

Pipe::Pipe(const String& name, Time::Timestamp accessedTime, Time::Timestamp modifiedTime,
           Time::Timestamp creationTime, uintptr_t inode, Filesystem* pFs, size_t size,
           File* pParent, bool bIsAnonymous)
    : File(name, accessedTime, modifiedTime, creationTime, inode, pFs, size, pParent),
      m_bIsAnonymous(bIsAnonymous),
      m_bIsEOF(false),
      m_Buffer(bufferChanged, this),
      m_ReaderCondition(),
      m_WriteGeneration(0),
      m_ErrorGeneration(0),
      m_HangupGeneration(0),
      m_nLifetimePins(0),
      m_bRetirementQueued(false) {
#if VERBOSE_KERNEL
  NOTICE("Pipe: new " << (bIsAnonymous ? "anonymous" : "named") << " pipe " << Hex << this);
#endif
}

Pipe::~Pipe() {
  // ensure anything else in the critical section can finish before we clean
  // up fully
  // this is useful for cases where ZombieQueue destroys us before we get a
  // chance to actually return from decreaseRefCount (which accesses the lock)
  m_Lock.acquire();
  m_Lock.release();
}

void Pipe::bufferChanged(void* context) {
  static_cast<Pipe*>(context)->dataChanged();
}

int Pipe::select(bool bWriting, int timeout) {
  if (bWriting) {
    return m_Buffer.canWrite(timeout > 0) ? 1 : 0;
  } else {
    return m_Buffer.canRead(timeout > 0) ? 1 : 0;
  }
}

ReadyMask Pipe::queryReady(bool reading, bool writing) {
  LockGuard<Mutex> guard(m_Lock);
  ReadyMask ready = ReadyNone;

  if (!m_nWriters) {
    ready |= ReadyHangup;
  }
  if (!m_nReaders) {
    ready |= ReadyError;
  }

  if (reading) {
    if (m_Buffer.canRead(false)) {
      ready |= ReadyRead;
    }
  }

  if (writing) {
    // A closed read end is immediately writable from poll/epoll's point of
    // view even though the subsequent write fails with EPIPE/SIGPIPE.
    if (!m_nReaders || m_Buffer.canWrite(false)) {
      ready |= ReadyWrite;
    }
  }

  return ready;
}

ReadinessGenerations Pipe::readinessGenerations() {
  LockGuard<Mutex> guard(m_Lock);
  ReadinessGenerations generations;
  generations.read = m_Buffer.readableGeneration();
  generations.write = m_Buffer.writableGeneration() + m_WriteGeneration;
  generations.error = m_ErrorGeneration;
  generations.hangup = m_HangupGeneration;
  return generations;
}

uint64_t Pipe::readBytewise(uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock) {
  // Need to read what's left in the pipe then EOF if there's no more readers!
  {
    LockGuard<Mutex> guard(m_Lock);
    if (m_nWriters == 0) {
      bCanBlock = false;
    }
  }

  uint8_t* pBuf = reinterpret_cast<uint8_t*>(buffer);
  return m_Buffer.read(pBuf, size, bCanBlock);
}

uint64_t Pipe::writeBytewise(uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock) {
  {
    LockGuard<Mutex> guard(m_Lock);
    if (m_nReaders == 0) {
      // no more readers, abort the write
      return 0;
    }
  }

  uint8_t* pBuf = reinterpret_cast<uint8_t*>(buffer);
  return size <= PIPE_BUF_MAX ? m_Buffer.writeAtomic(pBuf, size, bCanBlock)
                              : m_Buffer.write(pBuf, size, bCanBlock);
}

bool Pipe::isPipe() const {
  return getName().length() == 0 || m_bIsAnonymous;
}

bool Pipe::isFifo() const {
  return getName().length() > 0 && !m_bIsAnonymous;
}

void Pipe::increaseRefCount(bool bIsWriter) {
  {
    LockGuard<Mutex> guard(m_Lock);

    if (bIsWriter) {
      // A reader can still own unread bytes across the last writer's close.
      m_Buffer.enableWrites();
      m_nWriters++;
    } else {
      // A reader is now present so we can enable reads if they weren't.
      m_Buffer.enableReads();
      m_nReaders++;

      // The predicate is "at least one reader", so one arrival satisfies
      // every writer currently blocked in open().
      m_ReaderCondition.broadcast();
    }
  }

  dataChanged();
}

void Pipe::decreaseRefCount(bool bIsWriter) {
  // Make sure only one thread decreases the refcount at a time. This is
  // important as we add ourselves to the ZombieQueue if the refcount ticks
  // to zero. Getting pre-empted by another thread that also decreases the
  // refcount between the decrement and the check for zero may mean the pipe
  // is added to the ZombieQueue twice, which causes a double free.
  bool bDataChanged = false;
  bool queueRetirement = false;
  {
    LockGuard<Mutex> guard(m_Lock);

    if (m_nReaders == 0 && m_nWriters == 0) {
      // Refcount is already zero - don't decrement! (also, bad.)
      ERROR("Pipe: decreasing refcount when refcount is already zero.");
      return;
    }

    if (bIsWriter) {
      m_nWriters--;
      if (m_nWriters == 0) {
        ++m_HangupGeneration;
        // Wakes up readers waiting as they won't be able to be woken
        // by new bytes being written anymore.
        m_Buffer.disableWrites();
        bDataChanged = true;
      }
    } else {
      const bool wasReadyForWrite = !m_nReaders || m_Buffer.canWrite(false);
      m_nReaders--;
      if (m_nReaders == 0) {
        if (!wasReadyForWrite) {
          ++m_WriteGeneration;
        }
        ++m_ErrorGeneration;
        // Wake up any writers that were waiting for space - no more
        // readers (EOF condition, pipe other end has left).
        m_Buffer.disableReads();
        bDataChanged = true;
      }
    }

    if (!m_nReaders && !m_nWriters) {
      // Named FIFO storage survives its final open description, unlike its data.
      m_Buffer.wipe();
    }
    queueRetirement = shouldQueueRetirementLocked();
    if (queueRetirement) {
      bDataChanged = false;
    }
  }

  if (queueRetirement) {
    size_t pid = Processor::information().getCurrentThread()->getParent()->getId();
#if VERBOSE_KERNEL
    NOTICE("Adding pipe [" << pid << "] " << this << " to ZombieQueue");
#endif
    ZombieQueue::instance().addObject(new ZombiePipe(this));
    return;
  }

  if (bDataChanged) {
    dataChanged();
  }
}

bool Pipe::retainVfsReference() {
  if (!m_bIsAnonymous) {
    return File::retainVfsReference();
  }

  LockGuard<Mutex> guard(m_Lock);
  if (m_bRetirementQueued) {
    return false;
  }
  ++m_nLifetimePins;
  return true;
}

void Pipe::releaseVfsReference() {
  if (!m_bIsAnonymous) {
    File::releaseVfsReference();
    return;
  }

  bool queueRetirement = false;
  {
    LockGuard<Mutex> guard(m_Lock);
    assert(m_nLifetimePins);
    --m_nLifetimePins;
    queueRetirement = shouldQueueRetirementLocked();
  }

  if (queueRetirement) {
    size_t pid = Processor::information().getCurrentThread()->getParent()->getId();
#if VERBOSE_KERNEL
    NOTICE("Adding pipe [" << pid << "] " << this << " to ZombieQueue");
#endif
    ZombieQueue::instance().addObject(new ZombiePipe(this));
  }
}

bool Pipe::shouldQueueRetirementLocked() {
  if (!m_bIsAnonymous || m_bRetirementQueued || m_nLifetimePins || m_nReaders || m_nWriters) {
    return false;
  }

  m_bRetirementQueued = true;
  return true;
}

size_t Pipe::getReaderCount() {
  LockGuard<Mutex> guard(m_Lock);
  return m_nReaders;
}

size_t Pipe::getWriterCount() {
  LockGuard<Mutex> guard(m_Lock);
  return m_nWriters;
}

bool Pipe::waitForReader(bool bCanBlock) {
  m_Lock.acquire();
  while (!m_nReaders) {
    if (!bCanBlock) {
      m_Lock.release();
      return false;
    }

    ConditionVariable::Error error = ConditionVariable::NoError;
    if (!m_ReaderCondition.wait(m_Lock, error)) {
      if (ConditionVariable::mutexAcquired(error)) {
        m_Lock.release();
      }
      return false;
    }
  }
  m_Lock.release();
  return true;
}
