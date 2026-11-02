// Class `static` members (new extension beyond v1): static fields/methods in
// the shared `__statics__` table, `::` scope resolution, instance-level static
// access, and `static const`. Value-level assertions avoid stdout scraping.
#include <iostream>
#include <string>
#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"
using namespace qlang;

static int passes = 0, fails = 0;

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

static void expectThrow(const std::string &name, const std::string &src) {
  QLangContext ctx;
  try { ctx.run(src); std::cout << "[FAIL] throw: " << name << " (ran, expected error)\n"; fails++; }
  catch (const QLangError &e) { std::cout << "[PASS] throw: " << name << " -> " << e.what() << "\n"; passes++; }
}

static const std::string MONEY =
    "class Money {\n"
    "total = 0\n"
    "static banner = \"GreatAccount\"\n"
    "static const limit = 100\n"
    "static getBanner()[] { return banner }\n"
    "static setBanner(newBanner)[] { banner = newBanner }\n"
    "__init__ (self, v)[] { self.total = v }\n"
    "__str__ (self)[] { return f\"balance: {self.total} at {banner}\" }\n"
    "}\n";

int main() {
  // 1. static field read via class.
  expectStr("static field via class", MONEY + "local r = Money::banner;", "r", "GreatAccount");
  // 2. static field read via instance.
  expectStr("static field via instance", MONEY + "local m = Money(125)\nlocal r = m.banner;", "r", "GreatAccount");
  // 3. instance write mutates class-level storage.
  expectStr("instance write -> class", MONEY + "local m = Money(125)\nm.banner = \"Turtle\"\nlocal r = Money::banner;", "r", "Turtle");
  // 4. class write via instance visible across instances.
  expectStr("cross-instance shared", MONEY + "local m = Money(125)\nm.banner = \"Turtle\"\nlocal m2 = Money(1)\nlocal r = m2.banner;", "r", "Turtle");
  // 5. static method via class.
  expectStr("static method via class", MONEY + "Money::setBanner(\"X\")\nlocal r = Money::getBanner();", "r", "X");
  // 6. static method arity: getBanner takes 0 user args.
  expectThrow("static method arity", MONEY + "Money::getBanner(1);");
  // 7. f-string static via class.
  expectStr("f-string static via class", MONEY + "local r = f\"b={Money::banner}\";", "r", "b=GreatAccount");
  // 8. f-string static via instance.
  expectStr("f-string static via instance", MONEY + "local m = Money(5)\nlocal r = f\"b={m.banner}\";", "r", "b=GreatAccount");
  // 9. __str__ referencing a static.
  expectStr("__str__ references static", MONEY + "local m = Money(125)\nlocal r = str(m);", "r", "balance: 125 at GreatAccount");
  // 10. static const read via class + instance.
  expectVal("static const via class", MONEY + "local r = Money::limit;", "r", 100);
  expectVal("static const via instance", MONEY + "local m = Money(1)\nlocal r = m.limit;", "r", 100);
  // 11. static const write -> runtime error.
  expectThrow("static const write", MONEY + "local m = Money(1)\nm.limit = 5;");
  // 12. static local in class body -> compile error.
  expectThrow("static local", "class C { static local x = 1 }");
  // 13. static let in class body -> compile error.
  expectThrow("static let", "class C { static let x = 1 }");
  // 14. local static = 1 at top level -> compile error (reserved word).
  expectThrow("local static reserved", "local static = 1");
  // 15. C::missing on a class with statics -> compile error.
  expectThrow("missing static member", "class C { static a = 1 }\nlocal r = C::zz;");
  // 16. instance closed-field error intact (statics must not leak into schema).
  expectThrow("closed field intact", MONEY + "local m = Money(1)\nlocal r = m.notAField;");

  std::cout << "\n" << passes << " passed, " << fails << " failed\n";
  return fails ? 1 : 0;
}
