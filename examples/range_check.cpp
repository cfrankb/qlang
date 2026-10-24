// range keyword empirical tests: for...in range (counter loop),
// `in range` (bounds check), and arr = range() (materialization).
#include <iostream>
#include <string>
#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"
using namespace qlang;

static int passes = 0, fails = 0;

static void expectVal(const std::string &name, const std::string &src,
                      const std::string &var, int64_t expect) {
  QLangContext ctx;
  try {
    ctx.run(src);
    int64_t got = toInt64(ctx.get(var));
    if (got == expect) { std::cout << "[PASS] val:  " << name << " (" << var << "=" << got << ")\n"; passes++; }
    else { std::cout << "[FAIL] val:  " << name << " (" << var << " got " << got << " exp " << expect << ")\n"; fails++; }
  } catch (const QLangError &e) { std::cout << "[FAIL] val:  " << name << " -> " << e.what() << "\n"; fails++; }
}

int main() {
  // ---- for i in range(a,b): inclusive counter loop (spec: while i <= b) ----
  expectVal("for range 0..10 sum", "local s=0\nfor i in range(0,10){ s += i }\n", "s", 55);   // 0+..+10
  expectVal("for range 0..10 count", "local c=0\nfor i in range(0,10){ c += 1 }\n", "c", 11);
  expectVal("for range 2..5 sum", "local s=0\nfor i in range(2,5){ s += i }\n", "s", 14);     // 2+3+4+5
  expectVal("for range 0..0", "local s=0\nfor i in range(0,0){ s += i }\n", "s", 0);          // just i=0
  expectVal("for range 5..5", "local s=0\nfor i in range(5,5){ s += i }\n", "s", 5);          // just i=5
  expectVal("for range 0..1", "local s=0\nfor i in range(0,1){ s += i }\n", "s", 1);          // 0,1
  expectVal("for range 3..7 count", "local c=0\nfor i in range(3,7){ c += 1 }\n", "c", 5);    // 3,4,5,6,7

  // ---- break / continue inside range loop ----
  expectVal("for range break at 5", "local s=0\nfor i in range(0,10){\n  if i>=5 { break }\n  s += i\n}\n", "s", 10); // 0+1+2+3+4
  expectVal("for range continue skip 5", "local s=0\nfor i in range(0,10){\n  if i==5 { continue }\n  s += i\n}\n", "s", 50); // 55-5

  // ---- `l in range(a,b)`: inclusive bounds check ----
  expectVal("in range inside=5", "local r=0\nif 5 in range(0,10){ r=1 }\n", "r", 1);
  expectVal("in range low=0", "local r=0\nif 0 in range(0,10){ r=1 }\n", "r", 1);
  expectVal("in range high=10", "local r=0\nif 10 in range(0,10){ r=1 }\n", "r", 1);   // inclusive
  expectVal("in range above=11", "local r=0\nif 11 in range(0,10){ r=1 }\n", "r", 0);  // exclusive
  expectVal("in range below=-1", "local r=0\nif -1 in range(0,10){ r=1 }\n", "r", 0);  // exclusive
  expectVal("in range as value", "local r=(3 in range(0,10))\n", "r", 1);

  // ---- arr = range(a,b): materialization unchanged ----
  expectVal("arr range len", "local a = range(0,5)\nlocal n = len(a)\n", "n", 6);
  expectVal("arr range first", "local a = range(0,5)\nlocal v = a[0]\n", "v", 0);
  expectVal("arr range last", "local a = range(0,5)\nlocal v = a[5]\n", "v", 5);

  // ---- non-range for-in still works (arrays) ----
  expectVal("for-in array", "local s=0\nfor x in [1,2,3]{ s += x }\n", "s", 6);
  // ---- non-range `in` still works (array membership) ----
  expectVal("in array present", "local r=0\nif 2 in [1,2,3]{ r=1 }\n", "r", 1);
  expectVal("in array absent", "local r=0\nif 9 in [1,2,3]{ r=1 }\n", "r", 0);

  // ---- generator: for i in range { yield i } ----
  expectVal("gen range count", "def gen_r(a,b){ for i in range(a,b){ yield i } }\nlocal g = gen_r(0,10)\nlocal c=0\nlocal v=g()\nwhile v != nil { c += 1; v = g() }\n", "c", 11); // 0..10

  std::cout << "\n" << passes << " passed, " << fails << " failed\n";
  return fails ? 1 : 0;
}
