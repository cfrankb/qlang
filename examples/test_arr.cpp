#include <iostream>
#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"
using namespace qlang;

int main() {
  const char *src =
    "ar = [1,2,3]\n"
    "ar += [4,5]\n"
    "for i in ar {\n"
    "  printf(\"%d\", i)\n"
    "}\n"
    "println(\"\")\n"
    "printf(\"%d\", len(ar))\n"
    ;
  QLangContext ctx;
  try {
    ctx.run(src);
  } catch (const QLangError &e) {
    std::cerr << e.what() << std::endl;
    return 1;
  }
  return 0;
}
