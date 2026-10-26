#include "qlang/QLangBytecode.h"

namespace qlang {

void Bytecode::add(const Instruction &inst) { m_instr.push_back(inst); }

void Bytecode::add(Opcode op, int a, int b, int c) {
  Instruction inst;
  inst.op = op;
  inst.a = a;
  inst.b = b;
  inst.c = c;
  m_instr.push_back(inst);
}

int Bytecode::addConstant(QLangValue value) {
  int idx = static_cast<int>(m_consts.size());
  m_consts.push_back(std::move(value));
  return idx;
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
  case Opcode::CONST: return "const";
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
  case Opcode::DELETE: return "delete";
  case Opcode::CAST: return "cast";
  case Opcode::ARRGET: return "arrget";
  case Opcode::ARRPUT: return "arrput";
  case Opcode::ARRAPP: return "arrapp";
  case Opcode::ARRAPPV: return "arrappv";
  case Opcode::ARRLEN: return "arrlen";
  case Opcode::TABCREATE: return "tabcreate";
  case Opcode::TABGET: return "tabget";
  case Opcode::TABPUT: return "tabput";
  case Opcode::INDGET: return "indget";
  case Opcode::INDPUT: return "indput";
  case Opcode::JMP: return "jmp";
  case Opcode::JMPIFFALSE: return "jmpfalse";
  case Opcode::CALLFN: return "callfn";
  case Opcode::CALLCLOS: return "callclo";
  case Opcode::RETURN: return "return";
  case Opcode::MAKEFUNC: return "makefunc";
  case Opcode::REFLOAD: return "refload";
  case Opcode::REFSTORE: return "refstore";
  }
  return "?";
}

} // namespace qlang
