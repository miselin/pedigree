/* Copyright (c) 2026, Pedigree Developers. */
#include "seccomp-filter.h"

namespace PosixSeccomp {
namespace {
using namespace Bpf;
constexpr uint32_t Reachable = 1U << 16;

bool validInstruction(const Instruction& instruction, size_t remaining) {
  switch (instruction.code) {
    case LD | W | ABS:
      return instruction.k < sizeof(Data) && !(instruction.k & 3);
    case LD | MEM:
    case LDX | MEM:
    case ST:
    case STX:
      return instruction.k < 16;
    case ALU | DIV | K:
      return instruction.k != 0;
    case ALU | LSH | K:
    case ALU | RSH | K:
      return instruction.k < 32;
    case JMP | JA:
      return instruction.k < remaining;
    case JMP | JEQ | K:
    case JMP | JEQ | X:
    case JMP | JGT | K:
    case JMP | JGT | X:
    case JMP | JGE | K:
    case JMP | JGE | X:
    case JMP | JSET | K:
    case JMP | JSET | X:
      return instruction.jt < remaining && instruction.jf < remaining;
    case LD | IMM:
    case LDX | IMM:
    case LD | W | LEN:
    case LDX | W | LEN:
    case ALU | ADD | K:
    case ALU | ADD | X:
    case ALU | SUB | K:
    case ALU | SUB | X:
    case ALU | MUL | K:
    case ALU | MUL | X:
    case ALU | DIV | X:
    case ALU | OR | K:
    case ALU | OR | X:
    case ALU | AND | K:
    case ALU | AND | X:
    case ALU | LSH | X:
    case ALU | RSH | X:
    case ALU | NEG:
    case ALU | XOR | K:
    case ALU | XOR | X:
    case RET | K:
    case RET | A:
    case MISC | TAX:
    case MISC | TXA:
      return true;
    default:
      return false;
  }
}

bool validatePaths(const Instruction* instructions, size_t count, uint32_t* incoming) {
  incoming[0] = Reachable;
  const auto merge = [&](size_t destination, uint32_t initialized) {
    incoming[destination] =
        incoming[destination] ? incoming[destination] & initialized : initialized;
  };
  for (size_t pc = 0; pc < count; ++pc) {
    const Instruction& instruction = instructions[pc];
    if (!validInstruction(instruction, count - pc - 1)) {
      return false;
    }
    if (!incoming[pc]) {
      continue;
    }
    uint32_t initialized = incoming[pc];
    if (instruction.code == (LD | MEM) || instruction.code == (LDX | MEM)) {
      if (!(initialized & (1U << instruction.k))) {
        return false;
      }
    } else if (instruction.code == ST || instruction.code == STX) {
      initialized |= 1U << instruction.k;
    }

    // Forward-only jumps make this a single pass: every predecessor has
    // contributed its initialized cells by the time we visit a join.
    if ((instruction.code & 7) == RET) {
      continue;
    }
    if (instruction.code == (JMP | JA)) {
      merge(pc + 1 + instruction.k, initialized);
    } else if ((instruction.code & 7) == JMP) {
      merge(pc + 1 + instruction.jt, initialized);
      merge(pc + 1 + instruction.jf, initialized);
    } else {
      if (pc + 1 == count) {
        return false;
      }
      merge(pc + 1, initialized);
    }
  }
  return true;
}
}  // namespace

bool validate(const Instruction* instructions, size_t count) {
  if (!instructions || !count || count > MaximumInstructions) {
    return false;
  }
  const uint16_t last = instructions[count - 1].code;
  if (last != (RET | K) && last != (RET | A)) {
    return false;
  }
  // Keep the bounded control-flow workspace off the kernel stack.
  uint32_t* incoming = new uint32_t[count]();
  if (!incoming) {
    return false;
  }
  const bool valid = validatePaths(instructions, count, incoming);
  delete[] incoming;
  return valid;
}

uint32_t evaluate(const Instruction* instructions, size_t count, const Data& data) {
  if (!instructions || !count || count > MaximumInstructions) {
    return KillProcess;
  }
  uint32_t accumulator = 0, index = 0, initialized = 0;
  uint32_t memory[16] = {};
  for (size_t pc = 0; pc < count; ++pc) {
    const Instruction& instruction = instructions[pc];
    if (!validInstruction(instruction, count - pc - 1)) {
      return KillProcess;
    }
    const uint32_t operand = instruction.code & X ? index : instruction.k;
    switch (instruction.code) {
      case LD | W | ABS: {
        // seccomp loads use native byte order, including halves of 64-bit
        // arguments. Copy bytes to avoid aliasing the Data object as uint32_t.
        const unsigned char* source = reinterpret_cast<const unsigned char*>(&data) + instruction.k;
        unsigned char* destination = reinterpret_cast<unsigned char*>(&accumulator);
        for (size_t byte = 0; byte < sizeof(accumulator); ++byte) {
          destination[byte] = source[byte];
        }
        break;
      }
      case LD | IMM:
        accumulator = instruction.k;
        break;
      case LDX | IMM:
        index = instruction.k;
        break;
      case LD | W | LEN:
        accumulator = sizeof(Data);
        break;
      case LDX | W | LEN:
        index = sizeof(Data);
        break;
      case LD | MEM:
      case LDX | MEM:
        if (!(initialized & (1U << instruction.k))) {
          return KillProcess;
        }
        if (instruction.code == (LD | MEM)) {
          accumulator = memory[instruction.k];
        } else {
          index = memory[instruction.k];
        }
        break;
      case ST:
      case STX:
        memory[instruction.k] = instruction.code == ST ? accumulator : index;
        initialized |= 1U << instruction.k;
        break;
      case ALU | ADD | K:
      case ALU | ADD | X:
        accumulator += operand;
        break;
      case ALU | SUB | K:
      case ALU | SUB | X:
        accumulator -= operand;
        break;
      case ALU | MUL | K:
      case ALU | MUL | X:
        accumulator *= operand;
        break;
      case ALU | DIV | K:
      case ALU | DIV | X:
        if (!operand) {
          return KillProcess;
        }
        accumulator /= operand;
        break;
      case ALU | OR | K:
      case ALU | OR | X:
        accumulator |= operand;
        break;
      case ALU | AND | K:
      case ALU | AND | X:
        accumulator &= operand;
        break;
      case ALU | XOR | K:
      case ALU | XOR | X:
        accumulator ^= operand;
        break;
      case ALU | LSH | K:
      case ALU | LSH | X:
        accumulator <<= operand & 31;
        break;
      case ALU | RSH | K:
      case ALU | RSH | X:
        accumulator >>= operand & 31;
        break;
      case ALU | NEG:
        accumulator = 0U - accumulator;
        break;
      case JMP | JA:
        pc += instruction.k;
        break;
      case JMP | JEQ | K:
      case JMP | JEQ | X:
        pc += accumulator == operand ? instruction.jt : instruction.jf;
        break;
      case JMP | JGT | K:
      case JMP | JGT | X:
        pc += accumulator > operand ? instruction.jt : instruction.jf;
        break;
      case JMP | JGE | K:
      case JMP | JGE | X:
        pc += accumulator >= operand ? instruction.jt : instruction.jf;
        break;
      case JMP | JSET | K:
      case JMP | JSET | X:
        pc += accumulator & operand ? instruction.jt : instruction.jf;
        break;
      case RET | K:
        return instruction.k;
      case RET | A:
        return accumulator;
      case MISC | TAX:
        index = accumulator;
        break;
      case MISC | TXA:
        accumulator = index;
        break;
      default:
        return KillProcess;
    }
  }
  return KillProcess;
}
}  // namespace PosixSeccomp
