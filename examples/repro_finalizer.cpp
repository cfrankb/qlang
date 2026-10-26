#include <qlang/QLangContext.h>
#include <iostream>
#include <vector>

static void tryProg(const char *name, const std::string &src) {
  try {
    qlang::QLangContext ctx;
    ctx.run(src);
    std::cout << "[OK]   " << name << "\n";
  } catch (const std::exception &e) {
    std::cout << "[ERR]  " << name << " -> " << e.what() << "\n";
  }
}

int main() {
  // A: field default nil + __init__ (no destroy method)
  tryProg("A: nil default + init",
          "local log = {n = 0}\n"
          "class D {\n  log = nil\n"
          "__init__ (self, lg) [ ] { self.log = lg }\n}\n"
          "local a = D(log)\nlocal n = a.log.n;");
  // B: same but with __destroy__ (no release)
  tryProg("B: + destroy method",
          "local log = {n = 0}\n"
          "class D {\n  log = nil\n"
          "__init__ (self, lg) [ ] { self.log = lg }\n"
          "__destroy__ (self) [ ] { local lg = self.log\nlg.n = lg.n + 1 }\n}\n"
          "local a = D(log)\nlocal n = a.log.n;");
  // C: int default + init
  tryProg("C: int default + init",
          "class R { v = 7 }\nlocal a = R()\nlocal x = a.v;");
  // D: no field default, init only
  tryProg("D: init only",
          "local log = {n = 0}\n"
          "class D {\n"
          "__init__ (self, lg) [ ] { self.log = lg }\n}\n"
          "local a = D(log)\nlocal n = a.log.n;");
  return 0;
}
