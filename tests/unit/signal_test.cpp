// Signals (docs/spec/17-signals.md): external-type signal declaration
// (QL_SIGNAL), script `emit`, connect/disconnect (function value +
// string-slot), synchronous delivery, auto-disconnect on sender destruction,
// re-entrancy contained by the step budget, and the C++ hasSignal/emitSignal
// API. Value-level assertions avoid stdout scraping.
#include <iostream>
#include <memory>
#include <string>
#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"
#include "qlang/QLangMeta.h"
#include "qlang/QValue.h"
using namespace qlang;

// A sensor with one int-argument signal. The member function is declared
// (never defined); only its signature defines the emit argument types.
struct Sensor : qlang::QLObject<Sensor> {
  int level = 0;
  void levelChanged(int newLevel);  // declared only — never defined
  QL_OBJECT(QL_PROPERTY(level), QL_SIGNAL(levelChanged));
};

// A signal name that clashes with a method name of the same type. (A C++
// class cannot have a data member and a method sharing a name, so the clash
// is against an invokable method.)
struct Clash : qlang::QLObject<Clash> {
  void level(int v) {}  // defined (empty); QL_INVOKABLE needs a body
  QL_OBJECT(QL_INVOKABLE(level), QL_SIGNAL(level));
};

// A duplicate signal name.
struct DupSig : qlang::QLObject<DupSig> {
  void levelChanged(int v);  // declared only
  QL_OBJECT(QL_SIGNAL(levelChanged), QL_SIGNAL(levelChanged));
};

static int passes = 0, fails = 0;

static void ok(const std::string &name, bool cond, const std::string &detail = "") {
  if (cond) { std::cout << "[PASS] " << name << "\n"; passes++; }
  else { std::cout << "[FAIL] " << name << (detail.empty() ? "" : " (" + detail + ")") << "\n"; fails++; }
}

// Run a script that ends by assigning the expression to `var`; compare.
static void expectVal(const std::string &name, QLangContext &ctx,
                      const std::string &src, const std::string &var, int64_t expect) {
  try {
    ctx.run(src);
    int64_t got = toInt64(ctx.get(var));
    ok(name, got == expect, "got " + std::to_string(got) + " exp " + std::to_string(expect));
  } catch (const QLangError &e) { ok(name, false, e.what()); }
}

// Run a script that must throw a QLangError.
static void expectThrow(const std::string &name, const std::string &src) {
  QLangContext ctx;
  ctx.import<Sensor>();
  try { ctx.run(src); ok(name, false, "ran, expected error"); }
  catch (const QLangError &e) { ok(name, true); }
}

int main() {
  // 1. emit with zero connections is a silent no-op.
  {
    QLangContext ctx;
    auto pt = ctx.import<Sensor>();
    auto s = std::make_shared<Sensor>();
    ctx.set("sensor", qval_external_borrowed<Sensor>(pt, std::weak_ptr<Sensor>(s)));
    try { ctx.run("emit sensor.levelChanged(3)"); ok("emit no-op", true); }
    catch (const QLangError &e) { ok("emit no-op", false, e.what()); }
  }

  // 2. connect (function value) + emit -> handler runs with the emitted args.
  {
    QLangContext ctx;
    auto pt = ctx.import<Sensor>();
    auto s = std::make_shared<Sensor>();
    ctx.set("sensor", qval_external_borrowed<Sensor>(pt, std::weak_ptr<Sensor>(s)));
    expectVal("connect+emit function value", ctx,
              "seen = 0\ndef onLevel(v) { global seen; seen = v }\n"
              "connect(sensor, \"levelChanged\", onLevel)\n"
              "emit sensor.levelChanged(9)\nlocal r = seen;",
              "r", 9);
  }

  // 3. connect (string-slot) + emit -> target.method(args) runs.
  {
    QLangContext ctx;
    auto pt = ctx.import<Sensor>();
    auto s = std::make_shared<Sensor>();
    ctx.set("sensor", qval_external_borrowed<Sensor>(pt, std::weak_ptr<Sensor>(s)));
    expectVal("connect+emit string-slot", ctx,
              "class UI { shown = 0\nshow(self, v)[] { self.shown = v } }\n"
              "local ui = UI()\n"
              "connect(sensor, \"levelChanged\", ui, \"show\")\n"
              "emit sensor.levelChanged(7)\nlocal r = ui.shown;",
              "r", 7);
  }

  // 4. string-slot with a missing method is a silent no-op.
  {
    QLangContext ctx;
    auto pt = ctx.import<Sensor>();
    auto s = std::make_shared<Sensor>();
    ctx.set("sensor", qval_external_borrowed<Sensor>(pt, std::weak_ptr<Sensor>(s)));
    expectVal("string-slot missing method no-op", ctx,
              "class UI { shown = 0\nshow(self, v)[] { self.shown = v } }\n"
              "local ui = UI()\n"
              "connect(sensor, \"levelChanged\", ui, \"nope\")\n"
              "emit sensor.levelChanged(7)\nlocal r = ui.shown;",
              "r", 0);
  }

  // 5. arity error: 0 args for a 1-arg signal.
  expectThrow("arity error", "emit sensor.levelChanged()");

  // 6. argument type error: string for an int parameter.
  expectThrow("argument type error", "emit sensor.levelChanged(\"x\")");

  // 7. unknown signal.
  expectThrow("unknown signal", "emit sensor.nope(1)");

  // 8. disconnect by handle: a subsequent emit does not call the handler.
  {
    QLangContext ctx;
    auto pt = ctx.import<Sensor>();
    auto s = std::make_shared<Sensor>();
    ctx.set("sensor", qval_external_borrowed<Sensor>(pt, std::weak_ptr<Sensor>(s)));
    expectVal("disconnect by handle", ctx,
              "seen = 0\ndef onLevel(v) { global seen; seen = v }\n"
              "local c = connect(sensor, \"levelChanged\", onLevel)\n"
              "disconnect(c)\n"
              "emit sensor.levelChanged(9)\nlocal r = seen;",
              "r", 0);
  }

  // 9. disconnect with a stale handle is a silent no-op.
  {
    QLangContext ctx;
    auto pt = ctx.import<Sensor>();
    auto s = std::make_shared<Sensor>();
    ctx.set("sensor", qval_external_borrowed<Sensor>(pt, std::weak_ptr<Sensor>(s)));
    try { ctx.run("disconnect(999)"); ok("stale handle no-op", true); }
    catch (const QLangError &e) { ok("stale handle no-op", false, e.what()); }
  }

  // 10. disconnect by signal: all connections for that signal are dropped.
  {
    QLangContext ctx;
    auto pt = ctx.import<Sensor>();
    auto s = std::make_shared<Sensor>();
    ctx.set("sensor", qval_external_borrowed<Sensor>(pt, std::weak_ptr<Sensor>(s)));
    expectVal("disconnect by signal", ctx,
              "seen = 0\ndef onLevel(v) { global seen; seen = v }\n"
              "connect(sensor, \"levelChanged\", onLevel)\n"
              "connect(sensor, \"levelChanged\", onLevel)\n"
              "disconnect(sensor, \"levelChanged\")\n"
              "emit sensor.levelChanged(9)\nlocal r = seen;",
              "r", 0);
  }

  // 11. auto-disconnect on sender destruction: the finalizer drain drops the
  // connection, so a later emit does not call the stale handler.
  {
    QLangContext ctx;
    auto pt = ctx.import<Sensor>();
    auto s = std::make_shared<Sensor>();
    ctx.set("sensor", qval_external_borrowed<Sensor>(pt, std::weak_ptr<Sensor>(s)));
    ctx.run("seen = 0\ndef onLevel(v) { global seen; seen = v }\n"
            "connect(sensor, \"levelChanged\", onLevel)");
    s.reset();              // destroy the sender
    ctx.processFinalizers();  // drain -> dropExpiredConnections
    ok("auto-disconnect on destruction",
       ctx.signalConnections(ctx.get("sensor"), "levelChanged").empty());
  }

  // 12. re-entrant emit: a handler that emits the same signal recurses and is
  // contained by the step budget (not a re-entrancy guard).
  expectThrow("re-entrant emit step limit",
              "seen = 0\n"
              "def onLevel(v) { global seen, sensor; seen = v; emit sensor.levelChanged(v) }\n"
              "connect(sensor, \"levelChanged\", onLevel)\n"
              "emit sensor.levelChanged(1)");

  // 13. C++-side emitSignal: the connected handler runs.
  {
    QLangContext ctx;
    auto pt = ctx.import<Sensor>();
    auto s = std::make_shared<Sensor>();
    ctx.set("sensor", qval_external_borrowed<Sensor>(pt, std::weak_ptr<Sensor>(s)));
    ctx.run("seen = 0\ndef onLevel(v) { global seen; seen = v }\n"
            "connect(sensor, \"levelChanged\", onLevel)");
    ctx.emitSignal(ctx.get("sensor"), "levelChanged", {QLangValue::Int(42)});
    ok("C++ emitSignal", toInt64(ctx.get("seen")) == 42);
  }

  // 14. hasSignal: true for a declared signal, false otherwise.
  {
    QLangContext ctx;
    auto pt = ctx.import<Sensor>();
    auto s = std::make_shared<Sensor>();
    ctx.set("sensor", qval_external_borrowed<Sensor>(pt, std::weak_ptr<Sensor>(s)));
    ok("hasSignal true", ctx.hasSignal(ctx.get("sensor"), "levelChanged"));
    ok("hasSignal false", !ctx.hasSignal(ctx.get("sensor"), "nope"));
  }

  // 15. emit on a non-external object is a type error.
  expectThrow("emit on non-external", "local t = {}\nemit t.signal(1)");

  // 16. signal name clashing with a member (method) is rejected at registration.
  {
    bool rejected = false;
    try { QLangContext c2; c2.import<Clash>(); }
    catch (const QLangError &) { rejected = true; }
    ok("signal/method clash rejected", rejected);
  }

  // 17. duplicate signal name is rejected at registration.
  {
    bool rejected = false;
    try { QLangContext c2; c2.import<DupSig>(); }
    catch (const QLangError &) { rejected = true; }
    ok("duplicate signal rejected", rejected);
  }

  std::cout << "\n" << passes << " passed, " << fails << " failed\n";
  return fails ? 1 : 0;
}
