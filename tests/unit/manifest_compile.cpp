// Compile-time external-type validation from a QLM1 manifest
// (docs/plans/portable-module-plan.md P4). Drives QLangCompiler directly with
// hand-built ManifestType fixtures and NO context registration — the
// engine-agnostic compile. Covers the plan's negative matrix (unknown
// field/method, ro write, arity, tag mismatch, no-ctor construction, typed
// vs context ctor, unknown signal, bad T::M), the receiver-liveness rule
// (checked on a single init, lenient after a second write), shadowing inside
// function bodies, manifest-enum folding and registerEnum drift, and
// (argv[1] = qlang_run binary) the --qlmeta CLI green/red smoke.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
#include <sys/wait.h> // WEXITSTATUS for the CLI smoke

#include "qlang/QLangCompiler.h"
#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"
#include "qlang/QLangLexer.h"
#include "qlang/QLangManifest.h"
#include "qlang/QLangParser.h"
#include "qlang/QLangVM.h"

using namespace qlang;

static int failures = 0;
static int checks = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    ++checks;                                                           \
    if (!(cond)) {                                                      \
      ++failures;                                                       \
      printf("FAIL %d: %s\n", __LINE__, #cond);                         \
    }                                                                   \
  } while (0)

// ---------------------------------------------------------------------------
// Fixtures: the manifest shapes P1 made describable, hand-built (no
// QL_OBJECT needed — the compiler consumes exactly what readManifest feeds).
// ---------------------------------------------------------------------------

static ManifestType pointType() {
  ManifestType t;
  t.name = "Point";
  t.hasCtor = true;
  t.ctorArgTags = {QTypeTag::Int, QTypeTag::Float}; // typed QL_CTOR form
  ManifestMember id;
  id.kind = 1; // ro prop
  id.name = "id";
  id.getTag = QTypeTag::Int;
  ManifestMember x;
  x.kind = 0; // rw prop
  x.name = "x";
  x.getTag = x.setTag = QTypeTag::Int;
  ManifestMember mass;
  mass.kind = 0;
  mass.name = "mass";
  mass.getTag = mass.setTag = QTypeTag::Float;
  ManifestMember scaled;
  scaled.kind = 2;
  scaled.name = "scaled";
  scaled.retTag = QTypeTag::Float;
  scaled.argTags = {QTypeTag::Float};
  ManifestMember movedTo;
  movedTo.kind = 2;
  movedTo.name = "moved_to";
  movedTo.argTags = {QTypeTag::Int, QTypeTag::Int};
  ManifestMember distance;
  distance.kind = 2;
  distance.name = "distance";
  distance.argTags = {QTypeTag::External}; // dynamic-side arg stays silent
  t.members = {id, x, mass, scaled, movedTo, distance};
  EnumMeta side;
  side.typeName = "Side";
  side.entries = {{"Left", 0}, {"Right", 5}};
  t.enums = {side};
  ManifestSignal moved;
  moved.name = "moved";
  moved.argTags = {QTypeTag::Int, QTypeTag::Int};
  t.signals = {moved};
  return t;
}

static ManifestType widgetType() {
  ManifestType t;
  t.name = "Widget";
  t.hasCtor = false;
  ManifestMember label;
  label.kind = 0;
  label.name = "label";
  label.getTag = label.setTag = QTypeTag::Str;
  t.members = {label};
  return t;
}

static ManifestType bagType() {
  ManifestType t;
  t.name = "Bag";
  t.hasCtor = true;
  t.ctorArgTags = {}; // context-taking QL_CTOR: presence-only checks
  return t;
}

// ---------------------------------------------------------------------------
// Compile helpers — no QLangContext anywhere (engine-agnostic compile).
// ---------------------------------------------------------------------------

static std::vector<ManifestType> fixtures() {
  return {pointType(), widgetType(), bagType()};
}

static CompiledProgram compileSrc(
    const std::string &src, const std::vector<ManifestType> &types,
    const std::unordered_set<std::string> &natives = {}) {
  QLangLexer lexer(src);
  QLangParser parser(lexer.tokenize());
  QLangCompiler compiler(parser.parseProgram());
  compiler.addExternalFuncs(natives);
  if (!types.empty())
    compiler.addExternalTypes(types);
  return compiler.compile();
}

static bool compiles(const std::string &src,
                     const std::vector<ManifestType> &types = fixtures(),
                     const std::unordered_set<std::string> &natives = {}) {
  try {
    compileSrc(src, types, natives);
    return true;
  } catch (const QLangError &e) {
    printf("  unexpected compile error: %s\n", e.message().c_str());
    return false;
  }
}

static bool failsWith(const std::string &src, const std::string &fragment,
                      const std::vector<ManifestType> &types = fixtures(),
                      const std::unordered_set<std::string> &natives = {}) {
  try {
    compileSrc(src, types, natives);
    printf("  expected failure (%s): but compiled\n", fragment.c_str());
    return false;
  } catch (const QLangError &e) {
    if (e.message().find(fragment) == std::string::npos) {
      printf("  wrong message: [%s] want [%s]\n", e.message().c_str(),
             fragment.c_str());
      return false;
    }
    return true;
  }
}

int main(int argc, char **argv) {
  // 1. Positive: a full valid script compiles with no ctx registration —
  // type name, ctor, props, methods, type-qualified enum, signal emit.
  CHECK(compiles("local p = Point(1, 2.5)\n"
                 "p.x = 3\n"
                 "p.mass = 4.0\n"
                 "local i = p.id\n"
                 "local r = Point.Right\n"
                 "p.moved_to(1, 2)\n"
                 "print(p.scaled(2.0))\n"
                 "print(p.distance(p))\n"
                 "emit p.moved(1, 2)\n"));

  // 2. Unknown field / unknown method on a known receiver.
  CHECK(failsWith("local p = Point(1, 2.5)\nprint(p.zzz)\n",
                  "type 'Point' has no field 'zzz'"));
  CHECK(failsWith("local p = Point(1, 2.5)\np.nope()\n",
                  "type 'Point' has no method 'nope'"));

  // 3. Read-only property write (VM wording) — plain and compound.
  CHECK(failsWith("local p = Point(1, 2.5)\np.id = 1\n",
                  "cannot assign to const field 'id'"));
  CHECK(failsWith("local p = Point(1, 2.5)\np.id += 1\n",
                  "cannot assign to const field 'id'"));
  CHECK(failsWith("local p = Point(1, 2.5)\np.scaled = 1\n",
                  "field 'scaled' on type 'Point' is a method"));

  // 4. Method arity + argument tags (inferred literals only; the binding's
  // wording). External-tagged parameters stay silent.
  CHECK(failsWith("local p = Point(1, 2.5)\np.scaled(1.0, 2.0)\n",
                  "method expects 1 argument(s), got 2"));
  CHECK(failsWith("local p = Point(1, 2.5)\np.scaled(\"s\")\n",
                  "argument 1 expects Float, got Str"));
  CHECK(compiles("local p = Point(1, 2.5)\nprint(p.scaled(1))\n")); // Int->Float
  CHECK(compiles("local p = Point(1, 2.5)\nlocal t = p.x\nprint(p.scaled(t))\n")); // dynamic: silent

  // 5. Construction: no ctor, typed-ctor arity/tags, context-form presence.
  CHECK(failsWith("local w = Widget(1)\n",
                  "type 'Widget' has no constructor"));
  CHECK(failsWith("local p = Point(1)\n",
                  "constructor expects 2 argument(s), got 1"));
  CHECK(failsWith("local p = Point(\"a\", 1.0)\n",
                  "argument 1 expects Int, got Str"));
  CHECK(compiles("local b = Bag(1, 2, 3, \"x\")\n")); // presence-only
  // The type-name checks hold inside function bodies too.
  CHECK(failsWith("def g()\n  local w = Widget(1)\nend\n",
                  "type 'Widget' has no constructor"));

  // 6. Signals: emit existence + arity, connect/disconnect static names.
  CHECK(failsWith("local p = Point(1, 2.5)\nemit p.zap(1)\n",
                  "unknown signal 'zap' on type 'Point'"));
  CHECK(failsWith("local p = Point(1, 2.5)\nemit p.moved(1)\n",
                  "signal 'moved' on type 'Point' expects 2 argument(s), got 1"));
  CHECK(failsWith("local p = Point(1, 2.5)\nconnect(p, \"zap\", 0)\n",
                  "unknown signal 'zap' on type 'Point'", fixtures(),
                  {"connect"}));
  CHECK(compiles("local p = Point(1, 2.5)\nconnect(p, \"moved\", 0)\n",
                 fixtures(), {"connect"}));
  CHECK(compiles("disconnect(0, \"whatever\")\n",
                 fixtures(), {"disconnect"})); // non-variable receiver: lenient

  // 7. Static access: T::M / T.M for an enum entry folds, unknown is refused,
  // a method read bare off the type name names the call spelling.
  CHECK(compiles("local a = Point::Right\nlocal b = Point.Right\n"
                 "local c = Point::Left\n",
                 fixtures()));
  CHECK(failsWith("local v = Point::nope\n", "type 'Point' has no field 'nope'"));
  CHECK(failsWith("local v = Point.nope\n", "type 'Point' has no field 'nope'"));
  CHECK(failsWith("local f = Point.scaled\n", "is a method, not a value"));
  CHECK(compiles("local p = Point(1, 2.5)\nlocal f = p.scaled\n")); // BoundMethod

  // 8. Static method call on a type name: membership validated (the dynamic
  // TypeRef path stays runtime); unknown is refused at compile time.
  CHECK(compiles("local v = Point.scaled(1.0)\n"));
  CHECK(compiles("local v = Point::scaled(1.0)\n"));
  CHECK(failsWith("Point.nope()\n", "type 'Point' has no method 'nope'"));

  // 9. Receiver liveness: checked with a single init (declaration form OR a
  // plain assignment), lenient once a second write exists anywhere.
  CHECK(failsWith("p = Point(1, 2.5)\nprint(p.zzz)\n",
                  "type 'Point' has no field 'zzz'"));
  CHECK(compiles("local p = Point(1, 2.5)\np = 5\np.nope()\n"));
  CHECK(compiles("local p = Point(1, 2.5)\ndef f()\n  p = 5\nend\n"
                 "p.nope()\n"));
  // A parameter shadowing a known receiver name keeps function bodies
  // lenient (the receiver check is top-level only in v1).
  CHECK(compiles("local p = Point(1, 2.5)\n"
                 "def f(p)\n  local t = p.zzz\nend\n"
                 "print(p.x)\n"));
  CHECK(failsWith("local p = Point(1, 2.5)\nprint(p.zzz)\n",
                  "has no field", fixtures()));

  // 10. Enum folding executes as a plain Int constant (no bindings, no
  // TypeRef seeding needed): the VM leaves 5 in r.
  {
    CompiledProgram p = compileSrc("r = Point.Right\n", fixtures());
    QLangContext ctx;
    QLangVM vm(p, ctx);
    vm.run();
    CHECK(ctx.get("r").asInt() == 5);
  }

  // 11. Manifest enums inject like registerEnum (hidden var + fold) and
  // refuse value drift between the two sources.
  {
    QLangLexer lexer("local v = Side.Right\n");
    QLangParser parser(lexer.tokenize());
    QLangCompiler compiler(parser.parseProgram());
    compiler.addExternalEnums(
        {{"Side", {{"Left", QLangValue::Int(0)}, {"Right", QLangValue::Int(5)}}}});
    compiler.addExternalTypes(fixtures()); // agrees — must not throw
    CHECK(true);
  }
  {
    QLangLexer lexer("local v = 1\n");
    QLangParser parser(lexer.tokenize());
    QLangCompiler compiler(parser.parseProgram());
    compiler.addExternalEnums(
        {{"Side", {{"Left", QLangValue::Int(0)}, {"Right", QLangValue::Int(9)}}}});
    bool threw = false;
    try {
      compiler.addExternalTypes(fixtures());
    } catch (const QLangError &e) {
      threw = e.message().find("disagrees") != std::string::npos;
    }
    CHECK(threw);
  }
  // A duplicate type name in the manifest is refused (never silently kept).
  {
    QLangLexer lexer("local v = 1\n");
    QLangParser parser(lexer.tokenize());
    QLangCompiler compiler(parser.parseProgram());
    bool threw = false;
    try {
      compiler.addExternalTypes({pointType(), pointType()});
    } catch (const QLangError &e) {
      threw = e.message().find("duplicate external type 'Point'") !=
              std::string::npos;
    }
    CHECK(threw);
  }

  // 12. No manifest -> no new checks (today's lenient path).
  CHECK(compiles("local p\nprint(p.zzz)\np.nope(1, 2, 3)\n", {}));

  // 13. CLI smoke (argv[1] = qlang_run binary): engine-agnostic compile is
  // green with the manifest, red on a checked violation, file errors exit 1.
  if (argc > 1) {
    const std::string dir = "/tmp/opencode/manifest_compile";
    std::system(("mkdir -p " + dir).c_str());
    std::vector<ManifestType> m;
    m.push_back(pointType());
    m.push_back(widgetType());
    {
      std::ofstream out(dir + "/host.qlm", std::ios::binary | std::ios::trunc);
      writeManifest(out, m);
    }
    auto writeFile = [](const std::string &path, const std::string &body) {
      std::ofstream f(path);
      f << body;
    };
    // Green script must also RUN (qlang_run executes after -c): enum folding
    // is a pure Int constant — no constructor natives needed without a host.
    writeFile(dir + "/good.ql", "print(Point.Right)\n");
    writeFile(dir + "/bad.ql", "local p = Point(1, 2.5)\nprint(p.zzz)\n");
    const std::string run = std::string(argv[1]);
    CHECK(WEXITSTATUS(std::system((run + " --qlmeta " + dir +
                                   "/host.qlm -c " + dir + "/good.ql" +
                                   " 2>/dev/null")
                                      .c_str())) == 0);
    CHECK(WEXITSTATUS(std::system((run + " --qlmeta " + dir +
                                   "/host.qlm -c " + dir + "/bad.ql" +
                                   " 2>/dev/null")
                                      .c_str())) == 3);
    // Without the manifest the same red script is lenient again (undeclared
    // Point is a different failure — but no 'has no field' message).
    CHECK(WEXITSTATUS(std::system((run + " -c " + dir + "/bad.ql" +
                                   " 2>&1 | grep -q \"has no field\"")
                                      .c_str())) != 0);
    CHECK(WEXITSTATUS(std::system((run + " --qlmeta " + dir +
                                   "/missing.qlm -c " + dir + "/good.ql" +
                                   " 2>/dev/null")
                                      .c_str())) == 1);
  }

  if (failures) {
    printf("manifest_compile: %d FAILURE(S)\n", failures);
    return 1;
  }
  printf("manifest_compile: all %d checks passed\n", checks);
  return 0;
}
