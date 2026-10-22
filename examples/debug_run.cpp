#include <iostream>
#include "qlang/QValue.h"
#include "qlang/QLangContext.h"
using namespace qlang;
int main() {
  QLangContext ctx;
  ctx.setFunc("native_double", [](QLangContext&, const std::vector<QLangValue>& a) -> QLangValue {
    return qval_int(static_cast<int32_t>(a[0].asInt()) * 2);
  });
  std::string src = "def square(x) { return x * x }\ndef greet(name) { println("hi " + name); return "ok" }\n";
  ctx.run(src);
  std::cout << "RUN OK\n";
  return 0;
}
