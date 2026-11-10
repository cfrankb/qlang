// Module system smoke test.
#include <iostream>
#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"
using namespace qlang;

static int64_t toInt64_(const QLangValue &v) {
  switch (v.type()) {
  case QLangValue::Type::Bool:    return static_cast<int64_t>(v.asInt());
  case QLangValue::Type::Char:    return static_cast<int64_t>(v.asChar());
  case QLangValue::Type::Byte:    return static_cast<int64_t>(v.asByte());
  case QLangValue::Type::Word:    return static_cast<int64_t>(v.asWord());
  case QLangValue::Type::Short:   return static_cast<int64_t>(v.asShort());
  case QLangValue::Type::Int:     return static_cast<int64_t>(v.asInt());
  case QLangValue::Type::Unsigned:return static_cast<int64_t>(v.asUnsigned());
  case QLangValue::Type::Qword:   return static_cast<int64_t>(v.asQword());
  case QLangValue::Type::Long:    return v.asLong();
  case QLangValue::Type::Float:   return static_cast<int64_t>(v.asFloat());
  default: return 0;
  }
}

int main() {
  QLangContext ctx;

  // Register a math module with min, max, sum.
  QLangContext::Module mathMod;
  mathMod.name = "math";
  mathMod.funcs["min"] = [](QLangContext&, const std::vector<QLangValue>& args) -> QLangValue {
    if (args.empty()) return QLangValue::Int(0);
    int64_t v = toInt64_(args[0]);
    for (size_t i = 1; i < args.size(); i++) {
      int64_t a = toInt64_(args[i]);
      if (a < v) v = a;
    }
    return QLangValue::Int(static_cast<int32_t>(v));
  };
  mathMod.funcs["max"] = [](QLangContext&, const std::vector<QLangValue>& args) -> QLangValue {
    if (args.empty()) return QLangValue::Int(0);
    int64_t v = toInt64_(args[0]);
    for (size_t i = 1; i < args.size(); i++) {
      int64_t a = toInt64_(args[i]);
      if (a > v) v = a;
    }
    return QLangValue::Int(static_cast<int32_t>(v));
  };
  mathMod.funcs["sum"] = [](QLangContext&, const std::vector<QLangValue>& args) -> QLangValue {
    int64_t s = 0;
    for (auto& a : args) s += toInt64_(a);
    return QLangValue::Int(static_cast<int32_t>(s));
  };
  ctx.registerModule("math", mathMod);

  // Test 1: `import math` binds the MODULE — every entity is reached through
  // the qualifier and the bare names stay invisible (SPEC §modules "import
  // statement"; the flat forms of tests 2, 5 and 6 are what make a bare name
  // visible).
  std::cout << "Test 1 (import binds the module): ";
  try {
    ctx.run(R"(
      import math
      local a = math.min(3, 1, 4, 1, 5)
      local b = math.max(3, 1, 4, 1, 5)
      local c = math.sum(1, 2, 3)
    )");
    if (ctx.get("a").asInt() != 1 || ctx.get("b").asInt() != 5 ||
        ctx.get("c").asInt() != 6) {
      std::cout << "FAIL (wrong values)\n";
      return 1;
    }
  } catch (const QLangError& e) {
    std::cout << "FAIL: " << e.what() << "\n";
    return 1;
  }
  try {
    ctx.run("import math\nlocal a = min(3, 1, 4)");
    std::cout << "FAIL (bare 'min' is visible)\n";
    return 1;
  } catch (const QLangError& e) {
    if (std::string(e.what()).find("unknown function 'min'") == std::string::npos) {
      std::cout << "FAIL (wrong error: " << e.what() << ")\n";
      return 1;
    }
  }
  std::cout << "OK\n";

  // Test 2: import single function
  ctx.clear();
  QLangContext::Module mathMod2;
  mathMod2.name = "math";
  mathMod2.funcs["min"] = [](QLangContext&, const std::vector<QLangValue>&) -> QLangValue {
    return QLangValue::Int(0);
  };
  ctx.registerModule("math", mathMod2);
  ctx.run(R"(
    import math.min
    local a = min(10, 20, 5)
  )");
  std::cout << "Test 2 (import single): ";
  try {
    std::cout << "OK\n";
  } catch (...) {
    std::cout << "FAIL\n";
    return 1;
  }

  // Test 3: undefined module error
  ctx.clear();
  std::cout << "Test 3 (undefined module): ";
  try {
    ctx.run("import nonexistent");
    std::cout << "FAIL (no error)\n";
    return 1;
  } catch (const QLangError& e) {
    std::string msg = e.what();
    if (msg.find("undefined module") != std::string::npos) {
      std::cout << "OK\n";
    } else {
      std::cout << "FAIL (wrong error: " << msg << ")\n";
      return 1;
    }
  }

  // Test 4: undefined function in module
  ctx.clear();
  QLangContext::Module mathMod3;
  mathMod3.name = "math";
  mathMod3.funcs["min"] = [](QLangContext&, const std::vector<QLangValue>&) -> QLangValue {
    return QLangValue::Int(0);
  };
  ctx.registerModule("math", mathMod3);
  std::cout << "Test 4 (undefined func in module): ";
  try {
    ctx.run("import math.max");
    std::cout << "FAIL (no error)\n";
    return 1;
  } catch (const QLangError& e) {
    std::string msg = e.what();
    if (msg.find("undefined function") != std::string::npos) {
      std::cout << "OK\n";
    } else {
      std::cout << "FAIL (wrong error: " << msg << ")\n";
      return 1;
    }
  }

  // Test 5: from math import *
  ctx.clear();
  QLangContext::Module mathMod4;
  mathMod4.name = "math";
  mathMod4.funcs["min"] = [](QLangContext&, const std::vector<QLangValue>& args) -> QLangValue {
    if (args.empty()) return QLangValue::Int(0);
    int64_t v = toInt64_(args[0]);
    for (size_t i = 1; i < args.size(); i++) {
      int64_t a = toInt64_(args[i]);
      if (a < v) v = a;
    }
    return QLangValue::Int(static_cast<int32_t>(v));
  };
  mathMod4.funcs["max"] = [](QLangContext&, const std::vector<QLangValue>& args) -> QLangValue {
    if (args.empty()) return QLangValue::Int(0);
    int64_t v = toInt64_(args[0]);
    for (size_t i = 1; i < args.size(); i++) {
      int64_t a = toInt64_(args[i]);
      if (a > v) v = a;
    }
    return QLangValue::Int(static_cast<int32_t>(v));
  };
  ctx.registerModule("math", mathMod4);
  std::cout << "Test 5 (from import *): ";
  try {
    ctx.run(R"(
      from math import *
      local a = min(3, 1, 4)
      local b = max(3, 1, 4)
    )");
    std::cout << "OK\n";
  } catch (const QLangError& e) {
    std::cout << "FAIL: " << e.what() << "\n";
    return 1;
  } catch (...) {
    std::cout << "FAIL (unknown)\n";
    return 1;
  }

  // Test 6: from math import min, max
  ctx.clear();
  QLangContext::Module mathMod5;
  mathMod5.name = "math";
  mathMod5.funcs["min"] = [](QLangContext&, const std::vector<QLangValue>& args) -> QLangValue {
    if (args.empty()) return QLangValue::Int(0);
    int64_t v = toInt64_(args[0]);
    for (size_t i = 1; i < args.size(); i++) {
      int64_t a = toInt64_(args[i]);
      if (a < v) v = a;
    }
    return QLangValue::Int(static_cast<int32_t>(v));
  };
  mathMod5.funcs["max"] = [](QLangContext&, const std::vector<QLangValue>& args) -> QLangValue {
    if (args.empty()) return QLangValue::Int(0);
    int64_t v = toInt64_(args[0]);
    for (size_t i = 1; i < args.size(); i++) {
      int64_t a = toInt64_(args[i]);
      if (a > v) v = a;
    }
    return QLangValue::Int(static_cast<int32_t>(v));
  };
  ctx.registerModule("math", mathMod5);
  std::cout << "Test 6 (from import specific): ";
  try {
    ctx.run(R"(
      from math import min, max
      local a = min(10, 5, 20)
      local b = max(10, 5, 20)
    )");
    std::cout << "OK\n";
  } catch (const QLangError& e) {
    std::cout << "FAIL: " << e.what() << "\n";
    return 1;
  } catch (...) {
    std::cout << "FAIL (unknown)\n";
    return 1;
  }

  // Test 7: import math as m
  ctx.clear();
  QLangContext::Module mathMod6;
  mathMod6.name = "math";
  mathMod6.funcs["min"] = [](QLangContext&, const std::vector<QLangValue>& args) -> QLangValue {
    if (args.empty()) return QLangValue::Int(0);
    int64_t v = toInt64_(args[0]);
    for (size_t i = 1; i < args.size(); i++) {
      int64_t a = toInt64_(args[i]);
      if (a < v) v = a;
    }
    return QLangValue::Int(static_cast<int32_t>(v));
  };
  mathMod6.funcs["max"] = [](QLangContext&, const std::vector<QLangValue>& args) -> QLangValue {
    if (args.empty()) return QLangValue::Int(0);
    int64_t v = toInt64_(args[0]);
    for (size_t i = 1; i < args.size(); i++) {
      int64_t a = toInt64_(args[i]);
      if (a > v) v = a;
    }
    return QLangValue::Int(static_cast<int32_t>(v));
  };
  ctx.registerModule("math", mathMod6);
  std::cout << "Test 7 (import with alias): ";
  try {
    ctx.run(R"(
      import math as m
      local a = m.min(7, 3, 9)
      local b = m.max(7, 3, 9)
    )");
    if (ctx.get("a").asInt() != 3 || ctx.get("b").asInt() != 9) {
      std::cout << "FAIL (wrong values)\n";
      return 1;
    }
    // The alias REPLACES the module name: neither `math.f` nor a bare `f` is a
    // way in (SPEC §modules "import statement").
    bool refused = false;
    try {
      ctx.run("import math as m\nlocal a = min(7, 3, 9)");
    } catch (const QLangError&) {
      refused = true;
    }
    if (!refused) {
      std::cout << "FAIL (bare 'min' is visible)\n";
      return 1;
    }
    refused = false;
    try {
      ctx.run("import math as m\nlocal a = math.min(7, 3, 9)");
    } catch (const QLangError&) {
      refused = true;
    }
    if (!refused) {
      std::cout << "FAIL (the alias did not replace the module name)\n";
      return 1;
    }
    std::cout << "OK\n";
  } catch (const QLangError& e) {
    std::cout << "FAIL: " << e.what() << "\n";
    return 1;
  } catch (...) {
    std::cout << "FAIL (unknown)\n";
    return 1;
  }

  std::cout << "\nAll module tests passed.\n";
  return 0;
}
