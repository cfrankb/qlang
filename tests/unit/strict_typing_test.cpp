// Strict typing (docs/spec/16-strict_typing.md, docs/plans/strict-typing-
// plan.md). M1: the QLangContext level API (STRICT_OFF/STRICT_DECLARED/
// STRICT_ALL, clamping, survives clear()) and the duplicate-declaration
// diagnostic matrix — `local a; local a` is silent at level 0 (the default,
// so legacy programs are untouched), an OUT_WARNING at level 1, and a compile
// error at level 2. Shadowing and parameters are deliberately not duplicates
// (Q5). M2: the annotation grammar — typed parameters, `-> type` return
// annotations and annotated assignments parse at every level and stay
// unchecked at level 0. Call-site/return enforcement arrives with M3/M4.
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

  std::cout << passes << " passed, " << fails << " failed\n";
  return fails ? 1 : 0;
}
