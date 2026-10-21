// Extended empirical spec-coverage check: gaps + changed-status items.
#include <iostream>
#include <string>
#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"
using namespace qlang;

static int passes = 0, fails = 0;

static void expectRun(const std::string &name, const std::string &src) {
  QLangContext ctx;
  try { ctx.run(src); std::cout << "[PASS] run:  " << name << "\n"; passes++; }
  catch (const QLangError &e) { std::cout << "[FAIL] run:  " << name << " -> " << e.what() << "\n"; fails++; }
}
static void expectThrow(const std::string &name, const std::string &src) {
  QLangContext ctx;
  try { ctx.run(src); std::cout << "[FAIL] throw:" << name << " (ran, expected error)\n"; fails++; }
  catch (const QLangError &e) { std::cout << "[PASS] throw:" << name << " -> " << e.what() << "\n"; passes++; }
}
static void expectVal(const std::string &name, const std::string &src,
                      const std::string &var, int expect) {
  QLangContext ctx;
  try {
    ctx.run(src);
    int got = ctx.get(var).asInt();
    if (got == expect) { std::cout << "[PASS] val:  " << name << " (" << var << "=" << got << ")\n"; passes++; }
    else { std::cout << "[FAIL] val:  " << name << " (" << var << " got " << got << " exp " << expect << ")\n"; fails++; }
  } catch (const QLangError &e) { std::cout << "[FAIL] val:  " << name << " -> " << e.what() << "\n"; fails++; }
}
static void expectStr(const std::string &name, const std::string &src,
                      const std::string &var, const std::string &expect) {
  QLangContext ctx;
  try {
    ctx.run(src);
    std::string got = ctx.get(var).asString();
    if (got == expect) { std::cout << "[PASS] str:  " << name << " (" << var << "=\"" << got << "\")\n"; passes++; }
    else { std::cout << "[FAIL] str:  " << name << " (" << var << "=\"" << got << "\" exp \"" << expect << "\")\n"; fails++; }
  } catch (const QLangError &e) { std::cout << "[FAIL] str:  " << name << " -> " << e.what() << "\n"; fails++; }
}

int main() {
  // -- const (spec 53-58, 119-120) --
  expectVal("const read", "const year = 2026;\nlocal sz = year;", "sz", 2026);
  expectThrow("const immutable", "const year = 2026;\nyear = 5");
  expectRun("const typed byte", "const month:byte = 10");

  // -- nil + float (106, 107) --
  expectRun("local z nil", "local z");
  expectStr("float toStr", "local f = 1.13\nlocal s = toStr(f);", "s", "1.13");

  // -- concat (111,114,117) --
  expectStr("sx += 123", "sx = \"test\"\nsx += 123", "sx", "test123");
  expectThrow("sv += test", "sv = 123\nsv += \"test\"");

  // -- array concat + access (88-104) --
  expectVal("array concat len", "local aa = [1,2,3]\nlocal ab = [4,5]\nlocal ac = aa + ab\nlocal n = len(ac);", "n", 5);
  expectVal("array append", "local ab = [1]\nab[] = 4\nlocal n = len(ab);", "n", 2);
  expectStr("table member set", "local t:table = {name=\"Frank\"}\nt.name = \"John\"\nlocal s = t.name;", "s", "John");

  // -- blob (16) --
  expectRun("blob()", "local b = blob()");

  // -- helpers (214-221) --
  expectVal("len", "local a = len([1,2,3]);", "a", 3);
  expectRun("type()", "local t = type(5);");
  expectThrow("substr unknown fn", "local s = substr(\"hello\", 1, 3);");

  // -- delete (222) --
  expectRun("delete var", "local a = 1\ndelete a");

  // -- postfix + while --a (205, 175) --
  expectVal("post-inc", "local a = 5\na++\na = a;", "a", 6);
  expectVal("while --a", "local a = 5\nwhile (a > 0)\n--a\nend", "a", 0);

  // -- GAP: hex, in, continue/break, for-in, modulo, not, str, def --
  expectVal("hex 0xc", "local n = 0xc\nlocal m = n;", "m", 12);
  expectThrow("in operator", "local a = [1,2]\nlocal b = 2\nif (b in a) then\nb = 3\nend");
  expectRun("continue stmt", "local n = 0\nfor x = 1 to 3\ncontinue\nnext x");
  expectRun("break stmt", "local n = 0\nfor x = 1 to 3\nbreak\nnext x");
  expectThrow("for in", "for i in [1,2,3]\nlocal s = toStr(i)\nnext i");
  expectThrow("modulo", "local a = 5 % 2\nlocal m = a;");
  expectThrow("not !", "local a = !false\nlocal m = a;");
  expectThrow("str type", "local s:str = \"x\"");
  expectThrow("def function", "def foo(a, b)\nreturn a\nend\nfoo(1,2)");

  std::cout << "\n==== " << passes << " passed, " << fails << " failed ====\n";
  return fails ? 1 : 0;
}
