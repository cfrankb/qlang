#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "QLangValue.h"

namespace qlang {

// Register-machine opcodes. This is a small register VM: each script variable
// maps to a register, and arithmetic/comparison results are written to a
// register. Constants live in a separate pool and are loaded with CONST.
enum class Opcode {
  // Data movement
  CONST, // r[a] = constants[b]
  MOV,   // r[a] = r[b]
  CONV,  // r[a] = convert(r[b], type c)  -- enforce a typed declaration
  NEG,   // r[a] = -r[b]
  INC,   // r[a] += 1
  DEC,   // r[a] -= 1

  // Arithmetic
  ADD, // r[a] = r[b] + r[c]
  SUB,
  MUL,
  DIV,

  // Bitwise / shifts
  AND, // r[a] = r[b] & r[c]
  OR,
  XOR,
  SHL, // r[a] = r[b] << r[c]
  SHR, // r[a] = r[b] >> r[c]

  // Logical (combine truthiness; non-short-circuit)
  LAND, // r[a] = truth(r[b]) && truth(r[c])
  LOR,  // r[a] = truth(r[b]) || truth(r[c])

  // Comparison -> boolean (1/0 stored as Int)
  EQ, // r[a] = (r[b] == r[c])
  NE,
  LT,
  GT,
  LE,
  GE,

  // Control flow
  JMP,       // pc = a
  JMPIFFALSE, // if !truth(r[a]) pc = b
};

// A single instruction. Operands are plain ints interpreted per-opcode:
//   register indices for MOV/ADD/.../JMPIFFALSE's first operand,
//   a constant-pool index for CONST's second operand,
//   a program index for JMP/JMPIFFALSE's second operand.
struct Instruction {
  Opcode op = Opcode::CONST;
  int a = 0;
  int b = 0;
  int c = 0;
};

// The compiled program: an instruction stream plus a constant pool.
class Bytecode {
public:
  void add(const Instruction &inst);
  void add(Opcode op, int a = 0, int b = 0, int c = 0);

  // Append a constant, returning its pool index.
  int addConstant(QLangValue value);

  const std::vector<Instruction> &instructions() const { return m_instr; }
  std::vector<Instruction> &instructions() { return m_instr; }
  const std::vector<QLangValue> &constants() const { return m_consts; }
  size_t size() const { return m_instr.size(); }

  // Mutable access to instruction i (used to patch jump targets after a
  // forward reference is resolved).
  Instruction &at(size_t i) { return m_instr[i]; }

  // Human-readable opcode mnemonic (for dumping/debugging).
  static const char *name(Opcode op);

private:
  std::vector<Instruction> m_instr;
  std::vector<QLangValue> m_consts;
};

} // namespace qlang
