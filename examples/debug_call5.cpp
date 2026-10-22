#include <iostream>
#include "qlang/QValue.h"
#include "qlang/QLangContext.h"
using namespace qlang;
int main() {
  QLangContext ctx;
  ctx.run("def square(x) { return x * x }\n");
  std::cout << "RUN OK\n";
  QLangValue r = ctx.call("square", {qval_int(7)});
  std::cout << "CALL OK type=" << (int)r.type() << " val=" << r.asInt() << "\n";
  return 0;
}
