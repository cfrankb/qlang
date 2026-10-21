#include "qlang/QLangVM.h"

#include <cstdint>
#include <stdexcept>
#include <string>

#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"

namespace qlang {

namespace {

bool isNumeric(const QLangValue &v) {
  auto t = v.type();
  return t == QLangValue::Type::Byte || t == QLangValue::Type::Word ||
         t == QLangValue::Type::Short || t == QLangValue::Type::Int ||
         t == QLangValue::Type::Unsigned;
}

int64_t toInt64(const QLangValue &v) {
  switch (v.type()) {
  case QLangValue::Type::Byte: return static_cast<int64_t>(v.asByte());
  case QLangValue::Type::Word: return static_cast<int64_t>(v.asWord());
  case QLangValue::Type::Short: return static_cast<int64_t>(v.asShort());
  case QLangValue::Type::Int: return static_cast<int64_t>(v.asInt());
  case QLangValue::Type::Unsigned: return static_cast<int64_t>(v.asUnsigned());
  default: return 0;
  }
}

// Numeric operand with a clear error if the type is wrong.
int64_t num(const QLangValue &v, const char *op) {
  if (!isNumeric(v))
    throw QLangError(QLangErrorKind::Type, 0, 0,
                     std::string("operator '") + op +
                         "' operand must be numeric, got " + v.typeName());
  return toInt64(v);
}

int cmp(const QLangValue &a, const QLangValue &b, const char *op) {
  bool numA = isNumeric(a), numB = isNumeric(b);
  if (numA && numB) {
    int64_t x = toInt64(a), y = toInt64(b);
    return (x < y) ? -1 : (x > y) ? 1 : 0;
  }
  if (a.type() == QLangValue::Type::String &&
      b.type() == QLangValue::Type::String) {
    return a.asString().compare(b.asString());
  }
  throw QLangError(QLangErrorKind::Type, 0, 0,
                   std::string("operator '") + op +
                       "' cannot compare " + a.typeName() + " and " +
                       b.typeName());
}

} // namespace

QLangVM::QLangVM(const CompiledProgram &program, QLangContext &scope)
    : m_prog(program), m_scope(scope),
      m_registers(program.registerCount, QLangValue()) {}

bool QLangVM::isTrue(const QLangValue &v) {
  switch (v.type()) {
  case QLangValue::Type::Nil: return false;
  case QLangValue::Type::String: return !v.asString().empty();
  default: return isNumeric(v) && toInt64(v) != 0;
  }
}

// Convert a value to the target declared type. Integer types truncate/wrap
// (C-style cast semantics); other types pass through unchanged.
QLangValue convert(const QLangValue &v, QLangValue::Type type) {
  switch (type) {
  case QLangValue::Type::Byte:
    return QLangValue::Byte(static_cast<uint8_t>(toInt64(v)));
  case QLangValue::Type::Word:
    return QLangValue::Word(static_cast<uint16_t>(toInt64(v)));
  case QLangValue::Type::Short:
    return QLangValue::Short(static_cast<int16_t>(toInt64(v)));
  case QLangValue::Type::Int:
    return QLangValue::Int(static_cast<int32_t>(toInt64(v)));
  case QLangValue::Type::Unsigned:
    return QLangValue::Unsigned(static_cast<uint32_t>(toInt64(v)));
  default: return v;
  }
}

QLangValue QLangVM::doUnary(Opcode op, const QLangValue &a) {
  switch (op) {
  case Opcode::NEG:
    return QLangValue::Int(static_cast<int32_t>(-num(a, "-")));
  case Opcode::INC:
    return QLangValue::Int(static_cast<int32_t>(num(a, "+") + 1));
  case Opcode::DEC:
    return QLangValue::Int(static_cast<int32_t>(num(a, "-") - 1));
  default:
    throw QLangError(QLangErrorKind::Runtime, 0, 0,
                     std::string("unary operation not implemented: ") +
                     Bytecode::name(op));
  }
}

QLangValue QLangVM::doBinary(Opcode op, const QLangValue &a, const QLangValue &b) {
  switch (op) {
  case Opcode::ADD:
    if (a.type() == QLangValue::Type::String ||
        b.type() == QLangValue::Type::String) {
      if (a.type() == QLangValue::Type::String &&
          b.type() == QLangValue::Type::String)
        return QLangValue::String(a.asString() + b.asString());
      throw QLangError(QLangErrorKind::Type, 0, 0,
                       "operator '+' cannot mix string and numeric operands");
    }
    return QLangValue::Int(static_cast<int32_t>(num(a, "+") + num(b, "+")));
  case Opcode::SUB:
    return QLangValue::Int(static_cast<int32_t>(num(a, "-") - num(b, "-")));
  case Opcode::MUL:
    return QLangValue::Int(static_cast<int32_t>(num(a, "*") * num(b, "*")));
  case Opcode::DIV: {
    int64_t d = num(b, "/");
    if (d == 0)
      throw QLangError(QLangErrorKind::Type, 0, 0, "division by zero");
    return QLangValue::Int(static_cast<int32_t>(num(a, "/") / d));
  }
  case Opcode::AND:
    return QLangValue::Int(static_cast<int32_t>(num(a, "&") & num(b, "&")));
  case Opcode::OR:
    return QLangValue::Int(static_cast<int32_t>(num(a, "|") | num(b, "|")));
  case Opcode::XOR:
    return QLangValue::Int(static_cast<int32_t>(num(a, "^") ^ num(b, "^")));
  case Opcode::SHL:
    return QLangValue::Int(static_cast<int32_t>(num(a, "<<") << num(b, "<<")));
  case Opcode::SHR:
    return QLangValue::Int(static_cast<int32_t>(num(a, ">>") >> num(b, ">>")));
  case Opcode::LAND:
    return QLangValue::Int(isTrue(a) && isTrue(b) ? 1 : 0);
  case Opcode::LOR:
    return QLangValue::Int(isTrue(a) || isTrue(b) ? 1 : 0);
  case Opcode::EQ:
    return QLangValue::Int(cmp(a, b, "==") == 0 ? 1 : 0);
  case Opcode::NE:
    return QLangValue::Int(cmp(a, b, "!=") != 0 ? 1 : 0);
  case Opcode::LT:
    return QLangValue::Int(cmp(a, b, "<") < 0 ? 1 : 0);
  case Opcode::GT:
    return QLangValue::Int(cmp(a, b, ">") > 0 ? 1 : 0);
  case Opcode::LE:
    return QLangValue::Int(cmp(a, b, "<=") <= 0 ? 1 : 0);
  case Opcode::GE:
    return QLangValue::Int(cmp(a, b, ">=") >= 0 ? 1 : 0);
  default:
    throw QLangError(QLangErrorKind::Runtime, 0, 0,
                     std::string("binary operation not implemented: ") +
                     Bytecode::name(op));
  }
}

void QLangVM::run() {
  // Seed registers from any pre-existing scope variables.
  for (const auto &[name, reg] : m_prog.vars)
    if (m_scope.has(name))
      m_registers[reg] = m_scope.get(name);

  const auto &instrs = m_prog.code.instructions();
  const auto &consts = m_prog.code.constants();

  size_t pc = 0;
  while (pc < instrs.size()) {
    if (++m_steps > kMaxSteps)
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "max instruction steps exceeded (possible infinite loop)");

    const Instruction &i = instrs[pc];
    switch (i.op) {
    case Opcode::CONST: m_registers[i.a] = consts[i.b]; break;
    case Opcode::MOV: m_registers[i.a] = m_registers[i.b]; break;
    case Opcode::NEG: m_registers[i.a] = doUnary(i.op, m_registers[i.b]); break;
    case Opcode::INC: m_registers[i.a] = doUnary(i.op, m_registers[i.b]); break;
    case Opcode::DEC: m_registers[i.a] = doUnary(i.op, m_registers[i.b]); break;
    case Opcode::CONV:
      m_registers[i.a] = convert(m_registers[i.b],
                                 static_cast<QLangValue::Type>(i.c));
      break;
    case Opcode::ADD:
    case Opcode::SUB:
    case Opcode::MUL:
    case Opcode::DIV:
    case Opcode::AND:
    case Opcode::OR:
    case Opcode::XOR:
    case Opcode::SHL:
    case Opcode::SHR:
    case Opcode::LAND:
    case Opcode::LOR:
    case Opcode::EQ:
    case Opcode::NE:
    case Opcode::LT:
    case Opcode::GT:
    case Opcode::LE:
    case Opcode::GE:
      m_registers[i.a] = doBinary(i.op, m_registers[i.b], m_registers[i.c]);
      break;
    case Opcode::JMP:
      pc = static_cast<size_t>(i.a);
      continue;
    case Opcode::JMPIFFALSE:
      if (!isTrue(m_registers[i.a]))
        pc = static_cast<size_t>(i.b);
      else
        pc = pc + 1;
      continue;
    }
    pc += 1;
  }

  // Capture final variable state back into the scope.
  for (const auto &[name, reg] : m_prog.vars)
    m_scope.set(name, m_registers[reg]);
}

} // namespace qlang
