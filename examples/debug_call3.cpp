#include <iostream>
#include "qlang/QValue.h"
#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"
using namespace qlang;

int main() {
  QLangContext ctx;
  std::string src = "def square(x) { return x * x }\n";
  ctx.run(src);
  std::cerr << "[DBG] run() completed, calling square(7)...\n" << std::flush;
  try {
    QLangValue r = ctx.call("square", {qval_int(7)});
    std::cout << "square(7) = " << r.asInt() << "\n";
  } catch (const QLangError& e) {
    std::cerr << "ERROR: " << e.what() << "\n";
  }
  return 0;
}
