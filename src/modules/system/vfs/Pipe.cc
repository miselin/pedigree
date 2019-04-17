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

class ZombiePipe : public ZombieObject
{
  public:
    ZombiePipe(Pipe *pPipe) : m_pPipe(pPipe)
    {
    }
    virtual ~ZombiePipe();

  private:
    Pipe *m_pPipe;
};

ZombiePipe::~ZombiePipe()
{
    NOTICE("ZombiePipe: freeing " << m_pPipe);
    delete m_pPipe;
}

Pipe::Pipe()
    : File(), m_bIsAnonymous(true), m_bIsEOF(false), m_Buffer(PIPE_BUF_MAX),
      m_ReaderSem(0)
{
#if VERBOSE_KERNEL
    NOTICE("Pipe: new anonymous pipe " << reinterpret_cast<uintptr_t>(this));
#endif
}

Pipe::Pipe(
    const String &name, Time::Timestamp accessedTime,
    Time::Timestamp modifiedTime, Time::Timestamp creationTime, uintptr_t inode,
    Filesystem *pFs, size_t size, File *pParent, bool bIsAnonymous)
    : File(
          name, accessedTime, modifiedTime, creationTime, inode, pFs, size,
          pParent),
      m_bIsAnonymous(bIsAnonymous), m_bIsEOF(false), m_Buffer(PIPE_BUF_MAX),
      m_ReaderSem(0)
{
#if VERBOSE_KERNEL
    NOTICE(
        "Pipe: new " << (bIsAnonymous ? "anonymous" : "named") << " pipe "
                     << Hex << this);
#endif
}

Pipe::~Pipe()
{
    // ensure anything else in the critical section can finish before we clean
    // up fully
    // this is useful for cases where ZombieQueue destroys us before we get a
    // chance to actually return from decreaseRefCount (which accesses the lock)
    m_Lock.acquire();
    m_Lock.release();
}

int Pipe::select(bool bWriting, int timeout)
{
    if (bWriting)
    {
        return m_Buffer.canWrite(timeout > 0) ? 1 : 0;
    }
    else
    {
        return m_Buffer.canRead(timeout > 0) ? 1 : 0;
    }
}

uint64_t Pipe::readBytewise(
    uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock)
{
    // Need to read what's left in the pipe then EOF if there's no more readers!
    if (m_nWriters == 0)
    {
        bCanBlock = false;
    }

    uint8_t *pBuf = reinterpret_cast<uint8_t *>(buffer);
    return m_Buffer.read(pBuf, size, bCanBlock);
}

uint64_t Pipe::writeBytewise(
    uint64_t location, uint64_t size, uintptr_t buffer, bool bCanBlock)
{
    if (m_nReaders == 0)
    {
        // no more readers, abort the write
        return 0;
    }

    uint8_t *pBuf = reinterpret_cast<uint8_t *>(buffer);
    uint64_t result = m_Buffer.write(pBuf, size, bCanBlock);
    if (result)
    {
        dataChanged();
    }

    return result;
}

bool Pipe::isPipe() const
{
    return getName().length() == 0 || m_bIsAnonymous;
}

bool Pipe::isFifo() const
{
    return getName().length() > 0 && !m_bIsAnonymous;
}

void Pipe::increaseRefCount(bool bIsWriter)
{
    if (bIsWriter)
    {
        // Enable writes if they were previously disabled.
        if (!m_Buffer.enableWrites())
        {
            // Writes were disabled previously (EOF), so wipe the pipe.
            m_Buffer.wipe();
        }
        m_nWriters++;
    }
    else
    {
        // A reader is now present so we can enable reads if they weren't.
        m_Buffer.enableReads();
        m_nReaders++;

        m_ReaderSem.release();
    }
}

void Pipe::decreaseRefCount(bool bIsWriter)
{
    // Make sure only one thread decreases the refcount at a time. This is
    // important as we add ourselves to the ZombieQueue if the refcount ticks
    // to zero. Getting pre-empted by another thread that also decreases the
    // refcount between the decrement and the check for zero may mean the pipe
    // is added to the ZombieQueue twice, which causes a double free.
    bool bDataChanged = false;
    {
        LockGuard<Mutex> guard(m_Lock);

        if (m_nReaders == 0 && m_nWriters == 0)
        {
            // Refcount is already zero - don't decrement! (also, bad.)
            ERROR("Pipe: decreasing refcount when refcount is already zero.");
            return;
        }

        if (bIsWriter)
        {
            m_nWriters--;
            if (m_nWriters == 0)
            {
                // Wakes up readers waiting as they won't be able to be woken
                // by new bytes being written anymore.
                m_Buffer.disableWrites();
                bDataChanged = true;
            }
        }
        else
        {
            m_nReaders--;
            if (m_nReaders == 0)
            {
                // Wake up any writers that were waiting for space - no more
                // readers (EOF condition, pipe other end has left).
                m_Buffer.disableReads();
                bDataChanged = true;
            }
        }

        if (m_nReaders == 0 && m_nWriters == 0)
        {
            // If we're anonymous, die completely.
            if (m_bIsAnonymous)
            {
                size_t pid = Processor::information()
                                 .getCurrentThread()
                                 ->getParent()
                                 ->getId();
#if VERBOSE_KERNEL
                NOTICE(
                    "Adding pipe [" << pid << "] " << this
                                    << " to ZombieQueue");
#endif
                ZombieQueue::instance().addObject(new ZombiePipe(this));
                bDataChanged = false;
            }
        }
    }

    if (bDataChanged)
    {
        dataChanged();
    }
}

bool Pipe::waitForReader()
{
    return m_ReaderSem.acquire();
}
