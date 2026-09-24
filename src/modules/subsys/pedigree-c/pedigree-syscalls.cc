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

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/linker/KernelElf.h"
#include "pedigree/kernel/machine/KeymapManager.h"
#include "pedigree/kernel/process/Process.h"
#include "pedigree/kernel/process/TerminationDeferral.h"
#include "pedigree/kernel/process/Thread.h"
#include "pedigree/kernel/process/eventNumbers.h"
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/SyscallManager.h"
#include "pedigree/kernel/syscallError.h"

#include "modules/system/vfs/File.h"
#include "modules/system/vfs/MemoryMappedFile.h"
#include "modules/system/vfs/MountView.h"
#include "modules/system/vfs/VFS.h"

#include "pedigree-syscalls.h"

// Module handling functions

// Load a module
void pedigree_module_load(char* _file) {
  TerminationDeferral lifetime;
  Process* process = Processor::information().getCurrentThread()->getParent();
  auto context = process->acquireFilesystemContext();
  auto* view = VFS::instance().mountView();
  FilesystemPathRef selected;
  VfsMountView::ResolveOptions options;
  if (!view || !context ||
      !view->resolve(context, FilesystemPathRef(), String(_file), options, selected)) {
    SYSCALL_ERROR(DoesNotExist);
    return;
  }
  File* file = selected->node();

  if (file->isDirectory()) {
    // Error - is directory.
    SYSCALL_ERROR(IsADirectory);
    return;
  }

  // Map the module in the memory
  uintptr_t buffer = 0;
  MemoryMappedObject* pMmFile =
      MemoryMapManager::instance().mapFile(file, buffer, file->getSize(), MemoryMappedObject::Read);
  KernelElf::instance().loadModule(reinterpret_cast<uint8_t*>(buffer), file->getSize(), true);
  MemoryMapManager::instance().unmap(pMmFile);
}

// Unload a module
int pedigree_module_unload(char* name) {
  return KernelElf::instance().unloadModule(name, true) ? 1 : 0;
}

// Check if a module is loaded
int pedigree_module_is_loaded(char* name) {
  return (KernelElf::instance().moduleIsLoaded(name) ? 1 : 0);
}

// Get the first module that depends on the specified module
int pedigree_module_get_depending(char* name, char* buf, size_t bufsz) {
  char* dep = KernelElf::instance().getDependingModule(name);
  if (dep)
    StringCopyN(buf, dep, bufsz);
  else
    return 0;
  return 1;
}

void pedigree_input_inhibit_events(int inhibit) {
  Thread* pThread = Processor::information().getCurrentThread();
  pThread->inhibitEvent(EventNumbers::InputEvent, inhibit == 1);
}

int pedigree_load_keymap(uint32_t* buf, size_t len) {
  /// \todo check parameter is mapped in
  if (!KeymapManager::instance().useCompiledKeymap(buf, len)) {
    return -1;
  } else {
    return 0;
  }
}

int pedigree_event_return() {
  if (!SyscallManager::instance().requestEventReturn()) {
    SYSCALL_ERROR(InvalidArgument);
    return -1;
  }
  return 0;
}

void* pedigree_sys_request_mem(size_t len) {
  Process* pProcess = Processor::information().getCurrentThread()->getParent();
  uintptr_t mapAddress = 0;
  if (!pProcess->allocateUserRange(Process::UserRegion::Dynamic, len, mapAddress)) {
    if (!pProcess->allocateUserRange(Process::UserRegion::Normal, len, mapAddress)) {
      return 0;
    }
  }

  return reinterpret_cast<void*>(mapAddress);
}

void pedigree_haltfs() {
  // Synchronises all filesystems and disks and unloads them.
  /// \todo Implement
  NOTICE("Stubbed: pedigree_haltfs");
}
