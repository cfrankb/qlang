#include <iostream>
#include <string>
#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"
using namespace qlang;

static int passes = 0, fails = 0;

static void expectVal(const std::string &name, const std::string &src,
                      const std::string &var, int expect) {
  QLangContext ctx;
  try {
    ctx.run(src);
    int got = toInt64(ctx.get(var));
    if (got == expect) { std::cout << "[PASS] val:  " << name << " (" << var << "=" << got << ")\n"; passes++; }
    else { std::cout << "[FAIL] val:  " << name << " (" << var << " got " << got << " exp " << expect << ")\n"; fails++; }
  } catch (const QLangError &e) { std::cout << "[FAIL] val:  " << name << " -> " << e.what() << "\n"; fails++; }
}

int main() {
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
  std::cout << "\n==== " << passes << " passed, " << fails << " failed ====\n";
  return fails ? 1 : 0;
}
