// value_semantics.cpp — Phase 2 tests: pointer-backed array / set / blob.
//
// Validates docs/plans/deepcopy-plan.md Phase 2 against
// docs/spec/01-types.md `## value semantics`: every non-primitive except
// `string` shares its storage on assignment and on call, and a literal
// evaluates to fresh storage every time it is evaluated.
//
// Each test gets an isolated QLangContext. The executable exits 0 only if
// every expectation holds.
//
//   S1  Array shares on assignment: `let b = a` then `b[0] = 99` is visible
//       through `a`, and both handles name the same payload.
//
//   S2  Array shares on call: a function mutating its parameter mutates the
//       caller's array.
//
//   S3  Set shares on assignment (insert through the alias is visible).
//
//   S4  Array shares on call through a nested element (alias reached via an
//       outer array still names the inner payload).
//
//   L1  Array literal freshness: two `[1,2,3]` sites (and two identical ones
//       folded to a single constant-pool entry) never alias. Mutating the
//       first must not reach the second.
//
//   L2  Nested literal freshness: the inner array of `[[1,2]]` is fresh too.
//
//   L3  Table literal freshness: the constant-pool table aliasing bug — a
//       `{n=0, v=0}` literal built in a loop used to hand out one shared
//       table, so `cards` collected the same instance 500 times.
//
//   L4  Set literal freshness.
//
//   L5  Enum table still resolves after the MOVC clone (the enum hidden
//       table is a pool constant too).
//
//   D1  deepcopy independence: mutating a nested array of the copy leaves
//       the original alone, and the payloads differ.
//
//   D2  Array cycle: `a[] = a` is constructible now, and deepcopy preserves
//       the cycle (copy's element names the copy, not the original).
//
//   D3  Blob: assignment shares the payload, deepcopy does not.
//
//   X1  `a + b` returns a fresh array and never reaches into `a`.
//
//   X2  `cast<array>(set)` returns a fresh payload (a conversion is not an
//       alias).
//
//   X3  Self-append (`x += x`, `x[] = x`) terminates and doubles the array
//       instead of corrupting it through invalidated iterators.
//
//   X4  String stays a value type: it is the spec's locked exception, so a
//       copy is independent and `deepcopy` returns an equal string.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"
#include "qlang/QLangValue.h"

using namespace qlang;

namespace {

int g_failures = 0;

void pass(const char *label) { fprintf(stderr, "ok   %s\n", label); }

void failf(const char *label, const char *detail) {
  fprintf(stderr, "FAIL %s: %s\n", label, detail);
  g_failures++;
}

void checkTrue(const char *label, bool cond, const char *detail) {
  if (cond)
    pass(label);
  else
    failf(label, detail);
}

void checkInt(const char *label, int64_t got, int64_t want) {
  if (got == want) {
    fprintf(stderr, "ok   %s = %lld\n", label, (long long)got);
  } else {
    char buf[128];
    snprintf(buf, sizeof buf, "got %lld, expected %lld", (long long)got,
             (long long)want);
    failf(label, buf);
  }
}

// Run `src`, then read the top-level variable `var` back out of the context.
QLangValue runGet(QLangContext &ctx, const char *src, const char *var) {
  ctx.run(src);
  return ctx.get(var);
}

int64_t asInt(const QLangValue &v) { return (long long)toInt64(v); }

// Element view of an array/set value (throws if the type is neither, which the
// checks below treat as a test failure via the try/catch in main). Sets reuse
// the array payload alternative, so both are read the same way.
const std::vector<QLangValue> &elems(const QLangValue &v) {
  return v.type() == QLangValue::Type::Set ? *v.asSet() : *v.asArray();
}

int64_t elemInt(const QLangValue &v, size_t i) { return asInt(elems(v)[i]); }

} // namespace

int main() {
  // ---------------------------------------------------------------- S1
  // Array shares on assignment.
  {
    QLangContext ctx;
    try {
      QLangValue a = runGet(ctx,
                            "local a = [1, 2]\n"
                            "local b = a\n"
                            "b[0] = 99\n",
                            "a");
      checkInt("S1 a[0] visible through the alias", elemInt(a, 0), 99);
      QLangValue b = ctx.get("b");
      checkTrue("S1 both handles name one payload",
                a.asArray().get() == b.asArray().get(),
                "assignment copied the array");
    } catch (const std::exception &e) {
      failf("S1", e.what());
    }
  }

  // ---------------------------------------------------------------- S2
  // Array shares on call.
  {
    QLangContext ctx;
    try {
      QLangValue c = runGet(ctx,
                            "def mutate(x)\n"
                            "  x[0] = 42\n"
                            "end\n"
                            "local c = [1, 2]\n"
                            "mutate(c)\n",
                            "c");
      checkInt("S2 callee mutation reaches the caller", elemInt(c, 0), 42);
    } catch (const std::exception &e) {
      failf("S2", e.what());
    }
  }

  // ---------------------------------------------------------------- S3
  // Set shares on assignment.
  {
    QLangContext ctx;
    try {
      QLangValue s1 = runGet(ctx,
                             "local s1 = set(1, 2)\n"
                             "local s2 = s1\n"
                             "s2[] = 99\n",
                             "s1");
      checkInt("S3 insert through the alias is visible",
               (int64_t)elems(s1).size(), 3);
      QLangValue s2 = ctx.get("s2");
      checkTrue("S3 both handles name one payload",
                s1.asSet().get() == s2.asSet().get(),
                "assignment copied the set");
    } catch (const std::exception &e) {
      failf("S3", e.what());
    }
  }

  // ---------------------------------------------------------------- S4
  // An alias reached through an outer array names the inner payload.
  {
    QLangContext ctx;
    try {
      QLangValue outer = runGet(
          ctx,
          "local inner = [1, 2]\n"
          "local outer = [inner]\n"
          "let got = outer[0]\n"
          "got[0] = 77\n",
          "outer");
      checkInt("S4 mutation through outer[0]",
               elemInt(elems(outer)[0], 0), 77);
      QLangValue inner = ctx.get("inner");
      checkInt("S4 the original sees it too", elemInt(inner, 0), 77);
    } catch (const std::exception &e) {
      failf("S4", e.what());
    }
  }

  // ---------------------------------------------------------------- L1
  // Array literal freshness (the constant pool must not leak its payload).
  {
    QLangContext ctx;
    try {
      QLangValue l2 = runGet(ctx,
                             "local l1 = [1, 2, 3]\n"
                             "l1[0] = 99\n"
                             "local l2 = [1, 2, 3]\n",
                             "l2");
      checkInt("L1 second literal unaffected", elemInt(l2, 0), 1);
      QLangValue l1 = ctx.get("l1");
      checkTrue("L1 literals do not share a payload",
                l1.asArray().get() != l2.asArray().get(),
                "both literals name the constant-pool payload");
    } catch (const std::exception &e) {
      failf("L1", e.what());
    }
  }

  // ---------------------------------------------------------------- L2
  // Nested literal freshness.
  {
    QLangContext ctx;
    try {
      QLangValue n1 = runGet(ctx,
                             "local n1 = [[1, 2]]\n"
                             "let inner = n1[0]\n"
                             "inner[0] = 99\n"
                             "local n2 = [[1, 2]]\n",
                             "n1");
      checkInt("L2 mutation through the nested literal",
               elemInt(elems(n1)[0], 0), 99);
      QLangValue n2 = ctx.get("n2");
      checkInt("L2 second nested literal unaffected",
               elemInt(elems(n2)[0], 0), 1);
    } catch (const std::exception &e) {
      failf("L2", e.what());
    }
  }

  // ---------------------------------------------------------------- L3
  // Table literal freshness: the regression perf_stress.ql exposed. One
  // shared pool table made every `cards` entry the same instance, so `acc`
  // summed 500 copies of the LAST row instead of 1..500.
  {
    QLangContext ctx;
    try {
      QLangValue acc = runGet(
          ctx,
          "local cards = []\n"
          "for i = 1 to 5\n"
          "  local c = {n = 0, v = 0}\n"
          "  c.n = i\n"
          "  c.v = i * 10\n"
          "  cards[] = c\n"
          "next i\n"
          "local acc = 0\n"
          "for i = 0 to 4\n"
          "  acc = acc + cards[i].v\n"
          "next i\n",
          "acc");
      // 10 + 20 + 30 + 40 + 50 (the aliased pool table used to yield 250).
      checkInt("L3 table literal is fresh per evaluation", asInt(acc), 150);
    } catch (const std::exception &e) {
      failf("L3", e.what());
    }
  }

  // ---------------------------------------------------------------- L4
  // Set literal freshness.
  {
    QLangContext ctx;
    try {
      QLangValue q1 = runGet(ctx,
                             "local q1 = set(1, 2)\n"
                             "q1[] = 3\n"
                             "local q2 = set(1, 2)\n",
                             "q1");
      checkInt("L4 mutated set literal", (int64_t)elems(q1).size(), 3);
      QLangValue q2 = ctx.get("q2");
      checkInt("L4 second set literal unaffected",
               (int64_t)elems(q2).size(), 2);
    } catch (const std::exception &e) {
      failf("L4", e.what());
    }
  }

  // ---------------------------------------------------------------- L5
  // The enum hidden table is a pool constant too: cloning it at MOVC must
  // not break member lookup.
  {
    QLangContext ctx;
    try {
      QLangValue v = runGet(ctx,
                            "enum Color {\n"
                            "  RED = 7,\n"
                            "  GREEN\n"
                            "}\n"
                            "local g = Color.GREEN\n"
                            "local n = len(Color)\n",
                            "g");
      checkInt("L5 enum member after the MOVC clone", asInt(v), 8);
      checkInt("L5 enum table size", asInt(ctx.get("n")), 2);
    } catch (const std::exception &e) {
      failf("L5", e.what());
    }
  }

  // ---------------------------------------------------------------- D1
  // deepcopy independence through a nested array.
  {
    QLangContext ctx;
    try {
      QLangValue d1 = runGet(ctx,
                             "local d1 = [1, [2, 3]]\n"
                             "local d2 = deepcopy(d1)\n"
                             "let dn = d2[1]\n"
                             "dn[0] = 99\n",
                             "d1");
      checkInt("D1 original nested element untouched",
               elemInt(elems(d1)[1], 0), 2);
      QLangValue d2 = ctx.get("d2");
      checkInt("D1 copy nested element mutated", elemInt(elems(d2)[1], 0), 99);
      checkTrue("D1 copy does not share the outer payload",
                d1.asArray().get() != d2.asArray().get(),
                "deepcopy returned the same handle");
      checkTrue("D1 copy does not share the nested payload",
                elems(d1)[1].asArray().get() != elems(d2)[1].asArray().get(),
                "deepcopy shared the nested array");
    } catch (const std::exception &e) {
      failf("D1", e.what());
    }
  }

  // ---------------------------------------------------------------- D2
  // A cycle through an array is constructible now, and deepcopy preserves
  // it: the copy's element names the COPY (not the original, not a second
  // infinite expansion).
  {
    QLangContext ctx;
    try {
      QLangValue cyc = runGet(ctx,
                              "local cyc = [1]\n"
                              "cyc[] = cyc\n"
                              "local cc = deepcopy(cyc)\n",
                              "cyc");
      checkInt("D2 original is [1, <self>]", (int64_t)elems(cyc).size(), 2);
      QLangValue cc = ctx.get("cc");
      checkInt("D2 copy carries the same shape", (int64_t)elems(cc).size(), 2);
      checkInt("D2 copy head element", elemInt(cc, 0), 1);
      checkTrue("D2 copy is a distinct payload",
                cyc.asArray().get() != cc.asArray().get(),
                "deepcopy returned the original handle");
      checkTrue("D2 the cycle points at the copy",
                elems(cc)[1].asArray().get() == cc.asArray().get(),
                "deepcopy did not preserve the cycle");
      checkTrue("D2 the cycle does not point back at the original",
                elems(cc)[1].asArray().get() != cyc.asArray().get(),
                "deepcopy shared the original cycle");
    } catch (const std::exception &e) {
      failf("D2", e.what());
    }
  }

  // ---------------------------------------------------------------- D3
  // Blob: assignment shares the payload, deepcopy does not. Blobs have no
  // script-visible mutation operator, so this is exercised host-side.
  {
    QLangContext ctx;
    QLangValue b = QLangValue::Blob(std::vector<uint8_t>{1, 2, 3});
    const void *payload = b.asBlob().get();
    ctx.set("b", b);
    ctx.run("local alias = b\nlocal c = deepcopy(b)\n");
    QLangValue alias = ctx.get("alias");
    checkTrue("D3 assignment shares the blob payload",
              alias.asBlob().get() == payload, "assignment copied the blob");
    QLangValue c = ctx.get("c");
    checkTrue("D3 deepcopy returns a distinct payload",
              c.asBlob().get() != payload, "deepcopy shared the blob");
    checkTrue("D3 the copy carries the same bytes",
              *c.asBlob() == *b.asBlob(), "deepcopy lost bytes");
  }

  // ---------------------------------------------------------------- X1
  // `a + b` is a fresh array.
  {
    QLangContext ctx;
    try {
      QLangValue ca = runGet(ctx,
                             "local ca = [1]\n"
                             "local cb = ca + [2]\n",
                             "ca");
      checkInt("X1 left operand untouched", (int64_t)elems(ca).size(), 1);
      QLangValue cb = ctx.get("cb");
      checkInt("X1 result carries both", (int64_t)elems(cb).size(), 2);
      checkTrue("X1 result is a fresh payload",
                ca.asArray().get() != cb.asArray().get(),
                "concat aliased the left operand");
    } catch (const std::exception &e) {
      failf("X1", e.what());
    }
  }

  // ---------------------------------------------------------------- X2
  // A conversion is not an alias.
  {
    QLangContext ctx;
    try {
      QLangValue cs = runGet(ctx,
                             "local cs = set(1, 2)\n"
                             "local car = cast<array>(cs)\n"
                             "car[] = 3\n",
                             "cs");
      checkInt("X2 source set untouched", (int64_t)elems(cs).size(), 2);
      QLangValue car = ctx.get("car");
      checkInt("X2 cast result mutated", (int64_t)elems(car).size(), 3);
    } catch (const std::exception &e) {
      failf("X2", e.what());
    }
  }

  // ---------------------------------------------------------------- X3
  // Self-append: the source and destination payloads are the same vector,
  // so the insert must snapshot instead of iterating what it is growing.
  {
    QLangContext ctx;
    try {
      QLangValue x = runGet(ctx,
                            "local x = [1, 2]\n"
                            "x += x\n",
                            "x");
      checkInt("X3 x += x doubles the array", (int64_t)elems(x).size(), 4);
      checkInt("X3 first element survives", elemInt(x, 0), 1);
      checkInt("X3 wrapped element", elemInt(x, 2), 1);
    } catch (const std::exception &e) {
      failf("X3", e.what());
    }
  }

  // ---------------------------------------------------------------- X4
  // String stays the value-type exception.
  {
    QLangContext ctx;
    try {
      QLangValue st = runGet(ctx,
                             "local st = \"ab\"\n"
                             "local st2 = deepcopy(st)\n",
                             "st");
      checkTrue("X4 deepcopy of a string is a string",
                st.type() == QLangValue::Type::String, "type changed");
      QLangValue st2 = ctx.get("st2");
      checkTrue("X4 the copy carries the same text",
                st2.asString() == "ab", st2.asString().c_str());
      checkTrue("X4 string storage is not pointer-backed",
                st.type() == QLangValue::Type::String, "string was pointerized");
    } catch (const std::exception &e) {
      failf("X4", e.what());
    }
  }

  if (g_failures) {
    fprintf(stderr, "%d FAILURES\n", g_failures);
    return 1;
  }
  fprintf(stderr, "value_semantics: all checks passed\n");
  return 0;
}
