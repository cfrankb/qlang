#include "qlang/QLangVM.h"

#include <cstdint>
#include <cstdio>
#include <deque>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>
#include <cmath>

#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"

namespace qlang {

namespace {

bool isNumeric(const QLangValue &v) {
  auto t = v.type();
  return t == QLangValue::Type::Bool || t == QLangValue::Type::Char ||
         t == QLangValue::Type::Byte || t == QLangValue::Type::Word ||
         t == QLangValue::Type::Short || t == QLangValue::Type::Int ||
         t == QLangValue::Type::Unsigned || t == QLangValue::Type::Dword ||
         t == QLangValue::Type::Float;
}

int64_t toInt64(const QLangValue &v) {
  switch (v.type()) {
  case QLangValue::Type::Bool: return static_cast<int64_t>(v.asInt());
  case QLangValue::Type::Char: return static_cast<int64_t>(v.asChar());
  case QLangValue::Type::Byte: return static_cast<int64_t>(v.asByte());
  case QLangValue::Type::Word: return static_cast<int64_t>(v.asWord());
  case QLangValue::Type::Short: return static_cast<int64_t>(v.asShort());
  case QLangValue::Type::Int: return static_cast<int64_t>(v.asInt());
  case QLangValue::Type::Unsigned: return static_cast<int64_t>(v.asUnsigned());
  case QLangValue::Type::Dword: {
    uint64_t u = v.asDword();
    if (u > static_cast<uint64_t>(INT64_MAX))
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "dword value exceeds int64_t range");
    return static_cast<int64_t>(u);
  }
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
  case QLangValue::Type::Bool: return std::to_string(static_cast<int32_t>(v.asInt()));
  case QLangValue::Type::Char: return std::to_string(v.asChar());
  case QLangValue::Type::Byte: return std::to_string(v.asByte());
  case QLangValue::Type::Word: return std::to_string(v.asWord());
  case QLangValue::Type::Short: return std::to_string(v.asShort());
  case QLangValue::Type::Int: return std::to_string(v.asInt());
  case QLangValue::Type::Unsigned: return std::to_string(v.asUnsigned());
  case QLangValue::Type::Dword: return std::to_string(v.asDword());
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

// Sequential access for `for i in <collection>`: returns the idx-th element of
// an array, the idx-th table value in insertion (std::map) order, or the
// idx-th character of a string (returned as a byte value).
QLangValue seqGet(const QLangValue &col, int64_t idx) {
  if (col.type() == QLangValue::Type::String) {
    const auto &s = col.asString();
    if (idx < 0 || idx >= static_cast<int64_t>(s.size()))
      throw QLangError(QLangErrorKind::Runtime, 0, 0, "string index out of range");
    return QLangValue::Byte(static_cast<uint8_t>(s[static_cast<size_t>(idx)]));
  }
  if (col.type() == QLangValue::Type::Array) {
    const auto &elems = col.asArray();
    if (idx < 0 || idx >= static_cast<int64_t>(elems.size()))
      throw QLangError(QLangErrorKind::Runtime, 0, 0, "array index out of range");
    return elems[static_cast<size_t>(idx)];
  }
  if (col.type() == QLangValue::Type::Table) {
    const auto &kv = col.asTable();
    if (idx < 0 || idx >= static_cast<int64_t>(kv.size()))
      throw QLangError(QLangErrorKind::Runtime, 0, 0, "index out of range");
    auto it = kv.begin();
    std::advance(it, static_cast<std::ptrdiff_t>(idx));
    return it->second;
  }
  throw QLangError(QLangErrorKind::Type, 0, 0,
                   std::string("cannot iterate non-collection of type ") +
                       col.typeName());
}

// Dispatch a built-in function call. `args` are the evaluated operands; the
// return value is stored by the caller into the call's destination register.
// `scope` is the host scope (seeded at run start, synced at run end).
// `prog.vars` maps variable names to frame registers; `deleted` tracks
// registers removed by `delete`. Used by isset() to check existence during
// execution (vars live in registers, not scope, until run completes).
QLangValue callBuiltin(Builtin id, const std::vector<QLangValue> &args,
                       QLangContext &scope, const CompiledProgram &prog,
                       const std::unordered_set<int> &deleted) {
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
        return QLangValue::Int(0);
      }
    }
    // nil and other non-numeric types return 0 per spec §237
    return QLangValue::Int(0);
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
  case Builtin::Sprintf: {
    // sprintf(fmt, ...) — same format as printf but returns the formatted string.
    if (args.empty())
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "sprintf expects at least a format string");
    if (args[0].type() != QLangValue::Type::String)
      throw QLangError(QLangErrorKind::Type, 0, 0,
                       "sprintf: format string must be a string");
    std::string fmt = args[0].asString();
    std::string out;
    size_t ai = 1; // next format argument
    for (size_t i = 0; i < fmt.size();) {
      if (fmt[i] == '%' && i + 1 < fmt.size()) {
        // Skip optional width/precision: digits and dots after %
        size_t j = i + 1;
        while (j < fmt.size() && (fmt[j] == '.' || (fmt[j] >= '0' && fmt[j] <= '9')))
          j++;
        if (j >= fmt.size()) {
          // Trailing %, treat as literal
          out += '%';
          i++;
          continue;
        }
        char spec = fmt[j];
        if (spec == '%') {
          out += '%';
          i = j + 1;
          continue;
        }
        if (ai >= args.size())
          throw QLangError(QLangErrorKind::Runtime, 0, 0,
                           "sprintf: not enough arguments for format spec");
        const QLangValue &av = args[ai++];
        switch (spec) {
        case 'd': out += std::to_string(toInt64(av)); break;
        case 'f': {
          // Extract precision from format (e.g. %.2f).
          int precision = 6;
          {
            size_t k = i + 1;
            if (k < fmt.size() && fmt[k] == '.') {
              k++;
              int prec = 0;
              while (k < fmt.size() && fmt[k] >= '0' && fmt[k] <= '9') {
                prec = prec * 10 + (fmt[k] - '0');
                k++;
              }
              if (k > i + 2)
                precision = prec;
            }
          }
          char buf[64];
          std::snprintf(buf, sizeof(buf), "%.*f", precision, toDouble(av));
          out += buf;
          break;
        }
        case 's': out += valueToString(av); break;
        case 'c': out += static_cast<char>(toInt64(av)); break;
        default: out += spec; break;
        }
        i = j + 1;
      } else {
        out += fmt[i++];
      }
    }
    return QLangValue::String(out);
  }
  case Builtin::Len: {
    need(1, "len");
    return QLangValue::Int(static_cast<int32_t>(lenOf(args[0])));
  }
  case Builtin::Blob: {
    need(0, "blob");
    return QLangValue::Blob(std::vector<uint8_t>{});
  }
  case Builtin::Substr: {
    // substr(str, start, [length]) — 0-based start (consistent with array
    // indexing, spec §100); length optional (defaults to end of string).
    if (args.size() < 2 || args.size() > 3)
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "substr expects 2 or 3 args (str, start, [length])");
    if (args[0].type() != QLangValue::Type::String)
      throw QLangError(QLangErrorKind::Type, 0, 0,
                       "substr: first argument must be a string");
    const std::string &s = args[0].asString();
    int64_t start = toInt64(args[1]);
    int64_t len = (args.size() == 3)
                      ? toInt64(args[2])
                      : static_cast<int64_t>(s.size()) - start;
    if (start < 0) start = 0;
    if (len < 0) len = 0;
    if (start > static_cast<int64_t>(s.size()))
      return QLangValue::String("");
    if (start + len > static_cast<int64_t>(s.size()))
      len = static_cast<int64_t>(s.size()) - start;
    return QLangValue::String(
        s.substr(static_cast<size_t>(start), static_cast<size_t>(len)));
  }
  case Builtin::Slice: {
    // slice(array, start, [length]) — 0-based start; length optional (defaults
    // to rest of array). Returns a new array.
    if (args.size() < 2 || args.size() > 3)
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "slice expects 2 or 3 args (array, start, [length])");
    if (args[0].type() != QLangValue::Type::Array)
      throw QLangError(QLangErrorKind::Type, 0, 0,
                       "slice: first argument must be an array");
    const std::vector<QLangValue> &arr = args[0].asArray();
    int64_t start = toInt64(args[1]);
    int64_t len = (args.size() == 3)
                      ? toInt64(args[2])
                      : static_cast<int64_t>(arr.size()) - start;
    if (start < 0) start = 0;
    if (len < 0) len = 0;
    if (start >= static_cast<int64_t>(arr.size()))
      return QLangValue::Array({});
    if (start + len > static_cast<int64_t>(arr.size()))
      len = static_cast<int64_t>(arr.size()) - start;
    std::vector<QLangValue> result;
    result.reserve(static_cast<size_t>(len));
    for (int64_t i = 0; i < len; ++i)
      result.push_back(arr[static_cast<size_t>(start) + i]);
    return QLangValue::Array(result);
  }
  case Builtin::Range: {
    // range(start, end) — inclusive on both ends; returns [start, start+1, ...,
    // end]. For start > end returns empty array.
    if (args.size() != 2)
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "range expects 2 args (start, end)");
    int64_t start = toInt64(args[0]);
    int64_t end = toInt64(args[1]);
    std::vector<QLangValue> result;
    if (start <= end) {
      result.reserve(static_cast<size_t>(end - start + 1));
      for (int64_t i = start; i <= end; ++i)
        result.push_back(QLangValue::Int(static_cast<int32_t>(i)));
    }
    return QLangValue::Array(result);
  }
  case Builtin::Join: {
    // join(separator, array) — concatenates array elements with separator.
    // Non-string elements are converted via valueToString.
    if (args.size() != 2)
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "join expects 2 args (separator, array)");
    if (args[1].type() != QLangValue::Type::Array)
      throw QLangError(QLangErrorKind::Type, 0, 0,
                       "join: second argument must be an array");
    const std::string &sep = args[0].asString();
    const auto &elems = args[1].asArray();
    std::string result;
    for (size_t i = 0; i < elems.size(); ++i) {
      if (i > 0) result += sep;
      result += valueToString(elems[i]);
    }
    return QLangValue::String(result);
  }
  case Builtin::IsSet: {
    // isset(var_name) — returns true (1) if the named variable exists in
    // scope (or current frame registers during execution), false (0) otherwise.
    need(1, "isset");
    if (args[0].type() != QLangValue::Type::String)
      throw QLangError(QLangErrorKind::Type, 0, 0,
                       "isset: argument must be a string (variable name)");
    const std::string &name = args[0].asString();
    // Check scope first (for pre-seeded vars set before run()).
    if (scope.has(name)) return QLangValue::Int(1);
    // During execution, vars live in frame registers. Check the program's
    // name->reg map and the deleted set.
    auto it = prog.vars.find(name);
    if (it != prog.vars.end() && !deleted.count(it->second))
      return QLangValue::Int(1);
    return QLangValue::Int(0);
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
  case QLangValue::Type::Bool:
    return QLangValue::Bool(toInt64(v) ? 1 : 0);
  case QLangValue::Type::Char:
    return QLangValue::Char(static_cast<int8_t>(toInt64(v)));
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
  case QLangValue::Type::Dword:
    return QLangValue::Dword(static_cast<uint64_t>(toInt64(v)));
  default: return v;
  }
}

QLangValue QLangVM::doUnary(Opcode op, const QLangValue &a) {
  switch (op) {
  case Opcode::NEG:
    return QLangValue::Int(static_cast<int32_t>(-num(a, "-")));
  case Opcode::NOT:
    return QLangValue::Int(isTrue(a) ? 0 : 1);
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
  case Opcode::MOD: {
    if (a.type() == QLangValue::Type::Float ||
        b.type() == QLangValue::Type::Float) {
      double d = toDouble(b);
      if (d == 0.0)
        throw QLangError(QLangErrorKind::Type, 0, 0, "modulo by zero");
      return QLangValue::Float(std::fmod(toDouble(a), d));
    }
    int64_t d = num(b, "%");
    if (d == 0)
      throw QLangError(QLangErrorKind::Type, 0, 0, "modulo by zero");
    return QLangValue::Int(static_cast<int32_t>(num(a, "%") % d));
  }
  case Opcode::POW: {
    // Exponential: pow(base, exp)
    if (a.type() == QLangValue::Type::Float ||
        b.type() == QLangValue::Type::Float) {
      return QLangValue::Float(std::pow(toDouble(a), toDouble(b)));
    }
    int64_t base = num(a, "**");
    int64_t exp = num(b, "**");
    if (exp < 0) {
      // Negative exponent with integer operands: return 0 (integer arithmetic)
      if (base == 1) return QLangValue::Int(1);
      if (base == -1) return QLangValue::Int(exp % 2 == 0 ? 1 : -1);
      return QLangValue::Int(0);
    }
    int64_t result = 1;
    for (int64_t i = 0; i < exp; i++)
      result *= base;
    return QLangValue::Int(static_cast<int32_t>(result));
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
  case Opcode::IN: {
    // Membership test: `a in b` where b is an array (element match) or a
    // table (key match). Returns boolean 1/0 (spec §143-147).
    const QLangValue &col = b;
    if (col.type() == QLangValue::Type::Array) {
      for (const QLangValue &e : col.asArray())
        if (cmp(a, e, "in") == 0)
          return QLangValue::Int(1);
      return QLangValue::Int(0);
    }
    if (col.type() == QLangValue::Type::Table) {
      std::string key = valueToString(a);
      return QLangValue::Int(col.asTable().count(key) ? 1 : 0);
    }
    throw QLangError(QLangErrorKind::Type, 0, 0,
                     std::string("operator 'in' requires an array or table, got ") +
                         col.typeName());
  }
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
  cf.callDest = dest;
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
    case Opcode::NOT: frame.regs[i.a] = doUnary(i.op, frame.regs[i.b]); break;
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
    case Opcode::LEN:
      frame.regs[i.a] =
          QLangValue::Int(static_cast<int32_t>(lenOf(frame.regs[i.b])));
      break;
    case Opcode::SEQGET:
      frame.regs[i.a] = seqGet(frame.regs[i.b], toInt64(frame.regs[i.c]));
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
    case Opcode::TABCREATE: {
      frame.regs[i.a] = QLangValue::Table(TableValue{});
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
      if (i.b < 0) {
        // Native (C++) function: index into compiledProgram.nativeFuncs.
        int idx = -1 - i.b;
        if (idx < 0 || idx >= static_cast<int>(m_prog.nativeFuncs.size()))
          throw QLangError(QLangErrorKind::Runtime, 0, 0, "unknown native function");
        frame.regs[dest] = m_prog.nativeFuncs[static_cast<size_t>(idx)](m_scope, args);
      } else {
        frame.regs[dest] = callBuiltin(static_cast<Builtin>(i.b), args, m_scope, m_prog, m_deleted);
      }
      break;
    }
    case Opcode::CALLFN: {
      // User `def` function; callee index is operand b.
      // Advance the caller past the call *before* pushCall (which may
      // reallocate m_frames and invalidate `frame`), so that when the callee
      // returns we resume at the next instruction.
      frame.pc += 1;
      pushCall(i.b, frame.regs, i.a, i.c);
      // Bind capture refs to the current (caller) frame so REFLOAD/REFSTORE
      // inside the callee address the correct enclosing-frame registers.
      const Function &calleeFn = m_prog.functions[i.b];
      m_frames[m_cur].captures = calleeFn.captures;
      for (auto &cr : m_frames[m_cur].captures)
        cr.frame = &m_frames[m_cur - 1];
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
      // Copy the closure's live-binding capture table into the new callee frame
      // so that REFLOAD/REFSTORE inside the closure address the correct enclosing
      // frame registers.
      m_frames[m_cur].captures = cv.asFunction().captures;
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
      // Capture the callee's return value and its caller-dest before unwinding
      // (the deque keeps frame pointers stable, so the caller frame is valid).
      QLangValue retVal = frame.regs.empty() ? QLangValue() : frame.regs[0];
      int dest = frame.callDest;
      if (m_cur == 0) {
        // Returning from main: sync the frame back into the scope before
        // unwinding so script variables persist.
        for (const auto &[name, reg] : m_prog.vars) {
          if (m_deleted.count(reg))
            m_scope.remove(name);
          else if (m_prog.constVars.count(name))
            ; // skip: const-injected vars are read-only, do not sync back
          else
            m_scope.set(name, frame.regs[reg]);
        }
      }
      m_frames.pop_back();
      m_cur--;
      if (dest >= 0 && m_cur >= 0)
        m_frames[m_cur].regs[dest] = retVal;
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
    case Opcode::MOD:
    case Opcode::POW:
    case Opcode::IN:
      frame.regs[i.a] = doBinary(op, frame.regs[i.b], frame.regs[i.c]);
      break;
    case Opcode::JMP:
      frame.pc = static_cast<size_t>(i.a);
      continue;
    case Opcode::JMPIFTRUE:
      if (isTrue(frame.regs[i.a]))
        frame.pc = static_cast<size_t>(i.b);
      else
        frame.pc = frame.pc + 1;
      continue;
    case Opcode::CONTINUE:
      frame.pc = static_cast<size_t>(i.a);
      continue;
    case Opcode::BREAK:
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

void QLangVM::runFrames(int startFrame)
{
  while (m_cur >= startFrame) {
    Frame &frame = m_frames[m_cur];
    const Function &fn = m_prog.functions[frame.funcIndex];
    const auto &instrs = fn.instructions;
    const auto &consts = fn.constants;

    if (frame.pc >= instrs.size()) {
      // Fell off the end: unwind with nil.
      m_frames.pop_back();
      m_cur--;
      continue;
    }

    if (++m_steps > kMaxSteps)
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "max instruction steps exceeded (possible infinite loop)");

    const Instruction &i = instrs[frame.pc];
    Opcode op = i.op;

    switch (op) {
    case Opcode::CONST: frame.regs[i.a] = consts[i.b]; break;
    case Opcode::MOV: frame.regs[i.a] = frame.regs[i.b]; break;
    case Opcode::NEG: frame.regs[i.a] = doUnary(i.op, frame.regs[i.b]); break;
    case Opcode::NOT: frame.regs[i.a] = doUnary(i.op, frame.regs[i.b]); break;
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
                         "cannot append non-array to array");
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
    case Opcode::LEN:
      frame.regs[i.a] =
          QLangValue::Int(static_cast<int32_t>(lenOf(frame.regs[i.b])));
      break;
    case Opcode::SEQGET:
      frame.regs[i.a] = seqGet(frame.regs[i.b], toInt64(frame.regs[i.c]));
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
    case Opcode::TABCREATE: {
      frame.regs[i.a] = QLangValue::Table(TableValue{});
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
      std::vector<QLangValue> callArgs;
      callArgs.reserve(argCount);
      for (int k = 0; k < argCount; k++)
        callArgs.push_back(frame.regs[dest + 1 + k]);
      if (i.b < 0) {
        int idx = -1 - i.b;
        if (idx < 0 || idx >= static_cast<int>(m_prog.nativeFuncs.size()))
          throw QLangError(QLangErrorKind::Runtime, 0, 0, "unknown native function");
        frame.regs[dest] = m_prog.nativeFuncs[static_cast<size_t>(idx)](m_scope, callArgs);
      } else {
        frame.regs[dest] = callBuiltin(static_cast<Builtin>(i.b), callArgs, m_scope, m_prog, m_deleted);
      }
      break;
    }
    case Opcode::CALLFN: {
      frame.pc += 1;
      pushCall(i.b, frame.regs, i.a, i.c);
      const Function &calleeFn = m_prog.functions[i.b];
      m_frames[m_cur].captures = calleeFn.captures;
      for (auto &cr : m_frames[m_cur].captures)
        cr.frame = &m_frames[m_cur - 1];
      continue;
    }
    case Opcode::CALLCLOS: {
      const QLangValue &cv = frame.regs[i.a];
      if (cv.type() != QLangValue::Type::Function)
        throw QLangError(QLangErrorKind::Type, 0, 0,
                         "attempt to call a non-function value");
      frame.pc += 1;
      pushCall(cv.asFunction().funcIndex, frame.regs, i.a, i.c);
      m_frames[m_cur].captures = cv.asFunction().captures;
      continue;
    }
    case Opcode::MAKEFUNC: {
      const QLangValue &cv = consts[i.b];
      if (cv.type() != QLangValue::Type::Function)
        throw QLangError(QLangErrorKind::Runtime, 0, 0,
                         "invalid function constant");
      const Closure &spec = cv.asFunction();
      auto closure = std::make_shared<Closure>();
      closure->funcIndex = spec.funcIndex;
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
      const CaptureRef &cr = frame.captures[i.b];
      auto *framePtr = static_cast<Frame *>(cr.frame);
      frame.regs[i.a] = framePtr->regs[cr.reg];
      break;
    }
    case Opcode::REFSTORE: {
      CaptureRef &cr = frame.captures[i.b];
      auto *framePtr = static_cast<Frame *>(cr.frame);
      framePtr->regs[cr.reg] = frame.regs[i.a];
      break;
    }
    case Opcode::RETURN: {
      QLangValue retVal = frame.regs.empty() ? QLangValue() : frame.regs[0];
      m_frames.pop_back();
      m_cur--;
      if (m_cur >= 0)
        m_frames[m_cur].regs[frame.callDest] = retVal;
      continue;
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
    case Opcode::MOD:
    case Opcode::POW:
    case Opcode::IN:
      frame.regs[i.a] = doBinary(op, frame.regs[i.b], frame.regs[i.c]);
      break;
    case Opcode::JMP:
      frame.pc = static_cast<size_t>(i.a);
      continue;
    case Opcode::JMPIFTRUE:
      if (isTrue(frame.regs[i.a]))
        frame.pc = static_cast<size_t>(i.b);
      else
        frame.pc = frame.pc + 1;
      continue;
    case Opcode::CONTINUE:
      frame.pc = static_cast<size_t>(i.a);
      continue;
    case Opcode::BREAK:
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

QLangValue QLangVM::callFunction(int funcIndex, const std::vector<QLangValue> &args)
{
  if (funcIndex < 0 || funcIndex >= static_cast<int>(m_prog.functions.size()))
    throw QLangError(QLangErrorKind::Runtime, 0, 0, "unknown function");

  // Save current state so we can restore after the call.
  int savedCur = m_cur;
  size_t savedFramesSize = m_frames.size();

  // Create a temporary caller regs vector for pushCall.
  std::vector<QLangValue> callerRegs(static_cast<size_t>(args.size() + 1), QLangValue());
  for (size_t k = 0; k < args.size(); ++k)
    callerRegs[1 + k] = args[k];

  // pushCall pushes the callee frame. We pass dest=0 so args are copied from
  // callerRegs[1..argc] into the callee's regs[0..argc-1]. The result will
  // be stored in the callee's reg 0 on RETURN.
  pushCall(funcIndex, callerRegs, 0, static_cast<int>(args.size()));

  // Bind captures for the new callee frame.
  const Function &calleeFn = m_prog.functions[funcIndex];
  m_frames[m_cur].captures = calleeFn.captures;
  for (auto &cr : m_frames[m_cur].captures)
    cr.frame = &m_frames[m_cur - 1];

  // The callee frame index.
  int calleeFrameIdx = m_cur;

  // Run the VM manually until the callee returns (m_cur drops below calleeFrameIdx).
  while (m_cur >= calleeFrameIdx) {
    Frame &frame = m_frames[m_cur];
    const Function &fn = m_prog.functions[frame.funcIndex];
    const auto &instrs = fn.instructions;
    const auto &consts = fn.constants;

    if (frame.pc >= instrs.size()) {
      // Fell off the end: unwind with nil.
      m_frames.pop_back();
      m_cur--;
      continue;
    }

    if (++m_steps > kMaxSteps)
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "max instruction steps exceeded (possible infinite loop)");

    const Instruction &i = instrs[frame.pc];
    Opcode op = i.op;

    switch (op) {
    case Opcode::CONST: frame.regs[i.a] = consts[i.b]; break;
    case Opcode::MOV: frame.regs[i.a] = frame.regs[i.b]; break;
    case Opcode::NEG: frame.regs[i.a] = doUnary(i.op, frame.regs[i.b]); break;
    case Opcode::NOT: frame.regs[i.a] = doUnary(i.op, frame.regs[i.b]); break;
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
                         "cannot append non-array to array");
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
    case Opcode::LEN:
      frame.regs[i.a] =
          QLangValue::Int(static_cast<int32_t>(lenOf(frame.regs[i.b])));
      break;
    case Opcode::SEQGET:
      frame.regs[i.a] = seqGet(frame.regs[i.b], toInt64(frame.regs[i.c]));
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
    case Opcode::TABCREATE: {
      frame.regs[i.a] = QLangValue::Table(TableValue{});
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
      std::vector<QLangValue> callArgs;
      callArgs.reserve(argCount);
      for (int k = 0; k < argCount; k++)
        callArgs.push_back(frame.regs[dest + 1 + k]);
      if (i.b < 0) {
        int idx = -1 - i.b;
        if (idx < 0 || idx >= static_cast<int>(m_prog.nativeFuncs.size()))
          throw QLangError(QLangErrorKind::Runtime, 0, 0, "unknown native function");
        frame.regs[dest] = m_prog.nativeFuncs[static_cast<size_t>(idx)](m_scope, callArgs);
      } else {
        frame.regs[dest] = callBuiltin(static_cast<Builtin>(i.b), callArgs, m_scope, m_prog, m_deleted);
      }
      break;
    }
    case Opcode::CALLFN: {
      frame.pc += 1;
      pushCall(i.b, frame.regs, i.a, i.c);
      const Function &calleeFn2 = m_prog.functions[i.b];
      m_frames[m_cur].captures = calleeFn2.captures;
      for (auto &cr : m_frames[m_cur].captures)
        cr.frame = &m_frames[m_cur - 1];
      continue;
    }
    case Opcode::CALLCLOS: {
      const QLangValue &cv = frame.regs[i.a];
      if (cv.type() != QLangValue::Type::Function)
        throw QLangError(QLangErrorKind::Type, 0, 0,
                         "attempt to call a non-function value");
      frame.pc += 1;
      pushCall(cv.asFunction().funcIndex, frame.regs, i.a, i.c);
      m_frames[m_cur].captures = cv.asFunction().captures;
      continue;
    }
    case Opcode::MAKEFUNC: {
      const QLangValue &cv = consts[i.b];
      if (cv.type() != QLangValue::Type::Function)
        throw QLangError(QLangErrorKind::Runtime, 0, 0,
                         "invalid function constant");
      const Closure &spec = cv.asFunction();
      auto closure = std::make_shared<Closure>();
      closure->funcIndex = spec.funcIndex;
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
      const CaptureRef &cr = frame.captures[i.b];
      auto *framePtr = static_cast<Frame *>(cr.frame);
      frame.regs[i.a] = framePtr->regs[cr.reg];
      break;
    }
    case Opcode::REFSTORE: {
      CaptureRef &cr = frame.captures[i.b];
      auto *framePtr = static_cast<Frame *>(cr.frame);
      framePtr->regs[cr.reg] = frame.regs[i.a];
      break;
    }
    case Opcode::RETURN: {
      // Capture the result BEFORE unwinding.
      QLangValue retVal = frame.regs.empty() ? QLangValue() : frame.regs[0];
      m_frames.pop_back();
      m_cur--;
      // Restore state and return.
      while (m_frames.size() > savedFramesSize)
        m_frames.pop_back();
      m_cur = savedCur;
      return retVal;
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
    case Opcode::MOD:
    case Opcode::POW:
    case Opcode::IN:
      frame.regs[i.a] = doBinary(op, frame.regs[i.b], frame.regs[i.c]);
      break;
    case Opcode::JMP:
      frame.pc = static_cast<size_t>(i.a);
      continue;
    case Opcode::JMPIFTRUE:
      if (isTrue(frame.regs[i.a]))
        frame.pc = static_cast<size_t>(i.b);
      else
        frame.pc = frame.pc + 1;
      continue;
    case Opcode::CONTINUE:
      frame.pc = static_cast<size_t>(i.a);
      continue;
    case Opcode::BREAK:
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

  // Fell off the end without a RETURN.
  while (m_frames.size() > savedFramesSize)
    m_frames.pop_back();
  m_cur = savedCur;
  return QLangValue();
}

} // namespace qlang
