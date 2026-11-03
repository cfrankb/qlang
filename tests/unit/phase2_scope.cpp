// phase2_scope.cpp — Phase 2 tests: single global variable home.
//
// Validates the context-owned GlobalTable design where the main VM frame
// aliases globalSlots() directly (no seed-in / sync-out). Each test gets an
// isolated QLangContext. The executable exits 0 only if every expectation
// holds.
//
//   G1  Live global capture: a closure defined after a top-level assignment
//       sees the current value; a later re-assignment is visible on the next
//       call.  Expectation: r1 == 1, r2 == 2.
//
//   G2  Closure created before re-assignment sees the later value (S2
//       shape): the global slot is live-aliased, so the value at call time
//       is what matters, not the value at closure-creation time.
//       Expectation: r == 42.
//
//   G3  delete of a top-level var erases only that name (P2 shape): deleting
//       `a` must not poison `b`'s slot.
//       Expectation: a is nil, b == 200.
//
//   G4  Host set() before run() + script top-level var of a *different* name
//       must not alias the host slot.  ctx.set("x", 1); ctx.run("a = 5")
//       must leave x == 1.
//
//   G5  Mid-run throw leaves the table consistent: a throw after `a = 1`
//       must not lose the assignment.  Expectation: get("a") == 1.

#include <cstdio>
#include <string>

#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"
#include "qlang/QLangValue.h"

using namespace qlang;

namespace {

int g_failures = 0;

void checkInt(const char *label, const QLangContext &ctx, const char *var,
              int64_t expected) {
  try {
    QLangValue v = ctx.get(var);
    if (v.type() != QLangValue::Type::Int || toInt64(v) != expected) {
      fprintf(stderr, "FAIL %s: %s = type %d value %lld, expected %lld\n", label,
              var, (int)v.type(), (long long)(v.type() == QLangValue::Type::Int
                                                 ? toInt64(v)
                                                 : -1),
              (long long)expected);
      g_failures++;
    } else {
      fprintf(stderr, "ok   %s: %s = %lld\n", label, var, (long long)expected);
    }
  } catch (const std::exception &e) {
    fprintf(stderr, "FAIL %s: %s threw: %s\n", label, var, e.what());
    g_failures++;
  }
}

} // namespace

int main() {
  // ---------------------------------------------------------------- G1
  // Live global capture: closure defined after assignment sees current
  // value; later re-assignment is visible on the next call.
  {
    QLangContext ctx;
    try {
      ctx.run("x = 1\n"
              "def f() { global x; return x }\n"
              "local r1 = f()\n"
              "x = 2\n"
              "local r2 = f()\n");
      checkInt("G1 first call", ctx, "r1", 1);
      checkInt("G1 second call (live)", ctx, "r2", 2);
    } catch (const std::exception &e) {
      fprintf(stderr, "FAIL G1: script threw: %s\n", e.what());
      g_failures += 2;
    }
  }

  // ---------------------------------------------------------------- G2
  // Closure created before re-assignment sees the later value (S2 shape):
  // the global slot is live-aliased, so the value at call time is what
  // matters, not the value at closure-creation time.
  {
    QLangContext ctx;
    try {
      ctx.run("x = 0\n"
              "def f() { global x; return x }\n"
              "x = 42\n"
              "local r = f()\n");
      checkInt("G2 closure-before-reassignment", ctx, "r", 42);
    } catch (const std::exception &e) {
      fprintf(stderr, "FAIL G2: script threw: %s\n", e.what());
      g_failures++;
    }
  }

  // ---------------------------------------------------------------- G3
  // delete of a top-level var erases only that name (P2 shape): deleting
  // `a` must not poison `b`'s slot.
  {
    QLangContext ctx;
    try {
      ctx.run("a = 100\n"
              "b = 200\n"
              "delete a\n");
      if (ctx.has("a")) {
        fprintf(stderr, "FAIL G3: deleted var 'a' still exists\n");
        g_failures++;
      } else {
        fprintf(stderr, "ok   G3: deleted var 'a' is gone\n");
      }
      checkInt("G3 sibling var survives", ctx, "b", 200);
    } catch (const std::exception &e) {
      fprintf(stderr, "FAIL G3: script threw: %s\n", e.what());
      g_failures += 2;
    }
  }

  // ---------------------------------------------------------------- G4
  // Host set() before run() + script top-level var of a different name
  // must not alias the host slot.
  {
    QLangContext ctx;
    ctx.set("x", QLangValue::Int(1));
    try {
      ctx.run("a = 5\n");
      checkInt("G4 host var not clobbered", ctx, "x", 1);
    } catch (const std::exception &e) {
      fprintf(stderr, "FAIL G4: script threw: %s\n", e.what());
      g_failures++;
    }
  }

  // ---------------------------------------------------------------- G5
  // Mid-run throw leaves the table consistent: a runtime error after
  // `a = 1` must not lose the assignment.
  {
    QLangContext ctx;
    try {
      ctx.run("a = 1\n"
              "local x = 1 / 0\n");
      fprintf(stderr, "FAIL G5: script did not throw\n");
      g_failures++;
    } catch (const std::exception &) {
      // Expected: the runtime error is caught here.
      checkInt("G5 table consistent after throw", ctx, "a", 1);
    }
  }

  if (g_failures == 0) {
    fprintf(stderr, "ALL PASS (phase2_scope)\n");
    return 0;
  }
  fprintf(stderr, "%d FAILURE(S) (phase2_scope)\n", g_failures);
  return 1;
}
