#include <iostream>
#include "qlang/QValue.h"
#include "qlang/QLangContext.h"
using namespace qlang;
int main()
{
  QLangContext ctx;
  ctx.run("println(\"hello\")\n");
  std::cout << "done\n";
  return 0;
}
