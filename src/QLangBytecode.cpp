#include <climits>

#include "qlang/QLangBytecode.h"
#include "qlang/QLangErrors.h"

namespace qlang {

void Bytecode::add(const Instruction &inst) { m_instr.push_back(inst); }

void Bytecode::add(Opcode op, int a, int b, int c) {
  Instruction inst;
  inst.op = op;
  if (usesI32(op)) {
    // 32-bit pc target: JMP/CONTINUE/BREAK carry it in `a`,
    // JMPIFFALSE/JMPIFTRUE in `b` (condition register stays in `a`).
    if (op == Opcode::JMPIFFALSE || op == Opcode::JMPIFTRUE) {
      if (a < INT16_MIN || a > INT16_MAX)
        throw QLangError(QLangErrorKind::Runtime, 0, 0,
                         "internal compiler error: instruction operand out of "
                         "int16 range (op " +
                         std::to_string(static_cast<int>(op)) + ")");
      inst.a = static_cast<int16_t>(a);
    }
    inst.i32 = (op == Opcode::JMPIFFALSE || op == Opcode::JMPIFTRUE) ? b : a;
  } else {
    if (a < INT16_MIN || a > INT16_MAX || b < INT16_MIN || b > INT16_MAX ||
        c < INT16_MIN || c > INT16_MAX)
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "internal compiler error: instruction operand out of "
                       "int16 range (op " +
                       std::to_string(static_cast<int>(op)) + ")");
    inst.a = static_cast<int16_t>(a);
    inst.b = static_cast<int16_t>(b);
    inst.c = static_cast<int16_t>(c);
  }
  m_instr.push_back(std::move(inst));
}

int Bytecode::addConstant(QLangValue value) {
  int idx = static_cast<int>(m_consts.size());
  m_consts.push_back(std::move(value));
  return idx;
}

void Bytecode::setConstants(std::vector<QLangValue> consts) {
  m_consts = std::move(consts);
}

int Bytecode::addFunction(const std::string &name, int paramCount,
                          int registerCount, std::vector<Instruction> instrs,
                          std::vector<QLangValue> constants) {
  Function fn;
  fn.name = name;
  fn.paramCount = paramCount;
  fn.registerCount = registerCount;
  fn.instructions = std::move(instrs);
  fn.constants = std::move(constants);
  int idx = static_cast<int>(m_functions.size());
  m_functions.push_back(std::move(fn));
  return idx;
}

const char *Bytecode::name(Opcode op) {
  switch (op) {
  case Opcode::MOVC: return "movc"; // mov-from-constant-pool
  case Opcode::MOV: return "mov";
  case Opcode::CONV: return "conv";
  case Opcode::NEG: return "neg";
  case Opcode::INC: return "inc";
  case Opcode::DEC: return "dec";
  case Opcode::ADD: return "add";
  case Opcode::SUB: return "sub";
  case Opcode::MUL: return "mul";
  case Opcode::DIV: return "div";
  case Opcode::AND: return "and";
  case Opcode::OR: return "or";
  case Opcode::XOR: return "xor";
  case Opcode::SHL: return "shl";
  case Opcode::SHR: return "shr";
  case Opcode::LAND: return "land";
  case Opcode::LOR: return "lor";
  case Opcode::EQ: return "eq";
  case Opcode::NE: return "ne";
  case Opcode::LT: return "lt";
  case Opcode::GT: return "gt";
  case Opcode::LE: return "le";
  case Opcode::GE: return "ge";
  case Opcode::MOD: return "mod";
  case Opcode::POW: return "pow";
  case Opcode::NOT: return "not";
  case Opcode::LEN: return "len";
  case Opcode::SEQGET: return "seqget";
  case Opcode::CONTINUE: return "continue";
  case Opcode::BREAK: return "break";
  case Opcode::YIELD: return "yield";
  case Opcode::CALL: return "call";
  case Opcode::DELETEx: return "delete";
  case Opcode::CAST: return "cast";
  case Opcode::ARRGET: return "arrget";
  case Opcode::ARRPUT: return "arrput";
  case Opcode::ARRAPP: return "arrapp";
  case Opcode::ARRAPPV: return "arrappv";
  case Opcode::ARRLEN: return "arrlen";
  case Opcode::INx: return "in";
  case Opcode::TABCREATE: return "tabcreate";
  case Opcode::TABGET: return "tabget";
  case Opcode::TABPUT: return "tabput";
  case Opcode::TABFINALIZE: return "tabfinalize";
  case Opcode::INDGET: return "indget";
  case Opcode::INDPUT: return "indput";
  case Opcode::EVALKEY: return "evalkey";
  case Opcode::JMP: return "jmp";
  case Opcode::JMPIFFALSE: return "jmpfalse";
  case Opcode::JMPIFTRUE: return "jmptrue";
  case Opcode::CALLFN: return "callfn";
  case Opcode::CALLCLOS: return "callclo";
  case Opcode::RETURN: return "return";
  case Opcode::MAKEFUNC: return "makefunc";
  case Opcode::MAKEGEN: return "makegen";
  case Opcode::REFLOAD: return "refload";
  case Opcode::REFSTORE: return "refstore";
  case Opcode::MOVI: return "movi"; // 32-bit int literal -> register (no pool)
  case Opcode::ADDI: return "addi"; // r[a] = r[a] + 32-bit int immediate (no pool)
  case Opcode::SUBI: return "subi"; // r[a] = r[a] - 32-bit int immediate (no pool)
  case Opcode::CMP: return "cmp"; // flags = cmp(regs[a], regs[b])
  case Opcode::CMPI: return "cmpi"; // flags = cmp(regs[a], Int(i32))
  case Opcode::JE: return "je"; // if eq            pc = i32
  case Opcode::JNE: return "jne"; // if !eq          pc = i32
  case Opcode::JG: return "jg"; // if gr            pc = i32
  case Opcode::JGE: return "jge"; // if gr+eq        pc = i32
  case Opcode::JL: return "jl"; // if lt            pc = i32
  case Opcode::JLE: return "jle"; // if lt+eq        pc = i32
  case Opcode::kOpcodeCount:
    return "?";
  }
  return "???";
}

} // namespace qlang
