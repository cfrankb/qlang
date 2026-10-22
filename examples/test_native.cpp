// Test native C++ function injection.
#include <iostream>
#include "qlang/QValue.h"
#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"
using namespace qlang;

int main() {
  QLangContext ctx;

  // Register native functions
  ctx.setFunc("double", [](QLangContext&, const std::vector<QLangValue>& a) -> QLangValue {
    return QLangValue::Int(static_cast<int32_t>(static_cast<int32_t>(a[0].asInt()) * 2));
  });

  ctx.setFunc("add", [](QLangContext&, const std::vector<QLangValue>& a) -> QLangValue {
    return QLangValue::Int(static_cast<int32_t>(static_cast<int32_t>(a[0].asInt()) + static_cast<int32_t>(a[1].asInt())));
  });

  ctx.setFunc("greet", [](QLangContext& scope, const std::vector<QLangValue>& a) -> QLangValue {
    std::cout << "hello " << a[0].asString() << "\n";
    return QLangValue::String("done");
  });

  // Test: call native func, read result
  std::string src = R"(
r = double(21)
println(r)
s = add(3, 4)
println(s)
t = greet("world")
println(t)
)";
  try {
    ctx.run(src);
  } catch (const QLangError& e) {
    std::cerr << "FAIL: " << e.what() << "\n";
    return 1;
  }

  // Verify scope state after run
  if (ctx.get("r").asInt() != 42) {
    std::cerr << "FAIL: r should be 42, got " << ctx.get("r").asInt() << "\n";
    return 1;
  }
  if (ctx.get("s").asInt() != 7) {
    std::cerr << "FAIL: s should be 7, got " << ctx.get("s").asInt() << "\n";
    return 1;
  }
  if (ctx.get("t").asString() != "done") {
    std::cerr << "FAIL: t should be 'done', got " << ctx.get("t").asString() << "\n";
    return 1;
  }

  std::cout << "ALL PASS\n";
  return 0;
}
