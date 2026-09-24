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
#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/SyscallManager.h"
#include "pedigree/kernel/processor/state.h"
#include "pedigree/kernel/syscallError.h"
#include "pedigree/kernel/utilities/String.h"

#include "PedigreeCSyscallManager.h"
#include "pedigreecSyscallNumbers.h"

#include "pedigree-syscalls.h"

PedigreeCSyscallManager::PedigreeCSyscallManager() {}

PedigreeCSyscallManager::~PedigreeCSyscallManager() {}

bool PedigreeCSyscallManager::initialise() {
  if (!SyscallManager::instance().registerSyscallHandler(pedigree_c, this, m_Registration)) {
    return false;
  }
  return true;
}

bool PedigreeCSyscallManager::shutdown() {
  return m_Registration.reset();
}

uintptr_t PedigreeCSyscallManager::call(uintptr_t function, uintptr_t p1, uintptr_t p2,
                                        uintptr_t p3, uintptr_t p4, uintptr_t p5) {
  if (function >= serviceEnd) {
    ERROR("PedigreeCSyscallManager: invalid function called: " << Dec
                                                               << static_cast<int>(function));
    return 0;
  }
  return SyscallManager::instance().syscall(posix, function, p1, p2, p3, p4, p5);
}

uintptr_t PedigreeCSyscallManager::syscall(SyscallState& state) {
  uintptr_t p1 = state.getSyscallParameter(0);
  uintptr_t p2 = state.getSyscallParameter(1);
  uintptr_t p3 = state.getSyscallParameter(2);

  // We're interruptible.
  Processor::setInterrupts(true);

  switch (state.getSyscallNumber()) {
    // Pedigree system calls, called from POSIX applications
    case PEDIGREE_LOGIN:
      return pedigree_login(static_cast<int>(p1));
    case PEDIGREE_LOAD_KEYMAP:
      return pedigree_load_keymap(reinterpret_cast<uint32_t*>(p1), p2);
    case PEDIGREE_GET_MOUNT:
      return pedigree_get_mount(reinterpret_cast<char*>(p1), reinterpret_cast<char*>(p2), p3);
    case PEDIGREE_REBOOT:
      pedigree_reboot();
      return 0;
    case PEDIGREE_MODULE_LOAD:
      pedigree_module_load(reinterpret_cast<char*>(p1));
      return 0;
    case PEDIGREE_MODULE_UNLOAD: {
      if (!p1) {
        SYSCALL_ERROR(InvalidArgument);
        return -1;
      }

      // KernelElf must not invoke this module's exit routine while its
      // syscall handler is still pinned on the current stack.
      String moduleName(reinterpret_cast<const char*>(p1));
      if (moduleName.compare("pedigree-c")) {
        SYSCALL_ERROR(DeviceBusy);
        return -1;
      }

      if (!pedigree_module_unload(const_cast<char*>(moduleName.cstr()))) {
        SYSCALL_ERROR(DeviceBusy);
        return -1;
      }
      return 0;
    }
    case PEDIGREE_MODULE_IS_LOADED:
      return pedigree_module_is_loaded(reinterpret_cast<char*>(p1));
    case PEDIGREE_MODULE_GET_DEPENDING:
      return pedigree_module_get_depending(reinterpret_cast<char*>(p1), reinterpret_cast<char*>(p2),
                                           p3);
    case PEDIGREE_INPUT_INHIBIT_EVENTS:
      pedigree_input_inhibit_events(p1);
      return 0;
    case PEDIGREE_EVENT_RETURN:
      return pedigree_event_return();
    case PEDIGREE_SYS_REQUEST_MEM:
      return reinterpret_cast<uintptr_t>(pedigree_sys_request_mem(p1));
    case PEDIGREE_HALTFS:
      pedigree_haltfs();
      return 0;
    default:
      ERROR("PedigreeCSyscallManager: invalid syscall received: " << Dec
                                                                  << state.getSyscallNumber());
      return 0;
  }
}
