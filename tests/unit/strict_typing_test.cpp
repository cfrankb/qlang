// Strict typing (docs/spec/16-strict_typing.md, docs/plans/strict-typing-
// plan.md). M1: the QLangContext level API (STRICT_OFF/STRICT_DECLARED/
// STRICT_ALL, clamping, survives clear()) and the duplicate-declaration
// diagnostic matrix — `local a; local a` is silent at level 0 (the default,
// so legacy programs are untouched), an OUT_WARNING at level 1, and a compile
// error at level 2. Shadowing and parameters are deliberately not duplicates
// (Q5). M2: the annotation grammar — typed parameters, `-> type` return
// annotations and annotated assignments parse at every level and stay
// unchecked at level 0. M3: signature plumbing onto the Function record
// (behavior-neutral) and the level-2 `undeclared type for '<site>'` checks
// at definition sites. M4: the VM call-boundary and return checks at levels
// 1–2 with the spec's pinned messages.
#include <iostream>
#include <string>
#include <vector>

#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"

using namespace qlang;

static int passes = 0, fails = 0;

static void ok(const std::string &name, bool cond,
               const std::string &detail = "") {
  if (cond) { std::cout << "[PASS] " << name << "\n"; passes++; }
  else { std::cout << "[FAIL] " << name
                   << (detail.empty() ? "" : " (" + detail + ")") << "\n";
    fails++; }
}

namespace {

struct Msg {
  std::string text;
  int level;
};

// A context whose output callback records every message.
struct Captured {
  QLangContext ctx;
  std::vector<Msg> msgs;
  Captured() {
    ctx.setOutputCallback([this](const std::string &m, int level,
                                 const std::string &, int) {
      msgs.push_back({m, level});
    });
  }
  // Concatenation of all OUT_WARNING messages.
  std::string warnings() const {
    std::string out;
    for (const auto &m : msgs)
      if (m.level == OUT_WARNING) out += m.text;
    return out;
  }
};

// Run `src`; returns the thrown message, or "" when the run succeeded.
std::string runErr(QLangContext &ctx, const std::string &src) {
  try {
    ctx.run(src);
    return "";
  } catch (const QLangError &e) {
    return e.message();
  }
}

const char *kDup = "local a\nlocal a\n";
const char *kDupMsg = "duplicated variable \"a\" declaration within scope";

}  // namespace

int main() {
  // 1. API: default STRICT_OFF, named constants, round-trip, survives clear().
  {
    QLangContext ctx;
    ok("default level is STRICT_OFF",
       ctx.strictTypingLevel() == QLangContext::STRICT_OFF &&
           QLangContext::STRICT_OFF == 0 &&
           QLangContext::STRICT_DECLARED == 1 && QLangContext::STRICT_ALL == 2);
    ctx.setStrictTypingLevel(QLangContext::STRICT_ALL);
    ok("setter/getter round-trip",
       ctx.strictTypingLevel() == QLangContext::STRICT_ALL);
    ctx.clear();
    ok("level survives clear()",
       ctx.strictTypingLevel() == QLangContext::STRICT_ALL);
  }

  // 2. Out-of-range levels clamp to the nearest constant with a warning.
  {
    Captured cap;
    cap.ctx.setStrictTypingLevel(7);
    ok("high level clamps to STRICT_ALL",
       cap.ctx.strictTypingLevel() == QLangContext::STRICT_ALL);
    ok("clamp reports a warning",
       cap.warnings().find("clamped to 2") != std::string::npos,
       cap.warnings());
    cap.ctx.setStrictTypingLevel(-4);
    ok("low level clamps to STRICT_OFF",
       cap.ctx.strictTypingLevel() == QLangContext::STRICT_OFF);
  }

  // 3. Level matrix on `local a; local a`: silent / warning / error.
  {
    Captured off;  // level 0 (default)
    off.ctx.run(kDup);
    ok("level 0: silent, runs", off.msgs.empty());
  }
  {
    Captured warn;
    warn.ctx.setStrictTypingLevel(QLangContext::STRICT_DECLARED);
    std::string err = runErr(warn.ctx, kDup);
    ok("level 1: no error", err.empty(), err);
    ok("level 1: exact warning text",
       warn.warnings() == "warning: " + std::string(kDupMsg) + "\n",
       warn.warnings());
  }
  {
    Captured all;
    all.ctx.setStrictTypingLevel(QLangContext::STRICT_ALL);
    std::string err = runErr(all.ctx, kDup);
    ok("level 2: compile error with spec text", err == kDupMsg, err);
    ok("level 2: no warning emitted first", all.warnings().empty(),
       all.warnings());
  }

  // 4. `const` after a declaration is the same diagnostic.
  {
    Captured warn;
    warn.ctx.setStrictTypingLevel(QLangContext::STRICT_DECLARED);
    warn.ctx.run("local a\nconst a = 2\n");
    ok("level 1: const after local warns",
       warn.warnings() == "warning: " + std::string(kDupMsg) + "\n",
       warn.warnings());
  }

  // 5. Bare assignment then explicit declaration (03-declarations example).
  {
    Captured warn;
    warn.ctx.setStrictTypingLevel(QLangContext::STRICT_DECLARED);
    warn.ctx.run("t = 5\nlocal t = 9\n");
    ok("level 1: assigning then declaring warns",
       warn.warnings()
           == "warning: duplicated variable \"t\" declaration within scope\n",
       warn.warnings());
  }

  // 6. Deliberate non-matches stay silent at level 1 (Q5): block shadowing
  //    and `local` over a parameter.
  {
    Captured warn;
    warn.ctx.setStrictTypingLevel(QLangContext::STRICT_DECLARED);
    std::string err = runErr(warn.ctx,
                             "local a\nif true { local a = 5\nprintln(a) }\n"
                             "println(a)\n");
    ok("level 1: block shadowing is silent",
       err.empty() && warn.warnings().empty(), warn.warnings() + err);
    err = runErr(warn.ctx, "def f(a) { local a = 2\nreturn a }\n"
                           "local r = f(1)\nprintln(r)\n");
    ok("level 1: parameter is not a duplicate",
       err.empty() && warn.warnings().empty(), warn.warnings() + err);
  }

  // 7. M2 grammar: typed parameters, `-> type` return annotations and
  //    annotated assignments parse and run with today's (unchecked)
  //    semantics at level 0 — enforcement arrives with M3/M4.
  {
    Captured cap;  // level 0 (default)
    std::string err = runErr(cap.ctx,
                             "def hello_world(name:str, age:int) { }\n"
                             "hello_world(\"Tommy\", \"24\")\n"
                             "hello_world(112, 24)\n");
    ok("M2: spec annotated def parses; level 0 stays unchecked",
       err.empty() && cap.warnings().empty(), err + cap.warnings());
    err = runErr(cap.ctx,
                 "def sum_int2int(a:int, b:int) -> int { a += b }\n"
                 "sum_int2int(1, 2)\n");
    ok("M2: return annotation parses; no return check yet", err.empty(), err);
    err = runErr(cap.ctx,
                 "let hello_word = (name:str, age:int)[]->str{\n"
                 "    return \"hello \" + name\n"
                 "}\n"
                 "assert(hello_word(\"Tommy\", 24) == \"hello Tommy\", \"lambda\")\n");
    ok("M2: spec lambda literal parses and runs", err.empty(), err);
    err = runErr(cap.ctx,
                 "class Point {\n"
                 "  x = 0\n"
                 "  __init__(self, v)[] { self.x = v }\n"
                 "  scaled(self, k:int)[]->int { return self.x * k }\n"
                 "}\n"
                 "let p = Point(3)\n"
                 "assert(p.scaled(2) == 6, \"method\")\n");
    ok("M2: annotated class method parses and runs", err.empty(), err);
    err = runErr(cap.ctx,
                 "local t:table = { f = (a:int)[]->int { return a * 3 } }\n"
                 "assert(t.f(2) == 6, \"closure literal\")\n");
    ok("M2: annotated closure literal parses and runs", err.empty(), err);
  }

  // 8. Annotated assignment `age:byte = 24` re-coerces the live binding
  //    (params, block locals and function-scope names all resolve) and is
  //    not a duplicate-decl diagnostic at level 1 (Q5).
  {
    Captured cap;
    std::string err = runErr(cap.ctx,
                             "local age = 10\nage:byte = 24\n"
                             "assert(age == 24, \"retype\")\n"
                             "def f(p) { p:int = 9\nlocal b = 1\nb:byte = 2\n"
                             "if true { local q = 5\nq:int = 6\n"
                             "assert(q == 6, \"block\") }\n"
                             "return p + b }\n"
                             "assert(f(1) == 11, \"fn retype\")\n");
    ok("M2: annotated assignment re-coerces the live binding", err.empty(),
       err);
    cap.ctx.setStrictTypingLevel(QLangContext::STRICT_DECLARED);
    err = runErr(cap.ctx, "local b = 1\nb:byte = 2\n");
    ok("M2: re-type is not a duplicate-decl diagnostic (Q5)",
       err.empty() && cap.warnings().empty(), err + cap.warnings());
  }

  // 9. Malformed annotations stay parse errors; labels keep parsing (the
  //    `Ident ':'` re-type branch is gated on <builtin type> '=').
  {
    Captured cap;
    ok("M2: `def f(a int)` is a parse error",
       runErr(cap.ctx, "def f(a int) { }\n")
               .find("expected ')'") != std::string::npos);
    ok("M2: `->` without a type is a parse error",
       runErr(cap.ctx, "def f() -> { }\n")
               .find("expected return type after '->'") != std::string::npos);
    ok("M2: `-> nil` is not a return annotation",
       !runErr(cap.ctx, "def f() -> nil { }\n").empty());
    ok("M2: unknown annotation on assignment is a parse error",
       !runErr(cap.ctx, "x:notatype = 5\n").empty());
    ok("M2: label statements still parse (colon gate)",
       runErr(cap.ctx, "local a = 10\n:label1\n--a\nif a > 1 goto label1\n"
                       "assert(a == 1, \"label\")\n")
           .empty());
  }

  // 10. M3 — level-2 undeclared-type checks at definition sites (spec
  //     "level 2 — undeclared types"); exact diagnostic `undeclared type
  //     for '<name>'`. Exemptions: valueless `local z`, loop variables,
  //     class fields, hidden self/anchor. A missing `->` stays legal
  //     (enforced only where written).
  {
    Captured cap;
    cap.ctx.setStrictTypingLevel(QLangContext::STRICT_ALL);
    ok("M3: unannotated parameter is a compile error",
       runErr(cap.ctx, "def f(a) { }\n")
               .find("undeclared type for 'a'") != std::string::npos);
    ok("M3: annotated params without a return annotation are legal",
       runErr(cap.ctx, "def f(a:int) { return a }\nlocal r:int = f(1)\n")
           .empty());
    ok("M3: initialized declaration without a type is an error",
       runErr(cap.ctx, "local a = 1\n")
               .find("undeclared type for 'a'") != std::string::npos);
    ok("M3: annotated declaration is legal",
       runErr(cap.ctx, "local a:int = 1\n").empty());
    ok("M3: valueless local z is exempt (nil-implicit invariant)",
       runErr(cap.ctx, "local z\n").empty());
    ok("M3: unannotated const is an error; annotated const is legal",
       runErr(cap.ctx, "const k = 5\n")
               .find("undeclared type for 'k'") != std::string::npos &&
           runErr(cap.ctx, "const k:int = 5\n").empty());
    ok("M3: loop variables are exempt",
       runErr(cap.ctx, "for i = 1 to 3 { }\n").empty());
    ok("M3: re-type assignment stays legal (always annotated)",
       runErr(cap.ctx, "local a:int = 1\na:byte = 2\n").empty());
    ok("M3: unannotated closure-literal parameter is an error",
       runErr(cap.ctx, "local t:table = { f = (a)[] { return a } }\n")
               .find("undeclared type for 'a'") != std::string::npos);
    ok("M3: class fields exempt; unannotated method param errors (self exempt)",
       runErr(cap.ctx, "class T {\n x = 0\n m(self, a)[] { }\n}\n")
               .find("undeclared type for 'a'") != std::string::npos);
    ok("M3: annotated methods (hidden self exempt) compile and run",
       runErr(cap.ctx,
              "class T {\n x = 0\n __init__(self, v:int)[] { self.x = v }\n"
              " m(self, k:int)[] -> int { return self.x * k }\n}\n"
              "local p:table = T(3)\nassert(p.m(2) == 6, \"m3 method\")\n")
           .empty());
    ok("M3: static methods (hidden anchor exempt) compile and run",
       runErr(cap.ctx,
              "class T {\n static bump(k:int)[] -> int { return k + 1 }\n}\n"
              "local r:int = T::bump(1)\nassert(r == 2, \"m3 static\")\n")
           .empty());
    ok("M3: level-2 diagnostics are warnings-free (thrown, not warned)",
       cap.warnings().empty(), cap.warnings());
  }

  // 11. M3 plumbing is behavior-neutral below level 2: annotated callables
  //     run unchanged at the default level (Function::paramTypes/returnType
  //     are carried but unchecked until M4's call-boundary enforcement).
  {
    Captured cap;
    ok("M3: annotated def/closure run unchanged at level 0",
       runErr(cap.ctx,
              "def add(a:int, b:int) -> int { return a + b }\n"
              "local t:table = { d = (x:int)[] -> int { return x * 2 } }\n"
              "assert(add(1, 2) == 3, \"def\")\n"
              "assert(t.d(21) == 42, \"closure\")\n")
           .empty());
  }

  // 12. M4 — the spec's `## example of regular functions` and error-message
  //     table at the call boundary and the return (level 1): the pinned
  //     strings are `parameter 'age' of 'hello_world' expects int, got
  //     string` and `function 'sum_int2int' must return int, returned nil`.
  {
    Captured cap;
    cap.ctx.setStrictTypingLevel(QLangContext::STRICT_DECLARED);
    const std::string HW = "def hello_world(name:str, age:int) {\n}\n";
    ok("M4: string into the int parameter (spec case 1)",
       runErr(cap.ctx, HW + "hello_world(\"Tommy\", \"24\")\n") ==
           "parameter 'age' of 'hello_world' expects int, got string");
    ok("M4: int into the string parameter (spec case 2)",
       runErr(cap.ctx, HW + "hello_world(112, 24)\n") ==
           "parameter 'name' of 'hello_world' expects string, got int");
    ok("M4: long storage is too large for int (spec case 3)",
       runErr(cap.ctx,
              HW + "let age:long = 24\nhello_world(\"Tommy\", age)\n") ==
           "parameter 'age' of 'hello_world' expects int, got long");
    ok("M4: byte is not an int (spec case 4: cross-sign)",
       runErr(cap.ctx,
              HW + "let age:long = 24\nage:byte = 24\n"
                   "hello_world(\"Tommy\", age)\n") ==
           "parameter 'age' of 'hello_world' expects int, got byte");
    ok("M4: the correct call passes clean",
       runErr(cap.ctx, HW + "hello_world(\"Tommy\", 24)\n").empty());
    ok("M4: nil passes a declared parameter (Q13)",
       runErr(cap.ctx, HW + "hello_world(\"Tommy\", nil)\n").empty());
    ok("M4: same-group widening is implicit (short satisfies int)",
       runErr(cap.ctx,
              HW + "local s:short = 5\nhello_world(\"Tommy\", s)\n")
           .empty());
    ok("M4: storage is never narrowed implicitly (int into word)",
       runErr(cap.ctx,
              "def w(a:word) { }\nlocal x:int = 1\nw(x)\n")
               .find("parameter 'a' of 'w' expects word, got int") !=
           std::string::npos);
    ok("M4: byte satisfies word (same group, smaller storage)",
       runErr(cap.ctx, "def w(a:word) { }\nlocal b:byte = 1\nw(b)\n")
           .empty());
    ok("M4: a bool never satisfies an integer parameter",
       runErr(cap.ctx, "def ib(a:int) { }\nib(true)\n") ==
           "parameter 'a' of 'ib' expects int, got bool");
    ok("M4: only a bool satisfies a bool parameter",
       runErr(cap.ctx, "def bb(a:bool) { }\nbb(1)\n") ==
           "parameter 'a' of 'bb' expects bool, got int");
    ok("M4: the sum_int2int spec example raises at the return",
       runErr(cap.ctx,
              "def sum_int2int(a:int, b:int) -> int {\n    a += b\n}\n"
              "sum_int2int(1, 2)\n") ==
           "function 'sum_int2int' must return int, returned nil");
    ok("M4: a wrong return value is named by its type",
       runErr(cap.ctx, "def m5() -> int { return \"x\" }\nm5()\n") ==
           "function 'm5' must return int, returned string");
    ok("M4: a matching return passes",
       runErr(cap.ctx,
              "def m5() -> int { return 5 }\nassert(m5() == 5, \"m4\")\n")
           .empty());
    ok("M4: closure literal checked by parameter name",
       runErr(cap.ctx,
              "local t:table = { f = (a:int)[] { } }\nt.f(\"x\")\n") ==
           "parameter 'a' of 'closure' expects int, got string");
    ok("M4: generator arguments checked at instance creation",
       runErr(cap.ctx, "def gen(a:int) { yield a }\ngen(\"s\")\n") ==
           "parameter 'a' of 'gen' expects int, got string");
    ok("M4: class method checked (hidden self exempt)",
       runErr(cap.ctx,
              "class T {\n x = 0\n m(self, k:int)[] { }\n}\n"
              "local o:table = T()\no.m(\"bad\")\n") ==
           "parameter 'k' of 'm' expects int, got string");
    ok("M4: class annotation accepts instances (Q18)",
       runErr(cap.ctx,
              "class Person {\n name = \"\"\n __init__(self, n:str)[] { "
              "self.name = n }\n}\ndef greet(p:Person) { }\n"
              "greet(Person(\"x\"))\n")
           .empty());
    ok("M4: class annotation rejects foreign values by name",
       runErr(cap.ctx,
              "class Person2 {\n name = \"\"\n __init__(self, n:str)[] { "
              "self.name = n }\n}\ndef greet2(p:Person2) { }\n"
              "greet2(5)\n") ==
           "parameter 'p' of 'greet2' expects Person2, got int");
    ok("M4: a class-typed return names the class",
       runErr(cap.ctx,
              "class Person3 {\n name = \"\"\n}\ndef mk() -> Person3 { "
              "return 5 }\nmk()\n") ==
           "function 'mk' must return Person3, returned int");
    ok("M4: an unannotated callable is invisible at level 1",
       runErr(cap.ctx, "def u(x) { }\nu(\"anything\")\n").empty());
    ok("M4: enforcement throws, it never warns",
       cap.warnings().empty(), cap.warnings());
  }

  // 13. M4 gating: every level-1 refusal above runs silently at level 0, and
  //     the boundary checks hold at level 2 as well.
  {
    Captured cap;
    ok("M4: level 0 (default) does not check the call boundary",
       runErr(cap.ctx,
              "def hello_world(name:str, age:int) { }\n"
              "hello_world(\"Tommy\", \"24\")\n"
              "def sum_int2int(a:int, b:int) -> int { a += b }\n"
              "sum_int2int(1, 2)\n")
           .empty());
    Captured all;
    all.ctx.setStrictTypingLevel(QLangContext::STRICT_ALL);
    ok("M4: level 2 enforces the same call-boundary rule",
       runErr(all.ctx, "def f(a:int) { }\nf(\"s\")\n") ==
           "parameter 'a' of 'f' expects int, got string");
  }

  std::cout << passes << " passed, " << fails << " failed\n";
  return fails ? 1 : 0;
}
