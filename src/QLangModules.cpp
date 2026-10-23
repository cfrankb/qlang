#include "qlang/QLangContext.h"
#include "qlang/QLangValue.h"
#include "qlang/QLangErrors.h"

#include <algorithm>
#include <cmath>

namespace qlang {

namespace {

QLangValue mathMin(QLangContext&, const std::vector<QLangValue>& args) {
  if (args.empty()) return QLangValue::Int(0);
  int64_t v = toInt64(args[0]);
  for (size_t i = 1; i < args.size(); i++) {
    int64_t a = toInt64(args[i]);
    if (a < v) v = a;
  }
  return QLangValue::Int(static_cast<int32_t>(v));
}

QLangValue mathMax(QLangContext&, const std::vector<QLangValue>& args) {
  if (args.empty()) return QLangValue::Int(0);
  int64_t v = toInt64(args[0]);
  for (size_t i = 1; i < args.size(); i++) {
    int64_t a = toInt64(args[i]);
    if (a > v) v = a;
  }
  return QLangValue::Int(static_cast<int32_t>(v));
}

QLangValue mathSum(QLangContext&, const std::vector<QLangValue>& args) {
  int64_t s = 0;
  for (auto& a : args) s += toInt64(a);
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
