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

double toDouble(const QLangValue &v) {
  switch (v.type()) {
  case QLangValue::Type::Float: return v.asFloat();
  default: return static_cast<double>((long long)toInt64(v));
  }
}

// Text form of a value, used by toStr/print/printf and string concatenation.
std::string valueToString(const QLangValue &v) {
  switch (v.type()) {
  case QLangValue::Type::Nil: return "nil";
  case QLangValue::Type::String: return v.asString();
  case QLangValue::Type::Bool: return v.asInt() ? "true" : "false";
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
  case QLangValue::Type::Function: return "<function>";
  case QLangValue::Type::Generator: return "<generator>";
  case QLangValue::Type::Eof: return "<eof>";
  }
  return "?";
}

// Numeric operand with a clear error if the type is wrong.
int64_t num(const QLangValue &v, const char *op) {
  if (!isNumeric(v))
    throw QLangError(QLangErrorKind::Type, 0, 0,
                     std::string("operator '") + op +
                         "' operand must be numeric, got " + v.typeName());
  return (long long)toInt64(v);
}

int cmp(const QLangValue &a, const QLangValue &b, const char *op) {
  bool numA = isNumeric(a), numB = isNumeric(b);
  if (numA && numB) {
    if (a.type() == QLangValue::Type::Float ||
        b.type() == QLangValue::Type::Float) {
      double x = toDouble(a), y = toDouble(b);
      return (x < y) ? -1 : (x > y) ? 1 : 0;
    }
    int64_t x = (long long)toInt64(a), y = (long long)toInt64(b);
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

// Equality per spec §23 (== and !=): Int 1/0. nil == nil true; nil != x true.
// bool == int (bool stored as Int 1/0 at runtime). Number types compare equal
// when their int64 values match. Any other comparison: same type and same
// value. `!=` is the exact negation (handled by the caller).
bool isEqual(const QLangValue &a, const QLangValue &b) {
  // nil == nil; nil != anything else.
  if (a.type() == QLangValue::Type::Nil || b.type() == QLangValue::Type::Nil)
    return a.type() == b.type();
  // bool compared as int64 (bool stored as Int 1/0 at runtime).
  if (a.type() == QLangValue::Type::Bool || b.type() == QLangValue::Type::Bool)
    return toInt64(a) == toInt64(b);
  // number types compare equal when their int64 values match.
  if (isNumeric(a) && isNumeric(b))
    return toInt64(a) == toInt64(b);
  // otherwise: same type and same value.
  if (a.type() != b.type())
    return false;
  switch (a.type()) {
  case QLangValue::Type::String:
    return a.asString() == b.asString();
  case QLangValue::Type::Blob:
    return a.asBlob() == b.asBlob();
  case QLangValue::Type::Array: {
    const auto &ea = a.asArray(), eb = b.asArray();
    if (ea.size() != eb.size()) return false;
    for (size_t i = 0; i < ea.size(); ++i)
      if (!isEqual(ea[i], eb[i])) return false;
    return true;
  }
  case QLangValue::Type::Table: {
    const auto &ta = *a.asTable(), tb = *b.asTable();
    if (ta.size() != tb.size()) return false;
    for (const auto &kv : ta) {
      auto it = tb.find(kv.first);
      if (it == tb.end()) return false;
      if (!isEqual(kv.second, it->second)) return false;
    }
    return true;
  }
  default:
    return false;
  }
}

// Length for len(): string/array/table/blob lengths; any other type is 1
// (per spec: "other type return 1").
int64_t lenOf(const QLangValue &v) {
  switch (v.type()) {
  case QLangValue::Type::String: return v.asString().size();
  case QLangValue::Type::Array: return v.asArray().size();
  case QLangValue::Type::Table: return v.asTable()->size();
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
    const auto &kv = *col.asTable();
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
                       const std::unordered_set<int> &deleted,
                       int line = 0, int col = 0,
                       QLangVM *vm = nullptr) {
  auto need = [&](size_t n, const char *fn) {
    if (args.size() != static_cast<size_t>(n))
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       std::string(fn) + " expects " + std::to_string(n) +
                           " arg(s), got " + std::to_string(args.size()));
  };

  switch (id) {
  case Builtin::ToStr:
    need(1, "str");
    // Check for __str__ method on tables (class instances).
    if (args[0].type() == QLangValue::Type::Table) {
      const auto &t = *args[0].asTable();
      auto it = t.find("__str__");
      if (it != t.end() && it->second.type() == QLangValue::Type::Function && vm) {
        return vm->callClosure(it->second, {args[0]});
      }
    }
    return QLangValue::String(valueToString(args[0]));
  case Builtin::ToInt:
    need(1, "toInt");
    if (isNumeric(args[0]))
      return QLangValue::Int(static_cast<int32_t>((long long)toInt64(args[0])));
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
        case 'd': out += std::to_string((long long)toInt64(av)); break;
        case 'f': {
          char buf[64];
          std::snprintf(buf, sizeof(buf), "%.6f", toDouble(av));
          out += buf;
          break;
        }
        case 's': out += valueToString(av); break;
        case 'c': out += static_cast<char>((long long)toInt64(av)); break;
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
        case 'd': out += std::to_string((long long)toInt64(av)); break;
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
        case 'c': out += static_cast<char>((long long)toInt64(av)); break;
        default: out += spec; break;
        }
        i = j + 1;
      } else {
        out += fmt[i++];
      }
    }
    return QLangValue::String(out);
  }
  case Builtin::Format: {
    // format(template, arg1, arg2, ...) — f-string interpolation.
    // Supports full printf-style format specifiers: %s, %d, %f, %.*f, %5d, etc.
    if (args.empty())
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "format expects at least a template string");
    if (args[0].type() != QLangValue::Type::String)
      throw QLangError(QLangErrorKind::Type, 0, 0,
                       "format: template must be a string");
    std::string fmt = args[0].asString();
    std::string out;
    size_t ai = 1;
    for (size_t i = 0; i < fmt.size(); ) {
      if (fmt[i] == '%' && i + 1 < fmt.size()) {
        // Parse the full format spec: %[flags][width][.precision]conversion
        size_t j = i + 1;
        // Skip flags: -, +, space, 0, #
        while (j < fmt.size() && (fmt[j] == '-' || fmt[j] == '+' ||
                                   fmt[j] == ' ' || fmt[j] == '0' || fmt[j] == '#'))
          j++;
        // Skip width digits
        while (j < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[j])))
          j++;
        // Skip precision: .digits
        if (j < fmt.size() && fmt[j] == '.') {
          j++;
          while (j < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[j])))
            j++;
        }
        if (j >= fmt.size()) {
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
                           "format: not enough arguments for template placeholder");
        const QLangValue &av = args[ai++];
        // Use the original format substring (including flags/width/precision)
        // to let snprintf handle all formatting natively.
        // j points at the conversion specifier, so include it: length = j - i + 1.
        std::string sub(fmt, i, j - i + 1);
        if (av.type() == QLangValue::Type::String) {
          char buf[256];
          std::snprintf(buf, sizeof(buf), sub.c_str(), av.asString().c_str());
          out += buf;
        } else if (av.type() == QLangValue::Type::Float) {
          char buf[256];
          std::snprintf(buf, sizeof(buf), sub.c_str(), toDouble(av));
          out += buf;
        } else if (av.type() == QLangValue::Type::Bool) {
          char buf[256];
          std::snprintf(buf, sizeof(buf), sub.c_str(), (long long)toInt64(av));
          out += buf;
        } else if (isNumeric(av)) {
          // If the placeholder is %s but the arg is numeric, use %d instead.
          std::string actualFmt = sub;
          if (sub == "%s" || sub == "%+s" || sub == "% s" || sub == "%0s" ||
              sub == "%#s") {
            // Build a numeric placeholder with the same width/flags.
            // Strip the 's' and replace with 'd'.
            std::string prefix = sub.substr(0, sub.size() - 1);
            actualFmt = prefix + "d";
          }
          char buf[256];
          std::snprintf(buf, sizeof(buf), actualFmt.c_str(), (long long)toInt64(av));
          out += buf;
        } else {
          out += valueToString(av);
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
    int64_t start = (long long)toInt64(args[1]);
    int64_t len = (args.size() == 3)
                      ? (long long)toInt64(args[2])
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
    int64_t start = (long long)toInt64(args[1]);
    int64_t len = (args.size() == 3)
                      ? (long long)toInt64(args[2])
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
    int64_t start = (long long)toInt64(args[0]);
    int64_t end = (long long)toInt64(args[1]);
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
  case Builtin::Assert: {
    // assert(bool, str) — throws a runtime error with the given message if
    // the boolean argument is false (0).
    need(2, "assert");
    // Validate args regardless of condition.
    if (args[1].type() != QLangValue::Type::String)
      throw QLangError(QLangErrorKind::Type, line, col,
                       "assert: second argument must be a string (error message)");
    // Truthiness check (same logic as QLangVM::isTrue).
    bool truthy = false;
    const auto &arg0 = args[0];
    if (arg0.type() == QLangValue::Type::Nil) truthy = false;
    else if (arg0.type() == QLangValue::Type::Eof) truthy = false;
    else if (arg0.type() == QLangValue::Type::String)
      truthy = !arg0.asString().empty();
    else if (isNumeric(arg0))
      truthy = (long long)toInt64(arg0) != 0;
    else
      truthy = true;
    if (!truthy)
      throw QLangError(QLangErrorKind::Runtime, line, col, args[1].asString());
    return QLangValue();
  }
  case Builtin::Next: {
    need(1, "next");
    if (args[0].type() != QLangValue::Type::Generator)
      throw QLangError(QLangErrorKind::Type, 0, 0, "next expects a generator");
    auto &gen = const_cast<GeneratorValue &>(args[0].asGenerator());
    vm->trace(3, "[DBG-NEXT] calling generator, about to invoke gen()\n");
    auto opt = gen();
    vm->trace(3, "[DBG-NEXT] gen() returned, has_value=%d, type=%d\n",
            opt.has_value() ? 1 : 0,
            opt.has_value() ? (int)opt->type() : -1);
    if (opt.has_value()) {
      vm->trace(3, "[DBG-NEXT] returning value type=%d val=%lld\n",
              (int)opt->type(), (long long)toInt64(*opt));
    }
    return opt.value_or(QLangValue());
  }
  case Builtin::SetMaxSteps: {
    need(1, "set_max_steps");
    long long v = toInt64(args[0]);
    if (v < 0) v = 0;
    scope.setMaxSteps(static_cast<std::size_t>(v));
    return QLangValue();
  }
  case Builtin::GetMaxSteps:
    return QLangValue::Int(static_cast<int32_t>(scope.maxSteps()));
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
  case QLangValue::Type::Eof: return false;
  case QLangValue::Type::String: return !v.asString().empty();
  default: return isNumeric(v) && (long long)toInt64(v) != 0;
  }
}

QLangValue QLangVM::callClosure(const QLangValue &closure, const std::vector<QLangValue> &args) {
  if (closure.type() == QLangValue::Type::Generator) {
    // Generator: resume the saved frame and run until the next yield or exhaustion.
    auto &gen = closure.asGenerator();
    auto opt = gen();
    return opt.value_or(QLangValue());
  }
  if (closure.type() != QLangValue::Type::Function)
    throw QLangError(QLangErrorKind::Type, 0, 0, "attempt to call a non-function value");
  return callFunction(closure.asFunction().funcIndex, args);
}

// Convert a value to the target declared type. Numeric types truncate/wrap
// (C-style cast semantics); other types pass through unchanged.
// For typed declarations — lenient: pass through non-numeric mismatches.
// For runtime cast — use OP_CAST instead, which is strict.
QLangValue convert(const QLangValue &v, QLangValue::Type type) {
  switch (type) {
  case QLangValue::Type::Bool:
    return QLangValue::Bool((long long)toInt64(v) ? 1 : 0);
  case QLangValue::Type::Char:
    return QLangValue::Char(static_cast<int8_t>((long long)toInt64(v)));
  case QLangValue::Type::Byte:
    return QLangValue::Byte(static_cast<uint8_t>((long long)toInt64(v)));
  case QLangValue::Type::Word:
    return QLangValue::Word(static_cast<uint16_t>((long long)toInt64(v)));
  case QLangValue::Type::Short:
    return QLangValue::Short(static_cast<int16_t>((long long)toInt64(v)));
  case QLangValue::Type::Int:
    return QLangValue::Int(static_cast<int32_t>((long long)toInt64(v)));
  case QLangValue::Type::Unsigned:
    return QLangValue::Unsigned(static_cast<uint32_t>((long long)toInt64(v)));
  case QLangValue::Type::Dword:
    return QLangValue::Dword(static_cast<uint64_t>((long long)toInt64(v)));
  default: return v;
  }
}

// Strict runtime cast: throws on invalid source→target combinations.
// Used by OP_CAST for `cast<Type>(expr)` — distinct from OP_CONV which is
// lenient for typed declarations.
QLangValue strict_convert(const QLangValue &v, QLangValue::Type type) {
  // Numeric targets: must be numeric source.
  if (type == QLangValue::Type::Bool || type == QLangValue::Type::Char ||
      type == QLangValue::Type::Byte || type == QLangValue::Type::Word ||
      type == QLangValue::Type::Short || type == QLangValue::Type::Int ||
      type == QLangValue::Type::Unsigned || type == QLangValue::Type::Dword ||
      type == QLangValue::Type::Float) {
    if (v.type() == QLangValue::Type::Array || v.type() == QLangValue::Type::Table ||
        v.type() == QLangValue::Type::Blob) {
      throw QLangError(QLangErrorKind::Type, 0, 0,
                       std::string("cannot cast ") + v.typeName() + " to " +
                       QLangValue(type).typeName());
    }
    int64_t n = (long long)toInt64(v);
    switch (type) {
    case QLangValue::Type::Bool:
      return QLangValue::Bool(n ? 1 : 0);
    case QLangValue::Type::Char:
      return QLangValue::Char(static_cast<int8_t>(n));
    case QLangValue::Type::Byte:
      return QLangValue::Byte(static_cast<uint8_t>(n));
    case QLangValue::Type::Word:
      return QLangValue::Word(static_cast<uint16_t>(n));
    case QLangValue::Type::Short:
      return QLangValue::Short(static_cast<int16_t>(n));
    case QLangValue::Type::Int:
      return QLangValue::Int(static_cast<int32_t>(n));
    case QLangValue::Type::Unsigned:
      return QLangValue::Unsigned(static_cast<uint32_t>(n));
    case QLangValue::Type::Dword:
      return QLangValue::Dword(static_cast<uint64_t>(n));
    case QLangValue::Type::Float:
      return QLangValue::Float(static_cast<double>(n));
    default:
      return v;
    }
  }
  // String target: convert anything to string.
  if (type == QLangValue::Type::String) {
    return QLangValue::String(valueToString(v));
  }
  // Collection targets: allow same-type, reject others.
  if (type == QLangValue::Type::Array || type == QLangValue::Type::Table) {
    if (v.type() != QLangValue::Type::Array && v.type() != QLangValue::Type::Table) {
      throw QLangError(QLangErrorKind::Type, 0, 0,
                       std::string("cannot cast ") + v.typeName() + " to " +
                       QLangValue(type).typeName());
    }
    return v;
  }
  return v;
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
    return QLangValue::Int(isEqual(a, b) ? 1 : 0);
  case Opcode::NE:
    return QLangValue::Int(!isEqual(a, b) ? 1 : 0);
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
      return QLangValue::Int(col.asTable()->count(key) ? 1 : 0);
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
  trace(1, "[pushCall] calleeIndex=%d functions.size()=%zu dest=%d argc=%d callerRegs=%zu\n", calleeIndex, m_prog.functions.size(), dest, argc, callerRegs.size());
  const Function &callee = m_prog.functions[calleeIndex];
  Frame cf;
  cf.funcIndex = calleeIndex;
  cf.callDest = dest;
  cf.regs.resize(callee.registerCount, QLangValue());
  // Copy the call arguments (caller regs dest+1..dest+argc) into the callee's
  // parameter registers 0..argc-1.
  for (int k = 0; k < argc; k++)
    cf.regs[k] = callerRegs[dest + 1 + k];
  trace(1, "[pushCall] after arg copy, argc=%d\n", argc);
  m_frames.push_back(std::move(cf));
  trace(1, "[pushCall] after push_back\n");
  m_cur = static_cast<int>(m_frames.size()) - 1;
  m_frames[m_cur].pc = 0;
  trace(1, "[pushCall] done\n");
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

  // Debug: dump function info
  for (size_t fi = 0; fi < m_prog.functions.size(); fi++) {
    const auto &fn = m_prog.functions[fi];
    trace(2, "[DBG-VM] Function[%zu] '%s' has %zu instrs, %zu consts, %d regs, isGen=%d\n",
            fi, fn.name.c_str(), fn.instructions.size(), fn.constants.size(), fn.registerCount, fn.isGenerator);
    for (size_t i = 0; i < fn.instructions.size(); i++) {
      auto &inst = fn.instructions[i];
      trace(2, "[DBG-VM]   [%zu] op=%d a=%d b=%d c=%d\n", i, (int)inst.op, inst.a, inst.b, inst.c);
    }
    trace(2, "[DBG-VM]   Constants:\n");
    for (size_t i = 0; i < fn.constants.size(); i++) {
      auto &c = fn.constants[i];
      if (c.type() == QLangValue::Type::Int)
        trace(2, "[DBG-VM]     [%zu] Int(%d)\n", i, (int)c.asInt());
      else if (c.type() == QLangValue::Type::String)
        trace(2, "[DBG-VM]     [%zu] String('%s')\n", i, c.asString().c_str());
      else
        trace(2, "[DBG-VM]     [%zu] type=%d\n", i, (int)c.type());
    }
  }

  while (m_cur >= 0) {
    Frame &frame = m_frames[m_cur];
    const Function &fn = m_prog.functions[frame.funcIndex];
    const auto &instrs = fn.instructions;
    const auto &consts = fn.constants;

    if (frame.pc >= instrs.size()) {
      // Fell off the end without an explicit return: unwind with nil.
      trace(2, "[DBG] fell off end at func %d, pc=%zu instrs=%zu\n", frame.funcIndex, frame.pc, instrs.size());
      m_returnValue = frame.regs[0];
      m_frames.pop_back();
      m_cur--;
      continue;
    }

    if (++m_steps > m_scope.maxSteps())
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "infinite loop (step limit)");

    const Instruction &i = instrs[frame.pc];
    Opcode op = i.op;
    trace(2, "[VM] func=%d pc=%zu op=%d a=%d b=%d c=%d regs=%zu\n", frame.funcIndex, frame.pc, (int)op, i.a, i.b, i.c, frame.regs.size());
    trace(2, "[DBG-MAIN] mainFrameRegs=%p mainFrameSize=%zu generatorFrames.size=%zu\n",
            frame.regs.data(), frame.regs.size(), m_generatorFrames.size());
    for (auto &kv : m_generatorFrames) {
      QLangValue *r2 = (kv.second.regs.size() > 2) ? &kv.second.regs[2] : nullptr;
      trace(2, "[DBG-MAIN]   map[%d] base=%p size=%zu reg2=%p type=%d val=%lld overlapsMain=%d\n",
              kv.first, kv.second.regs.data(), kv.second.regs.size(), r2,
              r2 ? (int)r2->type() : -1, r2 ? (long long)toInt64(*r2) : -1,
              (r2 && (uintptr_t)r2 >= (uintptr_t)frame.regs.data() &&
                   (uintptr_t)r2 < (uintptr_t)frame.regs.data() + frame.regs.size()*sizeof(QLangValue)) ? 1 : 0);
    }

    if (op == Opcode::TABPUT) {
      trace(2, "[VM] About to execute TABPUT a=%d\n", i.a);
    }

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
    case Opcode::CAST:
      frame.regs[i.a] = strict_convert(frame.regs[i.b],
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
      int64_t idx = (long long)toInt64(frame.regs[i.c]);
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
      int64_t idx = (long long)toInt64(frame.regs[i.b]);
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
      frame.regs[i.a] = seqGet(frame.regs[i.b], (long long)toInt64(frame.regs[i.c]));
      break;
    case Opcode::TABGET: {
      QLangValue &base = frame.regs[i.b];
      if (base.type() != QLangValue::Type::Table)
        throw QLangError(QLangErrorKind::Type, 0, 0,
                         "cannot access member of non-table value");
      const std::string &key = valueToString(frame.regs[i.c]);
      const auto &kv = *base.asTable();
      auto it = kv.find(key);
      if (it == kv.end())
        throw QLangError(QLangErrorKind::Runtime, 0, 0,
                         "table has no member '" + key + "'");
      frame.regs[i.a] = it->second;
      break;
    }
    case Opcode::TABCREATE: {
      frame.regs[i.a] = QLangValue::Table();
      trace(3, "[VM-TABCREATE] a=%d shared_ptr=%p\n", i.a, frame.regs[i.a].asTable().get());
      break;
    }
    case Opcode::TABPUT: {
      QLangValue &base = frame.regs[i.a];
      trace(3, "[VM-TABPUT ENTER] a=%d type=%d regs.size=%zu\n", i.a, (int)base.type(), frame.regs.size());
      if (base.type() != QLangValue::Type::Table) {
        trace(3, "[VM-TABPUT] NOT A TABLE! type=%d\n", (int)base.type());
        throw QLangError(QLangErrorKind::Type, 0, 0,
                         "cannot assign member of non-table value");
      }
      auto tv = base.asTable();
      if (!tv) {
        trace(3, "[VM-TABPUT] NULL shared_ptr!\n");
        tv = std::make_shared<std::map<std::string, QLangValue>>();
        base = QLangValue::Table(tv);
      }
      const std::string &key = valueToString(frame.regs[i.b]);
      trace(3, "[VM-TABPUT] base.use_count=%ld key=%s\n",
              tv.use_count(), key.c_str());
      tv->operator[](key) = frame.regs[i.c];
      trace(3, "[VM-TABPUT] after put, size=%zu\n",
              tv->size());
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
        frame.regs[dest] = callBuiltin(static_cast<Builtin>(i.b), args, m_scope, m_prog, m_deleted, i.line, i.col, this);
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
      if (cv.type() == QLangValue::Type::Generator) {
        frame.pc += 1;
        auto &gen = const_cast<GeneratorValue &>(cv.asGenerator());
        auto result = gen();
        frame.regs[i.a] = result.value_or(QLangValue());
        continue;
      } else if (cv.type() == QLangValue::Type::Function) {
        frame.pc += 1;
        pushCall(cv.asFunction().funcIndex, frame.regs, i.a, i.c);
        // Copy the closure's live-binding capture table into the new callee frame
        // so that REFLOAD/REFSTORE inside the closure address the correct enclosing
        // frame registers.
        m_frames[m_cur].captures = cv.asFunction().captures;
        continue; // m_cur now points at the new frame; skip the trailing increment
      } else {
        throw QLangError(QLangErrorKind::Type, 0, 0,
                         "attempt to call a non-function value");
      }
      break;
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
    case Opcode::MAKEGEN: {
      const QLangValue &cv = consts[i.b];
      if (cv.type() != QLangValue::Type::Function)
        throw QLangError(QLangErrorKind::Runtime, 0, 0,
                         "invalid function constant for generator");
      const Closure &spec = cv.asFunction();
      auto gen = std::make_shared<GeneratorValue>();
      // Read call arguments from frame registers (already placed by compiler).
      std::vector<QLangValue> callArgs;
      callArgs.reserve(i.c);
      for (int k = 0; k < i.c; k++)
        callArgs.push_back(frame.regs[i.a + 1 + k]);
      *gen = [this, funcIdx = spec.funcIndex, captureSpec = spec.captures, capturedArgs = std::move(callArgs)]() -> std::optional<QLangValue> {
        return callFunction(funcIdx, capturedArgs);
      };
      frame.regs[i.a] = QLangValue::Generator(gen);
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
      trace(2, "[DBG-RET] func=%d dest=%d retVal=%lld\n", frame.funcIndex, dest, (long long)toInt64(retVal));
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
    case Opcode::YIELD: {
      // Yield inside a generator called from the top-level run() loop.
      // Save the frame and return the yielded value to the caller.
      // NOTE: capture the yielded value BEFORE popping, since `frame` is a
      // reference into m_frames that becomes dangling after pop_back().
      QLangValue yielded = frame.regs.empty() ? QLangValue() : frame.regs[0];
      Frame savedFrame = m_frames[m_cur];
      savedFrame.pc = frame.pc + 1;
      m_generatorFrames[frame.funcIndex] = std::move(savedFrame);
      m_frames.pop_back();
      m_cur--;
      if (m_cur >= 0 && m_frames[m_cur].callDest >= 0)
        m_frames[m_cur].regs[m_frames[m_cur].callDest] = yielded;
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
    case Opcode::IN: {
      QLangValue result = doBinary(op, frame.regs[i.b], frame.regs[i.c]);
      frame.regs[i.a] = result;
      trace(2, "[DBG-RES] op=%d a=%d b=%d c=%d -> regs[%d]=%lld (b=%lld, c=%lld)\n", (int)op, i.a, i.b, i.c, i.a, (long long)(long long)toInt64(result), (long long)(long long)toInt64(frame.regs[i.b]), (long long)(long long)toInt64(frame.regs[i.c]));
      break;
    }
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

    if (++m_steps > m_scope.maxSteps())
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "infinite loop (step limit)");

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
    case Opcode::CAST:
      frame.regs[i.a] = strict_convert(frame.regs[i.b],
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
      int64_t idx = (long long)toInt64(frame.regs[i.c]);
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
      int64_t idx = (long long)toInt64(frame.regs[i.b]);
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
      frame.regs[i.a] = seqGet(frame.regs[i.b], (long long)toInt64(frame.regs[i.c]));
      break;
    case Opcode::TABGET: {
      QLangValue &base = frame.regs[i.b];
      if (base.type() != QLangValue::Type::Table)
        throw QLangError(QLangErrorKind::Type, 0, 0,
                         "cannot access member of non-table value");
      const std::string &key = valueToString(frame.regs[i.c]);
      const auto &kv = *base.asTable();
      auto it = kv.find(key);
      if (it == kv.end())
        throw QLangError(QLangErrorKind::Runtime, 0, 0,
                         "table has no member '" + key + "'");
      frame.regs[i.a] = it->second;
      break;
    }
    case Opcode::TABCREATE: {
      frame.regs[i.a] = QLangValue::Table();
      trace(3, "[VM-TABCREATE] a=%d shared_ptr=%p\n", i.a, frame.regs[i.a].asTable().get());
      break;
    }
    case Opcode::TABPUT: {
      QLangValue &base = frame.regs[i.a];
      trace(3, "[VM-TABPUT ENTER] a=%d type=%d regs.size=%zu\n", i.a, (int)base.type(), frame.regs.size());
      if (base.type() != QLangValue::Type::Table) {
        trace(3, "[VM-TABPUT] NOT A TABLE! type=%d\n", (int)base.type());
        throw QLangError(QLangErrorKind::Type, 0, 0,
                         "cannot assign member of non-table value");
      }
      auto tv = base.asTable();
      if (!tv) {
        trace(3, "[VM-TABPUT] NULL shared_ptr!\n");
        tv = std::make_shared<std::map<std::string, QLangValue>>();
        base = QLangValue::Table(tv);
      }
      const std::string &key = valueToString(frame.regs[i.b]);
      trace(3, "[VM-TABPUT] base.use_count=%ld key=%s\n",
              tv.use_count(), key.c_str());
      tv->operator[](key) = frame.regs[i.c];
      trace(3, "[VM-TABPUT] after put, size=%zu\n",
              tv->size());
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
        frame.regs[dest] = callBuiltin(static_cast<Builtin>(i.b), callArgs, m_scope, m_prog, m_deleted, i.line, i.col, this);
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
    case Opcode::MAKEGEN: {
      const QLangValue &cv = consts[i.b];
      if (cv.type() != QLangValue::Type::Function)
        throw QLangError(QLangErrorKind::Runtime, 0, 0,
                         "invalid function constant for generator");
      const Closure &spec = cv.asFunction();
      auto gen = std::make_shared<GeneratorValue>();
      // Read call arguments from frame registers (already placed by compiler).
      std::vector<QLangValue> callArgs;
      callArgs.reserve(i.c);
      for (int k = 0; k < i.c; k++)
        callArgs.push_back(frame.regs[i.a + 1 + k]);
      *gen = [this, funcIdx = spec.funcIndex, captureSpec = spec.captures, capturedArgs = std::move(callArgs)]() -> std::optional<QLangValue> {
        return callFunction(funcIdx, capturedArgs);
      };
      frame.regs[i.a] = QLangValue::Generator(gen);
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
    case Opcode::YIELD: {
      // Yield is only valid inside generator functions. Save the frame,
      // pop it, and return the yielded value to the caller.
      trace(3, "[DBG-YIELD-ENTER] func=%d m_cur=%d m_frames.size=%zu\n", frame.funcIndex, m_cur, m_frames.size());
      // NOTE: capture the yielded value BEFORE popping to avoid a dangling
      // reference (frame is m_frames[m_cur] and becomes invalid after pop).
      QLangValue yielded = frame.regs.empty() ? QLangValue() : frame.regs[0];
      Frame savedFrame = m_frames[m_cur];
      savedFrame.pc = frame.pc + 1;
      m_generatorFrames[frame.funcIndex] = std::move(savedFrame);
      m_frames.pop_back();
      trace(3, "[DBG-YIELD-AFTERPOP] m_cur=%d m_frames.size=%zu\n", m_cur, m_frames.size());
      m_cur--;
      if (m_cur >= 0 && m_frames[m_cur].callDest >= 0)
        m_frames[m_cur].regs[m_frames[m_cur].callDest] = yielded;
      // Verify what was saved
      auto it = m_generatorFrames.find(frame.funcIndex);
      if (it != m_generatorFrames.end()) {
        trace(3, "[DBG-YIELD] Saved frame: pc=%zu regs.size=%zu\\n",
                it->second.pc, it->second.regs.size());
        for (size_t k = 0; k < it->second.regs.size(); k++) {
          trace(3, "[DBG-YIELD]   saved reg[%zu]=type%d val=%lld\\n",
                  k, (int)it->second.regs[k].type(), (long long)toInt64(it->second.regs[k]));
        }
      }
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

  const Function &fn = m_prog.functions[funcIndex];

  // --- Generator function: create or resume a suspended frame. ---
  if (fn.isGenerator) {
    // Check for a saved frame from a previous call.
    Frame *saved = nullptr;
    Frame savedFrame;
    trace(3, "[DBG-GEN-CALL] func=%d m_generatorFrames.size=%zu\n", funcIndex, m_generatorFrames.size());
    auto sit = m_generatorFrames.find(funcIndex);
    if (sit != m_generatorFrames.end()) {
      savedFrame = sit->second;   // copy OUT of the map BEFORE erase
      m_generatorFrames.erase(sit);
      saved = &savedFrame;         // stable pointer to the local copy
    }

    // Save the caller's state.
    int savedCur = m_cur;
    size_t savedFramesSize = m_frames.size();

    // Create or restore the callee frame.
    Frame frame;
    frame.funcIndex = funcIndex;
    frame.regs.resize(static_cast<size_t>(args.size() + fn.registerCount));
    for (size_t k = 0; k < args.size(); ++k)
      frame.regs[k] = args[k];
    frame.captures = fn.captures;
    for (auto &cr : frame.captures)
      cr.frame = &m_frames[savedCur];
    frame.callDest = 0;

    if (saved) {
      trace(3, "[DBG-GEN-SAVED] func=%d saved frame: pc=%zu regs.size=%zu regs.ptr=%p\\n",
              funcIndex, saved->pc, saved->regs.size(), saved->regs.data());
      for (size_t k = 0; k < saved->regs.size(); k++) {
        trace(3, "[DBG-GEN-SAVED]   saved reg[%zu]=type%d val=%lld ptr=%p\\n",
                k, (int)saved->regs[k].type(), (long long)toInt64(saved->regs[k]),
                &saved->regs[k]);
      }
      // Resume: restore PC and all registers from the saved frame.
      frame.pc = saved->pc;
      frame.regs = saved->regs;
      frame.captures = saved->captures;
      for (auto &cr : frame.captures)
        cr.frame = &m_frames[savedCur];
      trace(3, "[DBG-GEN-RESTORE] func=%d restoring frame, pc=%zu regs.size=%zu\\n",
              funcIndex, frame.pc, frame.regs.size());
      for (size_t k = 0; k < frame.regs.size(); k++) {
        trace(3, "[DBG-GEN-RESTORE]   reg[%zu]=type%d val=%lld\\n",
                k, (int)frame.regs[k].type(), (long long)toInt64(frame.regs[k]));
      }
    } else {
      trace(3, "[DBG-GEN-NO-SAVE] func=%d no saved frame found\\n", funcIndex);
    }

    m_frames.push_back(std::move(frame));
    m_cur = static_cast<int>(m_frames.size()) - 1;
    int calleeFrameIdx = m_cur;

    // Run the VM loop for the generator.
    trace(3, "[DBG-GEN-ENTER] func=%d entering generator loop, m_cur=%d calleeFrameIdx=%d\n",
            funcIndex, m_cur, calleeFrameIdx);
    while (m_cur >= calleeFrameIdx) {
      Frame &frame = m_frames[m_cur];
      // Generator already exhausted: return nil without running the body.
      if (frame.exhausted) {
        m_frames.pop_back();
        m_cur--;
        break;
      }
      const Function &genFn = m_prog.functions[frame.funcIndex];
      const auto &instrs = genFn.instructions;
      const auto &consts = genFn.constants;

      trace(3, "[DBG-GEN-LOOP] func=%d m_cur=%d calleeFrameIdx=%d frame.pc=%zu instrs.size=%zu\n",
              funcIndex, m_cur, calleeFrameIdx, frame.pc, instrs.size());
      if (frame.pc >= instrs.size()) {
        // Fell off the end: generator exhausted.
        trace(3, "[DBG-GEN-EXIT] func=%d exhausted at pc=%zu\\n", funcIndex, frame.pc);
        m_frames.pop_back();
        m_cur--;
        break;
      }

      if (++m_steps > m_scope.maxSteps())
        throw QLangError(QLangErrorKind::Runtime, 0, 0,
                         "infinite loop (step limit)");

      const Instruction &i = instrs[frame.pc];
      Opcode op = i.op;

      trace(3, "[DBG-GEN-STEP] func=%d pc=%zu op=%d a=%d b=%d c=%d (fn instrs.size=%zu)\\n", funcIndex, frame.pc, (int)op, i.a, i.b, i.c, instrs.size());

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
      case Opcode::CAST:
        frame.regs[i.a] = strict_convert(frame.regs[i.b],
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
        int64_t idx = (long long)toInt64(frame.regs[i.c]);
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
        int64_t idx = (long long)toInt64(frame.regs[i.b]);
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
        frame.regs[i.a] = seqGet(frame.regs[i.b], (long long)toInt64(frame.regs[i.c]));
        break;
      case Opcode::TABGET: {
        QLangValue &base = frame.regs[i.b];
        if (base.type() != QLangValue::Type::Table)
          throw QLangError(QLangErrorKind::Type, 0, 0,
                           "cannot access member of non-table value");
        const std::string &key = valueToString(frame.regs[i.c]);
        const auto &kv = *base.asTable();
        auto it = kv.find(key);
        if (it == kv.end())
          throw QLangError(QLangErrorKind::Runtime, 0, 0,
                           "table has no member '" + key + "'");
        frame.regs[i.a] = it->second;
        break;
      }
      case Opcode::TABCREATE:
        frame.regs[i.a] = QLangValue::Table();
        break;
      case Opcode::TABPUT: {
        QLangValue &base = frame.regs[i.a];
        if (base.type() != QLangValue::Type::Table)
          throw QLangError(QLangErrorKind::Type, 0, 0,
                           "cannot assign member of non-table value");
        auto tv = base.asTable();
        if (!tv) {
          tv = std::make_shared<std::map<std::string, QLangValue>>();
          base = QLangValue::Table(tv);
        }
        const std::string &key = valueToString(frame.regs[i.b]);
        tv->operator[](key) = frame.regs[i.c];
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
          QLangValue result = callBuiltin(static_cast<Builtin>(i.b), callArgs, m_scope, m_prog, m_deleted, i.line, i.col, this);
          trace(3, "[DBG-CALL-BUILTIN] builtin=%d dest=%d result type=%d val=%lld\\n",
                  i.b, dest, (int)result.type(), (long long)toInt64(result));
          frame.regs[dest] = result;
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
        if (cv.type() == QLangValue::Type::Generator) {
          // Generator call: inline the resume logic.
          frame.pc += 1;
          auto &innerGen = cv.asGenerator();
          auto innerOpt = innerGen();
          frame.regs[i.a] = innerOpt.value_or(QLangValue());
        } else if (cv.type() == QLangValue::Type::Function) {
          frame.pc += 1;
          pushCall(cv.asFunction().funcIndex, frame.regs, i.a, i.c);
          m_frames[m_cur].captures = cv.asFunction().captures;
          continue;
        } else {
          throw QLangError(QLangErrorKind::Type, 0, 0,
                           "attempt to call a non-function value");
        }
        break;
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
        // Generator exhaustion: keep the saved frame marked exhausted so the
        // next resume returns nil instead of restarting the body from scratch.
        Frame exhaustedFrame = frame;
        exhaustedFrame.exhausted = true;
        m_generatorFrames[frame.funcIndex] = std::move(exhaustedFrame);
        m_frames.pop_back();
        m_cur--;
        break;
      }
      case Opcode::YIELD: {
        // Save the current frame and return the yielded value.
        // NOTE: capture the yielded value BEFORE popping to avoid a dangling
        // reference (frame is m_frames[m_cur] and becomes invalid after pop).
        QLangValue yielded = frame.regs.empty() ? QLangValue() : frame.regs[0];
        trace(3, "[DBG-YIELD] func=%d yielded type=%d val=%lld pc=%zu regs[0]=%lld\n",
                frame.funcIndex, (int)yielded.type(), (long long)toInt64(yielded), frame.pc, (long long)toInt64(yielded));
        for (size_t ri = 0; ri < frame.regs.size() && ri < 10; ri++) {
          trace(3, "[DBG-YIELD]   reg[%zu]=type%d val=%lld\n", ri, (int)frame.regs[ri].type(), (long long)toInt64(frame.regs[ri]));
        }
        Frame savedFrame = m_frames[m_cur];
        savedFrame.pc = frame.pc + 1; // resume at next instruction
        m_generatorFrames[frame.funcIndex] = std::move(savedFrame);
        // Verify what was saved
        auto savedIt = m_generatorFrames.find(frame.funcIndex);
        if (savedIt != m_generatorFrames.end()) {
          trace(3, "[DBG-YIELD-SAVE] func=%d saved in map: pc=%zu regs.size=%zu regs.ptr=%p\\n",
                  frame.funcIndex, savedIt->second.pc, savedIt->second.regs.size(),
                  savedIt->second.regs.data());
          for (size_t k = 0; k < savedIt->second.regs.size(); k++) {
            trace(3, "[DBG-YIELD-SAVE]   map reg[%zu]=type%d val=%lld ptr=%p\\n",
                    k, (int)savedIt->second.regs[k].type(), (long long)toInt64(savedIt->second.regs[k]),
                    &savedIt->second.regs[k]);
          }
        }
        m_frames.pop_back();
        m_cur--;
        trace(3, "[DBG-YIELD] returning yielded value\\n");
        // Debug: check if main frame regs alias saved frame regs
        if (m_cur >= 0 && m_cur < m_frames.size()) {
          trace(3, "[DBG-YIELD-DEBUG] main frame regs.ptr=%p\\n", m_frames[m_cur].regs.data());
          for (size_t k = 0; k < m_frames[m_cur].regs.size(); k++) {
            trace(3, "[DBG-YIELD-DEBUG]   main reg[%zu]=type%d val=%lld\\n",
                    k, (int)m_frames[m_cur].regs[k].type(), (long long)toInt64(m_frames[m_cur].regs[k]));
          }
        }
        // Check saved frame again after pop
        auto savedIt2 = m_generatorFrames.find(frame.funcIndex);
        if (savedIt2 != m_generatorFrames.end()) {
          QLangValue *r2 = (savedIt2->second.regs.size() > 2) ? &savedIt2->second.regs[2] : nullptr;
          trace(3, "[DBG-YIELD-AFTERPOP] saved reg[2]=%s val=%lld ptr=%p\n",
                  r2 ? std::to_string((int)r2->type()).c_str() : "N/A",
                  r2 ? (long long)toInt64(*r2) : -1, r2 ? (void*)&savedIt2->second.regs[2] : nullptr);
        }
        return yielded;
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

    // Generator exhausted: keep the exhausted frame in the map so subsequent
    // resumes return nil rather than restarting the body from scratch.
    trace(3, "[DBG-GEN-EXIT] func=%d exhausted, returning nil\n", funcIndex);
    return QLangValue();
  }

  // --- Normal (non-generator) function. ---
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
    const Function &fn2 = m_prog.functions[frame.funcIndex];
    const auto &instrs = fn2.instructions;
    const auto &consts = fn2.constants;

    if (frame.pc >= instrs.size()) {
      // Fell off the end: unwind with nil.
      m_frames.pop_back();
      m_cur--;
      continue;
    }

    if (++m_steps > m_scope.maxSteps())
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "infinite loop (step limit)");

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
    case Opcode::CAST:
      frame.regs[i.a] = strict_convert(frame.regs[i.b],
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
      int64_t idx = (long long)toInt64(frame.regs[i.c]);
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
      int64_t idx = (long long)toInt64(frame.regs[i.b]);
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
      frame.regs[i.a] = seqGet(frame.regs[i.b], (long long)toInt64(frame.regs[i.c]));
      break;
    case Opcode::TABGET: {
      QLangValue &base = frame.regs[i.b];
      if (base.type() != QLangValue::Type::Table)
        throw QLangError(QLangErrorKind::Type, 0, 0,
                         "cannot access member of non-table value");
      const std::string &key = valueToString(frame.regs[i.c]);
      const auto &kv = *base.asTable();
      auto it = kv.find(key);
      if (it == kv.end())
        throw QLangError(QLangErrorKind::Runtime, 0, 0,
                         "table has no member '" + key + "'");
      frame.regs[i.a] = it->second;
      break;
    }
    case Opcode::TABCREATE: {
      frame.regs[i.a] = QLangValue::Table();
      break;
    }
    case Opcode::TABPUT: {
      QLangValue &base = frame.regs[i.a];
      if (base.type() != QLangValue::Type::Table) {
        throw QLangError(QLangErrorKind::Type, 0, 0,
                         "cannot assign member of non-table value");
      }
      auto tv = base.asTable();
      if (!tv) {
        tv = std::make_shared<std::map<std::string, QLangValue>>();
        base = QLangValue::Table(tv);
      }
      const std::string &key = valueToString(frame.regs[i.b]);
      tv->operator[](key) = frame.regs[i.c];
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
        frame.regs[dest] = callBuiltin(static_cast<Builtin>(i.b), callArgs, m_scope, m_prog, m_deleted, i.line, i.col, this);
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
      if (cv.type() == QLangValue::Type::Generator) {
        frame.pc += 1;
        auto &gen = const_cast<GeneratorValue &>(cv.asGenerator());
        auto result = gen();
        frame.regs[i.a] = result.value_or(QLangValue());
      } else if (cv.type() == QLangValue::Type::Function) {
        frame.pc += 1;
        pushCall(cv.asFunction().funcIndex, frame.regs, i.a, i.c);
        m_frames[m_cur].captures = cv.asFunction().captures;
        continue;
      } else {
        throw QLangError(QLangErrorKind::Type, 0, 0,
                         "attempt to call a non-function value");
      }
      break;
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
    case Opcode::MAKEGEN: {
      const QLangValue &cv = consts[i.b];
      if (cv.type() != QLangValue::Type::Function)
        throw QLangError(QLangErrorKind::Runtime, 0, 0,
                         "invalid function constant for generator");
      const Closure &spec = cv.asFunction();
      auto gen = std::make_shared<GeneratorValue>();
      // Read call arguments from frame registers (already placed by compiler).
      std::vector<QLangValue> callArgs;
      callArgs.reserve(i.c);
      for (int k = 0; k < i.c; k++)
        callArgs.push_back(frame.regs[i.a + 1 + k]);
      *gen = [this, funcIdx = spec.funcIndex, captureSpec = spec.captures, capturedArgs = std::move(callArgs)]() -> std::optional<QLangValue> {
        return callFunction(funcIdx, capturedArgs);
      };
      frame.regs[i.a] = QLangValue::Generator(gen);
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
