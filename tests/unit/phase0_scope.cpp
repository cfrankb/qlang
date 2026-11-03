// phase0_scope.cpp — Phase 0 red tests: scope / lifetime / instance defects.
//
// Each defect gets an isolated QLangContext. The executable exits 0 only if
// every expectation holds. On the pre-fix tree these tests are RED:
//
//   S2  CALLFN (def functions) rebinds `global` captures to the CALLER frame,
//       so a global read from a nested function reads the caller's register
//       instead of the top-level value. CALLCLOS (lambdas) is unaffected.
//       Expectation: r1 == r2 == r3 == 42.  Today: r2 == 0 (nil).
//
//   S3  All instances of a generator function share ONE VM-level saved frame
//       (m_savedGenFrame), so interleaved calls resume the wrong instance.
//       Expectation: a == 10, b == 20, c == 11.  Today: b == 11, c == 12
//       (g2() resumes g1's saved frame; g1() resumes it again).
//
//   P2  DELETEx inserts the register index into a VM-wide `m_deleted` set,
//       not a per-frame set. `delete t` in a nested function (t == reg 0
//       of that frame) poisons reg 0 of the main frame, so the sync-out
//       drops the top-level `a`.  Expectation: a == 100.
//       Today: `a` is undefined after f().
//
//   S1  A closure's capture stores a raw pointer to the frame that created
//       it. When that frame is popped the pointer dangles; calling the
//       closure after the creator returns is UB (crash / stale read).
//       Expectation: r == 10 (the capture must stay live after the creator
//       frame is gone).  Today: segfault or garbage.
//
// S1 runs in a forked child so a segfault there cannot prevent the other
// checks (and cannot kill the test harness). P3 (API level) does the same.

#include <sys/wait.h>
#include <unistd.h>

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

// Run `script` in a fresh context; return true iff it completes and
// `var` equals `expected`. Used for the direct (non-forking) checks.
bool runAndCheck(const char *label, const char *script, const char *var,
                 int64_t expected) {
  QLangContext ctx;
  try {
    ctx.run(script);
  } catch (const std::exception &e) {
    fprintf(stderr, "FAIL %s: script threw: %s\n", label, e.what());
    return false;
  }
  bool ok = true;
  checkInt(label, ctx, var, expected);
  return g_failures == 0;
}

} // namespace

int main() {
  // ---------------------------------------------------------------- S2
  {
    QLangContext ctx;
    try {
      ctx.run("x = 42\n"
              "def f() { global x; return x }\n"
              "def g() { return f() }\n"
              "local r1 = f()\n"
              "local r2 = g()\n"
              "local l = ()[x] { return x }\n"
              "local r3 = l()\n");
      checkInt("S2 direct call", ctx, "r1", 42);
      checkInt("S2 nested call", ctx, "r2", 42);
      checkInt("S2 lambda call", ctx, "r3", 42);
    } catch (const std::exception &e) {
      fprintf(stderr, "FAIL S2: script threw: %s\n", e.what());
      g_failures += 3;
    }
  }

  // ---------------------------------------------------------------- S3
  // Identity parameter sits in slot 1 (base): reg 0 is clobbered by YIELD,
  // so slot 0 cannot carry the per-instance identity.
  {
    QLangContext ctx;
    try {
      ctx.run("def gen(pad, base, q) {\n"
              "  for i = 0 to 2 { yield base * 10 + i }\n"
              "}\n"
              "local g1 = gen(0, 1, 0)\n"
              "local g2 = gen(0, 2, 0)\n"
              "local a = g1()\n"
              "local b = g2()\n"
              "local c = g1()\n");
      checkInt("S3 g1 first", ctx, "a", 10);
      checkInt("S3 g2 first", ctx, "b", 20);
      checkInt("S3 g1 second", ctx, "c", 11);
    } catch (const std::exception &e) {
      fprintf(stderr, "FAIL S3: script threw: %s\n", e.what());
      g_failures += 3;
    }
  }

  // ---------------------------------------------------------------- P2
  {
    QLangContext ctx;
    try {
      ctx.run("a = 100\n"
              "def f() {\n"
              "  local t = 5\n"
              "  delete t\n"
              "}\n"
              "f()\n");
      checkInt("P2 top-level var survives nested delete", ctx, "a", 100);
    } catch (const std::exception &e) {
      fprintf(stderr, "FAIL P2: script threw: %s\n", e.what());
      g_failures++;
    }
  }

  // ---------------------------------------------------------------- S1
  // Fork: the current tree dereferences a dangling frame pointer here
  // (segfault), which must not take down the other checks.
  {
    pid_t pid = fork();
    if (pid == 0) {
      int rc = 1;
      try {
        QLangContext ctx;
        ctx.run("def make() {\n"
                "  local n = 10\n"
                "  local f = ()[n] { return n }\n"
                "  local t = {}\n"
                "  t.f = f\n"
                "  return t\n"
                "}\n"
                "local h = make()\n"
                "def churn2() { return 1 }\n"
                "def churn1() { return churn2() }\n"
                "local sink = []\n"
                "for i = 0 to 2000 {\n"
                "  sink[] = sprintf(\"churn-%d-%d-%d\", i, i, i)\n"
                "  local s = 0\n"
                "  for j = 0 to 50 { s = s + churn1() }\n"
                "  sink[] = s\n"
                "}\n"
                "local r = h.f()\n");
        QLangValue v = ctx.get("r");
        if (v.type() == QLangValue::Type::Int && toInt64(v) == 10)
          rc = 42;
      } catch (...) {
        rc = 1;
      }
      _exit(rc);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    if (WIFEXITED(st) && WEXITSTATUS(st) == 42) {
      fprintf(stderr, "ok   S1: closure outlives creator frame (r = 10)\n");
    } else {
      const char *why = WIFSIGNALED(st) ? "crashed (dangling frame read)"
                                        : "wrong value";
      fprintf(stderr, "FAIL S1: closure outlives creator frame — %s\n", why);
      g_failures++;
    }
  }

  if (g_failures == 0) {
    fprintf(stderr, "ALL PASS (phase0_scope)\n");
    return 0;
  }
  fprintf(stderr, "%d FAILURE(S) (phase0_scope)\n", g_failures);
  return 1;
}
