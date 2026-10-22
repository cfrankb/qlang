#include <iostream>
#include "qlang/QValue.h"
#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"
using namespace qlang;

int main() {
  QLangContext ctx;
  std::string src = "def square(x) { return x * x }\n";
  ctx.run(src);

  try {
    QLangValue r = ctx.call("square", {qval_int(7)});
    std::cout << "square(7) type=" << (int)r.type() << " val=" << r.asInt() << "\n";
  } catch (const QLangError& e) {
    std::cerr << "ERROR: " << e.what() << "\n";
  }
  return 0;
}
