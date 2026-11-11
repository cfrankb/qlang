// Extended empirical spec-coverage check: gaps + changed-status items.
#include <initializer_list>
#include <iostream>
#include <string>
#include "qlang/QLangCompiler.h"
#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"
#include "qlang/QLangLexer.h"
#include "qlang/QLangObjects.h"
#include "qlang/QLangParser.h"
#include "qlang/QLangVM.h"
using namespace qlang;

static int passes = 0, fails = 0;

static void expectRunImpl(const std::string &name, const std::string &src, int line) {
  QLangContext ctx;
  try { ctx.run(src); std::cout << "[PASS] run:  [L" << line << "] " << name << "\n"; passes++; }
  catch (const QLangError &e) { std::cout << "[FAIL] run:  [L" << line << "] " << name << " -> " << e.what() << "\n"; fails++; }
}
static void expectThrowImpl(const std::string &name, const std::string &src, int line) {
  QLangContext ctx;
  try { ctx.run(src); std::cout << "[FAIL] throw:[L" << line << "] " << name << " (ran, expected error)\n"; fails++; }
  catch (const QLangError &e) { std::cout << "[PASS] throw:[L" << line << "] " << name << " -> " << e.what() << "\n"; passes++; }
}
static void expectValImpl(const std::string &name, const std::string &src,
                          const std::string &var, int expect, int line) {
  QLangContext ctx;
  try {
    ctx.run(src);
    int got = toInt64(ctx.get(var));
    if (got == expect) { std::cout << "[PASS] val:  [L" << line << "] " << name << " (" << var << "=" << got << ")\n"; passes++; }
    else { std::cout << "[FAIL] val:  [L" << line << "] " << name << " (" << var << " got " << got << " exp " << expect << ")\n"; fails++; }
  } catch (const QLangError &e) { std::cout << "[FAIL] val:  [L" << line << "] " << name << " -> " << e.what() << "\n"; fails++; }
}
// Like expectVal, but drains pending instance finalizers (Phase 3) between
// the run and the read: a `__destroy__` finalizer is enqueued at final
// release and runs at the next safe point (processFinalizers), not from
// the shared_ptr deleter.
static void expectValDrainImpl(const std::string &name, const std::string &src,
                               const std::string &var, int expect, int line) {
  QLangContext ctx;
  try {
    ctx.run(src);
    ctx.processFinalizers();
    int got = toInt64(ctx.get(var));
    if (got == expect) { std::cout << "[PASS] valD: [L" << line << "] " << name << " (" << var << "=" << got << ")\n"; passes++; }
    else { std::cout << "[FAIL] valD: [L" << line << "] " << name << " (" << var << " got " << got << " exp " << expect << ")\n"; fails++; }
  } catch (const QLangError &e) { std::cout << "[FAIL] valD: [L" << line << "] " << name << " -> " << e.what() << "\n"; fails++; }
}
static void expectStrImpl(const std::string &name, const std::string &src,
                          const std::string &var, const std::string &expect, int line) {
  QLangContext ctx;
  try {
    ctx.run(src);
    std::string got = ctx.get(var).asString();
    if (got == expect) { std::cout << "[PASS] str:  [L" << line << "] " << name << " (" << var << "=\""<< got << "\")\n"; passes++; }
    else { std::cout << "[FAIL] str:  [L" << line << "] " << name << " (" << var << "=\""<< got << "\" exp \""<< expect << ")\n"; fails++; }
  } catch (const QLangError &e) { std::cout << "[FAIL] str:  [L" << line << "] " << name << " -> " << e.what() << "\n"; fails++; }
}
// Assert that running `src` throws a QLangError whose what() contains every
// given substring (SPEC # error reporting: line numbers, function names,
// module, frame chain).
static void expectMsgImpl(const std::string &name, const std::string &src,
                          std::initializer_list<const char *> substrs, int line) {
  QLangContext ctx;
  std::string msg;
  try { ctx.run(src); }
  catch (const QLangError &e) { msg = e.what(); }
  if (msg.empty()) {
    std::cout << "[FAIL] msg:  [L" << line << "] " << name << " (no error)\n";
    fails++;
    return;
  }
  for (const char *s : substrs)
    if (msg.find(s) == std::string::npos) {
      std::cout << "[FAIL] msg:  [L" << line << "] " << name << " missing '" << s
                << "' in: " << msg << "\n";
      fails++;
      return;
    }
  std::cout << "[PASS] msg:  [L" << line << "] " << name << "\n";
  passes++;
}
// Macros so __LINE__ expands at the CALL site (a default arg would expand at the definition).
#define expectRun(name, src)        expectRunImpl(name, src, __LINE__)
#define expectThrow(name, src)      expectThrowImpl(name, src, __LINE__)
#define expectVal(name, src, var, expect)  expectValImpl(name, src, var, expect, __LINE__)
#define expectValDrain(name, src, var, expect)  expectValDrainImpl(name, src, var, expect, __LINE__)
#define expectStr(name, src, var, expect)  expectStrImpl(name, src, var, expect, __LINE__)
#define expectMsg(name, src, ...)   expectMsgImpl(name, src, {__VA_ARGS__}, __LINE__)
// Assert a C++-level condition (handle identity / payload independence).
#define expectTrue(name, cond)                                             \
  do {                                                                     \
    if (cond) {                                                            \
      std::cout << "[PASS] id:   [L" << __LINE__ << "] " << name << "\n";  \
      passes++;                                                            \
    } else {                                                               \
      std::cout << "[FAIL] id:   [L" << __LINE__ << "] " << name << "\n";  \
      fails++;                                                             \
    }                                                                      \
  } while (0)

int main() {
  // -- const (spec 53-58, 119-120) --
  expectVal("const read", "const year = 2026;\nlocal sz = year;", "sz", 2026);
  expectThrow("const immutable", "const year = 2026;\nyear = 5");
  expectRun("const typed byte", "const month:byte = 10");

  // -- nil + float (106, 107) --
  expectRun("local z valueless", "local z");
  // valueless local is implicit int, default 0 (SPEC §declarations)
  expectVal("valueless local -> 0", "local z\nlocal s = z;", "s", 0);
  expectStr("valueless local type int", "local z\nlocal s = type(z);", "s", "int");
  expectVal("valueless typed local -> 0", "local b:byte\nlocal s = b;", "s", 0);
  expectStr("float toStr", "local f = 1.13\nlocal s = toStr(f);", "s", "1.13");

  // -- concat (111,114,117) --
  expectStr("sx += 123", "sx = \"test\"\nsx += 123", "sx", "test123");
  expectThrow("sv += test", "sv = 123\nsv += \"test\"");

  // -- array concat + access (88-104) --
  expectVal("array concat len", "local aa = [1,2,3]\nlocal ab = [4,5]\nlocal ac = aa + ab\nlocal n = len(ac);", "n", 5);
  expectVal("array append", "local ab = [1]\nab[] = 4\nlocal n = len(ab);", "n", 2);
  expectVal("array += concat", "local ar = [1,2,3]\nar += [4,5]\nlocal n = len(ar);",
            "n", 5);
  expectStr("table member set", "local t:table = {name=\"Frank\"}\nt.name = \"John\"\nlocal s = t.name;", "s", "John");

  // -- blob (16) --
  expectRun("blob()", "local b = blob()");

  // -- helpers (214-221) --
  expectVal("len array", "local a = len([1,2,3]);", "a", 3);
  expectVal("len string", "local s = len(\"hello\");", "s", 5);
  expectVal("len table", "local t = {a=1,b=2,c=3}\nlocal n = len(t);", "n", 3);
  // -- NEW: len() storage byte size for numbers; nil is an error (spec errata) --
  expectVal("len int -> 4", "local n = len(123);", "n", 4);
  expectVal("len unsigned -> 4", "local u:unsigned = 3\nlocal n = len(u);", "n", 4);
  expectVal("len qword -> 8", "local d:qword = 5\nlocal n = len(d);", "n", 8);
  expectVal("len float -> 8", "local n = len(1.5);", "n", 8);
  expectVal("len byte -> 1", "local b:byte = 3\nlocal n = len(b);", "n", 1);
  expectVal("len word -> 2", "local w:word = 3\nlocal n = len(w);", "n", 2);
  expectVal("len bool -> 1", "local n = len(true);", "n", 1);
  expectVal("len class instance -> 1", "class C { x = 1 }\nlocal c = C()\nlocal n = len(c);", "n", 1);
  expectThrow("len nil -> error", "local n = len(nil);");
  // -- NEW: delete guards (spec: external variable/struct or nil is an error) --
  expectThrow("delete nil -> error", "local x = 1\ndelete nil;");
  expectRun("delete local -> ok", "local x = 1\ndelete x;");
  expectRun("type()", "local t = type(5);");
  expectStr("substr", "local s = substr(\"hello\", 1, 3);", "s", "ell");

  // -- delete (222) --
  expectRun("delete var", "local a = 1\ndelete a");

  // -- postfix + while --a (205, 175) --
  expectVal("post-inc", "local a = 5\na++\na = a;", "a", 6);
  expectVal("while --a", "local a = 5\nwhile (a > 0)\n--a\nend", "a", 0);

  // -- hex, in, continue/break, for-in, modulo, not, str type, def --
  expectVal("hex 0xc", "local n = 0xc\nlocal m = n;", "m", 12);
  // -- numeric literal suffixes: 1.0f (float), 44u (unsigned) --
  expectStr("float suffix type", "local my_float = 1.0f\nlocal s = type(my_float);", "s", "float");
  expectStr("uint suffix type", "local my_uint = 44u\nlocal s = type(my_uint);", "s", "unsigned");
  expectVal("uint suffix value", "local my_uint = 44u\nlocal m = my_uint;", "m", 44);
  expectVal("float suffix arithmetic", "local a = 1.0f\nlocal b = a + 1\nlocal r = b;", "r", 2);
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
  expectVal("nested fn", "def outer(a)\n  local inner = (b)[a] { return a + b }\n  return inner(10)\nend\nlocal r = outer(5);", "r", 15);

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
  expectRun("closure in table", "local events:table = { onLoad = (name)[] { printf(\"onLOAD -->%s\n\", name) } }\nevents.onLoad(\"test\")");
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

  // -- NEW: slice as builtin (not keyword) — can be used as identifier --
  expectVal("slice as ident", "local slice = 42\nlocal r = slice;", "r", 42);
  expectRun("slice as builtin call", "local a = [1,2,3]\nlocal b = slice(a, 0, 1)\nlocal n = len(b);");

  // -- NEW: char type (spec §18) --
  expectVal("char typed -50", "local c:char = -50\nlocal r = c;", "r", -50);
  expectStr("char type returns char", "local c:char = -1\nlocal s = type(c);", "s", "char");
  expectVal("char max", "local c:char = 127\nlocal r = c;", "r", 127);
  expectVal("char min", "local c:char = -128\nlocal r = c;", "r", -128);
  expectVal("char arithmetic", "local a:char = 10\nlocal b:char = 20\nlocal r = a + b;", "r", 30);
  expectRun("char const", "const ch:char = 42");
  // char and byte have different type strings
  expectVal("char != byte type", "local b:byte = 42\nlocal c:char = 42\nlocal r = (type(b) == \"byte\");", "r", 1);
  expectVal("char != byte type2", "local b:byte = 42\nlocal c:char = 42\nlocal r = (type(c) == \"char\");", "r", 1);

  // -- NEW: qword type (spec §24; was `dword`, renamed per spec errata) --
  expectStr("qword typed", "local d:qword = 4294967296\nlocal s = toStr(d);", "s", "4294967296");
  expectStr("qword type returns qword", "local d:qword = 1\nlocal s = type(d);", "s", "qword");
  expectStr("qword max u32", "local d:qword = 4294967295\nlocal s = toStr(d);", "s", "4294967295");
  expectRun("qword const", "const big:qword = 10000000000");
  expectVal("qword arithmetic", "local a:qword = 100\nlocal b:qword = 200\nlocal r = a + b;", "r", 300);

  // -- NEW: dword is an alias for unsigned (spec errata: 4 bytes, uint32_t) --
  expectStr("dword alias typed", "local d:dword = 4294967295\nlocal s = toStr(d);", "s", "4294967295");
  expectStr("dword alias type is unsigned", "local d:dword = 1\nlocal s = type(d);", "s", "unsigned");
  expectVal("dword alias len 4", "local d:dword = 1\nlocal r = len(d);", "r", 4);
  expectRun("dword alias const", "const dw:dword = 100");
  // -- NEW: long type (spec §24; int64_t, 8 bytes) --
  expectStr("long typed", "local l:long = 9007199254740993\nlocal s = toStr(l);", "s", "9007199254740993");
  expectStr("long type", "local l:long = 5\nlocal s = type(l);", "s", "long");
  expectVal("long len 8", "local l:long = 5\nlocal r = len(l);", "r", 8);
  expectRun("long const", "const ll:long = 42");
  expectStr("long cast", "local l:long = cast<long>(9007199254740993)\nlocal s = toStr(l);", "s", "9007199254740993");

  // -- NEW: switch/case/default (spec §208-221) --
  expectVal("switch basic int", "local x = 2\nswitch (x) {\n  case 1: x = 10\n  case 2: x = 20\n  case 3: x = 30\n  default: x = 99\n}\nlocal r = x;", "r", 20);
  expectVal("switch basic string", "local x = \"hello\"\nswitch (x) {\n  case \"world\": x = 1\n  case \"hello\": x = 2\n  default: x = 3\n}\nlocal r = x;", "r", 2);
  expectVal("switch default", "local x = 99\nswitch (x) {\n  case 1: x = 1\n  case 2: x = 2\n  default: x = 999\n}\nlocal r = x;", "r", 999);
  expectVal("switch break", "local x = 2\nlocal result = 0\nswitch (x) {\n  case 1: result = 1; break\n  case 2: result = 2; break\n  default: result = 99\n}\nlocal r = result;", "r", 2);
  expectVal("switch in case", "local x = 3\nswitch (x) {\n  case in [1,2]: x = 10\n  case in [3,4,5]: x = 30\n  default: x = 99\n}\nlocal r = x;", "r", 30);
  expectVal("switch range case", "local x = 15\nswitch (x) {\n  case in range(1,10): x = 1\n  case in range(11,20): x = 2\n  default: x = 99\n}\nlocal r = x;", "r", 2);
  expectRun("switch empty", "local x = 1\nswitch (x) {\n  default:\n}");

  // -- NEW: join() builtin (spec §367) --
  expectStr("join basic", "local a = [1,2,3]\nlocal r = join(\", \", a);", "r", "1, 2, 3");
  expectStr("join empty", "local a = []\nlocal r = join(\", \", a);", "r", "");
  expectStr("join single", "local a = [42]\nlocal r = join(\", \", a);", "r", "42");
  expectStr("join strings", "local a = [\"a\",\"b\",\"c\"]\nlocal r = join(\"-\", a);", "r", "a-b-c");
  expectStr("join mixed types", "local a = [1, \"two\", 3.0]\nlocal r = join(\"\", a);", "r", "1two3");
  expectStr("join no sep", "local a = [1,2,3]\nlocal r = join(\"\", a);", "r", "123");
  expectThrow("join wrong count", "local a = [1]\nlocal r = join(a);");
  expectThrow("join non-array", "local r = join(\", \", \"not an array\");");

  // -- NEW: type reserved word (spec §67) --
  expectThrow("type as ident", "local type = 1");
  expectThrow("type as type decl", "local x:type = 1");
  expectStr("type builtin call", "local s = type(42);", "s", "int");
  expectStr("type builtin string", "local s = type(\"hello\");", "s", "string");
  expectStr("type builtin array", "local a = [1,2]\nlocal s = type(a);", "s", "array");

  // -- NEW: isset() builtin (spec §381) --
  expectVal("isset undefined", "local r = isset(\"undefined_var\");", "r", 0);
  expectVal("isset defined", "local x = 1\nlocal r = isset(\"x\");", "r", 1);
  expectVal("isset after delete", "local x = 1\ndelete x\nlocal r = isset(\"x\");", "r", 0);
  expectThrow("isset wrong count", "local r = isset();");
  expectThrow("isset non-string", "local r = isset(123);");

  // -- NEW: sprintf() builtin (spec §374) --
  expectStr("sprintf basic", "local s = sprintf(\"hello %s\", \"world\");", "s", "hello world");
  expectStr("sprintf int", "local s = sprintf(\"num=%d\", 42);", "s", "num=42");
  expectStr("sprintf float", "local s = sprintf(\"pi=%.2f\", 3.14);", "s", "pi=3.14");
  expectStr("sprintf multi", "local s = sprintf(\"%s is %d\", \"age\", 99);", "s", "age is 99");
  expectStr("sprintf char", "local s = sprintf(\"c=%c\", 65);", "s", "c=A");
  expectStr("sprintf percent", "local s = sprintf(\"%d%%\", 100);", "s", "100%");
  expectStr("sprintf no args", "local s = sprintf(\"plain\");", "s", "plain");
  expectThrow("sprintf no format", "local s = sprintf();");
  expectThrow("sprintf bad type", "local s = sprintf(123);");
  expectThrow("sprintf missing arg", "local s = sprintf(\"%s\");");

  // -- NEW: global keyword (spec §530-564) --
  // Global const visible everywhere
  expectStr("global const visible",
    "const HELLO = \"Hello World\"\ndef test() { return HELLO }\nlocal r = test();",
    "r", "Hello World");

  // Global function visible everywhere
  expectVal("global func visible",
    "def add(a,b) { return a+b }\ndef wrapper() { return add(3,4) }\nlocal r = wrapper();",
    "r", 7);

  // Global var invisible without keyword
  expectThrow("global var invisible without keyword",
    "a = 42\ndef test() { return a }\ntest()");

  // Basic global keyword: import and read
  expectVal("global basic read",
    "a = 42\ndef test() { global a; return a }\nlocal r = test();",
    "r", 42);

  // Global keyword modifies persist after function return
  expectVal("global modify persists",
    "a = [1,2,3]\ndef test() { global a; a = a + [4,5,6] }\ntest()\nlocal r = len(a);",
    "r", 6);

  // Global with multiple variables
  expectVal("global multiple vars",
    "a = 10\nb = 20\ndef test() { global a, b; return a + b }\nlocal r = test();",
    "r", 30);

  // Global with local shadow — global wins after declaration
  expectVal("global with local shadow",
    "a = 10\ndef test() { local a = 99; global a; return a }\nlocal r = test();",
    "r", 10);

  // Global at top level is error
  expectThrow("global at top level error",
    "a = 1\nglobal a");

  // Global undeclared variable error
  expectThrow("global undeclared var",
    "def test() { global z }\ntest()");

  // -- NEW: assert() builtin (spec §397) --
  expectRun("assert true", "assert(true, \"should not fail\")");
  expectThrow("assert false", "assert(false, \"assertion failed\")");
  expectThrow("assert wrong count", "assert(true)");
  expectThrow("assert bad msg type", "assert(true, 123)");

  // -- NEW: generators / yield (spec §608–623) --
  expectVal("gen basic yield",
    "def gen_range(mi, mx) { for i = mi to mx { yield i } }\n"
    "local gen = gen_range(1, 3)\n"
    "local r = gen();",
    "r", 1);
  expectVal("gen second yield",
    "def gen_range(mi, mx) { for i = mi to mx { yield i } }\n"
    "local gen = gen_range(1, 3)\n"
    "gen(); gen(); local r = gen();",
    "r", 3);
  expectVal("gen exhausted returns nil",
    "def gen_range(mi, mx) { for i = mi to mx { yield i } }\n"
    "local gen = gen_range(1, 1)\n"
    "gen(); gen(); local r = gen();",
    "r", 0);
  expectVal("gen loop accumulate",
    "def gen_range(mi, mx) { for i = mi to mx { yield i } }\n"
    "local total = 0\n"
    "local gen = gen_range(1, 3)\n"
    "local v = gen()\n"
    "while v != nil { total = total + v; v = gen() }\n"
    "local r = total;",
    "r", 6);
  expectVal("gen empty yield",
    "def gen_once() { yield; yield 42 }\n"
    "local gen = gen_once()\n"
    "local r = gen();",
    "r", 0);
  expectVal("gen capture outer",
    "def make_gen(start) { for i = start to start+2 { yield i } }\n"
    "local gen = make_gen(10)\n"
    "local r = gen();",
    "r", 10);
  expectStr("gen type is generator",
    "def gen_foo() { yield 1 }\n"
    "local gen = gen_foo()\n"
    "local s = type(gen);",
    "s", "generator");

  // -- EOF value (first-class end-of-file sentinel) --
  expectStr("eof type", "local s = type(eof);", "s", "eof");
  expectThrow("eof uppercase not valid", "EOF");
  expectRun("eof in if false", "if eof { local x = 1 } local r = 0;");
  expectThrow("eof assert fails", "assert(eof, \"eof is falsy\")");
  expectStr("eof assign and read", "local e = eof\nlocal r = type(e);", "r", "eof");
  expectStr("eof in table", "local t = {x=eof}\nlocal r = type(t.x);", "r", "eof");

  // -- == / != semantics (spec §23) -- (explicit nil: valueless `local` is int 0)
  expectVal("eq nil nil", "local a = nil\nlocal r = (a == nil);", "r", 1);
  expectVal("ne nil nil", "local a = nil\nlocal r = (a != nil);", "r", 0);
  expectVal("ne nil x", "local a = nil\nlocal r = (a != 5);", "r", 1);
  expectVal("eq nil x", "local a = nil\nlocal r = (a == 5);", "r", 0);
  expectVal("eq bool int", "local r = (true == 1);", "r", 1);
  expectVal("eq bool int zero", "local r = (false == 0);", "r", 1);
  expectVal("ne bool int", "local r = (true != 0);", "r", 1);
  expectVal("eq num float", "local r = (5 == 5.0);", "r", 1);
  expectVal("ne num", "local r = (5 != 6);", "r", 1);
  expectVal("eq string", "local r = (\"a\" == \"a\");", "r", 1);
  expectVal("ne string", "local r = (\"a\" != \"b\");", "r", 1);
  expectVal("eq cross type", "local r = (5 == \"5\");", "r", 0);
  expectVal("ne cross type", "local r = (5 != \"5\");", "r", 1);
  expectVal("eq array same", "local a1 = [1,2,3]\nlocal a2 = [1,2,3]\nlocal r = (a1 == a2);", "r", 1);
  expectVal("eq array diff", "local a1 = [1,2,3]\nlocal a2 = [1,2,4]\nlocal r = (a1 == a2);", "r", 0);
  expectVal("eq table same", "local t1 = {a=1,b=2}\nlocal t2 = {a=1,b=2}\nlocal r = (t1 == t2);", "r", 1);
  expectVal("eq table diff", "local t1 = {a=1,b=2}\nlocal t2 = {a=1,b=3}\nlocal r = (t1 == t2);", "r", 0);

  // -- string == object via __str__ (spec §operator == and !=) --
  expectVal("eq str object __str__ match", "class C { __str__(self)[] { return \"toto\" } }\nlocal r = (\"toto\" == C());", "r", 1);
  expectVal("ne str object __str__ mismatch", "class C { __str__(self)[] { return \"nope\" } }\nlocal r = (\"toto\" == C());", "r", 0);
  expectVal("eq object str reversed", "class C { __str__(self)[] { return \"toto\" } }\nlocal r = (C() == \"toto\");", "r", 1);
  expectVal("ne str object negation", "class C { __str__(self)[] { return \"toto\" } }\nlocal r = (\"toto\" != C());", "r", 0);
  expectVal("eq num object false", "class C { __str__(self)[] { return \"1\" } }\nlocal r = (1 == C());", "r", 0);
  expectVal("eq str plain table false", "local t = {a=1}\nlocal r = (\"x\" == t);", "r", 0);

  // -- operators < and > (spec §operators < and >) --
  expectVal("lt number number", "local r = (1 < 2);", "r", 1);
  expectVal("gt number float", "local r = (2.5 > 2);", "r", 1);
  expectVal("lt string string", "local r = (\"ab\" < \"ac\");", "r", 1);
  expectVal("lt string object __str__",
            "local t = {n=1}\nt.__str__ = (self)[] { return \"abc\" }\nlocal r = (\"ab\" < t);",
            "r", 1);
  expectVal("lt string class",
            "class C { __str__(self)[] { return \"xyz\" } }\nlocal r = (\"xy\" < C());",
            "r", 1);
  expectVal("lt string array",
            "local r = (\"[1\" < [1,2,3]);", "r", 1);
  expectVal("lt table table per-index",
            "local r = ({a=1,b=2} < {a=1,b=3});", "r", 1);
  expectVal("lt table table prefix shorter less",
            "local r = ({a=1} < {a=1,b=2});", "r", 1);
  expectVal("le table table equal",
            "local r = ({a=1} <= {a=1});", "r", 1);
  expectVal("lt array array per-index",
            "local r = ([1,2] < [1,3]);", "r", 1);
  expectVal("gt array array per-index",
            "local r = ([1,9] > [1,3]);", "r", 1);
  expectVal("lt array array prefix shorter less",
            "local r = ([1] < [1,2]);", "r", 1);
  expectVal("ge array array equal",
            "local r = ([1,2] >= [1,2]);", "r", 1);
  expectThrow("lt number string invalid", "local r = (1 < \"a\");");
  expectThrow("lt number object invalid", "local r = (1 < {a=1});");
  expectThrow("lt nil any invalid", "local r = (nil < 1);");
  expectThrow("lt table class invalid",
              "class C { x = 1 }\nlocal r = ({a=1} < C());");
  expectThrow("lt class class invalid",
              "class C { x = 1 }\nlocal r = (C() < C());");
  expectThrow("lt table array invalid",
              "local r = ({a=1} < [1]);");
  expectThrow("lt class array invalid",
              "class C { x = 1 }\nlocal r = (C() < [1]);");

  // -- function return values (spec §13) --
  expectVal("fn returns last expr", "def f() { return 42 }\nlocal r = f();", "r", 42);
  expectVal("fn returns nil no expr", "def g() { x = 5 }\nlocal r = g();\nlocal n = (r == nil);", "n", 1);
  expectVal("fn returns nil explicit", "def h() { return nil }\nlocal r = h();\nlocal n = (r == nil);", "n", 1);
  expectVal("fn returns nil via print", "def p() { print(\"hi\") }\nlocal r = p();\nlocal n = (r == nil);", "n", 1);

  // -- NEW: for ... step (direction-based guard model) --
  // ascending, constant step 2
  expectStr("for step2 asc",
    "local s = \"\"\nfor x = 0 to 10 step 2 { s = s + toStr(x) + \",\" }\nlocal r = s;",
    "r", "0,2,4,6,8,10,");
  // ascending, constant step 3
  expectStr("for step3 asc",
    "local s = \"\"\nfor x = 0 to 10 step 3 { s = s + toStr(x) + \",\" }\nlocal r = s;",
    "r", "0,3,6,9,");
  // descending, constant step -1 (user step matches natural direction)
  expectStr("for step -1 desc",
    "local s = \"\"\nfor x = 5 to 0 step -1 { s = s + toStr(x) + \",\" }\nlocal r = s;",
    "r", "5,4,3,2,1,0,");
  // descending, constant step -2
  expectStr("for step -2 desc",
    "local s = \"\"\nfor x = 8 to 0 step -2 { s = s + toStr(x) + \",\" }\nlocal r = s;",
    "r", "8,6,4,2,0,");
  // wrong direction (asc bounds, neg step): body runs exactly once at start
  expectStr("for step neg asc once",
    "local s = \"\"\nfor x = 0 to 10 step -1 { s = s + toStr(x) + \",\" }\nlocal r = s;",
    "r", "0,");
  // wrong direction with larger magnitude, still once
  expectStr("for step neg asc once2",
    "local s = \"\"\nfor x = 0 to 10 step -3 { s = s + toStr(x) + \",\" }\nlocal r = s;",
    "r", "0,");
  // start == end, positive step (natural dir is desc, step is asc -> once)
  expectStr("for same start end pos step",
    "local s = \"\"\nfor x = 5 to 5 step 1 { s = s + toStr(x) + \",\" }\nlocal r = s;",
    "r", "5,");
  // start == end, negative step (matches desc -> x=5 only, then 4 < 5 stop)
  expectStr("for same start end neg step",
    "local s = \"\"\nfor x = 5 to 5 step -1 { s = s + toStr(x) + \",\" }\nlocal r = s;",
    "r", "5,");
  // step 0 -> infinite loop (x frozen, break never triggers)
  expectThrow("for step0 infinite",
    "local s = \"\"\nfor x = 0 to 10 step 0 { s = s + toStr(x) + \",\"; if x > 5 { break } }\nlocal r = s;");
  // variable step (positive), ascending
  expectStr("for var step pos",
    "local k = 2\nlocal s = \"\"\nfor x = 0 to 10 step k { s = s + toStr(x) + \",\" }\nlocal r = s;",
    "r", "0,2,4,6,8,10,");
  // variable step (negative), descending
  expectStr("for var step neg",
    "local k = 2\nlocal s = \"\"\nfor x = 10 to 0 step -k { s = s + toStr(x) + \",\" }\nlocal r = s;",
    "r", "10,8,6,4,2,0,");
  // variable step wrong direction -> once
  expectStr("for var step neg asc once",
    "local k = 2\nlocal s = \"\"\nfor x = 0 to 10 step -k { s = s + toStr(x) + \",\" }\nlocal r = s;",
    "r", "0,");
  // variable step 0 -> infinite, guarded by break
  expectStr("for var step0 break",
    "local k = 0\nlocal s = \"\"\nfor x = 0 to 10 step k { s = s + toStr(x) + \",\"; if x > 4 { break } }\nlocal r = s;",
    "r", "0,1,2,3,4,5,");
  // for-in range: ascending step 2
  expectStr("forin range step2",
    "local s = \"\"\nfor i in range(0, 10) step 2 { s = s + toStr(i) + \",\" }\nlocal r = s;",
    "r", "0,2,4,6,8,10,");
  // for-in range: descending step -1
  expectStr("forin range step -1",
    "local s = \"\"\nfor i in range(5, 0) step -1 { s = s + toStr(i) + \",\" }\nlocal r = s;",
    "r", "5,4,3,2,1,0,");
  // for-in range: wrong direction -> once
  expectStr("forin range step neg once",
    "local s = \"\"\nfor i in range(0, 10) step -1 { s = s + toStr(i) + \",\" }\nlocal r = s;",
    "r", "0,");
  // for-in range: step 0 -> infinite loop (i frozen, break never triggers)
  expectThrow("forin range step0 infinite",
    "local s = \"\"\nfor i in range(0, 10) step 0 { s = s + toStr(i) + \",\"; if i > 3 { break } }\nlocal r = s;");
  // for-in over array literal + step -> error
  expectThrow("forin array step error", "for i in [1,2,3] step 1 { }");
  // continue inside a stepped loop
  expectStr("for step continue",
    "local s = \"\"\nfor x = 0 to 8 step 2 { if x == 4 { continue } s = s + toStr(x) + \",\" }\nlocal r = s;",
    "r", "0,2,6,8,");

  // -- Phase 1.4: closed field set (class instances reject undeclared fields) --
  expectVal("class declared field set/get",
    "class Money {\ntotal = 0\nadd (self, n) [ ] {\nself.total = self.total + n\n}\n}\n"
    "local m = Money()\nm.total = 100\nm.add(5)\nlocal r = m.total;",
    "r", 105);
  expectThrow("class undeclared field get",
    "class Money {\ntotal = 0\n}\nlocal m = Money()\nlocal r = m.undeclared;");
  expectThrow("class undeclared field set",
    "class Money {\ntotal = 0\n}\nlocal m = Money()\nm.newfield = 1");
  expectStr("class __class__ name",
    "class Money {\ntotal = 0\n}\nlocal m = Money()\nlocal r = m.__class__;",
    "r", "Money");

  // -- Phase 2.4: const fields (read-only, enforced in SETFIELD) --
  expectThrow("class const field assign",
    "class Money {\nconst total = 0\n}\nlocal m = Money()\nm.total = 1");
  expectRun("class const field read",
    "class Money {\nconst total = 0\n}\nlocal m = Money()\nlocal r = m.total;");
  expectVal("class nonconst field set",
    "class Money {\nconst total = 0\nother = 5\n}\nlocal m = Money()\nm.other = 99\nlocal r = m.other;",
    "r", 99);
  expectVal("class const in __const_fields__",
    "class Money {\nconst total = 0\nother = 5\n}\nlocal m = Money()\nlocal r = m.__const_fields__.total;",
    "r", 1);

  // -- Phase 3.3: unique method names (duplicate -> compile error) --
  expectThrow("class duplicate method",
    "class C {\nm()[] {}\nm()[] {}\n}");
  expectRun("class distinct methods ok",
    "class C {\na()[] {}\nb()[] {}\n}\nlocal c = C();");

  // -- Phase 4.5: fixed arity checking --
  expectThrow("method call too many args",
    "class Calc {\nadd (self, a, b) [ ] { return a + b }\n}\nlocal c = Calc();\nc.add(1, 2, 3);");
  expectThrow("method call too few args",
    "class Calc {\nadd (self, a, b) [ ] { return a + b }\n}\nlocal c = Calc();\nc.add(1);");
  expectVal("method call right arity ok",
    "class Calc {\nadd (self, a, b) [ ] { return a + b }\n}\nlocal c = Calc();\nlocal r = c.add(2, 3);",
    "r", 5);
  expectThrow("constructor too many args",
    "class P {\nx = 0\n__init__ (self, v) [ ] { self.x = v }\n}\nlocal p = P(1, 2);");
  expectThrow("constructor too few args",
    "class P {\nx = 0\n__init__ (self, v) [ ] { self.x = v }\n}\nlocal p = P();");
  expectVal("constructor right arity ok",
    "class P {\nx = 0\n__init__ (self, v) [ ] { self.x = v }\n}\nlocal p = P(7);\nlocal r = p.x;",
    "r", 7);
  expectThrow("class without __init__ takes zero args",
    "class C {\n}\nlocal c = C(1);");
  expectRun("class without __init__ zero args ok",
    "class C {\n}\nlocal c = C();");

  // -- Phase 5.4: field type inference --
  expectThrow("field type violation (string into int field)",
    "class P {\ncount = 0\n__init__ (self, v) [ ] { self.count = v }\n}\nlocal p = P(\"x\");");
  expectThrow("field type violation (int into string field)",
    "class S {\nname = \"\"\n__init__ (self, v) [ ] { self.name = v }\n}\nlocal s = S(42);");
  expectVal("field type match ok",
    "class P {\ncount = 0\n__init__ (self, v) [ ] { self.count = v }\n}\nlocal p = P(9);\nlocal r = p.count;",
    "r", 9);
  expectVal("numeric tower compatible (int into qword field)",
    "class D {\nbig = 3000000000\n__init__ (self, v) [ ] { self.big = v }\n}\nlocal d = D(5);\nlocal r = d.big;",
    "r", 5);
  expectRun("nil default field unconstrained",
    "class N {\nx = nil\n__init__ (self, v) [ ] { self.x = v }\n}\nlocal n = N(\"anything\");\nlocal m = N(42);");
  expectThrow("field type violation via method",
    "class T {\nlabel = \"\"\nset (self, v) [ ] { self.label = v }\n}\nlocal t = T();\nt.set(7);");

  // -- Phase 0.5: type() on Object instances --
  {
    // Create an ObjectDescriptor with a custom ObjectOps (kindName = "Person").
    static const ObjectOps personOps = {
        .kindName = "Person",
        .getField = table_ops::getField,
        .setField = table_ops::setField,
        .callMethod = table_ops::callMethod,
        .toStr = table_ops::toStr,
        .base = table_ops::base,
    };
    auto desc = std::make_shared<ObjectDescriptor>();
    desc->ops = &personOps;
    QLangValue obj = QLangValue::Object(desc);

    QLangContext ctx;
    ctx.setFunc("make_person", [&](QLangContext &, const std::vector<QLangValue> &) -> QLangValue {
      return obj;
    });
    try {
      ctx.run("local o = make_person()\nlocal t = type(o)");
      std::string got = ctx.get("t").asString();
      if (got == "Person") {
        std::cout << "[PASS] str:  [L" << __LINE__ << "] type(object) returns class name (t=\"" << got << "\")\n";
        passes++;
      } else {
        std::cout << "[FAIL] str:  [L" << __LINE__ << "] type(object) returns class name (t=\"" << got << "\" exp \"Person\")\n";
        fails++;
      }
    } catch (const QLangError &e) {
      std::cout << "[FAIL] str:  [L" << __LINE__ << "] type(object) returns class name -> " << e.what() << "\n";
      fails++;
    }
  }
  {
    // Object with null ops should fall back to "object".
    auto desc2 = std::make_shared<ObjectDescriptor>();
    desc2->ops = nullptr;
    QLangValue obj2 = QLangValue::Object(desc2);

    QLangContext ctx2;
    ctx2.setFunc("make_obj", [&](QLangContext &, const std::vector<QLangValue> &) -> QLangValue {
      return obj2;
    });
    try {
      ctx2.run("local o = make_obj()\nlocal t = type(o)");
      std::string got = ctx2.get("t").asString();
      if (got == "object") {
        std::cout << "[PASS] str:  [L" << __LINE__ << "] type(object) null ops fallback (t=\"" << got << "\")\n";
        passes++;
      } else {
        std::cout << "[FAIL] str:  [L" << __LINE__ << "] type(object) null ops fallback (t=\"" << got << "\" exp \"object\")\n";
        fails++;
      }
    } catch (const QLangError &e) {
      std::cout << "[FAIL] str:  [L" << __LINE__ << "] type(object) null ops fallback -> " << e.what() << "\n";
      fails++;
    }
  }

  // -- Phase 5: __str__ --
  expectStr("str: default class name",
            "class P { x = 1 }\nlocal p = P()\nlocal s = str(p);", "s", "P");
  expectStr("str: custom __str__",
            "class Q { name = \"q\" \n__str__ (self) [ ] { return self.name }\n}\n"
            "local q = Q()\nlocal s = str(q);",
            "s", "q");
  expectStr("str: plain table renders entries (implicit __str__)",
            "local t = {a=1}\nlocal s = str(t);", "s", "{a=1}");
  expectStr("str: array renders elements (implicit __str__)",
            "local a = [0,1,2,3,4]\nlocal s = str(a);", "s", "[0, 1, 2, 3, 4]");
  expectStr("str: table strings quoted (implicit __str__)",
            "local t = {name=\"Bob\", age=9}\nlocal s = str(t);", "s",
            "{age=9, name=\"Bob\"}");
  expectStr("str: nested collections (implicit __str__)",
            "local n = {x=[1,\"two\"], y={z=true, w=nil}}\nlocal s = str(n);",
            "s", "{x=[1, \"two\"], y={w=nil, z=true}}");
  expectStr("f-string: array interpolation (implicit __str__)",
            "local a = [1,2,3]\nlocal s = f\"a={a}\";", "s", "a=[1, 2, 3]");
  expectStr("type: table class name",
            "class T2 { x = 1 }\nlocal t = T2()\nlocal s = type(t);", "s", "T2");

  // -- Phase 6: reference counting --
  expectVal("refcount: alias survives nil",
            "class R { v = 7 }\nlocal a = R()\nlocal b = a\nb = nil\nlocal x = a.v;",
            "x", 7);
  expectVal("refcount: other ref keeps alive",
            "class R { v = 3 }\nlocal a = R()\nlocal b = a\na = nil\nlocal x = b.v;",
            "x", 3);

  // -- Phase 7: __destroy__ (Phase 3: finalizers drain at a safe point,
  // not from the shared_ptr deleter) --
  expectValDrain("destroy: runs once on final release",
            "local n = 0\n"
            "class D {\n"
            "  __destroy__ (self) [n] { n = n + 1 }\n"
            "}\n"
            "local a = D()\nlocal b = a\na = nil\nb = nil;",
            "n", 1);
  expectVal("destroy: not before final ref",
            "local log = {n = 0}\n"
            "class D {\n  log = nil\n"
            "__init__ (self, lg) [ ] { self.log = lg }\n"
            "__destroy__ (self) [ ] { local lg = self.log\nlg.n = lg.n + 1 }\n}\n"
            "local a = D(log)\nlocal b = a\na = nil\nlocal n = log.n;",
            "n", 0);
  expectRun("destroy: absent finalizer no crash",
            "class E { v = 1 }\nlocal e = E()\ne = nil");
  expectRun("destroy: writes self field",
            "class F {\n  flag = 0\n  __destroy__ (self) [ ] { self.flag = 1 }\n}\n"
            "local f = F()\nf = nil");

  // -- $ dynamic key / scalar-to-string (Phase 9) --
  expectStr("$: scalar int", "local x = 42\nlocal s = $x;", "s", "42");
  expectStr("$: scalar string", "local x = \"abc\"\nlocal s = $x;", "s", "abc");
  expectRun("$: dynamic table key",
            "local k = \"a\"\nlocal t = {$k = 1}\nlocal r = t.a;");
  expectVal("$: dynamic table key val",
            "local k = \"a\"\nlocal t = {$k = 7}\nlocal r = t.a;", "r", 7);
  expectThrow("$: rejects table", "local t = {a = 1}\nlocal s = $t;");
  expectThrow("$: rejects instance",
              "class C { v = 1 }\nlocal c = C()\nlocal s = $c;");

  // --- SPEC # block scope ---
  expectStr("block scope shadow",
            "local i = 1\nif true {\nlocal i = 3\n}\nlocal s = $i;", "s", "1");
  expectStr("block scope crossing i",
            "local i = 1\nif true {\na = 2\ni = 3\n}\nlocal s = $i;", "s", "3");
  expectStr("block scope crossing a",
            "local i = 1\nif true {\na = 2\ni = 3\n}\nlocal s = $a;", "s", "2");
  expectStr("block scope let",
            "local j = 5\nwhile (false) {\nlet j = 9\n}\nlocal s = $j;", "s", "5");
  expectStr("block scope nested shadow",
            "local k = 1\nif true {\nlocal k = 2\nif true {\nlocal k = 4\n}\n}\nlocal s = $k;", "s", "1");
  expectStr("block scope for-in shadow",
            "local m = 0\nfor q in [1,2,3]\nlocal m = 9\nnext q\nlocal s = $m;", "s", "0");

  // -- boolean-producing operators return Bool (cmp.ql report) --
  expectStr("type: < returns bool",
            "local s = type(1 < 2);", "s", "bool");
  expectStr("type: > returns bool",
            "local s = type(2 > 5);", "s", "bool");
  expectStr("type: == returns bool",
            "local s = type(\"a\" == \"b\");", "s", "bool");
  expectStr("type: && returns bool",
            "local s = type(true && false);", "s", "bool");
  expectStr("type: ! returns bool",
            "local s = type(!true);", "s", "bool");
  expectStr("type: in returns bool",
            "local s = type(1 in [1,2,3]);", "s", "bool");
  expectStr("f-string: bool comparison renders true/false",
            "local s = f\"{1 < 2} {2 > 5}\";", "s", "true false");
  expectStr("f-string: float renders (no UB via %s)",
            "local s = f\"{1.24}\";", "s", "1.24");
  expectStr("f-string: unsigned > int32 max renders full value",
            "local a = cast<dword>(-1); local s = f\"{a}\";", "s", "4294967295");
  expectStr("f-string: qword max renders full value",
            "local a = cast<qword>(-1); local s = f\"{a}\";", "s", "18446744073709551615");
  expectStr("cast<float> of float keeps fraction",
            "local s = toStr(cast<float>(1.24));", "s", "1.24");
  expectVal("bool comparison still numeric in arithmetic", "local s = 1 + (1 < 2);", "s", 2);
  expectThrow("unknown function call is a compile error",
              "printl(\"x\");");

  // -- Phase 1: fetch reserved, math rand (dup-decl warning deferred) --
  expectThrow("fetch is a reserved word", "local fetch = 1");
  expectRun("math rand()", "import math\nlocal r = math.rand()");
  expectVal("math rand() in range",
            "import math\nlocal r = math.rand()\nlocal ok = (r >= 0) || (r < 0);",
            "ok", 1);

  // -- Phase 2: set type (SPEC §sets) --
  expectVal("set ctor count", "local s = set(1,5,7,9); local n = len(s);", "n", 4);
  expectVal("set dedup", "local s = set(1,5,7,8,9,5); local n = len(s);", "n", 5);
  expectVal("set append", "local s = set(1,5,7); s[] = 11; local n = len(s);", "n", 4);
  expectVal("set append dedup", "local s = set(1,5,7); s[] = 5; local n = len(s);", "n", 3);
  expectVal("set array coerce decl", "local s:set = [1,2,4.7]; local n = len(s);", "n", 3);
  expectVal("set array ctor", "local s = set([1,6,46,5]); local n = len(s);", "n", 4);
  expectStr("set type name", "local s = set(1,2); local t = type(s);", "t", "set");
  expectStr("set __str__", "local s = set(1,2,3); local t = str(s);", "t", "set(1, 2, 3)");
  expectStr("set __str__ strings", "local s = set(\"x\",\"y\",\"x\"); local t = str(s);", "t", "set(\"x\", \"y\")");
  expectVal("set empty falsy", "local s = set(); local n = 0; if s { n = 1; }", "n", 0);
  expectVal("set nonempty truthy", "local s = set(1); local n = 0; if s { n = 1; }", "n", 1);
  expectThrow("set rejects array element", "local s = set(1, [2]);");
  // Phase 2b: set `in`, `for-in`, `==`, cast (SPEC a350a25c)
  expectVal("set in true", "local r = (2 in set(1,2,3));", "r", 1);
  expectVal("set in false", "local r = (9 in set(1,2,3));", "r", 0);
  expectVal("set for-in sum", "local s = set(1,2,3); local t = 0; for x in s { t = t + x; }", "t", 6);
  expectVal("set while-in", "local x = 1; while (x in set(1,2,3)) { x = x + 1; }", "x", 4);
  expectVal("set == exact match", "local a = set(1,2,3); local b = set(3,1,2); local r = (a == b);", "r", 1);
  expectVal("set == differ", "local a = set(1,2); local b = set(1,3); local r = (a == b);", "r", 0);
  expectVal("set == size diff", "local a = set(1,2); local b = set(1,2,3); local r = (a == b);", "r", 0);
  expectVal("cast<set>(array)", "local s = cast<set>([1,2,2,3]); local n = len(s);", "n", 3);
  expectVal("cast<array>(set)", "local a = cast<array>(set(1,2,3)); local n = len(a);", "n", 3);
  expectThrow("cast<int>(set) rejected", "local n = cast<int>(set(1,2));");

  // -- Phase 3: enum type (SPEC §enum) --
  expectVal("enum implied values", "enum Animal { Dog, Cat, Horse } local d = Animal::Dog; local c = Animal::Cat; local h = Animal::Horse; local s = d + c + h;", "s", 3);
  expectVal("enum = N resumes sequence", "enum Language { Cpp = 100, Python = 101, Pascal } local p = Language::Pascal;", "p", 102);
  expectVal("enum dot notation", "enum Animal { Dog, Cat, Horse } local d = Animal.Dog;", "d", 0);
  expectVal("enum typed :byte", "enum SoftDrink:byte { CocaCola, Pepsi, SevenUp } local s = SoftDrink::Pepsi;", "s", 1);
  expectStr("enum typed :byte type", "enum SoftDrink:byte { CocaCola, Pepsi } local t = type(SoftDrink::CocaCola);", "t", "byte");
  expectVal("enum len = member count", "enum Language { Cpp = 100, Python = 101, Pascal } local n = len(Language);", "n", 3);
  expectVal("enum value arithmetic", "enum Language { Cpp = 100, Python = 101 } local r = Language::Python - Language::Cpp;", "r", 1);
  expectThrow("enum redeclare -> error", "enum Animal { Dog } enum Animal { Cat }");
  expectThrow("enum unknown reference -> error", "local x = NoSuchEnum::A;");
  expectThrow("enum unknown member -> error", "enum Animal { Dog } local x = Animal::Cat;");
  expectThrow("enum valueless -> error", "enum Empty { }");
  expectThrow("enum non-integer value -> error", "enum E { A = 1.5 }");
  expectRun("enum block scope (local)", "if true { local enum E { A, B } local x = E::B; }");
  expectThrow("enum block scope not visible outside", "if true { local enum E { A } } local x = E::A;");
  expectVal("enum global visible in function", "enum Color { Red, Green, Blue } def pick() return Color::Blue; end local r = pick();", "r", 2);
  expectThrow("enum member not assignable", "enum Animal { Dog } Animal.Dog = 5;");

  // -- error reporting (SPEC # error reporting) --
  // Lex error carries the offending line/col.
  expectMsg("lex error has line",
            "local a = \"unterminated",
            "lex error", "line 1");
  // Parse error points at the offending line.
  expectMsg("parse error has line",
            "local a = 1\nlocal b = 2\nlocal c = (1 + 2",
            "parse error", "line 3");
  // Runtime type error: module + line + kind + message.
  expectMsg("type error has module and line",
            "local a = 1\nlocal b = 2\nlocal c = a + \"x\"",
            "module 'main'", "line 3", "type error", "operator '+'");
  // Builtin argument-count error: line/col of the call + main frame.
  expectMsg("builtin arity error has line and frame",
            "local a = 1\nlocal n = len()",
            "module 'main'", "line 2", "in function 'main'", "at line 2 in main");
  // Function name in the header and in the trace.
  expectMsg("error in named function",
            "def foo()\n  local x = 1 + \"a\"\nend\nfoo()",
            "in function 'foo'", "at line 2 in foo", "at line 4 in main");
  // Deep call chain: one trace line per frame, innermost first.
  // (defs in dependency order — QLang does not hoist functions)
  expectMsg("deep call chain trace",
            "def c()\n  local x = 1 / 0\nend\n" 
            "def b()\n  c()\nend\n" 
            "def a()\n  b()\nend\n" 
            "a()",
            "in function 'c'",
            "at line 2 in c",
            "at line 5 in b",
            "at line 8 in a",
            "at line 10 in main");

  // -- MOVI (SPEC §bytecodes: new opcode MOVI) --
  // Int literals load via MOVI (no pool slot); values must run correctly.
  expectVal("movi: zero", "local x = 0;", "x", 0);
  expectVal("movi: one", "local x = 1;", "x", 1);
  expectVal("movi: negative one", "local x = -1;", "x", -1);
  expectVal("movi: int32 max", "local x = 2147483647;", "x", 2147483647);
  expectVal("movi: int32 min (unary neg)", "local x = -2147483648;", "x",
            -2147483648);
  // Type-fidelity guard: `true` is Bool (not Int) and must stay in the pool.
  expectStr("movi: bool literal stays bool", "local b = true; local t = type(b);",
            "t", "bool");
  // 64-bit literals are Qword (not Int) and stay in the pool.
  expectStr("movi: 64-bit literal stays qword",
            "local q = 9223372036854775807; local t = type(q);", "t", "qword");
  // Int literal inside a loop (VM dispatch in the loop paths).
  expectVal("movi: literal in loop",
            "local s = 0\nfor x = 1 to 10 { s = s + 3 }",
            "s", 30);
  // Int literal inside a generator (generator dispatch path).
  expectVal("movi: literal in generator",
            "def g() { yield 5; yield 7 }\n"
            "local gen = g()\nlocal a = gen()\nlocal b = gen()\n"
            "local s = a + b;",
            "s", 12);
  // Typed declaration: MOVI into the target register + in-place CONV.
  expectVal("movi: typed byte decl", "local a:byte = 0;", "a", 0);
  expectVal("movi: typed short decl", "local a:short = -5;", "a", -5);
  // Plain assignment of an Int literal (not just declarations).
  expectVal("movi: plain assignment", "local a = 9\na = 42;", "a", 42);

  // -- ADDI (SPEC §bytecodes: new OpCode addi) --
  // Compound `+=` with an Int-literal RHS adds in place (no temp, no pool).
  expectVal("addi: basic", "local a = 10\na += 12;", "a", 22);
  expectVal("addi: negative immediate", "local a = 10\na += -5;", "a", 5);
  // Float promotion preserved (routes through the ADD binary path).
  expectStr("addi: float promotion",
            "local f = 1.5\nf += 1\nlocal s = toStr(f);",
            "s", "2.5");
  // String RHS is not an Int literal: stays on the string concat path.
  expectStr("addi: string += untouched", "local s = \"x\"\ns += \" y\";",
            "s", "x y");
  // Index target (ARRGET temp + ADDI in place).
  expectVal("addi: index target", "local a = [10, 20]\na[1] += 5\nlocal r = a[1];",
            "r", 25);
  // Member target (TABGET temp + ADDI in place).
  expectVal("addi: member target", "local t = {k=1}\nt.k += 4\nlocal r = t.k;",
            "r", 5);
  // Captured variable: `cap += 5` adds 5, not INC (regression for the
  // pre-ADDI bug where any captured compound op compiled to INC/DEC).
  expectVal("addi: closure var += n",
            "local x = 1\nlocal f = (y)[x] { x += 5; return x }\nlocal r = f(0);\n",
            "r", 6);
  // Loop: ADDI dispatch in the loop paths.
  expectVal("addi: literal in loop", "local s = 0\nfor x = 1 to 10 { s += 3 }",
            "s", 30);
  // Generator: ADDI across yields (generator dispatch path).
  expectVal("addi: literal in generator",
            "def g() { local y = 10; yield y; y += 7; yield y }\n"
            "local gen = g()\nlocal a = gen()\nlocal b = gen()\n"
            "local s = a + b;",
            "s", 27);
  // int32 wrap-around, identical to ADD.
  expectVal("addi: int32 wrap", "local a = 2147483647\na += 1;", "a",
            -2147483648);

  // -- SUBI (SPEC §bytecodes: new OpCode subi) --
  // Compound `-=` with an Int-literal RHS subtracts in place (no temp, no
  // pool). Mirrors the ADDI block.
  expectVal("subi: basic", "local a = 10\na -= 12;", "a", -2);
  expectVal("subi: negative immediate", "local a = 10\na -= -5;", "a", 15);
  // Plain `n = n - 1` form (the spec's subi example) also fuses.
  expectVal("subi: n = n - 1", "local n = 5\nn = n - 1;", "n", 4);
  // Float promotion preserved (routes through the SUB binary path).
  expectStr("subi: float promotion",
            "local f = 1.5\nf -= 1\nlocal s = toStr(f);",
            "s", "0.5");
  // Non-Int (float) RHS is not an Int literal: stays on the pool + SUB path.
  expectStr("subi: float RHS pooled",
            "local f = 5.5\nf -= 2.5\nlocal s = toStr(f);",
            "s", "3");
  // Index target (ARRGET temp + SUBI in place).
  expectVal("subi: index target",
            "local a = [10, 20]\na[1] -= 5\nlocal r = a[1];",
            "r", 15);
  // Member target (TABGET temp + SUBI in place).
  expectVal("subi: member target",
            "local t = {k=1}\nt.k -= 4\nlocal r = t.k;",
            "r", -3);
  // Captured variable: `cap -= 5` subtracts 5 (not DEC).
  expectVal("subi: closure var -= n",
            "local x = 10\nlocal f = (y)[x] { x -= 5; return x }\nlocal r = f(0);\n",
            "r", 5);
  // Loop: SUBI dispatch in the loop paths.
  expectVal("subi: literal in loop", "local s = 30\nfor x = 1 to 10 { s -= 3 }",
            "s", 0);
  // Generator: SUBI across yields (generator dispatch path).
  expectVal("subi: literal in generator",
            "def g() { local y = 10; yield y; y -= 7; yield y }\n"
            "local gen = g()\nlocal a = gen()\nlocal b = gen()\n"
            "local s = a - b;",
            "s", 7);
  // int32 wrap-around, identical to SUB.
  expectVal("subi: int32 wrap", "local a = -2147483648\na -= 1;", "a",
            2147483647);

  // -- CMP/CMPI + j* (SPEC §bytecodes: alternate conditional jump) --
  // Int-vs-Int (literal) comparisons in `if` fuse to cmp/cmpi + a
  // conditional jump. All six relations, both directions, and the boundary
  // (== vs +/- 1) so every j* is exercised.
  expectVal("cmp: if > true", "local r = 0\nif (5 > 3) { r = 1 }", "r", 1);
  expectVal("cmp: if > false", "local r = 0\nif (3 > 5) { r = 1 }", "r", 0);
  expectVal("cmp: if < true", "local r = 0\nif (3 < 5) { r = 1 }", "r", 1);
  expectVal("cmp: if < false", "local r = 0\nif (5 < 3) { r = 1 }", "r", 0);
  expectVal("cmp: if == true", "local r = 0\nif (5 == 5) { r = 1 }", "r", 1);
  expectVal("cmp: if == false", "local r = 0\nif (5 == 6) { r = 1 }", "r", 0);
  expectVal("cmp: if != true", "local r = 0\nif (5 != 6) { r = 1 }", "r", 1);
  expectVal("cmp: if != false", "local r = 0\nif (5 != 5) { r = 1 }", "r", 0);
  // Boundary: <= and >= at the equal point (exercise jg/jle with EQ flag).
  expectVal("cmp: if <= equal", "local r = 0\nif (5 <= 5) { r = 1 }", "r", 1);
  expectVal("cmp: if <= over", "local r = 0\nif (6 <= 5) { r = 1 }", "r", 0);
  expectVal("cmp: if >= equal", "local r = 0\nif (5 >= 5) { r = 1 }", "r", 1);
  expectVal("cmp: if >= under", "local r = 0\nif (4 >= 5) { r = 1 }", "r", 0);
  // else-branch taken when the comparison is false.
  expectVal("cmp: if/else false->else",
            "local r = 0\nif (3 > 5) { r = 1 } else { r = 2 }", "r", 2);
  // Negative immediates through cmpi.
  expectVal("cmp: if negative literal", "local r = 0\nif (-5 < 3) { r = 1 }",
            "r", 1);
  // Non-Int (float) comparison stays on the legacy pooled path (no fusion).
  expectVal("cmp: float comparison unchanged",
            "local r = 0\nif (1.5 < 2.5) { r = 1 }", "r", 1);
  // Non-Int (string) comparison stays on the legacy path.
  expectVal("cmp: string comparison unchanged",
            "local r = 0\nif (\"a\" == \"a\") { r = 1 }", "r", 1);
  // for-range guard (Int vs Int) fused to cmp+j*: forward and backward.
  expectVal("cmp: for forward guard", "local s = 0\nfor x = 1 to 5 { s = s + 1 }",
            "s", 5);
  expectVal("cmp: for backward guard",
            "local s = 0\nfor x = 5 to 1 { s = s + 1 }", "s", 5);
  // for-in (array) guard `idx < len` fused: exercises the jge path.
  expectVal("cmp: for-in array guard",
            "local a = [10, 20, 30]\nlocal s = 0\nfor v in a { s = s + v }",
            "s", 60);

  // --- SPEC # function scope isolation (spec 14, 2026-11-01 example) ---
  // `a ++` inside a function must not touch global `a`: globals are
  // invisible in function scope, so the program fails to compile and the
  // global stays put (only `global a` can reach it).
  expectThrow("fn scope isolation: a++ in fn",
              "a = 10\ndef make_test() {\n a ++\n}\nmake_test()");
  // A `local a` inside a function shadows the global; the global is
  // untouched after the call.
  expectVal("fn scope isolation: local a in fn",
            "a = 10\ndef make_test() { local a = 5; a ++ }\nmake_test()\nlocal r = a;",
            "r", 10);
  // Spec 14 if-block variant: `local a` in an if body does not leak out.
  expectVal("fn scope isolation: if-block local a",
            "a = 10\nb = 20\nif (b == 20) { local a = 20 }\nlocal r = a;",
            "r", 10);
  // Block scope in a `for` body (spec 14 loop example, range form).
  expectStr("block scope for body",
            "local j = 0\nfor i in range(0, 10) {\n local j = i\n}\nlocal s = $j;",
            "s", "0");
  // Block scope in a `while` body with a real body (spec 14 loop example).
  expectStr("block scope while body",
            "local j = 0\nlocal i = 0\nwhile (i < 10) {\n i = i + 1;\n local j = i;\n}"
            "\nlocal s = $j;",
            "s", "0");

  // --- SPEC §boolean evaluation table (spec 03) ---
  // string: empty => false; non-empty => true
  expectVal("bool table: empty string falsy",
            "local n = 0\nif \"\" { n = 1; }\nlocal r = n;", "r", 0);
  expectVal("bool table: non-empty string truthy",
            "local n = 0\nif \"x\" { n = 1; }\nlocal r = n;", "r", 1);
  // array: empty => false; non-empty => true
  expectVal("bool table: empty array falsy",
            "local n = 0\nif [] { n = 1; }\nlocal r = n;", "r", 0);
  expectVal("bool table: non-empty array truthy",
            "local n = 0\nif [1, 2] { n = 1; }\nlocal r = n;", "r", 1);
  // table: empty => false; non-empty => true
  expectVal("bool table: empty table falsy",
            "local t = {}\nlocal n = 0\nif t { n = 1; }\nlocal r = n;", "r", 0);
  expectVal("bool table: non-empty table truthy",
            "local t = {a = 1}\nlocal n = 0\nif t { n = 1; }\nlocal r = n;",
            "r", 1);
  // blob: empty => false (the non-empty row is value-level below; the
  // language can only construct empty blobs)
  expectVal("bool table: empty blob falsy",
            "local b = blob()\nlocal n = 0\nif b { n = 1; }\nlocal r = n;",
            "r", 0);
  // number: 0 => false; non-zero => true (int and float)
  expectVal("bool table: 0 falsy", "local n = 0\nif 0 { n = 1; }\nlocal r = n;",
            "r", 0);
  expectVal("bool table: non-zero int truthy",
            "local n = 0\nif 7 { n = 1; }\nlocal r = n;", "r", 1);
  expectVal("bool table: 0.0 falsy", "local n = 0\nif 0.0 { n = 1; }\nlocal r = n;",
            "r", 0);
  expectVal("bool table: non-zero float truthy",
            "local n = 0\nif 0.5 { n = 1; }\nlocal r = n;", "r", 1);
  // nil => false (a closure with no return yields nil)
  expectVal("bool table: nil falsy",
            "local f = ()[] { }\nlocal r = f()\nlocal n = 0\nif r { n = 1; }\nlocal s = n;",
            "s", 0);
  // generator => true
  expectVal("bool table: generator truthy",
            "def g() { yield 1 }\nlocal gen = g()\nlocal n = 0\nif gen { n = 1; }"
            "\nlocal r = n;",
            "r", 1);
  // function/lambda: truthiness evaluated by its return value
  expectVal("bool table: lambda returning 1 truthy",
            "local f = ()[] { return 1 }\nlocal n = 0\nif f { n = 1; }\nlocal r = n;",
            "r", 1);
  expectVal("bool table: lambda returning 0 falsy",
            "local f = ()[] { return 0 }\nlocal n = 0\nif f { n = 1; }\nlocal r = n;",
            "r", 0);
  expectVal("bool table: lambda returning string truthy",
            "local f = ()[] { return \"hi\" }\nlocal n = 0\nif f { n = 1; }\nlocal r = n;",
            "r", 1);
  expectVal("bool table: lambda returning empty string falsy",
            "local f = ()[] { return \"\" }\nlocal n = 0\nif f { n = 1; }\nlocal r = n;",
            "r", 0);
  // !/&&/|| share the same truthiness (spec 03 applies to every
  // boolean context)
  expectVal("bool table: ! of empty array", "local r = ! [];", "r", 1);
  expectVal("bool table: generator && true",
            "def g() { yield 1 }\nlocal gen = g()\nlocal r = gen && true;",
            "r", 1);
  expectVal("bool table: empty array || true",
            "local r = [] || true;", "r", 1);

  // --- SPEC §boolean evaluation table (spec 03), value level ---
  // Rows not expressible in QLang source (a non-empty blob: the language
  // can only build empty blobs) are checked directly against
  // QLangVM::isTrue.
  {
    QLangContext ctx;
    QLangLexer lexer("local a = 1");
    QLangParser parser(lexer.tokenize());
    QLangCompiler compiler(parser.parseProgram(), &ctx);
    QLangVM vm(compiler.compile(), ctx);
    auto check = [&](const char *what, bool got, bool want) {
      if (got == want) {
        std::cout << "[PASS] bool value-level: " << what << "\n";
        passes++;
      } else {
        std::cout << "[FAIL] bool value-level: " << what << " (want "
                  << (want ? "true" : "false") << ")\n";
        fails++;
      }
    };
    check("empty blob falsy", vm.isTrue(QLangValue::Blob()), false);
    check("non-empty blob truthy",
          vm.isTrue(QLangValue::Blob(std::vector<uint8_t>{1, 2, 3})), true);
    // sanity: rows also expressible in scripts
    check("nil falsy", vm.isTrue(QLangValue()), false);
    check("0 falsy", vm.isTrue(QLangValue::Int(0)), false);
    check("non-zero int truthy", vm.isTrue(QLangValue::Int(5)), true);
    check("0.0 falsy", vm.isTrue(QLangValue::Float(0.0)), false);
    check("0.5 truthy", vm.isTrue(QLangValue::Float(0.5)), true);
    check("non-empty array truthy",
          vm.isTrue(QLangValue::Array({QLangValue::Int(1)})), true);
    check("non-empty string truthy",
          vm.isTrue(QLangValue::String("x")), true);
  }

  // --- SPEC # execution trace: nested frames beyond the Round-29 cases ---
  // Five nested frames: one `at line` per frame, innermost first, main last.
  expectMsg("trace: 5-frame chain",
            "def e()\n  local x = 1 / 0\nend\n"
            "def d()\n  e()\nend\n"
            "def c()\n  d()\nend\n"
            "def b()\n  c()\nend\n"
            "def a()\n  b()\nend\n"
            "a()",
            "in function 'e'",
            "at line 2 in e",
            "at line 5 in d",
            "at line 8 in c",
            "at line 11 in b",
            "at line 14 in a",
            "at line 16 in main");
  // Lambda frames are named after their binding variable.
  expectMsg("trace: lambda frame",
            "local f = ()[] {\n  local x = 1 / 0\n}\nf()",
            "in function 'f'",
            "at line 2 in f",
            "at line 4 in main");
  // Lambda nested in a function: three frames.
  expectMsg("trace: lambda in function",
            "def outer() {\n  local f = ()[] {\n    local x = 1 / 0\n  }\n  f()\n}\n"
            "outer()",
            "in function 'f'",
            "at line 3 in f",
            "at line 5 in outer",
            "at line 7 in main");
  // Generator frames appear in the chain.
  expectMsg("trace: generator frame",
            "def gen() {\n  yield 1\n  local x = 1 / 0\n}\n"
            "local g = gen()\ng()\ng()",
            "in function 'gen'",
            "at line 3 in gen",
            "at line 7 in main");
  // Class method frames are named after the method.
  expectMsg("trace: class method frame",
            "class C {\n  boom(self)[] {\n    local x = 1 / 0\n  }\n}\n"
            "local c = C()\nc.boom()",
            "in function 'boom'",
            "at line 3 in boom",
            "at line 7 in main");
  // Method calling method: three frames.
  expectMsg("trace: method calling method",
            "class C {\n"
            "  boom(self)[] {\n    self.inner()\n  }\n"
            "  inner(self)[] {\n    local x = 1 / 0\n  }\n"
            "}\n"
            "local c = C()\nc.boom()",
            "in function 'inner'",
            "at line 6 in inner",
            "at line 3 in boom",
            "at line 10 in main");
  // Header carries the column as well as the line.
  expectMsg("trace: header has col",
            "local a = 1\nlocal b = 2\nlocal c = a + \"x\"",
            "line 3, col 1 in function 'main'");

  // --- SPEC §operators "operator ?" (short-hand if / ternary) ---
  // Both branches (spec examples).
  expectStr("ternary true branch",
            "local a = 3\nlet b = a == 3 ? \"This is Three\" : \"This is NOT Three\";",
            "b", "This is Three");
  expectStr("ternary false branch",
            "local a = 3\nlet b = a == 5 ? \"This is Three\" : \"This is NOT Three\";",
            "b", "This is NOT Three");
  // Nesting: right-associative, a ? b : c ? d : e == a ? b : (c ? d : e).
  expectVal("ternary nested true", "local r = 1 == 1 ? 10 : 2 == 2 ? 20 : 30;", "r", 10);
  expectVal("ternary nested middle", "local r = 1 == 2 ? 10 : 2 == 2 ? 20 : 30;", "r", 20);
  expectVal("ternary nested false", "local r = 1 == 2 ? 10 : 2 == 3 ? 20 : 30;", "r", 30);
  // Block form (spec example): the block's `return` yields the value.
  expectStr("ternary block form true",
            "local a = 5\nlet c = a == 5 ? {\n  return \"this is 5\"\n} : {\n  return \"this is not 5\"\n};",
            "c", "this is 5");
  expectStr("ternary block form false",
            "local a = 3\nlet c = a == 5 ? {\n  return \"this is 5\"\n} : {\n  return \"this is not 5\"\n};",
            "c", "this is not 5");
  // Mixed branches: expression on one side, block on the other.
  expectStr("ternary mixed branches",
            "local a = 3\nlet c = a == 5 ? \"this is 5\" : {\n  return \"this is not 5\"\n};",
            "c", "this is not 5");
  // Block branches capture free variables (lowered to a closure call).
  expectVal("ternary block captures free var",
            "local x = 7\nlocal r = x > 5 ? {\n  return x * 2\n} : {\n  return 0\n};",
            "r", 14);
  // Precedence: `?` binds looser than every binary operator, incl. && / ||.
  expectVal("ternary precedence &&", "local r = 1 && 0 ? 10 : 20;", "r", 20);
  expectVal("ternary precedence ||", "local r = 0 || 1 ? 10 : 20;", "r", 10);
  expectVal("ternary precedence arithmetic", "local r = 1 + 2 == 3 ? 10 : 20;", "r", 10);
  // Ternary inside a function body: free vars in all three sub-expressions
  // must be captured by the enclosing closure.
  expectVal("ternary in closure captures",
            "def f(x) {\n  local y = 10\n  return x > 0 ? y + 1 : y - 1\n}\nlocal r = f(5);",
            "r", 11);
  expectVal("ternary in closure captures false side",
            "def f(x) {\n  local y = 10\n  return x > 0 ? y + 1 : y - 1\n}\nlocal r = f(-5);",
            "r", 9);
  // Block form inside a function body.
  expectVal("ternary block in closure",
            "def f(x) {\n  local y = 4\n  return x > 0 ? {\n    return y * 2\n  } : {\n    return y * 3\n  }\n}\nlocal r = f(1);",
            "r", 8);
  // Nested ternary with block branches.
  expectStr("ternary nested block",
            "local a = 2\nlocal r = a == 1 ? \"one\" : a == 2 ? {\n  return \"two\"\n} : \"other\";",
            "r", "two");

  // -- deepcopy (Phase 1: Opcode::DEEPCOPY 70, Builtin::Deepcopy) --
  // SPEC 01 value semantics, 06 ### deepcopy, 10 ## deepcopy, 13 ## OpCode deepcopy.
  expectVal("deepcopy int", "local a = 42\nlocal b = deepcopy(a);", "b", 42);
  expectStr("deepcopy string by value", "local a = \"hi\"\nlocal b = deepcopy(a);", "b", "hi");
  expectVal("deepcopy bool", "local a = true\nlocal b = deepcopy(a)\nlocal r = b ? 1 : 0;", "r", 1);
  // Independence: mutating the copy never touches the original (index
  // assignment also exercises the compileAssignTarget read-modify-write fix).
  expectVal("deepcopy array independent", "local a = [1,2,3,4]\nlocal b = deepcopy(a)\nb[0] = 55\nlocal r = a[0];", "r", 1);
  expectVal("deepcopy array copy mutated", "local a = [1,2,3,4]\nlocal b = deepcopy(a)\nb[0] = 55\nlocal r = b[0];", "r", 55);
  expectVal("deepcopy nested array independent", "local a = [[1,2],[3,4]]\nlocal b = deepcopy(a)\nb[0] = [9,9]\nlocal r = a[0][0];", "r", 1);
  // (chained member assign is not in the grammar; mutate via a local alias,
  // which shares the nested table handle inside the copy)
  expectVal("deepcopy nested table independent", "local t = {inner = {n = 1}}\nlocal u = deepcopy(t)\nlocal w = u.inner\nw.n = 2\nlocal r = t.inner.n;", "r", 1);
  expectVal("deepcopy nested table copy mutated", "local t = {inner = {n = 1}}\nlocal u = deepcopy(t)\nlocal w = u.inner\nw.n = 2\nlocal r = u.inner.n;", "r", 2);
  // Class instances: data fields recurse (independent), methods work on the
  // copy, type identity preserved (class-level meta keys shared).
  expectVal("deepcopy instance field independent",
            "class Point {\n  x = 0\n  move(self, dx)[] {\n    self.x = self.x + dx\n  }\n}\nlocal p = Point()\np.x = 1\nlocal q = deepcopy(p)\nq.move(5)\nlocal r = p.x;", "r", 1);
  expectVal("deepcopy instance copy mutated",
            "class Point {\n  x = 0\n  move(self, dx)[] {\n    self.x = self.x + dx\n  }\n}\nlocal p = Point()\np.x = 1\nlocal q = deepcopy(p)\nq.move(5)\nlocal r = q.x;", "r", 6);
  expectStr("deepcopy instance type preserved",
            "class Point {\n  x = 0\n}\nlocal p = Point()\nlocal q = deepcopy(p)\nlocal tn = type(q);", "tn", "Point");
  // Arity: exactly one argument (compile-time, SPEC 06 ### deepcopy).
  expectThrow("deepcopy no args", "local a = deepcopy()");
  expectThrow("deepcopy two args", "local a = 1\nlocal b = deepcopy(a, a)");
  // Handle sharing + cycle safety (C++-level pointer identity; the run
  // terminating at all proves the cycle walk is bounded).
  try {
    QLangContext dctx;
    dctx.run(
        "local f = (x)[] {\n  return x\n}\nlocal g = deepcopy(f)\n"
        "def gen_range(mi, mx) {\n  for i = mi to mx { yield i }\n}\n"
        "local gen = gen_range(1, 3)\nlocal h = deepcopy(gen)\n"
        "local t = {a = 1}\nt.self = t\nlocal u = deepcopy(t)");
    expectTrue("deepcopy function shares handle",
               &dctx.get("f").asFunction() == &dctx.get("g").asFunction());
    expectTrue("deepcopy generator shares handle",
               &dctx.get("gen").asGenerator() == &dctx.get("h").asGenerator());
    // Bind the QLangValue itself: get() returns by value, and a reference
    // into the temporary's member would dangle (no lifetime extension).
    const auto &uval = dctx.get("u");
    const auto &umap = uval.asTable();
    expectTrue("deepcopy table cycle u.self is u copy",
               umap->at("self").asTable().get() == umap.get());
    expectTrue("deepcopy table cycle u.self.self is u copy",
               umap->at("self").asTable()->at("self").asTable().get() == umap.get());
  } catch (const QLangError &e) {
    std::cout << "[FAIL] id:   deepcopy identity run -> " << e.what() << "\n";
    fails++;
  }
  try {
    QLangContext dctx;
    dctx.run("class Point {\n  x = 0\n}\nlocal p = Point()\nlocal q = deepcopy(p)\nlocal t = {a = 1}\nlocal u = deepcopy(t)");
    expectTrue("deepcopy table payload independent",
               dctx.get("t").asTable().get() != dctx.get("u").asTable().get());
    expectTrue("deepcopy instance payload independent",
               dctx.get("p").asTable().get() != dctx.get("q").asTable().get());
    expectTrue("deepcopy instance shares __class__",
               dctx.get("p").asTable()->at("__class__").asString() ==
                   dctx.get("q").asTable()->at("__class__").asString());
  } catch (const QLangError &e) {
    std::cout << "[FAIL] id:   deepcopy payload run -> " << e.what() << "\n";
    fails++;
  }

  // -- __deepcopy__ override (Phase 3, SPEC 10 ## deepcopy) --
  // A table carrying a callable `__deepcopy__` is copied by that hook: it
  // runs on the ORIGINAL (self = source), its return value IS the copy, and
  // it may throw to refuse. nil/self returns are runtime errors.
  expectVal("deepcopy hook return value used",
            "class Point {\n  x = 0\n  __deepcopy__(self) [ ] {\n    local c = {}\n    c.x = self.x + 100\n    return c\n  }\n}\nlocal p = Point()\np.x = 7\nlocal q = deepcopy(p)\nlocal r = q.x;",
            "r", 107);
  expectVal("deepcopy hook original untouched",
            "class Point {\n  x = 0\n  __deepcopy__(self) [ ] {\n    local c = {}\n    c.x = self.x + 100\n    return c\n  }\n}\nlocal p = Point()\np.x = 7\nlocal q = deepcopy(p)\nlocal r = p.x;",
            "r", 7);
  expectThrow("deepcopy hook returns self",
              "class Point {\n  x = 0\n  __deepcopy__(self) [ ] {\n    return self\n  }\n}\nlocal p = Point()\nlocal q = deepcopy(p)");
  expectThrow("deepcopy hook returns nil",
              "class Point {\n  x = 0\n  __deepcopy__(self) [ ] {\n    return nil\n  }\n}\nlocal p = Point()\nlocal q = deepcopy(p)");
  expectMsg("deepcopy hook throws to refuse",
            "class Point {\n  x = 0\n  __deepcopy__(self) [ ] {\n    assert(false, \"refuse\")\n    return {}\n  }\n}\nlocal p = Point()\nlocal q = deepcopy(p)",
            "refuse");
  // Re-entrancy: a hook that deep-copies `self` re-enters on the same
  // payload; the nested call skips the hook and takes the default field
  // copy, so the recursion terminates (the run finishing proves it).
  expectVal("deepcopy hook deepcopy(self) terminates",
            "class Point {\n  x = 0\n  __deepcopy__(self) [ ] {\n    local c = deepcopy(self)\n    return c\n  }\n}\nlocal p = Point()\np.x = 9\nlocal q = deepcopy(p)\nlocal r = q.x;",
            "r", 9);
  // Nested instance: the outer class has no hook; the default field walk
  // dispatches the INNER instance's hook.
  expectVal("deepcopy nested instance hook dispatched",
            "class Inner {\n  v = 0\n  __deepcopy__(self) [ ] {\n    local c = {}\n    c.v = self.v * 2\n    return c\n  }\n}\nclass Outer {\n  inner = nil\n}\nlocal o = Outer()\nlocal i = Inner()\ni.v = 21\no.inner = i\nlocal o2 = deepcopy(o)\nlocal r = o2.inner.v;",
            "r", 42);
  // Identity: two references to the same hooked original share ONE hook
  // result (the hook result is registered in `seen`), and the hook runs
  // exactly once for both.
  try {
    QLangContext dctx;
    dctx.run(
        "local cnt = 0\n"
        "class Point {\n  x = 0\n  __deepcopy__(self) [cnt] {\n    cnt = cnt + 1\n    local c = {}\n    c.x = self.x\n    return c\n  }\n}\n"
        "local p = Point()\n"
        "local t = {}\n"
        "t.a = p\n"
        "t.b = p\n"
        "local u = deepcopy(t)");
    expectTrue("deepcopy hook runs once for shared original",
               toInt64(dctx.get("cnt")) == 1);
    // Copy the handle: get() returns by value, so a reference into its
    // temporary's member dangles (no lifetime extension for a member call).
    const auto umap = dctx.get("u").asTable();
    expectTrue("deepcopy hook result shared by both refs",
               umap->at("a").asTable().get() == umap->at("b").asTable().get());
  } catch (const QLangError &e) {
    std::cout << "[FAIL] id:   deepcopy hook identity run -> " << e.what() << "\n";
    fails++;
  }
  // `__destroy__` interaction: the copy is a new instance with its OWN
  // finalization; releasing the original finalizes only the original, and
  // releasing the copy later finalizes the copy (SPEC 10 ## deepcopy).
  try {
    QLangContext dctx;
    dctx.run("local n = 0\n"
             "class D {\n  x = 0\n  __destroy__ (self) [n] { n = n + 1 }\n}\n"
             "local a = D()\n"
             "local b = deepcopy(a)\n"
             "a = nil\n");
    dctx.processFinalizers();
    expectTrue("deepcopy copy finalizes independently (original ran)",
               toInt64(dctx.get("n")) == 1);
    dctx.run("b = nil");
    dctx.processFinalizers();
    expectTrue("deepcopy copy finalizes independently (copy ran too)",
               toInt64(dctx.get("n")) == 2);
  } catch (const QLangError &e) {
    std::cout << "[FAIL] id:   deepcopy destroy run -> " << e.what() << "\n";
    fails++;
  }
  // Idempotent attach: a hook returning an instance the constructor already
  // registered must NOT be wrapped a second time — a double registration
  // would enqueue `__destroy__` twice for the returned instance (n would
  // reach 3). Also exercises the hook returning a pre-built instance. The
  // twin is a GLOBAL (captures alias the global slot, so no closure->value
  // reference cycle forms through the instance table).
  try {
    QLangContext dctx;
    dctx.run("local n = 0\n"
             "local twin = nil\n"
             "class D {\n  x = 0\n"
             "  __destroy__ (self) [n] { n = n + 1 }\n"
             "  __deepcopy__ (self) [twin] { return twin }\n}\n"
             "local a = D()\n"
             "twin = D()\n"
             "local c = deepcopy(a)\n");
    expectTrue("deepcopy hook result is the pre-built instance",
               dctx.get("c").asTable().get() == dctx.get("twin").asTable().get());
    dctx.run("a = nil\ntwin = nil\nc = nil");
    dctx.processFinalizers();
    expectTrue("deepcopy hook result finalized exactly once",
               toInt64(dctx.get("n")) == 2);
  } catch (const QLangError &e) {
    std::cout << "[FAIL] id:   deepcopy idempotent attach run -> " << e.what() << "\n";
    fails++;
  }

  // -- polymorphic subscript: INDEXGET / INDEXPUT (spec 13-bytecodes
  // "indexget"/"indexput"). SPEC §declarations: `t["age"] = 33  # valid;
  // equivalent to t.age`, `t[] = 44  # error`. The base kind is resolved at
  // runtime: arrays index, tables/class instances go through ObjectOps, so the
  // closed field set and const guards apply to the subscript form too.
  expectVal(R"(table subscript read)", R"(
local t:table = {age=99}
local v = t["age"]
)", "v", 99);
  expectVal(R"(table subscript write is equivalent to member write)", R"(
local t:table = {age=99}
t["age"] = 33
local v = t.age
)", "v", 33);
  expectVal(R"(table subscript adds a new field)", R"(
local t:table = {a=1}
t["b"] = 7
local v = t.b
)", "v", 7);
  expectVal(R"(table subscript key may be a variable)", R"(
local k = "age"
local t:table = {age=99}
local v = t[k]
)", "v", 99);
  expectVal(R"(table subscript compound assign)", R"(
local t:table = {n=1}
t["n"] += 5
local v = t.n
)", "v", 6);
  expectVal(R"(array subscript read unchanged)", R"(
local ab = [0,4,5]
local v = ab[1]
)", "v", 4);
  expectVal(R"(array subscript write unchanged)", R"(
local ab = [0,4,5]
ab[0] = 55
local v = ab[0]
)", "v", 55);
  expectVal(R"(array subscript compound assign unchanged)", R"(
local ab = [0,4,5]
ab[1] += 10
local v = ab[1]
)", "v", 14);
  expectVal(R"(empty subscript still appends to arrays)", R"(
local ab = [0,4,5]
ab[] = 4
local v = len(ab)
)", "v", 4);
  expectVal(R"(class instance subscript read)", R"(
class D {
  x = 7
}
local a = D()
local v = a["x"]
)", "v", 7);
  expectVal(R"(class instance subscript write)", R"(
class D {
  x = 7
}
local a = D()
a["x"] = 9
local v = a.x
)", "v", 9);
  expectMsg(R"(class instance subscript respects the closed field set)", R"(
class D {
  x = 7
}
local a = D()
a["zz"] = 1
)", "has no field 'zz'");
  expectMsg(R"(class instance subscript respects const fields)", R"(
class D {
  const x = 7
}
local a = D()
a["x"] = 9
)", "cannot assign to const field 'x'");
  expectMsg(R"(subscript read on a scalar)", "local x = 5\nlocal v = x[0]",
            "cannot subscript int value");
  expectMsg(R"(subscript write on a scalar)", "local x = 5\nx[0] = 1",
            "cannot assign into int value");
  expectMsg(R"(empty subscript on a table is an error)", R"(
local t:table = {a=1}
t[] = 44
)", "cannot append to non-array value");
  expectMsg(R"(array subscript out of range)", R"(
local ab = [1,2]
local v = ab[5]
)", "array index out of range");

  std::cout << "\n==== " << passes << " passed, " << fails << " failed ====\n";
  return fails ? 1 : 0;
}