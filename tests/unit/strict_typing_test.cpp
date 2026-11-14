// Strict typing M1 (docs/spec/16-strict_typing.md, docs/plans/strict-typing-
// plan.md M1): the QLangContext level API (STRICT_OFF/STRICT_DECLARED/
// STRICT_ALL, clamping, survives clear()) and the duplicate-declaration
// diagnostic matrix — `local a; local a` is silent at level 0 (the default,
// so legacy programs are untouched), an OUT_WARNING at level 1, and a compile
// error at level 2. Shadowing and parameters are deliberately not duplicates
// (Q5). Call-site/return enforcement arrives with M3/M4.
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

  std::cout << passes << " passed, " << fails << " failed\n";
  return fails ? 1 : 0;
}
