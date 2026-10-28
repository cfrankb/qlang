// cast<Type>(expr) tests
#include <iostream>
#include <string>
#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"
#include "qlang/QLangVM.h"  // for toInt64
using namespace qlang;

static int passes = 0, fails = 0;

static void expectVal(const std::string &name, const std::string &src,
                      const std::string &var, int64_t expect) {
  QLangContext ctx;
  try {
    ctx.run(src);
    QLangValue v = ctx.get(var);
    // Inline numeric extraction (toInt64 is not in a public header).
    int64_t got = 0;
    switch (v.type()) {
    case QLangValue::Type::Bool: got = v.asInt(); break;
    case QLangValue::Type::Char: got = v.asChar(); break;
    case QLangValue::Type::Byte: got = v.asByte(); break;
    case QLangValue::Type::Word: got = v.asWord(); break;
    case QLangValue::Type::Short: got = v.asShort(); break;
    case QLangValue::Type::Int: got = v.asInt(); break;
    case QLangValue::Type::Unsigned: got = static_cast<int64_t>(v.asUnsigned()); break;
    case QLangValue::Type::Qword: got = static_cast<int64_t>(v.asQword()); break;
    case QLangValue::Type::Float: got = static_cast<int64_t>(v.asFloat()); break;
    default: break;
    }
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

static void expectThrow(const std::string &name, const std::string &src) {
  QLangContext ctx;
  try { ctx.run(src); std::cout << "[FAIL] throw:" << name << " (ran, expected error)\n"; fails++; }
  catch (const QLangError &e) { std::cout << "[PASS] throw:" << name << " -> " << e.what() << "\n"; passes++; }
}

int main() {
  // cast<int>(float) — truncate decimals
  expectVal("cast int from float", "local a = cast<int>(1.12)\nlocal r = a;", "r", 1);
  // cast<float>(int) — toStr formats 1.0 as "1" (no trailing .0)
  expectStr("cast float from int", "local a = cast<float>(1)\nlocal s = toStr(a);", "s", "1");
  // cast<string>(int)
  expectStr("cast string from int", "local a = cast<string>(13)\nlocal s = a;", "s", "13");
  // cast<string>(float)
  expectStr("cast string from float", "local a = cast<string>(1.3)\nlocal s = a;", "s", "1.3");
  // cast<word>(-1) — removes signage
  expectVal("cast word from negative int", "local a = cast<word>(-1)\nlocal r = a;", "r", 65535);
  // cast<string>([]) — collections fallback via valueToString → "<array>"
  expectStr("cast string from empty array", "local a = cast<string>([])\nlocal s = a;", "s", "<array>");
  // cast<int>([]) — error: collections cannot be cast to int
  expectThrow("cast int from array", "local a = cast<int>([])");
  // cast<array>(123) — error: numbers cannot be cast to collections
  expectThrow("cast array from int", "local a = cast<array>(123)");
  // cast to string
  expectStr("cast to string", "local a = cast<string>(42)\nlocal s = a;", "s", "42");
  // cast to byte
  expectVal("cast to byte", "local a = cast<byte>(255)\nlocal r = a;", "r", 255);
  // cast to byte from negative
  expectVal("cast byte from negative", "local a = cast<byte>(-1)\nlocal r = a;", "r", 255);
  // cast to char
  expectVal("cast to char", "local a = cast<char>(65)\nlocal r = a;", "r", 65);
  // cast to short
  expectVal("cast to short", "local a = cast<short>(100)\nlocal r = a;", "r", 100);
  // cast to unsigned
  expectVal("cast to unsigned", "local a = cast<unsigned>(-5)\nlocal r = a;", "r", 4294967291u);
  // cast to qword
  expectStr("cast to qword", "local a = cast<qword>(4294967296)\nlocal s = toStr(a);", "s", "4294967296");
  // cast to bool
  expectVal("cast to bool from non-zero", "local a = cast<bool>(42)\nlocal r = a;", "r", 1);
  expectVal("cast to bool from zero", "local a = cast<bool>(0)\nlocal r = a;", "r", 0);

  // Nested cast: cast<float>(cast<int>(3.7)) → Float(3.0), toStr → "3" (%.15g drops .0)
  expectStr("nested cast float->int->float", "local a = cast<float>(cast<int>(3.7))\nlocal s = toStr(a);", "s", "3");

  // cast in expression context
  expectVal("cast in expr", "local r = cast<int>(2.9) + cast<int>(2.1);", "r", 4);

  // cast with nil
  expectVal("cast nil to int", "local z\nlocal a = cast<int>(z)\nlocal r = a;", "r", 0);

  std::cout << "\n==== " << passes << " passed, " << fails << " failed ====\n";
  return fails ? 1 : 0;
}
