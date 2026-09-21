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

#include "pedigree/kernel/Spinlock.h"
#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/process/ConditionVariable.h"
#include "pedigree/kernel/process/Mutex.h"
#include "pedigree/kernel/process/OperationBarrier.h"
#include "pedigree/kernel/process/Semaphore.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/Buffer.h"
#include "pedigree/kernel/utilities/SharedPointer.h"
#include "pedigree/kernel/utilities/String.h"
#include "pedigree/kernel/utilities/Vector.h"
#include "pedigree/kernel/utilities/utility.h"

#include "ConsoleDefines.h"
#include "modules/system/vfs/File.h"
#include "modules/system/vfs/Filesystem.h"

class Disk;
class Process;
class RequestQueue;

#define DEFAULT_FLAGS                                                                             \
  (static_cast<size_t>(ConsoleManager::OPostProcess) |                                            \
   static_cast<size_t>(ConsoleManager::IMapCRToNL) |                                              \
   static_cast<size_t>(ConsoleManager::OMapNLToCRNL) |                                            \
   static_cast<size_t>(ConsoleManager::LEcho) | static_cast<size_t>(ConsoleManager::LEchoErase) | \
   static_cast<size_t>(ConsoleManager::LEchoKill) |                                               \
   static_cast<size_t>(ConsoleManager::LCookedMode) |                                             \
   static_cast<size_t>(ConsoleManager::LGenerateEvent))

class EXPORTED_PUBLIC ConsoleControlState {
 public:
  enum class Character { Interrupt, Quit, Suspend };
  virtual ~ConsoleControlState() = default;
  virtual void controlCharacter(Character) {}
};

class EXPORTED_PUBLIC ConsoleIoState {
 public:
  ConsoleIoState();
  ~ConsoleIoState();
  bool revoked() const;
  void closeAdmission();
  void cancelAndDrain();
  OperationBarrier operations;
  Buffer<char> input;
  Buffer<char> output;
  Mutex inputLock;
  char line[LINEBUFFER_MAXIMUM];
  size_t lineSize;
  size_t firstNewline;
  Semaphore physicalWake;

 private:
  bool m_Revoked;
};

class EXPORTED_PUBLIC ConsoleFile : public File {
  friend class ConsoleMasterFile;
  friend class ConsoleSlaveFile;
  friend class ConsoleManager;

 public:
  ConsoleFile(size_t consoleNumber, String consoleName, Filesystem* pFs);
  ConsoleFile(size_t consoleNumber, String consoleName, Filesystem* pFs, File* pParent);
  virtual ~ConsoleFile() {}

  SharedPointer<ConsoleIoState> captureOpenEpoch(bool waitForReopen = true);
  bool beginRevocation(SharedPointer<ConsoleIoState>& retired);
  void finishRevocation(const SharedPointer<ConsoleIoState>& retired,
                        const SharedPointer<ConsoleIoState>& replacement);
  uint64_t readEpoch(const SharedPointer<ConsoleIoState>& epoch, uint64_t size, uintptr_t buffer,
                     bool canBlock);
  uint64_t writeEpoch(const SharedPointer<ConsoleIoState>& epoch, uint64_t size, uintptr_t buffer,
                      bool canBlock);
  ReadyMask queryEpoch(const SharedPointer<ConsoleIoState>& epoch, bool reading, bool writing);
  ReadinessGenerations epochGenerations(const SharedPointer<ConsoleIoState>& epoch);
  SharedPointer<ConsoleControlState> controlState();
  void setControlState(const SharedPointer<ConsoleControlState>& state);

  virtual bool isMaster() = 0;

  bool isPtySlave() {
    return !isMaster() && getParent() != nullptr;
  }

  bool isPtyMaster() {
    return isMaster() && m_pOther != nullptr && m_pOther->getParent() != nullptr;
  }

  virtual bool isSeekable() const {
    return false;
  }

  ReadyMask queryReady(bool reading, bool writing) override;
  ReadinessGenerations readinessGenerations() override;

  bool supportsReadinessNotifications() const override {
    return true;
  }

  /// Grabs the current array of control characters.
  void getControlCharacters(char* out) {
    MemoryCopy(out, m_ControlChars, MAX_CONTROL_CHAR);
  }

  virtual size_t getBlockSize() const {
    return PTY_BUFFER_SIZE;
  }

  size_t getConsoleNumber() const {
    return m_ConsoleNumber;
  }

  virtual size_t getPhysicalConsoleNumber() const {
    return ~0U;
  }

 protected:
  /// select - check and optionally for a particular state.
  virtual int select(bool bWriting, int timeout);

  virtual uint64_t readIo(ConsoleIoState& state, uint64_t size, uintptr_t buffer,
                          bool canBlock) = 0;
  virtual uint64_t writeIo(ConsoleIoState& state, uint64_t size, uintptr_t buffer,
                           bool canBlock) = 0;
  void changed();
  ConsoleFile* stateOwner();

  /// Other side of the console.
  ConsoleFile* m_pOther;

  size_t m_Flags;
  char m_ControlChars[MAX_CONTROL_CHAR];

  unsigned short m_Rows;
  unsigned short m_Cols;

  /// Output line discipline
  static size_t outputLineDiscipline(char* buf, size_t len, size_t maxSz, size_t flags = 0);

  /// Input processing.
  size_t processInput(char* buf, size_t len);

  /// Input line discipline
  void inputLineDiscipline(ConsoleIoState& state, char* buf, size_t len, bool canBlock,
                           size_t flags = ~0U, const char* controlChars = nullptr);

  mutable Mutex m_IoLock;
  ConditionVariable m_IoChanged;
  bool m_HangingUp;
  SharedPointer<ConsoleIoState> m_IoState;
  SharedPointer<ConsoleControlState> m_ControlState;

 private:
  size_t m_ConsoleNumber;
  String m_ConsoleName;

  bool isControlCharacter(size_t flags, char check, const char* controlChars);
  void notifyControlCharacter(char cause, const char* controlChars);

  virtual bool isBytewise() const {
    return true;
  }
};

class EXPORTED_PUBLIC ConsoleMasterFile : public ConsoleFile {
 public:
  ConsoleMasterFile(size_t consoleNumber, String consoleName, Filesystem* pFs);
  virtual ~ConsoleMasterFile() {}

  virtual uint64_t readBytewise(uint64_t location, uint64_t size, uintptr_t buffer,
                                bool bCanBlock = true);
  virtual uint64_t writeBytewise(uint64_t location, uint64_t size, uintptr_t buffer,
                                 bool bCanBlock = true);

  void setOther(ConsoleFile* pOther) {
    m_pOther = pOther;
    m_Flags = pOther->m_Flags;
  }

  /// Is this master locked (ie, already opened)?
  bool bLocked;

  /// Who holds the lock on the console? (ie, same process can 'lock'
  /// twice...)
  Process* pLocker;

  /// Whether the corresponding UNIX 98 slave rejects new opens.
  bool bSlaveLocked;

  virtual bool isMaster() {
    return true;
  }

 private:
  uint64_t readIo(ConsoleIoState&, uint64_t, uintptr_t, bool) override;
  uint64_t writeIo(ConsoleIoState&, uint64_t, uintptr_t, bool) override;
};

class EXPORTED_PUBLIC ConsoleSlaveFile : public ConsoleFile {
 public:
  ConsoleSlaveFile(size_t consoleNumber, String consoleName, Filesystem* pFs);
  ConsoleSlaveFile(size_t consoleNumber, String consoleName, Filesystem* pFs, File* pParent);
  virtual ~ConsoleSlaveFile() {}

  virtual uint64_t readBytewise(uint64_t location, uint64_t size, uintptr_t buffer,
                                bool bCanBlock = true);
  virtual uint64_t writeBytewise(uint64_t location, uint64_t size, uintptr_t buffer,
                                 bool bCanBlock = true);

  void setOther(ConsoleFile* pOther) {
    m_pOther = pOther;
  }

  virtual bool isMaster() {
    return false;
  }

 private:
  uint64_t readIo(ConsoleIoState&, uint64_t, uintptr_t, bool) override;
  uint64_t writeIo(ConsoleIoState&, uint64_t, uintptr_t, bool) override;
};

class EXPORTED_PUBLIC ConsolePhysicalFile : public ConsoleFile {
 public:
  ConsolePhysicalFile(size_t nth, File* pTerminal, String consoleName, Filesystem* pFs);
  virtual ~ConsolePhysicalFile() {}

  virtual uint64_t readBytewise(uint64_t location, uint64_t size, uintptr_t buffer,
                                bool bCanBlock = true);
  virtual uint64_t writeBytewise(uint64_t location, uint64_t size, uintptr_t buffer,
                                 bool bCanBlock = true);

  virtual bool isMaster() {
    return false;
  }

  virtual size_t getPhysicalConsoleNumber() const {
    return m_TerminalNumber;
  }

  virtual int select(bool bWriting, int timeout);

  ReadyMask queryReady(bool reading, bool writing) override {
    return File::queryReady(reading, writing);
  }

  bool supportsReadinessNotifications() const override {
    return true;
  }

  void terminalReadinessChanged(ReadyMask mask);

 private:
  File* m_pTerminal;
  size_t m_TerminalNumber;
  SharedPointer<ReadinessObserver> m_TerminalReadinessObserver;
  ReadinessSubscription m_TerminalReadinessSubscription;

  uint64_t readIo(ConsoleIoState&, uint64_t, uintptr_t, bool) override;
  uint64_t writeIo(ConsoleIoState&, uint64_t, uintptr_t, bool) override;
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
class EXPORTED_PUBLIC ConsoleManager : public Filesystem {
 public:
  enum IAttribute { IMapCRToNL = 1, IIgnoreCR = 2, IMapNLToCR = 4, IStripToSevenBits = 8 };
  enum OAttribute {
    OPostProcess = 16,
    OMapCRToNL = 32,
    ONoCrAtCol0 = 64,
    OMapNLToCRNL = 128,
    ONLCausesCR = 256
  };
  enum LAttribute {
    LEcho = 512,
    LEchoErase = 1024,
    LEchoKill = 2048,
    LEchoNewline = 4096,
    LCookedMode = 8192,
    LGenerateEvent = 16384
  };

  ConsoleManager();

  virtual ~ConsoleManager();

  static ConsoleManager& instance();

  //
  // ConsoleManager interface.
  //
  File* getConsole(String consoleName);
  ConsoleFile* getConsoleFile(RequestQueue* pBackend);

  /// Acquire a console master in such a way that it cannot be opened by
  /// another process.
  bool lockConsole(File* file);

  /// Release a console master locked as above.
  void unlockConsole(File* file);

  /// Set the UNIX 98 PTY slave lock associated with a master.
  bool setPtyLock(File* file, bool locked);

  /// Returns whether a PTY slave is currently locked.
  bool isPtySlaveLocked(File* file);

  /// Create a new console - /dev/ptyXY -> /dev/ttyXY, where X is @c and Y is
  /// @i.
  void newConsole(char c, size_t i);

  bool isConsole(File* file);
  bool isMasterConsole(File* file);

  void setAttributes(File* file, size_t flags);
  void getAttributes(File* file, size_t* flags);
  void getControlChars(File* file, void* p);
  void setControlChars(File* file, void* p);
  int getWindowSize(File* file, unsigned short* rows, unsigned short* cols);
  int setWindowSize(File* file, unsigned short rows, unsigned short cols);
  bool hasDataAvailable(File* file);
  void flush(File* file);

  File* getOther(File* file);

  //
  // Filesystem interface.
  //

  virtual bool initialise(Disk* pDisk) {
    return false;
  }
  virtual File* getRoot() const {
    return 0;
  }
  virtual const String& getVolumeLabel() const {
    static String volumeLabel("consolemanager");
    return volumeLabel;
  }

 protected:
  virtual bool createFile(File* parent, const String& filename, uint32_t mask) {
    return false;
  }
  virtual bool createDirectory(File* parent, const String& filename, uint32_t mask) {
    return false;
  }
  virtual bool createSymlink(File* parent, const String& filename, const String& value) {
    return false;
  }
  virtual bool removeNode(File*, const String&, File*) {
    return false;
  }

 private:
  Vector<ConsoleFile*> m_Consoles;
  static ConsoleManager m_Instance;
  Spinlock m_Lock;

  void newConsole(char c, size_t i, bool lock);
};

#endif
