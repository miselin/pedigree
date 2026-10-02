/* Copyright (c) 2026, Pedigree Developers. */
#include <string.h>
#include <vector>

#include "modules/subsys/posix/seccomp-filter.h"
#include <gtest/gtest.h>

using namespace PosixSeccomp;
using namespace PosixSeccomp::Bpf;

namespace {
constexpr uint32_t Allow = 0x7fff0000;
constexpr uint32_t Deny = 0x0005000d;
constexpr uint32_t Amd64 = 0xc000003e;

template <size_t Count>
uint32_t run(const Instruction (&instructions)[Count], const Data& data = {}) {
  EXPECT_TRUE(validate(instructions, Count));
  return evaluate(instructions, Count, data);
}
}  // namespace

TEST(SeccompFilter, PolicyChecksArchitectureSyscallAndFullWidthArgument) {
  const Instruction policy[] = {
      {LD | W | ABS, 0, 0, 4},  {JMP | JEQ | K, 1, 0, Amd64}, {RET | K, 0, 0, KillProcess},
      {LD | W | ABS, 0, 0, 0},  {JMP | JEQ | K, 0, 5, 257},   {LD | W | ABS, 0, 0, 36},
      {JMP | JEQ | K, 0, 2, 0}, {LD | W | ABS, 0, 0, 32},     {JMP | JSET | K, 0, 1, 3},
      {RET | K, 0, 0, Deny},    {RET | K, 0, 0, Allow},
  };
  Data data = {};
  data.arch = Amd64;
  data.nr = 257;
  EXPECT_EQ(run(policy, data), Allow);
  data.args[2] = 2;
  EXPECT_EQ(run(policy, data), Deny);
  data.args[2] = uint64_t(1) << 32;
  EXPECT_EQ(run(policy, data), Deny);
  data.nr = 1;
  EXPECT_EQ(run(policy, data), Allow);
  data.arch = 0x40000003;
  EXPECT_EQ(run(policy, data), KillProcess);
}

TEST(SeccompFilter, AbsoluteLoadsCoverNativeDataLayout) {
  const Data data = {-1,
                     Amd64,
                     0x123456789abcdef0,
                     {0x1111111122222222, 0x3333333344444444, 0x5555555566666666,
                      0x7777777788888888, 0x99999999aaaaaaaa, 0xbbbbbbbbcccccccc}};
  uint32_t words[16];
  memcpy(words, &data, sizeof(data));
  for (uint32_t word = 0; word < 16; ++word) {
    const Instruction policy[] = {{LD | W | ABS, 0, 0, word * 4}, {RET | A, 0, 0, 0}};
    EXPECT_EQ(run(policy, data), words[word]) << word;
  }
}

TEST(SeccompFilter, ScratchReadsRequireInitializationOnBothBranches) {
  Instruction policy[] = {
      {LD | W | ABS, 0, 0, 0}, {JMP | JEQ | K, 0, 3, 0}, {LD | IMM, 0, 0, 5}, {ST, 0, 0, 15},
      {JMP | JA, 0, 0, 2},     {LD | IMM, 0, 0, 9},      {ST, 0, 0, 15},      {LDX | MEM, 0, 0, 15},
      {LD | W | LEN, 0, 0, 0}, {ALU | ADD | X, 0, 0, 0}, {RET | A, 0, 0, 0},
  };
  Data data = {};
  EXPECT_EQ(run(policy, data), 69U);
  data.nr = 1;
  EXPECT_EQ(run(policy, data), 73U);

  policy[6].k = 14;
  EXPECT_FALSE(validate(policy, sizeof(policy) / sizeof(policy[0])));
  EXPECT_EQ(evaluate(policy, sizeof(policy) / sizeof(policy[0]), data), KillProcess);
}

TEST(SeccompFilter, TerminatedPathsDoNotJoinLaterScratchReads) {
  const Instruction policy[] = {
      {LD | W | ABS, 0, 0, 0},  {JMP | JEQ | K, 1, 0, 0}, {RET | K, 0, 0, Deny},
      {LDX | W | LEN, 0, 0, 0}, {STX, 0, 0, 0},           {LD | MEM, 0, 0, 0},
      {MISC | TAX, 0, 0, 0},    {LD | IMM, 0, 0, 0},      {MISC | TXA, 0, 0, 0},
      {RET | A, 0, 0, 0},
  };
  EXPECT_EQ(run(policy), sizeof(Data));
  Data data = {};
  data.nr = 1;
  EXPECT_EQ(run(policy, data), Deny);
}

TEST(SeccompFilter, ArithmeticIsUnsigned32BitForImmediateAndRegisterOperands) {
  const struct {
    uint16_t operation;
    uint32_t left;
    uint32_t right;
    uint32_t expected;
  } cases[] = {
      {ADD, 0xffffffff, 2, 1},          {SUB, 0, 1, 0xffffffff},    {MUL, 0x80000001, 2, 2},
      {DIV, 0xffffffff, 2, 0x7fffffff}, {OR, 0x1000, 0x21, 0x1021}, {AND, 0x1234, 0xff, 0x34},
      {XOR, 0xff, 0xf0, 0xf},           {LSH, 3, 31, 0x80000000},   {RSH, 0x80000000, 31, 1},
  };
  for (const auto& test : cases) {
    for (const uint16_t source : {K, X}) {
      const Instruction policy[] = {
          {LD | IMM, 0, 0, test.left},
          {LDX | IMM, 0, 0, test.right},
          {static_cast<uint16_t>(ALU | test.operation | source), 0, 0, test.right},
          {RET | A, 0, 0, 0},
      };
      EXPECT_EQ(run(policy), test.expected) << test.operation << " " << source;
    }
  }
  const Instruction negate[] = {{LD | IMM, 0, 0, 1}, {ALU | NEG, 0, 0, 0}, {RET | A, 0, 0, 0}};
  EXPECT_EQ(run(negate), 0xffffffffU);
}

TEST(SeccompFilter, RegisterShiftsAreDefinedAndZeroDivisorFailsClosed) {
  Instruction policy[] = {
      {LD | IMM, 0, 0, 6},
      {LDX | IMM, 0, 0, 33},
      {ALU | LSH | X, 0, 0, 0},
      {RET | A, 0, 0, 0},
  };
  EXPECT_EQ(run(policy), 12U);
  policy[2].code = ALU | RSH | X;
  EXPECT_EQ(run(policy), 3U);
  policy[1].k = 0;
  policy[2].code = ALU | DIV | X;
  EXPECT_EQ(run(policy), KillProcess);
}

TEST(SeccompFilter, ComparisonsAreUnsignedForImmediateAndRegisterOperands) {
  for (const uint16_t operation : {JEQ, JGT, JGE, JSET}) {
    for (const uint16_t source : {K, X}) {
      Instruction policy[] = {
          {LD | IMM, 0, 0, 0x80000000},
          {LDX | IMM, 0, 0, 0x80000000},
          {static_cast<uint16_t>(JMP | operation | source), 0, 1, 0x80000000},
          {RET | K, 0, 0, Allow},
          {RET | K, 0, 0, Deny},
      };
      EXPECT_EQ(run(policy), operation == JGT ? Deny : Allow);
      policy[1].k = policy[2].k = 1;
      EXPECT_EQ(run(policy), operation == JGT || operation == JGE ? Allow : Deny);
    }
  }
}

TEST(SeccompFilter, RejectsMalformedOpcodesBoundsAndUninitializedMemory) {
  const Instruction invalid[] = {
      {LD | W | ABS, 0, 0, 2},
      {LD | W | ABS, 0, 0, 64},
      {LD | W | ABS, 0, 0, 0xffffffff},
      {0x28, 0, 0, 0},  // Half-word packet load.
      {0x40, 0, 0, 0},  // Indirect packet load.
      {0x94, 0, 0, 2},  // MOD is not accepted by Linux's seccomp whitelist.
      {0x1006, 0, 0, Allow},
      {ST, 0, 0, 16},
      {STX, 0, 0, 0xffffffff},
      {LD | MEM, 0, 0, 0},
      {LDX | MEM, 0, 0, 15},
      {ALU | DIV | K, 0, 0, 0},
      {ALU | LSH | K, 0, 0, 32},
      {ALU | RSH | K, 0, 0, 0xffffffff},
      {JMP | JA, 0, 0, 1},
      {JMP | JA, 0, 0, 0xffffffff},
      {JMP | JEQ | K, 1, 0, 0},
      {JMP | JEQ | K, 0, 1, 0},
  };
  for (const Instruction& instruction : invalid) {
    const Instruction policy[] = {instruction, {RET | K, 0, 0, Allow}};
    EXPECT_FALSE(validate(policy, 2)) << instruction.code << " " << instruction.k;
    EXPECT_EQ(evaluate(policy, 2, {}), KillProcess) << instruction.code;
  }
  const Instruction unreachableInvalid[] = {
      {RET | K, 0, 0, Allow}, {0xffff, 0, 0, 0}, {RET | K, 0, 0, Allow}};
  EXPECT_FALSE(validate(unreachableInvalid, 3));
}

TEST(SeccompFilter, RequiresBoundedNonemptyTerminatedPrograms) {
  EXPECT_FALSE(validate(nullptr, 1));
  EXPECT_EQ(evaluate(nullptr, 1, {}), KillProcess);
  const Instruction fallthrough[] = {{LD | IMM, 0, 0, Allow}};
  EXPECT_FALSE(validate(fallthrough, 1));
  EXPECT_EQ(evaluate(fallthrough, 1, {}), KillProcess);
  EXPECT_FALSE(validate(fallthrough, 0));
  EXPECT_EQ(evaluate(fallthrough, 0, {}), KillProcess);

  std::vector<Instruction> policy(MaximumInstructions, {LD | IMM, 0, 0, Allow});
  policy.back() = {RET | A, 0, 0, 0};
  EXPECT_TRUE(validate(policy.data(), policy.size()));
  EXPECT_EQ(evaluate(policy.data(), policy.size(), {}), Allow);
  policy.push_back({RET | K, 0, 0, Allow});
  EXPECT_FALSE(validate(policy.data(), policy.size()));
  EXPECT_EQ(evaluate(policy.data(), policy.size(), {}), KillProcess);
}
