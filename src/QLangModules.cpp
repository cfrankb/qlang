#include "qlang/QLangContext.h"
#include "qlang/QLangValue.h"
#include "qlang/QLangErrors.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace qlang {

namespace {

// Return true if the value is a signed integer type (char, short, int, Qword-as-signed).
static bool isSignedInt(QLangValue::Type t) {
  return t == QLangValue::Type::Char || t == QLangValue::Type::Short ||
         t == QLangValue::Type::Int;
}

// Return true if the value is an unsigned integer type (byte, word, unsigned).
static bool isUnsignedInt(QLangValue::Type t) {
  return t == QLangValue::Type::Byte || t == QLangValue::Type::Word ||
         t == QLangValue::Type::Unsigned;
}

// Return true if the value is a Qword (unsigned 64-bit, but treat as unsigned).
static bool isQword(QLangValue::Type t) {
  return t == QLangValue::Type::Qword;
}

// Check whether a collection has mixed char (signed) and byte (unsigned).
// Spec §774: "sum return int if data is mixed signed and unsigned types (char and byte)"
// min/max §767: same rule. Other signed/unsigned combos (int+Qword) are allowed.
static void checkCharByteMix(const std::vector<QLangValue>& elems) {
  bool hasChar = false;
  bool hasByte = false;
  for (auto& e : elems) {
    if (e.type() == QLangValue::Type::Char) hasChar = true;
    else if (e.type() == QLangValue::Type::Byte) hasByte = true;
  }
  if (hasChar && hasByte)
    throw QLangError(QLangErrorKind::Type, 0, 0,
                     "min/max/sum: mixed char and byte types not allowed");
}

// Check whether min/max received mixed float and integer types.
// Throws on violation. Sum allows float+int (returns float).
static void checkMinMixFloat(const std::vector<QLangValue>& elems) {
  bool hasFloat = false;
  bool hasInt = false;
  for (auto& e : elems) {
    if (e.type() == QLangValue::Type::Float) hasFloat = true;
    else if (isSignedInt(e.type()) || isUnsignedInt(e.type()) || isQword(e.type()))
      hasInt = true;
  }
  if (hasFloat && hasInt)
    throw QLangError(QLangErrorKind::Type, 0, 0,
                     "min/max: mixed float and integer types not allowed");
}

// Flatten args into a single vector of numeric elements.
// For min/max: at most one array arg is allowed.
static std::vector<QLangValue> flattenArgs(const std::vector<QLangValue>& args,
                                            bool allowOneArray) {
  std::vector<QLangValue> elems;
  int arrayCount = 0;
  for (auto& a : args) {
    if (a.type() == QLangValue::Type::Array) {
      arrayCount++;
      if (allowOneArray && arrayCount > 1)
        throw QLangError(QLangErrorKind::Type, 0, 0,
                         "min/max: only 1 array allowed");
      // Flatten: for min/max only the first array; for sum all arrays.
      if (!allowOneArray || arrayCount == 1) {
        for (auto& e : a.asArray()) elems.push_back(e);
      }
    } else {
      elems.push_back(a);
    }
  }
  return elems;
}

QLangValue mathMin(QLangContext&, const std::vector<QLangValue>& args) {
  if (args.empty()) return QLangValue::Int(0);
  auto elems = flattenArgs(args, true);
  if (elems.empty()) return QLangValue::Int(0);
  checkCharByteMix(elems);
  checkMinMixFloat(elems);

  // Use double for comparison to handle floats correctly.
  double v = static_cast<double>(toInt64(elems[0]));
  bool hasFloat = false;
  for (size_t i = 1; i < elems.size(); i++) {
    double a = static_cast<double>(toInt64(elems[i]));
    if (elems[i].type() == QLangValue::Type::Float) hasFloat = true;
    if (a < v) v = a;
  }
  if (hasFloat) return QLangValue::Float(static_cast<double>(v));
  if (v < static_cast<double>(std::numeric_limits<int32_t>::min()) ||
      v > static_cast<double>(std::numeric_limits<int32_t>::max()))
    return QLangValue::Qword(static_cast<uint64_t>(v));
  return QLangValue::Int(static_cast<int32_t>(v));
}

QLangValue mathMax(QLangContext&, const std::vector<QLangValue>& args) {
  if (args.empty()) return QLangValue::Int(0);
  auto elems = flattenArgs(args, true);
  if (elems.empty()) return QLangValue::Int(0);
  checkCharByteMix(elems);
  checkMinMixFloat(elems);

  double v = static_cast<double>(toInt64(elems[0]));
  bool hasFloat = false;
  for (size_t i = 1; i < elems.size(); i++) {
    double a = static_cast<double>(toInt64(elems[i]));
    if (elems[i].type() == QLangValue::Type::Float) hasFloat = true;
    if (a > v) v = a;
  }
  if (hasFloat) return QLangValue::Float(static_cast<double>(v));
  if (v < static_cast<double>(std::numeric_limits<int32_t>::min()) ||
      v > static_cast<double>(std::numeric_limits<int32_t>::max()))
    return QLangValue::Qword(static_cast<uint64_t>(v));
  return QLangValue::Int(static_cast<int32_t>(v));
}

QLangValue mathSum(QLangContext&, const std::vector<QLangValue>& args) {
  auto elems = flattenArgs(args, false);  // sum allows multiple arrays
  if (elems.empty()) return QLangValue::Int(0);
  checkCharByteMix(elems);

  bool hasFloat = false;
  double s = 0.0;
  for (auto& e : elems) {
    if (e.type() == QLangValue::Type::Float) {
      hasFloat = true;
      s += e.asFloat();
    } else {
      s += static_cast<double>(toInt64(e));
    }
  }
  if (hasFloat) return QLangValue::Float(static_cast<double>(s));
  if (s < static_cast<double>(std::numeric_limits<int32_t>::min()) ||
      s > static_cast<double>(std::numeric_limits<int32_t>::max()))
    return QLangValue::Qword(static_cast<uint64_t>(s));
  return QLangValue::Int(static_cast<int32_t>(s));
}

}  // namespace

void registerBuiltinModules(QLangContext& ctx) {
  QLangContext::Module mathMod;
  mathMod.name = "math";
  mathMod.funcs["min"] = mathMin;
  mathMod.funcs["max"] = mathMax;
  mathMod.funcs["sum"] = mathSum;
  ctx.registerModule("math", mathMod);
}

}  // namespace qlang
