#include <iostream>
#include "qlang/QValue.h"
#include "qlang/QLangContext.h"
using namespace qlang;
int main() {
  QLangContext ctx;
  ctx.run("def square(x) { return x * x }\n");
  std::cout << "run done\n";
  QLangValue r = ctx.call("square", {qval_int(7)});
  std::cout << "square(7) = " << r.asInt() << "\n";
  return 0;
}
