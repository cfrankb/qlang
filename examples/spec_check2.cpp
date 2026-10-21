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
    if (got == expect) { std::cout << "[PASS] str:  " << name << " (" << var << "=\""<< got << "\")\n"; passes++; }
    else { std::cout << "[FAIL] str:  " << name << " (" << var << "=\""<< got << "\" exp \""<< expect << "\")\n"; fails++; }
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
  expectVal("len array", "local a = len([1,2,3]);", "a", 3);
  expectVal("len string", "local s = len(\"hello\");", "s", 5);
  expectVal("len table", "local t = {a=1,b=2,c=3}\nlocal n = len(t);", "n", 3);
  expectRun("type()", "local t = type(5);");
  expectStr("substr", "local s = substr(\"hello\", 1, 3);", "s", "ell");

  // -- delete (222) --
  expectRun("delete var", "local a = 1\ndelete a");

  // -- postfix + while --a (205, 175) --
  expectVal("post-inc", "local a = 5\na++\na = a;", "a", 6);
  expectVal("while --a", "local a = 5\nwhile (a > 0)\n--a\nend", "a", 0);

  // -- hex, in, continue/break, for-in, modulo, not, str type, def --
  expectVal("hex 0xc", "local n = 0xc\nlocal m = n;", "m", 12);
  expectVal("in operator", "local a = [1,2]\nlocal b = 2\nif (b in a) then\nb = 3\nend\nlocal r = b;", "r", 3);
  expectRun("continue stmt", "local n = 0\nfor x = 1 to 3\ncontinue\nnext x");
  expectRun("break stmt", "local n = 0\nfor x = 1 to 3\nbreak\nnext x");
  expectVal("for in", "local total = 0\nfor i in [1,2,3]\ntotal = total + i\nnext i\nlocal r = total;", "r", 6);
  expectVal("modulo", "local a = 5 % 2\nlocal m = a;", "m", 1);
  expectVal("not !", "local a = !false\nlocal m = a;", "m", 1);
  expectStr("str type", "local s:str = \"x\"", "s", "x");
  expectVal("def function", "def foo(a, b)\nreturn a + b\nend\nlocal r = foo(2,3);", "r", 5);
  expectVal("def brace form", "def bar(a, b)\nreturn a * b\nend\nlocal r = bar(4,5);", "r", 20);

  // -- NEW: str() / int() aliases (spec 236-237) --
  expectStr("str alias", "local n = 42\nlocal s = str(n);", "s", "42");
  expectVal("int alias", "local s = \"99\"\nlocal n = int(s);", "n", 99);
  expectVal("int alias float", "local s = \"3.14\"\nlocal n = int(s);", "n", 3);
  expectVal("int alias nil", "local z\nlocal n = int(z);", "n", 0);

  // -- NEW: brace-form loops (spec 174-187) --
  expectVal("for-brace", "local total = 0\nfor x = 1 to 3 {\n  total = total + x\n}\nlocal r = total;", "r", 6);
  expectVal("while-brace", "local a = 3\nwhile (a > 0) {\n  a = a - 1\n}\nlocal r = a;", "r", 0);
  expectVal("for-brace continue", "local total = 0\nfor x = 1 to 5 {\n  if x == 3 { continue }\n  total = total + x\n}\nlocal r = total;", "r", 12);
  expectVal("for-brace break", "local total = 0\nfor x = 1 to 5 {\n  if x == 3 { break }\n  total = total + x\n}\nlocal r = total;", "r", 3);
  expectVal("while-brace break", "local a = 5\nwhile (a > 0) {\n  if a == 3 { break }\n  a = a - 1\n}\nlocal r = a;", "r", 3);

  // -- NEW: closure live-binding (spec 264-285) --
  expectVal("closure live-bind read", "local x = 1\nlocal f = (y)[x] { return x + y }\nx = 10\nlocal r = f(20);\n", "r", 30);
  expectVal("closure live-bind write", "local x = 1\nlocal f = (y)[x] { x = x + y; return x }\nx = 10\nlocal r = f(5);\n", "r", 15);
  expectVal("closure live-bind outer change", "local x = 1\nlocal f = (y)[x] { return x + y }\nx = 100\nlocal r = f(1);\n", "r", 101);
  expectVal("closure no capture", "local f = (a, b)[] { return a + b }\nlocal r = f(7, 8);\n", "r", 15);

  // -- NEW: ** exponential (spec 223) --
  expectVal("pow int", "local r = 2 ** 8;", "r", 256);
  expectVal("pow float", "local r = 2 ** 3;", "r", 8);
  expectVal("pow zero exp", "local r = 5 ** 0;", "r", 1);
  expectVal("pow neg exp int", "local r = 2 ** -1;", "r", 0);
  expectVal("pow nested", "local r = 2 ** 2 ** 3;", "r", 256);

  // -- NEW: else / else if (spec 139-161) --
  expectVal("else if true", "local x = 2\nif x == 1 then\nx = 1\nelse if x == 2 then\nx = 22\nelse\nx = 99\nend\nlocal r = x;", "r", 22);
  expectVal("else else-if false", "local x = 3\nif x == 1 then\nx = 1\nelse if x == 2 then\nx = 22\nelse\nx = 99\nend\nlocal r = x;", "r", 99);
  expectVal("else if with braces", "local x = 2\nif x == 1 {\n  x = 1\n} else if x == 2 {\n  x = 22\n} else {\n  x = 99\n}\nlocal r = x;", "r", 22);
  expectVal("else with braces", "local x = 3\nif x == 1 {\n  x = 1\n} else {\n  x = 99\n}\nlocal r = x;", "r", 99);

  // -- NEW: len on blob --
  expectVal("len blob", "local b = blob()\nlocal n = len(b);", "n", 0);

  // -- NEW: slice() builtin (spec §222) --
  expectVal("slice basic", "local a = [10,20,30,40,50]\nlocal s = slice(a, 1, 3)\nlocal n = len(s);", "n", 3);
  expectVal("slice from zero", "local a = [1,2,3,4,5]\nlocal s = slice(a, 0, 2)\nlocal v = s[0];", "v", 1);
  expectVal("slice tail", "local a = [1,2,3,4,5]\nlocal s = slice(a, 3)\nlocal n = len(s);", "n", 2);
  expectVal("slice empty", "local a = [1,2,3]\nlocal s = slice(a, 5, 2)\nlocal n = len(s);", "n", 0);

  // -- NEW: range() builtin (spec §201–204) --
  expectVal("range 1-5 len", "local r = range(1,5)\nlocal n = len(r);", "n", 5);
  expectVal("range 1-5 first", "local r = range(1,5)\nlocal v = r[0];", "v", 1);
  expectVal("range 1-5 last", "local r = range(1,5)\nlocal v = r[4];", "v", 5);
  expectVal("range empty", "local r = range(5,1)\nlocal n = len(r);", "n", 0);
  expectVal("range in for", "local total = 0\nfor i in range(1,4)\ntotal = total + i\nnext i\nlocal r = total;", "r", 10);
  expectVal("in range", "local a = 0\nif (3 in range(1,5))\na = 1\nend\nlocal r = a;", "r", 1);

  // -- NEW: goto / labels (spec §207–222) --
  expectVal("goto basic", "local a = 10\n:label1\n--a\nif a > 1 goto label1\nlocal r = a;", "r", 1);
  expectVal("goto same scope", "local x = 0\n:inc\nx = x + 1\nif x < 5 goto inc\nlocal r = x;", "r", 5);
  expectThrow("goto undefined label", "goto nonexistent");

  // -- NEW: complex expressions --
  expectVal("complex expr", "local r = (2 + 3) * (4 - 1);", "r", 15);
  expectVal("bitwise mix", "local r = (5 & 3) | 4;", "r", 5);
  expectVal("shift mix", "local r = (1 << 3) + (8 >> 1);", "r", 12);
  expectVal("compound add", "local a = 10\na += 5\nlocal r = a;", "r", 15);
  expectVal("compound mul", "local a = 10\na *= 3\nlocal r = a;", "r", 30);

  // -- NEW: nested functions --
  expectVal("nested fn", "def outer(a)\n  def inner(b)\n    return a + b\n  end\n  return inner(10)\nend\nlocal r = outer(5);", "r", 15);

  // -- NEW: let alias for local (spec §55) --
  expectVal("let basic", "let n = 42\nlocal r = n;", "r", 42);
  expectStr("let typed", "let s:str = \"hello\"\nlocal g = s;", "g", "hello");
  expectRun("let nil", "let z");
  expectVal("let compound", "let a = 10\na += 5\nlocal r = a;", "r", 15);

  // -- NEW: for-in over string (spec §210-212) --
  expectVal("for-in string", "local total = 0\nfor c in \"abc\"\ntotal = total + c\nnext c\nlocal r = total;", "r", 294);

  // -- NEW: array in table --
  expectVal("array in table", "local t = {data=[1,2,3]}\nlocal r = t.data[1];", "r", 2);

  // -- NEW: closure in table literal + method call (spec §292–297) --
  expectRun("closure in table", "local events:table = { onLoad = (name)[] { printf(\"onLOAD -->%s\\n\", name) } }\nevents.onLoad(\"test\")");
  expectStr("closure in table return", "local events:table = { greet = (who)[] { return \"hi \" + who } }\nlocal r = events.greet(\"world\");", "r", "hi world");
  expectVal("method call via var", "local f = (x)[] { return x * 2 }\nlocal r = f(21);", "r", 42);
  expectVal("member call no args", "local t:table = { val = 99 }\nlocal r = t.val;", "r", 99);

  // -- NEW: bool type (spec §18-28) --
  expectVal("bool typed true", "local b:bool = true\nlocal r = b;", "r", 1);
  expectVal("bool typed false", "local b:bool = false\nlocal r = b;", "r", 0);
  expectStr("bool type returns bool", "local b:bool = true\nlocal s = type(b);", "s", "bool");
  expectVal("bool in comparison", "local b:bool = true\nlocal r = (b == 1);", "r", 1);
  expectVal("bool arithmetic", "local b:bool = true\nlocal r = b + 1;", "r", 2);
  expectVal("bool in if", "local b:bool = false\nlocal r = 0\nif !b then r = 1 end\nlocal result = r;", "result", 1);
  expectRun("bool const", "const flag:bool = true");

  // -- NEW: range/slice reserved-word status --
  expectThrow("range as ident", "local range = 1");
  expectThrow("slice as ident", "local slice = 1");

  std::cout << "\n==== " << passes << " passed, " << fails << " failed ====\n";
  return fails ? 1 : 0;
}
