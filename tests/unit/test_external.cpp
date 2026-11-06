// External C++ types: the TDD suite for docs/EXTERNAL_TYPES.md (plan v2,
// Tasks 5-6). Covers public-member setters, read-only computed props, methods
// (incl. extraction as a first-class BoundMethod value), enum constants,
// str, handoff integrity, registration-time validation, and QL_FLAGS
// (bitmask flag tables resolve and OR in script), and the registerEnum C++
// injection API (SPEC §enum).
//
// Written against the real throwing API: run() throws QLangError on
// failure; results come back via get().
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"
#include "qlang/QLangMeta.h"

using namespace qlang;

enum class Kind { A = 1, B = 2 };

// CRTP base `qlang::QLObject<Point>` names the class once and supplies the
// `SelfType` alias the entry macros use for qualified member pointers.
struct Point : qlang::QLObject<Point> {
  Point(double x_, double y_) : x(x_), y(y_) {}
  double x = 0, y = 0;                                  // public -> read-write
  double dist() const { return std::sqrt(x * x + y * y); }  // const getter -> read-only
  void translate(double dx, double dy) { x += dx; y += dy; }
  std::string str() const {
    return "Point(" + std::to_string(x) + "," + std::to_string(y) + ")";
  }

  QL_OBJECT(
      QL_PROPERTY(x),
      QL_PROPERTY(y),
      QL_PROPERTY(dist),
      QL_INVOKABLE(translate),
      QL_ENUM(Kind, A, B),
      QL_STR(str),
      QL_CTOR([](QLangContext &c, const std::vector<QValue> &a) {
        return std::make_shared<Point>(a[0].as<double>(), a[1].as<double>());
      }));
};

// Task 6: QL_FLAGS — a flag-style table (bitmask values). QL_FLAGS is the
// same enum_table as QL_ENUM (marked); the script treats the constants as
// native ints and combines them with |. Six bitmask enumerators.
enum class DockOption : int {
  None = 0,
  AnimatedDocks = 1,
  AllowTabbedDocks = 2,
  VerticalTabs = 4,
  AllowNestedDocks = 8,
  AllowFloat = 16,
};

struct DockArea : qlang::QLObject<DockArea> {
  QL_OBJECT(QL_FLAGS(DockOption, None, AnimatedDocks, AllowTabbedDocks,
                     VerticalTabs, AllowNestedDocks, AllowFloat));
};

static int failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      ++failures;                                                       \
      printf("FAIL %d: %s\n", __LINE__, #cond);                         \
    }                                                                   \
  } while (0)

// run() throws QLangError on failure; results come back via get().
static QValue runAndGet(QLangContext &ctx, const std::string &expr) {
  ctx.run("r = " + expr);
  return ctx.get("r");
}
static bool runThrows(QLangContext &ctx, const std::string &src) {
  try {
    ctx.run(src);
    return false;
  } catch (const QLangError &) {
    return true;
  }
}

int main() {
  QLangContext ctx;
  auto pt = ctx.import<Point>();  // shared handle (Phase 3)
  ctx.import<DockArea>();  // seeds the DockArea TypeRef (no instance needed)

  // 1. Public data member = read-write: script SETTER works and is visible to C++.
  auto e = std::make_shared<Point>(1, 2);
  ctx.set("e", qval_external_borrowed<Point>(pt, std::weak_ptr<Point>(e)));
  CHECK(!runThrows(ctx, "e.x = 10"));  // setter on public member
  CHECK(e->x == 10);                   // referential integrity: same object
  CHECK(runAndGet(ctx, "e.y + e.x").asFloat() == 12);

  // 2. Computed/const property is read-only.
  CHECK(runThrows(ctx, "e.dist = 9"));  // read-only -> error
  CHECK(runAndGet(ctx, "e.dist").asFloat() > 0);

  // 3. Method mutation (TABGET -> BoundMethod -> CALLCLOS).
  CHECK(!runThrows(ctx, "e.translate(1, 1)"));
  CHECK(e->x == 11 && e->y == 3);

  // 4. str / type (valueToString -> toStr wiring). The contract is
  // format-independent: the script's rendering must equal the C++ binding's
  // output exactly.
  CHECK(!runThrows(ctx, "s = f\"{e}\"; t = type(e)"));
  CHECK(ctx.get("s").asString() == e->str());
  CHECK(ctx.get("t").asString() == "Point");

  // 5. Script construction via the type name in the native table.
  CHECK(runAndGet(ctx, "Point(3, 4).dist").asFloat() == 5);

  // 6. Enum group: named constants via the TypeRef value + flag combination.
  CHECK(runAndGet(ctx, "Point.A | Point.B").asInt() == 3);

  // 7. Unknown member / method.
  CHECK(runThrows(ctx, "e.nope"));
  CHECK(runThrows(ctx, "e.warp()"));

  // 8. Engine destroys the object -> clean "object destroyed".
  e.reset();
  CHECK(runThrows(ctx, "e.x"));

  // 9. Validation: duplicate member name rejected.
  struct Dup : qlang::QLObject<Dup> {
    int a = 0, b = 0;
    int getB() const { return b; }
    void setB(int v) { b = v; }
    QL_OBJECT(QL_PROPERTY(a),
              QL_PROPERTY(a, READ(getB), WRITE(setB)));  // name "a" twice
  };
  bool rejected = false;
  try {
    QLangContext c2;
    c2.import<Dup>();
  } catch (const QLangError &) {
    rejected = true;
  }
  CHECK(rejected);

  // 10. Method extraction: a kind-2 read is a first-class BoundMethod value.
  // The CALLCLOS dispatch of that value is pinned by check 3 (the compiler
  // emits exactly MOV + CALLCLOS for `e.translate(1, 1)`); a bare-variable
  // callee like `m(...)` is a compile error in this zero-compiler-changes
  // design, so extraction is verified at the value level here.
  auto e2 = std::make_shared<Point>(0, 0);
  ctx.set("e2", qval_external_borrowed<Point>(pt, std::weak_ptr<Point>(e2)));
  CHECK(!runThrows(ctx, "m = e2.translate"));
  CHECK(ctx.get("m").type() == QValue::Type::BoundMethod);
  CHECK(runAndGet(ctx, "type(e2)").asString() == "Point");  // type() on the instance

  // 11. type-level value: type(Point) names the type; Point is not settable.
  CHECK(runAndGet(ctx, "type(Point)").asString() == "Point");
  CHECK(runThrows(ctx, "Point = 1"));

  // 12. QL_FLAGS (Task 6): a flag-style table (bitmask values) resolves and
  // ORs in script. Named constant resolves to its int value; two constants
  // combine with the native int bit-op (1 | 2 == 3).
  CHECK(runAndGet(ctx, "DockArea.AnimatedDocks").asInt() == 1);
  CHECK(runAndGet(ctx, "DockArea.AnimatedDocks | DockArea.AllowTabbedDocks")
            .asInt() == 3);

  // 13. registerEnum (SPEC §enum "C++ api needed to inject enums"): the
  // injected enum behaves like a top-level QLang enum — both member
  // notations fold, the bare name resolves to the hidden const table, and
  // the name is visible inside a function body (global-enum semantics).
  {
    QLangContext c;
    CHECK(c.registerEnum("Animal", {{"Dog", 0}, {"Cat", 1}, {"Horse", 2}}));
    CHECK(runAndGet(c, "Animal::Dog").asInt() == 0);
    CHECK(runAndGet(c, "Animal.Cat").asInt() == 1);
    CHECK(runAndGet(c, "Animal::Horse").asInt() == 2);
    CHECK(runAndGet(c, "len(Animal)").asInt() == 3);
    CHECK(!runThrows(c, "def f() { return Animal::Cat }; g = f()"));
    CHECK(c.get("g").asInt() == 1);
    // The hidden table is a const: not writable from script.
    CHECK(runThrows(c, "__enum_Animal = 1"));
  }

  // 14. Base type: members are typed like a declared enum's (byte -> the
  // member value converts losslessly; type() of the folded value is byte).
  {
    QLangContext c;
    c.registerEnum("Small", {{"Zero", 0}, {"One", 1}}, QLangValue::Type::Byte);
    CHECK(runAndGet(c, "cast<int>(Small::One)").asInt() == 1);
    CHECK(runAndGet(c, "type(Small::One)").asString() == "byte");
  }

  // 15. A script enum cannot redeclare an injected one; an unknown member
  // stays a compile error.
  {
    QLangContext c;
    c.registerEnum("Animal", {{"Dog", 0}});
    CHECK(runThrows(c, "enum Animal { A }"));
    CHECK(runThrows(c, "Animal::Nope"));
  }

  // 16. Validation: empty member list, duplicate member, name already in
  // use, reserved-word name, non-numeric base type -> QLangError.
  {
    QLangContext c;
    auto throws = [](const std::function<bool()> &fn) {
      try {
        fn();
        return false;
      } catch (const QLangError &) {
        return true;
      }
    };
    CHECK(throws([&] { return c.registerEnum("E", {}); }));
    CHECK(throws([&] { return c.registerEnum("E", {{"A", 0}, {"A", 1}}); }));
    c.set("Animal", QValue::Int(1));
    CHECK(throws([&] { return c.registerEnum("Animal", {{"Dog", 0}}); }));
    CHECK(throws([&] { return c.registerEnum("if", {{"A", 0}}); }));
    CHECK(throws([&] { return c.registerEnum("9bad", {{"A", 0}}); }));
    CHECK(throws([&] {
      return c.registerEnum("E", {{"A", 0}}, QLangValue::Type::String);
    }));
  }

  // 17. remove(): the enum and its hidden table vanish — the next compile
  // no longer resolves the name, and re-injection is accepted.
  {
    QLangContext c;
    c.registerEnum("Animal", {{"Dog", 0}});
    CHECK(!runThrows(c, "v = Animal::Dog"));
    c.remove("Animal");
    CHECK(runThrows(c, "Animal::Dog"));
    CHECK(!c.isset("__enum_Animal"));
    CHECK(c.registerEnum("Animal", {{"Cat", 5}}));
    CHECK(runAndGet(c, "Animal::Cat").asInt() == 5);
  }

  // 18. clear(): same invisibility; re-injection after clear works.
  {
    QLangContext c;
    c.registerEnum("Animal", {{"Dog", 0}});
    c.clear();
    CHECK(runThrows(c, "Animal::Dog"));
    CHECK(c.registerEnum("Animal", {{"Cat", 7}}));
    CHECK(runAndGet(c, "Animal::Cat").asInt() == 7);
  }

  // 19. hasMethod / callMethod context wrappers (missing-features-plan C3;
  // docs/EVENT_HANDLERS.md §4). Host-only surface over ObjectOps — the VM
  // path (TABGET -> BoundMethod -> CALLCLOS) is unchanged.
  {
    auto p = std::make_shared<Point>(1, 2);
    QValue ext = qval_external_borrowed<Point>(pt, std::weak_ptr<Point>(p));
    CHECK(ctx.hasMethod(ext, "translate"));   // QL_INVOKABLE method
    CHECK(!ctx.hasMethod(ext, "x"));          // property, not callable
    CHECK(!ctx.hasMethod(ext, "dist"));       // read-only property
    CHECK(!ctx.hasMethod(ext, "warp"));       // unknown member
    CHECK(!ctx.hasMethod(QValue::Int(7), "translate"));  // no ObjectOps
    CHECK(!runThrows(ctx, "h = {}"));         // ensure a program is compiled
    QValue handlers = ctx.get("h");
    CHECK(!ctx.hasMethod(handlers, "onLoad"));  // empty table: no member
    // Table closures take self as the first argument (the same convention
    // as `t.f()` in script: TABGET -> BoundMethod -> CALLCLOS passes self).
    CHECK(!runThrows(ctx, "h.onLoad = (self, a, b)[] { return a + b }"));
    CHECK(ctx.hasMethod(handlers, "onLoad"));   // table closure
    QValue r = ctx.callMethod(handlers, "onLoad", {QValue::Int(2), QValue::Int(3)});
    CHECK(r.asInt() == 5);                      // self passed as first arg
    CHECK(!runThrows(ctx, "h.n = 1"));
    CHECK(!ctx.hasMethod(handlers, "n"));       // member exists, not callable
    bool threw = false;
    try {
      ctx.callMethod(handlers, "nope", {});
    } catch (const QLangError &) {
      threw = true;
    }
    CHECK(threw);                               // unknown method throws
    threw = false;
    try {
      ctx.callMethod(QValue::Int(7), "f", {});
    } catch (const QLangError &) {
      threw = true;
    }
    CHECK(threw);                               // non-object value throws
  }

  // 20. callMethod on an external object dispatches ops->callMethod (zero
  // VM call sites in v1 — this wrapper is the only caller).
  {
    auto p = std::make_shared<Point>(10, 20);
    QValue ext = qval_external_borrowed<Point>(pt, std::weak_ptr<Point>(p));
    ctx.callMethod(ext, "translate", {QValue::Int(5), QValue::Int(-5)});
    CHECK(p->x == 15 && p->y == 15);
    bool threw = false;
    try {
      ctx.callMethod(ext, "warp", {});
    } catch (const QLangError &) {
      threw = true;
    }
    CHECK(threw);  // "no method 'warp' ..." — Q20: throw, mirroring emitSignal
  }

  if (failures) {
    printf("%d FAILURES\n", failures);
    return 1;
  }
  printf("test_external: all checks passed\n");
  return 0;
}
