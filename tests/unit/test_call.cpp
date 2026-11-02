#include <iostream>
#include "qlang/QValue.h"
#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"
using namespace qlang;

int main() {
  QLangContext ctx;

  ctx.setFunc("native_double", [](QLangContext&, const std::vector<QLangValue>& a) -> QLangValue {
    return qval_int(static_cast<int32_t>(a[0].asInt()) * 2);
  });

  std::string src = "def square(x) { return x * x }\ndef greet(name) { return name }\n";
  ctx.run(src);

  QLangValue r1 = ctx.call("square", {qval_int(7)});
  std::cout << "square(7) = " << r1.asInt() << "\n";
  if (r1.asInt() != 49) {
    std::cerr << "FAIL: square(7) != 49\n";
    return 1;
  }

  QLangValue r2 = ctx.call("greet", {qval_str("world")});
  std::cout << "greet = " << r2.asString() << "\n";
  if (r2.asString() != "world") {
    std::cerr << "FAIL: greet != world\n";
    return 1;
  }

  QLangValue r3 = ctx.call("native_double", {qval_int(21)});
  std::cout << "native_double(21) = " << r3.asInt() << "\n";
  if (r3.asInt() != 42) {
    std::cerr << "FAIL: native_double != 42\n";
    return 1;
  }

  std::string src2 = "def add_one(x) { return x + 1 }\n";
  ctx.run(src2);
  ctx.setFunc("call_add_one", [&](QLangContext& scope, const std::vector<QLangValue>& a) -> QLangValue {
    return scope.call("add_one", a);
  });
  QLangValue r4 = ctx.call("call_add_one", {qval_int(10)});
  std::cout << "call_add_one(10) = " << r4.asInt() << "\n";
  if (r4.asInt() != 11) {
    std::cerr << "FAIL: call_add_one != 11\n";
    return 1;
  }

  std::cout << "ALL PASS\n";
  return 0;
}
