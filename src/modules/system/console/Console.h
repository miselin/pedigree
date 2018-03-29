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

#ifndef CONSOLE_H
#define CONSOLE_H

#include "modules/system/vfs/File.h"
#include "modules/system/vfs/Filesystem.h"
#include "modules/system/vfs/VFS.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/utilities/Buffer.h"
#include "pedigree/kernel/utilities/RequestQueue.h"
#include "pedigree/kernel/utilities/Vector.h"

#include "ConsoleDefines.h"

#define DEFAULT_FLAGS                                            \
    (ConsoleManager::OPostProcess | ConsoleManager::IMapCRToNL | \
     ConsoleManager::OMapNLToCRNL | ConsoleManager::LEcho |      \
     ConsoleManager::LEchoErase | ConsoleManager::LEchoKill |    \
     ConsoleManager::LCookedMode | ConsoleManager::LGenerateEvent)

class ConsoleManager;
class ConsoleMasterFile;
class ConsoleSlaveFile;

class Process;

class ConsoleFile : public File
{
    friend class ConsoleMasterFile;
    friend class ConsoleSlaveFile;
    friend class ConsoleManager;

  public:
    ConsoleFile(size_t consoleNumber, String consoleName, Filesystem *pFs);
    virtual ~ConsoleFile()
    {
    }

    virtual uint64_t read(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true) = 0;
    virtual uint64_t write(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true) = 0;

    virtual bool isMaster() = 0;

    void setEvent(Event *e)
    {
        if (isMaster())
            m_pOther->m_pEvent = e;
        else
            m_pEvent = e;
    }

    /**
     * getLast - get the most recent character we handled.
     * This is to be used by event handlers, which will be called when
     * a special character is handled. The event handler can then call
     * this function to identify the character and perform the relevant
     * processing it needs to.
     */
    virtual char getLast()
    {
        return m_Last;
    }

    Event *getEvent() const
    {
        return m_pEvent;
    }

    /// Grabs the current array of control characters.
    void getControlCharacters(char *out)
    {
        MemoryCopy(out, m_ControlChars, MAX_CONTROL_CHAR);
    }

    virtual size_t getBlockSize() const
    {
        return PTY_BUFFER_SIZE;
    }

    size_t getConsoleNumber() const
    {
        return m_ConsoleNumber;
    }

    virtual size_t getPhysicalConsoleNumber() const
    {
        return ~0U;
    }

    /**
     * In order to ensure getLast is always the most recent character,
     * the thread that wrote a special character to the input stream
     * is put to sleep until the event handler calls this function.
     */
    virtual void eventComplete()
    {
        if (!isMaster())
            m_pOther->eventComplete();
        else
            m_EventTrigger.release();
    }

  protected:
    /// select - check and optionally for a particular state.
    virtual int select(bool bWriting, int timeout);

    /// inject - inject bytes into the ring buffer
    void inject(char *buf, size_t len, bool canBlock);

    /// Override to permit different injection semantics.
    /// The default is to call m_pOther->inject.
    virtual void performInject(char *buf, size_t len, bool canBlock);

    /// Performs an event trigger.
    /// The default is to call triggerEvent which uses m_pOther.
    virtual void performEventTrigger(char cause);

    /// Other side of the console.
    ConsoleFile *m_pOther;

    size_t m_Flags;
    char m_ControlChars[MAX_CONTROL_CHAR];

    unsigned short m_Rows;
    unsigned short m_Cols;

    /// Output line discipline
    static size_t
    outputLineDiscipline(char *buf, size_t len, size_t maxSz, size_t flags = 0);

    /// Input processing.
    size_t processInput(char *buf, size_t len);

    /// Input line discipline
    void inputLineDiscipline(
        char *buf, size_t len, size_t flags = ~0U,
        const char *controlChars = 0);

    /// Input line buffer.
    char m_LineBuffer[LINEBUFFER_MAXIMUM];

    /// Size of the input line buffer.
    size_t m_LineBufferSize;

    /// Location of the first newline in the line buffer. ~0 if none.
    size_t m_LineBufferFirstNewline;

    /// Character that triggered an event.
    char m_Last;

    Buffer<char> m_Buffer;

  private:
    size_t m_ConsoleNumber;
    String m_Name;

    /**
     * Event to fire when an event takes place that needs action. For
     * example, when ^C is typed. The handler for the event figures
     * out what to do.
     */
    Event *m_pEvent;

    /// Locked when we trigger an event, unlocked when eventComplete called.
    Mutex m_EventTrigger;

    /// Check if the given character requires an event.
    bool checkForEvent(size_t flags, char check, const char *controlChars);

    /// Triggers our event.
    void triggerEvent(char cause);
};

class EXPORTED_PUBLIC ConsoleMasterFile : public ConsoleFile
{
  public:
    ConsoleMasterFile(
        size_t consoleNumber, String consoleName, Filesystem *pFs);
    virtual ~ConsoleMasterFile()
    {
    }

    virtual uint64_t read(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);
    virtual uint64_t write(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);

    void setOther(ConsoleFile *pOther)
    {
        m_pOther = pOther;
        m_Flags = pOther->m_Flags;
    }

    /// Is this master locked (ie, already opened)?
    bool bLocked;

    /// Who holds the lock on the console? (ie, same process can 'lock'
    /// twice...)
    Process *pLocker;

    virtual bool isMaster()
    {
        return true;
    }

  private:
};

class EXPORTED_PUBLIC ConsoleSlaveFile : public ConsoleFile
{
  public:
    ConsoleSlaveFile(size_t consoleNumber, String consoleName, Filesystem *pFs);
    virtual ~ConsoleSlaveFile()
    {
    }

    virtual uint64_t read(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);
    virtual uint64_t write(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);

    void setOther(ConsoleFile *pOther)
    {
        m_pOther = pOther;
    }

    virtual bool isMaster()
    {
        return false;
    }

    virtual char getLast()
    {
        return m_pOther->getLast();
    }
};

class EXPORTED_PUBLIC ConsolePhysicalFile : public ConsoleFile
{
  public:
    ConsolePhysicalFile(size_t nth, File *pTerminal, String consoleName, Filesystem *pFs);
    virtual ~ConsolePhysicalFile()
    {
    }

    virtual uint64_t read(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);
    virtual uint64_t write(
        uint64_t location, uint64_t size, uintptr_t buffer,
        bool bCanBlock = true);

    virtual bool isMaster()
    {
        return false;
    }

    virtual char getLast()
    {
        /// \todo this is no good
        return '\0';
    }

    virtual size_t getPhysicalConsoleNumber() const
    {
        return m_TerminalNumber;
    }

    virtual int select(bool bWriting, int timeout);

  private:
    File *m_pTerminal;
    Buffer<char> m_ProcessedInput;
    size_t m_TerminalNumber;

    virtual void performInject(char *buf, size_t len, bool canBlock);
};

/** This class provides a way for consoles (TTYs) to be created to interact with
   applications.

    registerConsole is called by a class that subclasses RequestQueue.
   getConsole returns a File that forwards any read/write requests straight to
   that RequestQueue backend.

    read: request with p1: CONSOLE_READ, p2: param, p3: requested size, p4:
   buffer. write: request with p1: CONSOLE_WRITE, p2: param, p3: size of buffer,
   p4: buffer.
*/
class EXPORTED_PUBLIC ConsoleManager : public Filesystem
{
  public:
    enum IAttribute
    {
        IMapCRToNL = 1,
        IIgnoreCR = 2,
        IMapNLToCR = 4,
        IStripToSevenBits = 8
    };
    enum OAttribute
    {
        OPostProcess = 16,
        OMapCRToNL = 32,
        ONoCrAtCol0 = 64,
        OMapNLToCRNL = 128,
        ONLCausesCR = 256
    };
    enum LAttribute
    {
        LEcho = 512,
        LEchoErase = 1024,
        LEchoKill = 2048,
        LEchoNewline = 4096,
        LCookedMode = 8192,
        LGenerateEvent = 16384
    };

    ConsoleManager();

    virtual ~ConsoleManager();

    static ConsoleManager &instance();

    //
    // ConsoleManager interface.
    //
    File *getConsole(String consoleName);
    ConsoleFile *getConsoleFile(RequestQueue *pBackend);

    /// Acquire a console master in such a way that it cannot be opened by
    /// another process.
    bool lockConsole(File *file);

    /// Release a console master locked as above.
    void unlockConsole(File *file);

    /// Create a new console - /dev/ptyXY -> /dev/ttyXY, where X is @c and Y is
    /// @i.
    void newConsole(char c, size_t i);

    bool isConsole(File *file);
    bool isMasterConsole(File *file);

    void setAttributes(File *file, size_t flags);
    void getAttributes(File *file, size_t *flags);
    void getControlChars(File *file, void *p);
    void setControlChars(File *file, void *p);
    int getWindowSize(File *file, unsigned short *rows, unsigned short *cols);
    int setWindowSize(File *file, unsigned short rows, unsigned short cols);
    bool hasDataAvailable(File *file);
    void flush(File *file);

    File *getOther(File *file);

    //
    // Filesystem interface.
    //

    virtual bool initialise(Disk *pDisk)
    {
        return false;
    }
    virtual File *getRoot()
    {
        return 0;
    }
    virtual String getVolumeLabel()
    {
        return String("consolemanager");
    }

  protected:
    virtual bool createFile(File *parent, String filename, uint32_t mask)
    {
        return false;
    }
    virtual bool createDirectory(File *parent, String filename, uint32_t mask)
    {
        return false;
    }
    virtual bool createSymlink(File *parent, String filename, String value)
    {
        return false;
    }
    virtual bool remove(File *parent, File *file)
    {
        return false;
    }

  private:
    Vector<ConsoleFile *> m_Consoles;
    static ConsoleManager m_Instance;
    Spinlock m_Lock;
};

#endif
