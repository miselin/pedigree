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

#ifndef DWARFSTATE_H
#define DWARFSTATE_H
#include <config.h>

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/utility.h"

/** @addtogroup kerneldebugger
 * @{ */
#define DWARF_MAX_REGISTERS 50

// X86
#define DWARF_REG_EAX 0
#define DWARF_REG_ECX 1
#define DWARF_REG_EDX 2
#define DWARF_REG_EBX 3
#define DWARF_REG_ESP 4
#define DWARF_REG_EBP 5
#define DWARF_REG_ESI 6
#define DWARF_REG_EDI 7
// X64
#define DWARF_REG_RAX 0
#define DWARF_REG_RDX 1
#define DWARF_REG_RCX 2
#define DWARF_REG_RBX 3
#define DWARF_REG_RSI 4
#define DWARF_REG_RDI 5
#define DWARF_REG_RBP 6
#define DWARF_REG_RSP 7
#define DWARF_REG_R8 8
#define DWARF_REG_R9 9
#define DWARF_REG_R10 10
#define DWARF_REG_R11 11
#define DWARF_REG_R12 12
#define DWARF_REG_R13 13
#define DWARF_REG_R14 14
#define DWARF_REG_R15 15
#define DWARF_REG_RFLAGS 49
// Watch out! Register numbering is seemingly random - x86 and x86_64 ones are
// here: http://wikis.sun.com/display/SunStudio/Dwarf+Register+Numbering
/**
 * Holds one row of a Dwarf CFI table. We technically generate a table, but we
 * only keep track of the current row.
 */
class DwarfState {
 public:
  enum RegisterState {
    SameValue = 0,
    Undefined,
    Offset,
    ValOffset,
    Register,
    Expression,
    ValExpression,
    Architectural
  };

  DwarfState()
      : m_CfaState(ValOffset),
        m_CfaRegister(0),
        m_CfaOffset(0),
        m_CfaExpression(0),
        m_ReturnAddress(0) {
    ByteSet(static_cast<void*>(m_RegisterStates), 0, sizeof(RegisterState) * DWARF_MAX_REGISTERS);
    ByteSet(static_cast<void*>(m_R), 0, sizeof(uintptr_t) * DWARF_MAX_REGISTERS);
  }
  ~DwarfState() {}

  /**
   * Copy constructor.
   */
  DwarfState(const DwarfState& other)
      : m_CfaState(other.m_CfaState),
        m_CfaRegister(other.m_CfaRegister),
        m_CfaOffset(other.m_CfaOffset),
        m_CfaExpression(other.m_CfaExpression),
        m_ReturnAddress(other.m_ReturnAddress) {
    MemoryCopy(static_cast<void*>(m_RegisterStates),
               static_cast<const void*>(other.m_RegisterStates),
               sizeof(RegisterState) * DWARF_MAX_REGISTERS);
    MemoryCopy(static_cast<void*>(m_R), static_cast<const void*>(other.m_R),
               sizeof(uintptr_t) * DWARF_MAX_REGISTERS);
  }

  DwarfState& operator=(const DwarfState& other) {
    m_CfaState = other.m_CfaState;
    m_CfaRegister = other.m_CfaRegister;
    m_CfaOffset = other.m_CfaOffset;
    m_CfaExpression = other.m_CfaExpression;
    m_ReturnAddress = other.m_ReturnAddress;
    MemoryCopy(static_cast<void*>(m_RegisterStates),
               static_cast<const void*>(other.m_RegisterStates),
               sizeof(RegisterState) * DWARF_MAX_REGISTERS);
    MemoryCopy(static_cast<void*>(m_R), static_cast<const void*>(other.m_R),
               sizeof(uintptr_t) * DWARF_MAX_REGISTERS);
    return *this;
  }

  processor_register_t getCfa(const DwarfState& initialState) const {
    switch (m_CfaState) {
      case ValOffset: {
        return initialState.m_R[m_CfaRegister] + static_cast<ssize_t>(m_CfaOffset);
      }
      case ValExpression: {
        WARNING("DwarfState::getCfa: Expression type not implemented.");
        break;
      }
      default:
        ERROR("CfaState invalid!");
        return 0;
    }

    return 0;
  }

  processor_register_t getRegister(unsigned int nRegister, const DwarfState& initialState) const {
    //       NOTICE("GetRegister: r" << Dec << nRegister);
    switch (m_RegisterStates[nRegister]) {
      case Undefined:
        //           WARNING ("Request for undefined register: r" << Dec
        //           << nRegister);
        break;
      case SameValue:
        //           WARNING ("SameValue.");
        return initialState.m_R[nRegister];
      case Offset: {
        //           NOTICE("Offset: " << Hex << getCfa(initialState) <<
        //           ", " << m_R[nRegister]);
        /// \todo This needs to be better - we need to check if the CFA
        /// is borked so we
        ///       don't try to do a stupid read - This requires
        ///       VirtualAddressSpace, I think.
        if (getCfa(initialState) < 0x2000) {
          WARNING("Malformed CFA!");
          return 0x0;
        }
        // "The previous value of this register is saved at the address
        // CFA+N where CFA is the
        //  current CFA value and N is a signed offset."
        return *reinterpret_cast<processor_register_t*>(getCfa(initialState) +
                                                        static_cast<ssize_t>(m_R[nRegister]));
      }
      case ValOffset: {
        //           WARNING ("ValOffset.");
        // "The previous value of this register is the value CFA+N where
        // CFA is the current
        //  CFA value and N is a signed offset."
        return getCfa(initialState) + static_cast<ssize_t>(m_R[nRegister]);
      }
      case Register: {
        //           WARNING ("Register.");
        // "The previous value of this register is stored in another
        // register numbered R."
        return initialState.m_R[nRegister];
      }
      case Expression: {
        //           WARNING ("Expression not implemented, r" << Dec <<
        //           nRegister);
        return 0;
      }
      case ValExpression: {
        //           WARNING ("ValExpression not implemented, r" << Dec
        //           << nRegister);
        return 0;
      }
      case Architectural:
        //           WARNING ("Request for 'architectural' register: r"
        //           << Dec << nRegister);
        return 0;
    }
    return 0;
  }

  /**
   * Register states - these define how to interpret the m_R members.
   */
  RegisterState m_RegisterStates[DWARF_MAX_REGISTERS];

  /**
   * Registers (columns in the table).
   */
  processor_register_t m_R[DWARF_MAX_REGISTERS];

  /**
   * Current CFA (current frame address) - first and most important column in
   * the table.
   */
  RegisterState m_CfaState;
  /**
   * Current CFA register, and offset.
   */
  uint32_t m_CfaRegister;
  processor_register_t m_CfaOffset;
  /**
   * Current CFA expression, if applicable.
   */
  uint8_t* m_CfaExpression;

  /**
   * The column which contains the function return address.
   */
  uintptr_t m_ReturnAddress;
};

/** @} */

#endif
