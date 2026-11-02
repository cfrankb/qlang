// Empirical spec-coverage check for spec.md.
// Runs each snippet through QLangContext::run() and reports pass/fail.
#include <iostream>
#include <string>
#include <vector>
#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"
using namespace qlang;

static int passes = 0, fails = 0;

// Expect the snippet to RUN without throwing.
static void expectRun(const std::string &name, const std::string &src) {
  QLangContext ctx;
  try {
    ctx.run(src);
    std::cout << "[PASS] run:  " << name << "\n";
    passes++;
  } catch (const QLangError &e) {
    std::cout << "[FAIL] run:  " << name << " -> " << e.what() << "\n";
    fails++;
  }
}

// Expect the snippet to THROW.
static void expectThrow(const std::string &name, const std::string &src) {
  QLangContext ctx;
  try {
    ctx.run(src);
    std::cout << "[FAIL] throw:" << name << " (expected error, but ran)\n";
    fails++;
  } catch (const QLangError &e) {
    std::cout << "[PASS] throw:" << name << " -> " << e.what() << "\n";
    passes++;
  }
}

// Expect a specific final variable value.
static void expectVal(const std::string &name, const std::string &src,
                      const std::string &var, int expect) {
  QLangContext ctx;
  try {
    ctx.run(src);
    int got = ctx.get(var).asInt();
    if (got == expect) {
      std::cout << "[PASS] val:  " << name << " (" << var << "=" << got << ")\n";
      passes++;
    } else {
      std::cout << "[FAIL] val:  " << name << " (" << var << " got " << got
                << " expected " << expect << ")\n";
      fails++;
    }
  } catch (const QLangError &e) {
    std::cout << "[FAIL] val:  " << name << " -> " << e.what() << "\n";
    fails++;
  }
}

// Expect a specific final string value.
static void expectStr(const std::string &name, const std::string &src,
                      const std::string &var, const std::string &expect) {
  QLangContext ctx;
  try {
    ctx.run(src);
    std::string got = ctx.get(var).asString();
    if (got == expect) {
      std::cout << "[PASS] str:  " << name << " (" << var << "=\"" << got << "\")\n";
      passes++;
    } else {
      std::cout << "[FAIL] str:  " << name << " (" << var << "=\"" << got
                << "\" expected \"" << expect << "\")\n";
      fails++;
    }
  } catch (const QLangError &e) {
    std::cout << "[FAIL] str:  " << name << " -> " << e.what() << "\n";
    fails++;
  }
}

int main() {
  // --- constants keyword (spec line 24-27) ---
  expectRun("const int", "const year = 2026;");
  expectRun("const byte", "const month:byte = 10;");

  // --- comments ---
  expectRun("comment", "local a = 1; # a comment");

  // --- declarations ---
  expectRun("local int", "local a = 32;");
  expectRun("local byte no-semicolon", "local b:byte = 32");
  expectRun("local char->byte", "local bb = ' '");
  expectRun("local string", "local s = \"this is a string\"");
  expectRun("local multiline dquote", "\"\"\"\n   multilines string\n\"\"\"");
  expectRun("local multiline squote", "'''\n   multilines string\n'''");
  expectRun("local nil (no value)", "local z # this nil");
  expectRun("local float", "local f = 1.13");

  // --- copy / compound assign ---
  expectVal("copy a to c", "local a = 5;\nlocal c = a;", "c", 5);
  expectVal("a += 12", "local a = 32;\na += 12;", "a", 44);
  expectStr("string append", "local s = \"a\";\ns += \"abc\";", "s", "aabc");

  // --- type error cases ---
  expectThrow("sv += test (int+=string error)", "sv = 123\nsv += \"test\"");
  expectRun("sx += 123 (spec says test123)", "sx = \"test\"\nsx += 123");

  // --- for loops ---
  expectVal("for 1 to 32 count", "local n = 0\nfor x = 1 to 32\nn = n + 1\nnext x", "n", 32);
  expectVal("for to var", "local a = 5\nlocal n = 0\nfor x = 1 to a\nn = n + 1\nnext x", "n", 5);

  // --- if ---
  expectRun("if ==", "local a = 32\nif a == 32 then\na = 33\nend");
  expectRun("if <", "local a = 1\nif a < 32 then\na = 2\nend");
  expectRun("if >=", "local a = 32\nif a >= 32 then\na = 32\nend");
  expectRun("if ||", "local a = 32\nlocal b = 1\nif (a == 32) || (b == 32) then\na = 32\nend");

  // --- while ---
  expectVal("while dec", "local a = 5\nwhile (a > 0)\n--a\nend", "a", 0);

  // --- operators ---
  expectVal("shift left", "local a = 1\na = a << 4;", "a", 16);
  expectVal("shift right", "local a = 128\na = a >> 2;", "a", 32);
  expectVal("xor", "local a = 5\na = a ^ 3;", "a", 6);
  expectVal("bitand", "local a = 6\na = a & 3;", "a", 2);
  expectVal("bitor", "local a = 6\na = a | 3;", "a", 7);
  expectVal("land", "local a = 5\na = (a > 0) && (a < 10);", "a", 1);
  expectVal("lor", "local a = 5\na = (a > 10) || (a < 10);", "a", 1);
  expectVal("post-inc", "local a = 5\na++;\na = a;", "a", 6);

  // --- helpers toStr / toInt (spec line 99-100) ---
  expectRun("toStr call", "local a = 5\nlocal s = toStr(a);");
  expectRun("toInt call", "local s = \"5\"\nlocal a = toInt(s);");

  std::cout << "\n==== " << passes << " passed, " << fails << " failed ====\n";
  return fails ? 1 : 0;
}
