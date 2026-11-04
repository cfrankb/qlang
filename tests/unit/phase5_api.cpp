// phase5_api.cpp — Phase 5 tests: paper cuts.
//
// Validates the Phase 5 changes from
// docs/plans/architecture-remediation-plan.md:
//   F6  Step budget: the context owns the cumulative instruction counter;
//       VMs increment it (never reset it) per instruction; the host reads
//       stepsUsed()/budget() and may resetSteps().
//   H5  Threading contract: one context, one thread (debug-build assert;
//       here we verify single-threaded use is unaffected and the budget API
//       is reachable from C++).
//
// Each test gets an isolated QLangContext. The executable exits 0 only if
// every expectation holds.
//
//   S1  stepsUsed() starts at 0 on a fresh context.
//
//   S2  stepsUsed() accumulates across dispatches: a second run() continues
//       the count from where the first left off (no per-VM reset).
//
//   S3  Per-dispatch cost is measurable: stepsUsed() after a call() minus
//       the count before it is > 0 and stable for the same program.
//
//   S4  budget() returns the live limit (== maxSteps(), default 100000).
//
//   S5  resetSteps() zeroes the counter; the next dispatch starts from 0.
//
//   S6  The limit is still enforced: a runaway loop throws
//       "infinite loop (step limit)" once the cumulative count exceeds
//       budget() (set low via setMaxSteps for a fast failure).
//
//   S7  set_max_steps() from QLang still sets the budget (budget() reflects
//       it); the counter is independent of the limit.

#include <cstdio>
#include <string>
#include <vector>

#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"
#include "qlang/QLangValue.h"

using namespace qlang;

namespace {

int g_failures = 0;

void check(const char *label, bool ok) {
  if (ok) {
    fprintf(stderr, "ok   %s\n", label);
  } else {
    fprintf(stderr, "FAIL %s\n", label);
    g_failures++;
  }
}

// Expect ctx.run(src) to throw a QLangError. `kind`/`msg` (when non-null)
// receive the thrown error's kind and raw message for follow-up checks.
bool runThrows(QLangContext &ctx, const std::string &src,
               QLangErrorKind *kind = nullptr, std::string *msg = nullptr) {
  try {
    ctx.run(src);
    return false;
  } catch (const QLangError &e) {
    if (kind)
      *kind = e.kind();
    if (msg)
      *msg = e.message();
    return true;
  }
}

} // namespace

int main() {
  // ---------------------------------------------------------------- S1
  // stepsUsed() starts at 0 on a fresh context.
  {
    QLangContext ctx;
    check("S1: fresh context stepsUsed()==0", ctx.stepsUsed() == 0);
  }

  // ---------------------------------------------------------------- S2
  // stepsUsed() accumulates across dispatches: a second run() continues the
  // count from where the first left off (no per-VM reset).
  {
    QLangContext ctx;
    ctx.run("a = 1 + 1");
    std::size_t afterFirst = ctx.stepsUsed();
    check("S2: first run() advanced the counter", afterFirst > 0);
    ctx.run("b = 2 + 2");
    std::size_t afterSecond = ctx.stepsUsed();
    check("S2: second run() continued the count (no reset)",
          afterSecond > afterFirst);
  }

  // ---------------------------------------------------------------- S3
  // Per-dispatch cost is measurable: stepsUsed() after a call() minus the
  // count before it is > 0 and stable for the same program.
  {
    QLangContext ctx;
    ctx.run("def square(x) { return x * x }");
    std::size_t before = ctx.stepsUsed();
    QLangValue r = ctx.call("square", {QLangValue::Int(7)});
    std::size_t cost = ctx.stepsUsed() - before;
    check("S3: call() return value correct",
          r.type() == QLangValue::Type::Int && toInt64(r) == 49);
    check("S3: per-dispatch cost > 0", cost > 0);

    // Same program, same args -> same cost (deterministic dispatch).
    std::size_t before2 = ctx.stepsUsed();
    ctx.call("square", {QLangValue::Int(7)});
    std::size_t cost2 = ctx.stepsUsed() - before2;
    check("S3: per-dispatch cost is stable", cost == cost2);
  }

  // ---------------------------------------------------------------- S4
  // budget() returns the live limit (== maxSteps(), default 100000).
  {
    QLangContext ctx;
    check("S4: budget() == maxSteps()", ctx.budget() == ctx.maxSteps());
    check("S4: default budget is 100000", ctx.budget() == 100000);
    ctx.setMaxSteps(5000);
    check("S4: budget() tracks setMaxSteps()", ctx.budget() == 5000);
  }

  // ---------------------------------------------------------------- S5
  // resetSteps() zeroes the counter; the next dispatch starts from 0.
  {
    QLangContext ctx;
    ctx.run("a = 1 + 1");
    check("S5: counter advanced before reset", ctx.stepsUsed() > 0);
    ctx.resetSteps();
    check("S5: resetSteps() zeroes the counter", ctx.stepsUsed() == 0);
    ctx.run("b = 2 + 2");
    check("S5: dispatch after reset starts from 0", ctx.stepsUsed() > 0);
  }

  // ---------------------------------------------------------------- S6
  // The limit is still enforced: a runaway loop throws "infinite loop (step
  // limit)" once the cumulative count exceeds budget().
  {
    QLangContext ctx;
    ctx.setMaxSteps(1000); // low limit for a fast failure
    QLangErrorKind kind;
    std::string msg;
    bool threw = runThrows(ctx, "while true { }", &kind, &msg);
    check("S6: runaway loop throws", threw);
    check("S6: error kind is Runtime",
          threw && kind == QLangErrorKind::Runtime);
    check("S6: error message mentions step limit",
          threw && msg.find("step limit") != std::string::npos);
  }

  // ---------------------------------------------------------------- S7
  // set_max_steps() from QLang still sets the budget (budget() reflects it);
  // the counter is independent of the limit.
  {
    QLangContext ctx;
    ctx.run("set_max_steps(4242)");
    check("S7: set_max_steps() from QLang sets budget()",
          ctx.budget() == 4242);
    // The counter is independent of the limit: a small program advances the
    // counter without touching the (now large) limit.
    std::size_t before = ctx.stepsUsed();
    ctx.run("x = 1");
    check("S7: counter advances independently of the limit",
          ctx.stepsUsed() > before);
  }

  if (g_failures == 0) {
    fprintf(stderr, "ALL PASS (phase5_api)\n");
    return 0;
  }
  fprintf(stderr, "%d FAILURES (phase5_api)\n", g_failures);
  return 1;
}
