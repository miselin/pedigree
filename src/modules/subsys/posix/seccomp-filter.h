/* Copyright (c) 2026, Pedigree Developers. */
#ifndef POSIX_SECCOMP_FILTER_H
#define POSIX_SECCOMP_FILTER_H

#include <stddef.h>
#include <stdint.h>

namespace PosixSeccomp {
constexpr size_t MaximumInstructions = 4096;
constexpr uint32_t KillProcess = 0x80000000;

struct Instruction {
  uint16_t code;
  uint8_t jt;
  uint8_t jf;
  uint32_t k;
};

struct Data {
  int32_t nr;
  uint32_t arch;
  uint64_t instructionPointer;
  uint64_t args[6];
};

static_assert(sizeof(Instruction) == 8 && offsetof(Instruction, k) == 4, "Linux sock_filter ABI");
static_assert(sizeof(Data) == 64 && offsetof(Data, instructionPointer) == 8 &&
                  offsetof(Data, args) == 16,
              "Linux seccomp_data ABI");

namespace Bpf {
enum : uint16_t {
  LD = 0x00,
  LDX = 0x01,
  ST = 0x02,
  STX = 0x03,
  ALU = 0x04,
  JMP = 0x05,
  RET = 0x06,
  MISC = 0x07,
  W = 0x00,
  IMM = 0x00,
  ABS = 0x20,
  MEM = 0x60,
  LEN = 0x80,
  K = 0x00,
  X = 0x08,
  A = 0x10,
  ADD = 0x00,
  SUB = 0x10,
  MUL = 0x20,
  DIV = 0x30,
  OR = 0x40,
  AND = 0x50,
  LSH = 0x60,
  RSH = 0x70,
  NEG = 0x80,
  XOR = 0xa0,
  JA = 0x00,
  JEQ = 0x10,
  JGT = 0x20,
  JGE = 0x30,
  JSET = 0x40,
  TAX = 0x00,
  TXA = 0x80
};
}  // namespace Bpf

// The caller owns an immutable kernel copy of the instructions. Validate it
// before publication; evaluation allocates nothing and retains action/data bits.
bool validate(const Instruction* instructions, size_t count);
uint32_t evaluate(const Instruction* instructions, size_t count, const Data& data);
}  // namespace PosixSeccomp

#endif
