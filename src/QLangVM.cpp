#include "qlang/QLangVM.h"

#include <cstdint>
#include <cstdio>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"

namespace qlang {

namespace {

bool isNumeric(const QLangValue &v) {
  auto t = v.type();
  return t == QLangValue::Type::Byte || t == QLangValue::Type::Word ||
         t == QLangValue::Type::Short || t == QLangValue::Type::Int ||
         t == QLangValue::Type::Unsigned || t == QLangValue::Type::Float;
}

int64_t toInt64(const QLangValue &v) {
  switch (v.type()) {
  case QLangValue::Type::Byte: return static_cast<int64_t>(v.asByte());
  case QLangValue::Type::Word: return static_cast<int64_t>(v.asWord());
  case QLangValue::Type::Short: return static_cast<int64_t>(v.asShort());
  case QLangValue::Type::Int: return static_cast<int64_t>(v.asInt());
  case QLangValue::Type::Unsigned: return static_cast<int64_t>(v.asUnsigned());
  case QLangValue::Type::Float: return static_cast<int64_t>(v.asFloat());
  default: return 0;
  }
}

double toDouble(const QLangValue &v) {
  switch (v.type()) {
  case QLangValue::Type::Float: return v.asFloat();
  default: return static_cast<double>(toInt64(v));
  }
}

// Text form of a value, used by toStr/print/printf and string concatenation.
std::string valueToString(const QLangValue &v) {
  switch (v.type()) {
  case QLangValue::Type::Nil: return "nil";
  case QLangValue::Type::String: return v.asString();
  case QLangValue::Type::Byte: return std::to_string(v.asByte());
  case QLangValue::Type::Word: return std::to_string(v.asWord());
  case QLangValue::Type::Short: return std::to_string(v.asShort());
  case QLangValue::Type::Int: return std::to_string(v.asInt());
  case QLangValue::Type::Unsigned: return std::to_string(v.asUnsigned());
  case QLangValue::Type::Float: {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.15g", v.asFloat());
    return std::string(buf);
  }
  case QLangValue::Type::Blob: return "<blob>";
  case QLangValue::Type::Array: return "<array>";
  case QLangValue::Type::Table: return "<table>";
  }
  return "?";
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
    if (a.type() == QLangValue::Type::Float ||
        b.type() == QLangValue::Type::Float) {
      double x = toDouble(a), y = toDouble(b);
      return (x < y) ? -1 : (x > y) ? 1 : 0;
    }
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

// Length for len(): string/array/table/blob lengths; any other type is 1
// (per spec: "other type return 1").
int64_t lenOf(const QLangValue &v) {
  switch (v.type()) {
  case QLangValue::Type::String: return v.asString().size();
  case QLangValue::Type::Array: return v.asArray().size();
  case QLangValue::Type::Table: return v.asTable().size();
  case QLangValue::Type::Blob: return v.asBlob().size();
  default: return 1;
  }
}

// Dispatch a built-in function call. `args` are the evaluated operands; the
// return value is stored by the caller into the call's destination register.
QLangValue callBuiltin(Builtin id, const std::vector<QLangValue> &args) {
  auto need = [&](size_t n, const char *fn) {
    if (args.size() != static_cast<size_t>(n))
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       std::string(fn) + " expects " + std::to_string(n) +
                           " arg(s), got " + std::to_string(args.size()));
  };

  switch (id) {
  case Builtin::ToStr:
    need(1, "toStr");
    return QLangValue::String(valueToString(args[0]));
  case Builtin::ToInt:
    need(1, "toInt");
    if (isNumeric(args[0]))
      return QLangValue::Int(static_cast<int32_t>(toInt64(args[0])));
    if (args[0].type() == QLangValue::Type::String) {
      try {
        return QLangValue::Int(static_cast<int32_t>(std::stoi(args[0].asString())));
      } catch (...) {
        throw QLangError(QLangErrorKind::Runtime, 0, 0,
                         std::string("toInt: cannot convert '") +
                             args[0].asString() + "' to int");
      }
    }
    throw QLangError(QLangErrorKind::Type, 0, 0,
                     std::string("toInt: expected numeric or string, got ") +
                         args[0].typeName());
  case Builtin::Type:
    need(1, "type");
    return QLangValue::String(args[0].typeName());
  case Builtin::Print:
    need(1, "print");
    std::cout << valueToString(args[0]);
    return QLangValue();
  case Builtin::Println:
    need(1, "println");
    std::cout << valueToString(args[0]) << "\n";
    return QLangValue();
  case Builtin::Printf: {
    if (args.empty())
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "printf expects at least a format string");
    if (args[0].type() != QLangValue::Type::String)
      throw QLangError(QLangErrorKind::Type, 0, 0,
                       "printf: format string must be a string");
    std::string fmt = args[0].asString();
    std::string out;
    size_t ai = 1; // next format argument
    for (size_t i = 0; i < fmt.size();) {
      if (fmt[i] == '%' && i + 1 < fmt.size()) {
        char spec = fmt[i + 1];
        if (spec == '%') {
          out += '%';
          i += 2;
          continue;
        }
        if (ai >= args.size())
          throw QLangError(QLangErrorKind::Runtime, 0, 0,
                           "printf: not enough arguments for format spec");
        const QLangValue &av = args[ai++];
        switch (spec) {
        case 'd': out += std::to_string(toInt64(av)); break;
        case 'f': {
          char buf[64];
          std::snprintf(buf, sizeof(buf), "%.6f", toDouble(av));
          out += buf;
          break;
        }
        case 's': out += valueToString(av); break;
        case 'c': out += static_cast<char>(toInt64(av)); break;
        default: out += spec; break;
        }
        i += 2;
      } else {
        out += fmt[i++];
      }
    }
    std::cout << out;
    return QLangValue();
  }
  case Builtin::Len: {
    need(1, "len");
    return QLangValue::Int(static_cast<int32_t>(lenOf(args[0])));
  }
  case Builtin::Blob: {
    need(0, "blob");
    return QLangValue::Blob(std::vector<uint8_t>{});
  }
  }
  throw QLangError(QLangErrorKind::Runtime, 0, 0, "unknown builtin");
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
    // Left-is-string rule: if the left operand is a string, concat both sides.
    if (a.type() == QLangValue::Type::String)
      return QLangValue::String(a.asString() + valueToString(b));
    if (b.type() == QLangValue::Type::String)
      throw QLangError(QLangErrorKind::Type, 0, 0,
                       "operator '+' cannot mix numeric and string operands");
    if (a.type() == QLangValue::Type::Array &&
        b.type() == QLangValue::Type::Array) {
      // Array concatenation: aa + ab -> new array (spec: "append both arrays").
      ArrayValue result = a.asArray();
      const auto &extra = b.asArray();
      result.insert(result.end(), extra.begin(), extra.end());
      return QLangValue::Array(std::move(result));
    }
    if (a.type() == QLangValue::Type::Float ||
        b.type() == QLangValue::Type::Float)
      return QLangValue::Float(toDouble(a) + toDouble(b));
    return QLangValue::Int(static_cast<int32_t>(num(a, "+") + num(b, "+")));
  case Opcode::SUB:
    if (a.type() == QLangValue::Type::Float ||
        b.type() == QLangValue::Type::Float)
      return QLangValue::Float(toDouble(a) - toDouble(b));
    return QLangValue::Int(static_cast<int32_t>(num(a, "-") - num(b, "-")));
  case Opcode::MUL:
    if (a.type() == QLangValue::Type::Float ||
        b.type() == QLangValue::Type::Float)
      return QLangValue::Float(toDouble(a) * toDouble(b));
    return QLangValue::Int(static_cast<int32_t>(num(a, "*") * num(b, "*")));
  case Opcode::DIV: {
    if (a.type() == QLangValue::Type::Float ||
        b.type() == QLangValue::Type::Float) {
      double d = toDouble(b);
      if (d == 0.0)
        throw QLangError(QLangErrorKind::Type, 0, 0, "division by zero");
      return QLangValue::Float(toDouble(a) / d);
    }
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

void QLangVM::pushCall(int calleeIndex, std::vector<QLangValue> &callerRegs,
                       int dest, int argc) {
  fprintf(stderr, "[pushCall] calleeIndex=%d functions.size()=%zu dest=%d argc=%d callerRegs=%zu\n", calleeIndex, m_prog.functions.size(), dest, argc, callerRegs.size());
  const Function &callee = m_prog.functions[calleeIndex];
  Frame cf;
  cf.funcIndex = calleeIndex;
  cf.regs.resize(callee.registerCount, QLangValue());
  // Copy the call arguments (caller regs dest+1..dest+argc) into the callee's
  // parameter registers 0..argc-1.
  for (int k = 0; k < argc; k++)
    cf.regs[k] = callerRegs[dest + 1 + k];
  fprintf(stderr, "[pushCall] after arg copy, argc=%d\n", argc);
  m_frames.push_back(std::move(cf));
  fprintf(stderr, "[pushCall] after push_back\n");
  m_cur = static_cast<int>(m_frames.size()) - 1;
  m_frames[m_cur].pc = 0;
  fprintf(stderr, "[pushCall] done\n");
}

void QLangVM::run() {
  // Set up the main frame (functions[0]).
  Frame mainFrame;
  mainFrame.funcIndex = 0;
  mainFrame.regs.resize(m_prog.registerCount, QLangValue());
  // Seed from any pre-existing scope variables.
  for (const auto &[name, reg] : m_prog.vars)
    if (m_scope.has(name))
      mainFrame.regs[reg] = m_scope.get(name);
  m_frames.push_back(std::move(mainFrame));
  m_cur = 0;

  while (m_cur >= 0) {
    Frame &frame = m_frames[m_cur];
    const Function &fn = m_prog.functions[frame.funcIndex];
    const auto &instrs = fn.instructions;
    const auto &consts = fn.constants;

    if (frame.pc >= instrs.size()) {
      // Fell off the end without an explicit return: unwind with nil.
      fprintf(stderr, "[DBG] fell off end at func %d, pc=%zu instrs=%zu\n", frame.funcIndex, frame.pc, instrs.size());
      m_returnValue = frame.regs[0];
      m_frames.pop_back();
      m_cur--;
      continue;
    }

    if (++m_steps > kMaxSteps)
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "max instruction steps exceeded (possible infinite loop)");

    const Instruction &i = instrs[frame.pc];
    Opcode op = i.op;
    fprintf(stderr, "[VM] func=%d pc=%zu op=%d a=%d b=%d c=%d regs=%zu\n", frame.funcIndex, frame.pc, (int)op, i.a, i.b, i.c, frame.regs.size());

    switch (op) {
    case Opcode::CONST: frame.regs[i.a] = consts[i.b]; break;
    case Opcode::MOV: frame.regs[i.a] = frame.regs[i.b]; break;
    case Opcode::NEG: frame.regs[i.a] = doUnary(i.op, frame.regs[i.b]); break;
    case Opcode::INC: frame.regs[i.a] = doUnary(i.op, frame.regs[i.b]); break;
    case Opcode::DEC: frame.regs[i.a] = doUnary(i.op, frame.regs[i.b]); break;
    case Opcode::CONV:
      frame.regs[i.a] = convert(frame.regs[i.b],
                                static_cast<QLangValue::Type>(i.c));
      break;
    case Opcode::DELETE:
      m_deleted.insert(i.a);
      break;
    case Opcode::ARRGET: {
      QLangValue &base = frame.regs[i.b];
      if (base.type() != QLangValue::Type::Array)
        throw QLangError(QLangErrorKind::Type, 0, 0,
                         "cannot subscript non-array value");
      int64_t idx = toInt64(frame.regs[i.c]);
      const auto &elems = base.asArray();
      if (idx < 0 || idx >= static_cast<int64_t>(elems.size()))
        throw QLangError(QLangErrorKind::Runtime, 0, 0,
                         "array index out of range");
      frame.regs[i.a] = elems[static_cast<size_t>(idx)];
      break;
    }
    case Opcode::ARRPUT: {
      QLangValue &base = frame.regs[i.a];
      if (base.type() != QLangValue::Type::Array)
        throw QLangError(QLangErrorKind::Type, 0, 0,
                         "cannot subscript non-array value");
      int64_t idx = toInt64(frame.regs[i.b]);
      auto &elems = base.asArray();
      if (idx < 0 || idx >= static_cast<int64_t>(elems.size()))
        throw QLangError(QLangErrorKind::Runtime, 0, 0,
                         "array index out of range");
      elems[static_cast<size_t>(idx)] = frame.regs[i.c];
      break;
    }
    case Opcode::ARRAPP: {
      QLangValue &base = frame.regs[i.a];
      if (base.type() != QLangValue::Type::Array)
        throw QLangError(QLangErrorKind::Type, 0, 0,
                         "cannot append to non-array value");
      base.asArray().push_back(frame.regs[i.b]);
      break;
    }
    case Opcode::ARRAPPV: {
      QLangValue &base = frame.regs[i.a];
      if (base.type() != QLangValue::Type::Array)
        throw QLangError(QLangErrorKind::Type, 0, 0,
                         "cannot append to non-array value");
      if (frame.regs[i.b].type() != QLangValue::Type::Array)
        throw QLangError(QLangErrorKind::Type, 0, 0,
                         "cannot append non-array to array");
      auto &elems = base.asArray();
      const auto &extra = frame.regs[i.b].asArray();
      elems.insert(elems.end(), extra.begin(), extra.end());
      break;
    }
    case Opcode::ARRLEN:
      frame.regs[i.a] =
          QLangValue::Int(static_cast<int32_t>(lenOf(frame.regs[i.b])));
      break;
    case Opcode::TABGET: {
      QLangValue &base = frame.regs[i.b];
      if (base.type() != QLangValue::Type::Table)
        throw QLangError(QLangErrorKind::Type, 0, 0,
                         "cannot access member of non-table value");
      const std::string &key = valueToString(frame.regs[i.c]);
      const auto &kv = base.asTable();
      auto it = kv.find(key);
      if (it == kv.end())
        throw QLangError(QLangErrorKind::Runtime, 0, 0,
                         "table has no member '" + key + "'");
      frame.regs[i.a] = it->second;
      break;
    }
    case Opcode::TABPUT: {
      QLangValue &base = frame.regs[i.a];
      if (base.type() != QLangValue::Type::Table)
        throw QLangError(QLangErrorKind::Type, 0, 0,
                         "cannot assign member of non-table value");
      const std::string &key = valueToString(frame.regs[i.b]);
      base.asTable()[key] = frame.regs[i.c];
      break;
    }
    case Opcode::CALL: {
      int dest = i.a;
      int argCount = i.c;
      std::vector<QLangValue> args;
      args.reserve(argCount);
      for (int k = 0; k < argCount; k++)
        args.push_back(frame.regs[dest + 1 + k]);
      frame.regs[dest] = callBuiltin(static_cast<Builtin>(i.b), args);
      break;
    }
    case Opcode::CALLFN: {
      // User `def` function; callee index is operand b.
      // Advance the caller past the call *before* pushCall (which may
      // reallocate m_frames and invalidate `frame`), so that when the callee
      // returns we resume at the next instruction.
      frame.pc += 1;
      pushCall(i.b, frame.regs, i.a, i.c);
      continue; // m_cur now points at the new frame; skip the trailing increment
    }
    case Opcode::CALLCLOS: {
      // Arrow-closure value in register a; its funcIndex is read at runtime.
      const QLangValue &cv = frame.regs[i.a];
      if (cv.type() != QLangValue::Type::Function)
        throw QLangError(QLangErrorKind::Type, 0, 0,
                         "attempt to call a non-function value");
      frame.pc += 1;
      pushCall(cv.asFunction().funcIndex, frame.regs, i.a, i.c);
      continue; // m_cur now points at the new frame; skip the trailing increment
    }
    case Opcode::MAKEFUNC: {
      const QLangValue &cv = consts[i.b];
      if (cv.type() != QLangValue::Type::Function)
        throw QLangError(QLangErrorKind::Runtime, 0, 0,
                         "invalid function constant");
      const Closure &spec = cv.asFunction();
      auto closure = std::make_shared<Closure>();
      closure->funcIndex = spec.funcIndex;
      // Bind each captured register by reference to the enclosing frame.
      for (const CaptureRef &cr : spec.captures) {
        CaptureRef bound;
        bound.frame = &m_frames[m_cur];
        bound.reg = cr.reg;
        closure->captures.push_back(bound);
      }
      frame.regs[i.a] = QLangValue::Function(std::move(closure));
      break;
    }
    case Opcode::REFLOAD: {
      // r[a] = captures[b].frame->regs[captures[b].reg]  (live read).
      const CaptureRef &cr = frame.captures[i.b];
      auto *framePtr = static_cast<Frame *>(cr.frame);
      frame.regs[i.a] = framePtr->regs[cr.reg];
      break;
    }
    case Opcode::REFSTORE: {
      // captures[b].frame->regs[captures[b].reg] = r[a]  (write-back).
      CaptureRef &cr = frame.captures[i.b];
      auto *framePtr = static_cast<Frame *>(cr.frame);
      framePtr->regs[cr.reg] = frame.regs[i.a];
      break;
    }
    case Opcode::RETURN: {
      m_returnValue = frame.regs[i.a];
      if (m_cur == 0) {
        // Returning from main: sync the frame back into the scope before
        // unwinding so script variables persist.
        fprintf(stderr, "[DBG] main RETURN sync: vars=%zu regCount=%d\n", m_prog.vars.size(), m_prog.registerCount);
        for (const auto &[name, reg] : m_prog.vars) {
          fprintf(stderr, "[DBG]   var '%s' reg=%d val=%s\n", name.c_str(), reg, frame.regs[reg].typeName());
          if (m_deleted.count(reg))
            m_scope.remove(name);
          else
            m_scope.set(name, frame.regs[reg]);
        }
      }
      m_frames.pop_back();
      m_cur--;
      continue; // caller frame is re-fetched next iteration; skip trailing increment
    }
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
      frame.regs[i.a] = doBinary(op, frame.regs[i.b], frame.regs[i.c]);
      break;
    case Opcode::JMP:
      frame.pc = static_cast<size_t>(i.a);
      continue;
    case Opcode::JMPIFFALSE:
      if (!isTrue(frame.regs[i.a]))
        frame.pc = static_cast<size_t>(i.b);
      else
        frame.pc = frame.pc + 1;
      continue;
    }
    frame.pc += 1;
  }
}

} // namespace qlang
