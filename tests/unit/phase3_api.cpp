// phase3_api.cpp — Phase 3 tests: host API hardening.
//
// Validates the Phase 3 changes from
// docs/plans/architecture-remediation-plan.md:
//   3.1 Type registry with stable handles (shared_ptr, type_info validation)
//   3.2 Finalizer queue (enqueue at release, drain at safe points)
//   3.3 release() builtin for owned externals
//   3.4 Context split (ProgramCache + TypeRegistry)
//
// Each test gets an isolated QLangContext. The executable exits 0 only if
// every expectation holds.
//
//   T1  Type handle survives clear(): the shared_ptr from import<T>() stays
//       valid after the context's type registry is cleared (H4).
//
//   T2  Type mismatch is a clean error: a handoff whose concrete type does
//       not match the registered type throws QLangError(Type) at first use,
//       not UB (H2; EXTERNAL_TYPES §10.2).
//
//   T3  Expired borrow is a clean error: using a borrowed external after the
//       host dropped it throws QLangError(Runtime, "object destroyed").
//
//   F1  Finalizer does not run from the deleter: an instance released at
//       run end is enqueued, not dispatched; it runs at the next explicit
//       drain, exactly once (H3).
//
//   F2  Drain at the start of the next run(): a pending finalizer runs when
//       the next run() begins.
//
//   F3  Drain to a fixpoint: a finalizer that releases more instances runs
//       them in the same processFinalizers() call.
//
//   F4  Finalizer errors are swallowed: a throwing finalizer never takes
//       down the drain.
//
//   F5  clear() drains: instances released by clear() run their finalizers
//       during clear(), while the program cache is still intact.
//
//   F6  Thread-safe enqueue: the deleter may fire on a foreign thread; it
//       only enqueues, and the main thread drains.
//
//   F7  Re-entrancy guard: a finalizer that re-enters processFinalizers()
//       (via a native) is a no-op; no deadlock, no nested drain.
//
//   R1  release() frees an owned external: the object is destroyed and the
//       value becomes Nil (H2 retention leak).
//
//   R2  release() on a borrowed external is a type error: the host object
//       is untouched.
//
//   R3  release() on a non-external is a type error.
//
//   R4  Use after release is a clean nil error.
//
//   C1  Copied NativeFunc survives clear(): the constructor native captures
//       the shared handle, so a copied function stays valid after clear()
//       (H1/H4: the registry drops only its own reference).

#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"
#include "qlang/QLangMeta.h"
#include "qlang/QLangValue.h"

using namespace qlang;

// Test fixture types (same shape as test_external.cpp).
struct Point : qlang::QLObject<Point> {
  Point(double x_, double y_) : x(x_), y(y_) {}
  double x = 0, y = 0;
  void translate(double dx, double dy) { x += dx; y += dy; }

  QL_OBJECT(QL_PROPERTY(x),
            QL_PROPERTY(y),
            QL_INVOKABLE(translate),
            QL_CTOR([](QLangContext &c, const std::vector<QValue> &a) {
              return std::make_shared<Point>(a[0].as<double>(),
                                             a[1].as<double>());
            }));
};

struct DockArea : qlang::QLObject<DockArea> {
  int v = 0;
  double x = 0; // common member with Point: lets `e.x` resolve the member
                // and reach the type_info check (T2)
  QL_OBJECT(QL_PROPERTY(v), QL_PROPERTY(x));
};

namespace {

int g_failures = 0;

void checkInt(const char *label, const QLangContext &ctx, const char *var,
              int64_t expected) {
  try {
    QLangValue v = ctx.get(var);
    if (v.type() != QLangValue::Type::Int || toInt64(v) != expected) {
      fprintf(stderr, "FAIL %s: %s = type %d value %lld, expected %lld\n",
              label, var, (int)v.type(),
              (long long)(v.type() == QLangValue::Type::Int ? toInt64(v) : -1),
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
  // ---------------------------------------------------------------- T1
  // Type handle survives clear(): the shared_ptr from import<T>() stays
  // valid after the context's type registry is cleared (H4).
  {
    QLangContext ctx;
    auto pt = ctx.import<Point>();
    if (!pt || pt->name != "Point") {
      fprintf(stderr, "FAIL T1: import<Point>() handle is null or misnamed\n");
      g_failures++;
    } else {
      fprintf(stderr, "ok   T1: handle valid before clear\n");
    }
    ctx.clear();
    if (pt && pt->name == "Point") {
      fprintf(stderr, "ok   T1: handle valid after clear\n");
    } else {
      fprintf(stderr, "FAIL T1: handle invalid after clear\n");
      g_failures++;
    }
  }

  // ---------------------------------------------------------------- T2
  // Type mismatch is a clean error: a handoff whose concrete type does not
  // match the registered type throws QLangError(Type) at first use, not UB
  // (H2; EXTERNAL_TYPES §10.2).
  {
    QLangContext ctx;
    auto dock = ctx.import<DockArea>();
    auto p = std::make_shared<Point>(1, 2);
    ExternalRef ref;
    ref.type = dock;
    ref.owned = p;
    ref.expected = &typeid(DockArea);
    ref.check = [](const void *q) {
      return dynamic_cast<const DockArea *>(
                 static_cast<const QLObject<DockArea> *>(q)) != nullptr;
    };
    ctx.set("e", QValue::External(std::move(ref)));
    QLangErrorKind kind;
    std::string msg;
    if (runThrows(ctx, "e.x", &kind, &msg) &&
        kind == QLangErrorKind::Type &&
        msg.find("type mismatch") != std::string::npos) {
      fprintf(stderr, "ok   T2: mismatch is a clean type error (%s)\n",
              msg.c_str());
    } else {
      fprintf(stderr, "FAIL T2: expected clean type error, got kind %d msg "
                      "'%s'\n",
              (int)kind, msg.c_str());
      g_failures++;
    }
  }

  // ---------------------------------------------------------------- T3
  // Expired borrow is a clean error: using a borrowed external after the
  // host dropped it throws QLangError(Runtime, "object destroyed").
  {
    QLangContext ctx;
    auto pt = ctx.import<Point>();
    auto p = std::make_shared<Point>(1, 2);
    ctx.set("e", qval_external_borrowed<Point>(pt, std::weak_ptr<Point>(p)));
    p.reset();
    QLangErrorKind kind;
    std::string msg;
    if (runThrows(ctx, "e.x", &kind, &msg) &&
        kind == QLangErrorKind::Runtime &&
        msg.find("object destroyed") != std::string::npos) {
      fprintf(stderr, "ok   T3: expired borrow is a clean runtime error\n");
    } else {
      fprintf(stderr, "FAIL T3: expected 'object destroyed', got kind %d "
                      "msg '%s'\n",
              (int)kind, msg.c_str());
      g_failures++;
    }
  }

  // ---------------------------------------------------------------- F1
  // Finalizer does not run from the deleter: an instance released at run
  // end is enqueued, not dispatched; it runs at the next explicit drain,
  // exactly once (H3).
  {
    QLangContext ctx;
    try {
      ctx.run("local n = 0\n"
              "class D {\n"
              "  __destroy__ (self) [ ] { n = n + 1 }\n"
              "}\n"
              "local a = D()\n"
              "a = nil\n");
      checkInt("F1 not run from deleter", ctx, "n", 0);
      ctx.processFinalizers();
      checkInt("F1 runs at drain", ctx, "n", 1);
      ctx.processFinalizers();
      checkInt("F1 exactly once", ctx, "n", 1);
    } catch (const std::exception &e) {
      fprintf(stderr, "FAIL F1: threw: %s\n", e.what());
      g_failures += 3;
    }
  }

  // ---------------------------------------------------------------- F2
  // Drain at the start of the next run(): a pending finalizer runs when the
  // next run() begins.
  {
    QLangContext ctx;
    try {
      ctx.run("local n = 0\n"
              "class D {\n"
              "  __destroy__ (self) [ ] { n = n + 1 }\n"
              "}\n"
              "local a = D()\n"
              "a = nil\n");
      checkInt("F2 pending after run", ctx, "n", 0);
      ctx.run("x = 1");
      checkInt("F2 drained at next run", ctx, "n", 1);
    } catch (const std::exception &e) {
      fprintf(stderr, "FAIL F2: threw: %s\n", e.what());
      g_failures += 2;
    }
  }

  // ---------------------------------------------------------------- F3
  // Drain to a fixpoint: a finalizer that releases more instances runs them
  // in the same processFinalizers() call.
  {
    QLangContext ctx;
    try {
      ctx.run("local n = 0\n"
              "class D {\n"
              "  other = nil\n"
              "  __init__ (self, o) [ ] { self.other = o }\n"
              "  __destroy__ (self) [ ] { n = n + 1; self.other = nil }\n"
              "}\n"
              "local inner = D(nil)\n"
              "local a = D(inner)\n"
              "inner = nil\n"
              "a = nil\n");
      checkInt("F3 pending", ctx, "n", 0);
      ctx.processFinalizers();
      checkInt("F3 fixpoint: both ran", ctx, "n", 2);
    } catch (const std::exception &e) {
      fprintf(stderr, "FAIL F3: threw: %s\n", e.what());
      g_failures += 2;
    }
  }

  // ---------------------------------------------------------------- F4
  // Finalizer errors are swallowed: a throwing finalizer never takes down
  // the drain.
  {
    QLangContext ctx;
    try {
      ctx.run("class D {\n"
              "  __destroy__ (self) [ ] { assert(false) }\n"
              "}\n"
              "local a = D()\n"
              "a = nil\n");
      bool threw = false;
      try {
        ctx.processFinalizers();
      } catch (const std::exception &e) {
        threw = true;
        fprintf(stderr, "FAIL F4: drain threw: %s\n", e.what());
      }
      if (!threw) {
        fprintf(stderr, "ok   F4: finalizer error swallowed\n");
      } else {
        g_failures++;
      }
    } catch (const std::exception &e) {
      fprintf(stderr, "FAIL F4: setup threw: %s\n", e.what());
      g_failures++;
    }
  }

  // ---------------------------------------------------------------- F5
  // clear() drains: instances released by clear() run their finalizers
  // during clear(), while the program cache is still intact. The log table
  // is captured by shared_ptr before clear so it survives the global-table
  // reset.
  {
    QLangContext ctx;
    try {
      ctx.run("local log = {n = 0}\n"
              "class D {\n"
              "  __init__ (self, lg) [ ] { self.log = lg }\n"
              "  __destroy__ (self) [ ] { local lg = self.log; lg.n = lg.n + 1 }\n"
              "}\n"
              "local a = D(log)\n");
      TableValue logTable = ctx.get("log").asTable();
      ctx.clear();
      QLangValue n = (*logTable)["n"];
      if (n.type() == QLangValue::Type::Int && toInt64(n) == 1) {
        fprintf(stderr, "ok   F5: clear() drained the finalizer\n");
      } else {
        fprintf(stderr, "FAIL F5: log.n = type %d value %lld, expected 1\n",
                (int)n.type(),
                (long long)(n.type() == QLangValue::Type::Int ? toInt64(n)
                                                              : -1));
        g_failures++;
      }
    } catch (const std::exception &e) {
      fprintf(stderr, "FAIL F5: threw: %s\n", e.what());
      g_failures++;
    }
  }

  // ---------------------------------------------------------------- F6
  // Thread-safe enqueue: the deleter may fire on a foreign thread; it only
  // enqueues, and the main thread drains.
  {
    QLangContext ctx;
    try {
      ctx.run("local n = 0\n"
              "class D {\n"
              "  __destroy__ (self) [ ] { n = n + 1 }\n"
              "}\n"
              "local a = D()\n");
      QValue a = ctx.get("a"); // copy: shared table reference
      ctx.remove("a");         // drop the global reference
      // Move the last reference into the lambda so the deleter fires on the
      // foreign thread (a by-value capture would leave the local `a` holding
      // a reference, deferring the release to the main thread).
      std::thread t([a = std::move(a)]() mutable { a = QValue(); });
      t.join();
      checkInt("F6 enqueued from foreign thread, not run", ctx, "n", 0);
      ctx.processFinalizers();
      checkInt("F6 drained on main thread", ctx, "n", 1);
    } catch (const std::exception &e) {
      fprintf(stderr, "FAIL F6: threw: %s\n", e.what());
      g_failures += 2;
    }
  }

  // ---------------------------------------------------------------- F7
  // Re-entrancy guard: a finalizer that re-enters processFinalizers() (via
  // a native) is a no-op; no deadlock, no nested drain.
  {
    QLangContext ctx;
    try {
      ctx.setFunc("drain", [](QLangContext &c, const std::vector<QValue> &) {
        c.processFinalizers(); // nested: must be a no-op, must not deadlock
        return QValue::Int(1);
      });
      ctx.run("local n = 0\n"
              "class D {\n"
              "  __destroy__ (self) [ ] { n = n + 1; drain() }\n"
              "}\n"
              "local a = D()\n"
              "a = nil\n");
      checkInt("F7 pending", ctx, "n", 0);
      ctx.processFinalizers(); // must not deadlock
      checkInt("F7 ran once", ctx, "n", 1);
    } catch (const std::exception &e) {
      fprintf(stderr, "FAIL F7: threw: %s\n", e.what());
      g_failures += 2;
    }
  }

  // ---------------------------------------------------------------- R1
  // release() frees an owned external: the object is destroyed and the
  // value becomes Nil (H2 retention leak).
  {
    QLangContext ctx;
    auto pt = ctx.import<Point>();
    auto p = std::make_shared<Point>(1, 2);
    std::weak_ptr<Point> weak(p);
    ctx.set("e", qval_external_owned<Point>(pt, p));
    p.reset(); // the script value holds the last reference
    try {
      ctx.run("release(e)");
      if (weak.expired()) {
        fprintf(stderr, "ok   R1: owned external freed by release()\n");
      } else {
        fprintf(stderr, "FAIL R1: object still alive after release()\n");
        g_failures++;
      }
      if (ctx.get("e").isNil()) {
        fprintf(stderr, "ok   R1: value is Nil after release()\n");
      } else {
        fprintf(stderr, "FAIL R1: value is not Nil after release()\n");
        g_failures++;
      }
    } catch (const std::exception &e) {
      fprintf(stderr, "FAIL R1: threw: %s\n", e.what());
      g_failures += 2;
    }
  }

  // ---------------------------------------------------------------- R2
  // release() on a borrowed external is a type error: the host object is
  // untouched.
  {
    QLangContext ctx;
    auto pt = ctx.import<Point>();
    auto p = std::make_shared<Point>(1, 2);
    std::weak_ptr<Point> weak(p);
    ctx.set("e", qval_external_borrowed<Point>(pt, std::weak_ptr<Point>(p)));
    QLangErrorKind kind;
    if (runThrows(ctx, "release(e)", &kind) &&
        kind == QLangErrorKind::Type && !weak.expired()) {
      fprintf(stderr, "ok   R2: borrowed external rejected, object intact\n");
    } else {
      fprintf(stderr, "FAIL R2: expected type error with object intact "
                      "(kind %d, expired %d)\n",
              (int)kind, (int)weak.expired());
      g_failures++;
    }
  }

  // ---------------------------------------------------------------- R3
  // release() on a non-external is a type error.
  {
    QLangContext ctx;
    QLangErrorKind kind;
    if (runThrows(ctx, "release(42)", &kind) &&
        kind == QLangErrorKind::Type &&
        runThrows(ctx, "release(nil)", &kind) &&
        kind == QLangErrorKind::Type &&
        runThrows(ctx, "release(\"s\")", &kind) &&
        kind == QLangErrorKind::Type) {
      fprintf(stderr, "ok   R3: non-externals rejected\n");
    } else {
      fprintf(stderr, "FAIL R3: expected type errors for non-externals\n");
      g_failures++;
    }
  }

  // ---------------------------------------------------------------- R4
  // Use after release is a clean nil error.
  {
    QLangContext ctx;
    auto pt = ctx.import<Point>();
    auto p = std::make_shared<Point>(1, 2);
    ctx.set("e", qval_external_owned<Point>(pt, p));
    p.reset();
    try {
      ctx.run("release(e)");
      if (runThrows(ctx, "e.x")) {
        fprintf(stderr, "ok   R4: use after release is a clean error\n");
      } else {
        fprintf(stderr, "FAIL R4: use after release did not throw\n");
        g_failures++;
      }
    } catch (const std::exception &e) {
      fprintf(stderr, "FAIL R4: threw: %s\n", e.what());
      g_failures++;
    }
  }

  // ---------------------------------------------------------------- C1
  // Copied NativeFunc survives clear(): the constructor native captures the
  // shared handle, so a copied function stays valid after clear() (H1/H4:
  // the registry drops only its own reference).
  {
    QLangContext ctx;
    auto pt = ctx.import<Point>();
    auto ctor = ctx.nativeFuncs().at("Point"); // copy of the lambda
    ctx.clear();
    try {
      QValue v = ctor(ctx, {QValue::Float(1.0), QValue::Float(2.0)});
      if (v.type() == QValue::Type::External && v.asExternal().type == pt &&
          v.asExternal().self() != nullptr) {
        fprintf(stderr, "ok   C1: copied ctor valid after clear\n");
      } else {
        fprintf(stderr, "FAIL C1: copied ctor produced a bad value (type %d)\n",
                (int)v.type());
        g_failures++;
      }
    } catch (const std::exception &e) {
      fprintf(stderr, "FAIL C1: copied ctor threw after clear: %s\n", e.what());
      g_failures++;
    }
  }

  if (g_failures) {
    fprintf(stderr, "%d FAILURES\n", g_failures);
    return 1;
  }
  fprintf(stderr, "phase3_api: all checks passed\n");
  return 0;
}
