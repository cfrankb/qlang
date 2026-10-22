
#include <iostream>
#include "qlang/QLangContext.h"
using namespace qlang;
int main()
{
  QLangContext ctx;
  ctx.run("println(1+1)");
  std::cout << "OK";
  return 0;
}
