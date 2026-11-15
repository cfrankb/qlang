// Module exports (SPEC §bytecodes "keyword export", "export table (lookup)",
// plan docs/plans/archive/external-modules-plan-v4.md Phase 3a).
//
// `export` is the construct that turns a script into a module: it publishes a
// global name (a `def`, a `const`, or a variable) into the program's export
// table, and `export const MODULE_NAME = "<name>"` is the module's identity.
// This suite pins the three things Phase 3a owns —
//
//   * the syntax: both spec forms (the prefix `export def f()` / `export const
//     k = 1` / `export x = 1` and the standalone `export NAME;`), global-scope
//     only, semicolon optional, forward references allowed;
//   * the table: one entry per name in source order, `kind` from what the name
//     binds, and `ref` resolved to the function index (a `def`) or the register
//     (the shared cell a variable or constant lives in) — the handle a loader
//     dereferences, so a wrong ref is a wrong module, not a wrong name;
//   * the rules the table implies: a name cannot be exported twice (in any
//     mix of forms), an exported function's name cannot be rebound (the table
//     holds its function index, so a rebind would silently detach the export),
//     and the module name resolves source > driver > "main".
//
// The import side rides in through F4 (docs/plans/archive/class-export-plan.md):
// `testClassConstruction` compiles an image, loads it, and links an app
// against it to pin construction of an exported class.
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "qlang/QLangBytecode.h"
#include "qlang/QLangBytecodeIO.h"
#include "qlang/QLangCompiler.h"
#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"
#include "qlang/QLangLinker.h"
#include "qlang/QLangVM.h"

using namespace qlang;

static int passes = 0, fails = 0;

static bool check(bool cond, const std::string &what) {
  if (cond) {
    passes++;
  } else {
    fails++;
    std::cout << "  FAIL: " << what << "\n";
  }
  return cond;
}

namespace {

// Compile `source` exactly as `qlcc` does (addExternalConsts /
// addExternalFuncs / addModules from the context), so the export table is built
// by the same path the driver uses. `moduleName` is the driver-supplied name
// ("" leaves the compiler's default alone) and `libraryMode` compiles a module
// rather than a runnable script. Returns false with the error text when the
// compiler or parser threw.
bool compile(QLangContext &ctx, const std::string &source,
             CompiledProgram &out, std::string &err,
             const std::string &moduleName = "", bool libraryMode = false) {
  try {
    std::vector<StmtPtr> program = ctx.parse(source);
    QLangCompiler compiler(std::move(program), &ctx);
    compiler.addExternalConsts({});
    std::unordered_set<std::string> nativeNames;
    for (const auto &[name, _] : ctx.nativeFuncs()) nativeNames.insert(name);
    compiler.addExternalFuncs(nativeNames);
    compiler.addModules(ctx.modules());
    if (!moduleName.empty()) compiler.setModuleName(moduleName);
    compiler.setLibraryMode(libraryMode);
    out = compiler.compile();
    err.clear();
    return true;
  } catch (const QLangError &e) {
    err = e.what();
    return false;
  }
}

// True when compiling `source` fails and the message contains `fragment`. The
// fragment is checked rather than the whole message so the test pins the rule
// that fired, not the phrasing around it.
bool rejects(const std::string &source, const std::string &fragment) {
  QLangContext ctx;
  CompiledProgram prog;
  std::string err;
  if (compile(ctx, source, prog, err)) return false;
  return err.find(fragment) != std::string::npos;
}

// The error text compiling `source` produced ("" when it compiled).
std::string errorOf(const std::string &source) {
  QLangContext ctx;
  CompiledProgram prog;
  std::string err;
  if (compile(ctx, source, prog, err)) return "";
  return err;
}

// `compile` plus the native-slot wiring a LINKED run needs (linker_test's
// pattern): the name -> negative-slot map for the writer/linker, and the
// slot functions materialized on the program.
bool compileImage(QLangContext &ctx, const std::string &source,
                  CompiledProgram &out, std::string &err,
                  std::map<std::string, int> &nativeIds,
                  const std::string &moduleName = "",
                  bool libraryMode = false) {
  try {
    std::vector<StmtPtr> program = ctx.parse(source);
    QLangCompiler compiler(std::move(program), &ctx);
    compiler.addExternalConsts({});
    std::unordered_set<std::string> nativeNames;
    for (const auto &[name, _] : ctx.nativeFuncs()) nativeNames.insert(name);
    compiler.addExternalFuncs(nativeNames);
    compiler.addModules(ctx.modules());
    if (!moduleName.empty()) compiler.setModuleName(moduleName);
    compiler.setLibraryMode(libraryMode);
    out = compiler.compile();
    nativeIds = compiler.nativeSlotIds();
    for (const auto &[name, func] : ctx.nativeFuncs()) {
      auto it = nativeIds.find(name);
      if (it == nativeIds.end()) continue;
      const int idx = -1 - it->second;
      while (out.nativeFuncs.size() <= static_cast<size_t>(idx))
        out.nativeFuncs.push_back(nullptr);
      out.nativeFuncs[static_cast<size_t>(idx)] = func;
    }
    err.clear();
    return true;
  } catch (const QLangError &e) {
    err = e.what();
    return false;
  }
}

const ExportEntry *findExport(const CompiledProgram &prog,
                              const std::string &name) {
  for (const ExportEntry &e : prog.exports)
    if (e.name == name) return &e;
  return nullptr;
}

// The export table rendered as one string per entry, for order comparisons.
std::vector<std::string> exportListing(const CompiledProgram &prog) {
  std::vector<std::string> out;
  for (const ExportEntry &e : prog.exports) {
    const char *kind =
        e.kind == ExportKind::Func
            ? "fn"
            : (e.kind == ExportKind::Const
                   ? "const"
                   : (e.kind == ExportKind::Class ? "class" : "var"));
    out.push_back(std::string(kind) + " " + e.name + " " +
                  std::to_string(e.ref) + " " + std::to_string(e.initArity));
  }
  return out;
}

// The VM prints through std::cout, so swapping its buffer captures a run
// (same trick as codec_check's qlc-exec gate).
class StdoutCapture {
public:
  StdoutCapture() : m_saved(std::cout.rdbuf(m_ss.rdbuf())) {}
  ~StdoutCapture() { std::cout.rdbuf(m_saved); }
  StdoutCapture(const StdoutCapture &) = delete;
  StdoutCapture &operator=(const StdoutCapture &) = delete;
  std::string text() const { return m_ss.str(); }

private:
  std::ostringstream m_ss;
  std::streambuf *m_saved;
};

// Run `prog` on `ctx`, returning what it printed ("" when it threw).
std::string runText(CompiledProgram &prog, QLangContext &ctx) {
  QLangVM vm(prog, ctx);
  StdoutCapture cap;
  try {
    vm.run();
  } catch (const QLangError &) {
    return "";
  } catch (const std::exception &) {
    return "";
  }
  return cap.text();
}

} // namespace

// ---------------------------------------------------------------------------
// The prefix form: every declaration `export` may prefix
// ---------------------------------------------------------------------------

static void testPrefixForms() {
  QLangContext ctx;
  CompiledProgram prog;
  std::string err;
  const std::string src = R"(
export const MODULE_NAME = "greeter"
export const AUTHOR = "Louis"
export def greet(name)
  return "hello " + name
end
export counter = 0
export local stride = 1
export twice = (x)[] { return x * 2 }
def hidden(x)
  return x
end
println(greet("Louis"))
)";
  if (!check(compile(ctx, src, prog, err), "prefix forms: compiles (" + err + ")"))
    return;

  check(prog.moduleName == "greeter",
        "prefix forms: MODULE_NAME names the module (got '" + prog.moduleName +
            "')");
  check(prog.exports.size() == 6,
        "prefix forms: six entries (got " + std::to_string(prog.exports.size()) +
            ")");
  // Source order, and each entry's kind is what the name binds. The refs are
  // checked against the program's own layout below rather than against pinned
  // register numbers, which the allocator is free to hand out differently.
  const std::vector<std::pair<std::string, ExportKind>> want = {
      {"MODULE_NAME", ExportKind::Const},
      {"AUTHOR", ExportKind::Const},
      {"greet", ExportKind::Func},
      {"counter", ExportKind::Var},
      {"stride", ExportKind::Var},
      {"twice", ExportKind::Var}};
  bool orderOk = prog.exports.size() == want.size();
  for (size_t i = 0; orderOk && i < want.size(); i++)
    orderOk = prog.exports[i].name == want[i].first &&
              prog.exports[i].kind == want[i].second;
  check(orderOk, "prefix forms: table in source order with the right kinds");

  // A `def` resolves to its function index, and that index really is the
  // function (the ref is a handle, so pinning the pair is the non-vacuous
  // check).
  const ExportEntry *greet = findExport(prog, "greet");
  if (check(greet != nullptr, "prefix forms: greet exported")) {
    check(greet->kind == ExportKind::Func, "prefix forms: greet is a Func");
    check(greet->ref >= 0 &&
              static_cast<size_t>(greet->ref) < prog.functions.size() &&
              prog.functions[greet->ref].name == "greet",
          "prefix forms: greet's ref is its function index");
  }
  // A variable or constant resolves to the register holding it — the cell an
  // importer will share (SPEC §bytecodes "technical specifications" 3).
  for (const char *name : {"AUTHOR", "counter", "stride", "twice"}) {
    const ExportEntry *e = findExport(prog, name);
    if (!check(e != nullptr, std::string("prefix forms: ") + name + " exported"))
      continue;
    auto it = prog.vars.find(name);
    check(it != prog.vars.end() && it->second == e->ref,
          std::string("prefix forms: ") + name + "'s ref is its register");
  }
  const ExportEntry *author = findExport(prog, "AUTHOR");
  check(author && author->kind == ExportKind::Const &&
            prog.constVars.count("AUTHOR") == 1,
        "prefix forms: a const export keeps its const guard");
  const ExportEntry *counter = findExport(prog, "counter");
  check(counter && counter->kind == ExportKind::Var &&
            prog.constVars.count("counter") == 0,
        "prefix forms: a variable export is a Var, not a Const");
  // An arrow closure is a Var, not a Func: it has no function-table entry, the
  // register holds the closure value.
  const ExportEntry *dbl = findExport(prog, "twice");
  check(dbl && dbl->kind == ExportKind::Var,
        "prefix forms: an arrow closure exports as a Var");
  // The unexported `def` is compiled but absent from the table.
  check(findExport(prog, "hidden") == nullptr,
        "prefix forms: an unexported def is not in the table");
  check(prog.imports.empty(), "prefix forms: the import table stays empty (3a)");

  // Exporting changes nothing about running the program.
  check(runText(prog, ctx) == "hello Louis\n",
        "prefix forms: the program still runs unchanged");
}

// ---------------------------------------------------------------------------
// The standalone form: `export NAME [;]`
// ---------------------------------------------------------------------------

static void testStandaloneForm() {
  QLangContext ctx;
  CompiledProgram prog;
  std::string err;
  const std::string src = R"(
export AUTHOR
export greet
const AUTHOR = "Louis"
def greet(name)
  return "hello " + name
end
counter = 0
export counter;
println(greet(AUTHOR) + counter)
)";
  if (!check(compile(ctx, src, prog, err),
             "standalone: compiles (" + err + ")"))
    return;
  // The three names are exported before (two of them) they are declared, and
  // each still resolves to the layout the declarations produced.
  const std::vector<std::pair<std::string, ExportKind>> want = {
      {"AUTHOR", ExportKind::Const},
      {"greet", ExportKind::Func},
      {"counter", ExportKind::Var}};
  bool orderOk = prog.exports.size() == want.size();
  for (size_t i = 0; orderOk && i < want.size(); i++)
    orderOk = prog.exports[i].name == want[i].first &&
              prog.exports[i].kind == want[i].second;
  check(orderOk, "standalone: resolves forward references in source order");
  if (const ExportEntry *g = findExport(prog, "greet"))
    check(g->ref >= 0 && static_cast<size_t>(g->ref) < prog.functions.size() &&
              prog.functions[g->ref].name == "greet",
          "standalone: the forward export resolved to the function");
  for (const char *name : {"AUTHOR", "counter"})
    if (const ExportEntry *e = findExport(prog, name))
      check(prog.vars.count(name) == 1 && prog.vars.at(name) == e->ref,
            std::string("standalone: ") + name + " resolved to its register");

  check(runText(prog, ctx) == "hello Louis0\n",
        "standalone: the program still runs unchanged");

  // A bare assignment is a declaration for export purposes, and the first
  // binding of a name wins over a later write to it.
  CompiledProgram p2;
  if (check(compile(ctx, "x = 1\nexport x\nx = 2\nprintln(x)\n", p2, err),
            "standalone: exports a name bound by assignment (" + err + ")")) {
    check(p2.exports.size() == 1 && p2.exports[0].kind == ExportKind::Var &&
              p2.exports[0].ref == p2.vars.at("x"),
          "standalone: assignment export resolves to the register");
    check(runText(p2, ctx) == "2\n", "standalone: the module may still write it");
  }
}

// ---------------------------------------------------------------------------
// Module identity: source > driver > default
// ---------------------------------------------------------------------------

static void testModuleName() {
  QLangContext ctx;
  CompiledProgram prog;
  std::string err;

  if (check(compile(ctx, "println(1)\n", prog, err),
            "module name: compiles (" + err + ")"))
    check(prog.moduleName == "main",
          "module name: defaults to 'main' (got '" + prog.moduleName + "')");

  if (check(compile(ctx, "println(1)\n", prog, err, "greeter"),
            "module name: driver-named compile"))
    check(prog.moduleName == "greeter",
          "module name: the driver's name is used (got '" + prog.moduleName +
              "')");

  if (check(compile(
              ctx, "export const MODULE_NAME = \"from_source\"\nprintln(1)\n",
              prog, err, "greeter"),
            "module name: source-named compile"))
    check(prog.moduleName == "from_source",
          "module name: the module's own MODULE_NAME wins (got '" +
              prog.moduleName + "')");

  // MODULE_NAME is an export like any other, so it is in the table too.
  check(prog.exports.size() == 1 && prog.exports[0].name == "MODULE_NAME" &&
            prog.exports[0].kind == ExportKind::Const,
        "module name: MODULE_NAME is itself exported");

  // It has to be a constant string: a loader reads the identity out of the
  // image without running the module, so anything else names nothing.
  check(rejects("export const MODULE_NAME = 42\n", "must be a string"),
        "module name: a non-string MODULE_NAME is rejected");
  check(rejects("export const MODULE_NAME = \"a\" + \"b\"\n", "non-constant"),
        "module name: a non-constant MODULE_NAME is rejected");
  check(rejects("export MODULE_NAME\n", "export of undeclared name"),
        "module name: an undeclared MODULE_NAME is rejected");
  check(rejects("local MODULE_NAME = \"x\"\nexport MODULE_NAME\n",
                "export const MODULE_NAME"),
        "module name: a variable MODULE_NAME is rejected");
}

// ---------------------------------------------------------------------------
// Library mode: a module's top level is `<init>`, and it must be named
// ---------------------------------------------------------------------------

static void testLibraryMode() {
  QLangContext ctx;
  CompiledProgram prog;
  std::string err;
  const std::string src = R"(
export const MODULE_NAME = "greeter"
export def greet(name)
  return "hello " + name
end
)";
  if (!check(compile(ctx, src, prog, err, "", true),
             "library: compiles (" + err + ")"))
    return;
  check(!prog.functions.empty() && prog.functions[0].name == "<init>",
        "library: the top level is <init>, not main");
  check(prog.moduleName == "greeter", "library: carries its name");
  check(prog.exports.size() == 2, "library: both exports recorded");

  QLangContext ctx2;
  CompiledProgram p2;
  std::string err2;
  check(!compile(ctx2, "export def f()\n  return 1\nend\n", p2, err2, "", true) &&
            err2.find("MODULE_NAME") != std::string::npos,
        "library: a module without MODULE_NAME is rejected");
  // The same source in script mode keeps today's behaviour: it is runnable and
  // named by the driver.
  QLangContext ctx3;
  CompiledProgram p3;
  std::string err3;
  if (check(compile(ctx3, "export def f()\n  return 1\nend\n", p3, err3, "app"),
            "library: the same source compiles as a script (" + err3 + ")"))
    check(p3.functions[0].name == "main" && p3.moduleName == "app",
          "library: script mode keeps main and the driver's name");
}

// ---------------------------------------------------------------------------
// Scope: `export` is a global-scope-only keyword
// ---------------------------------------------------------------------------

static void testGlobalScopeOnly() {
  const std::string why = "only valid at global scope";
  check(rejects("def f()\n  export const K = 1\nend\n", why),
        "scope: rejected inside a def body");
  check(rejects("def f()\n  export K\nend\n", why),
        "scope: rejected inside a def body (standalone)");
  check(rejects("local f = (x)[] {\n  export x\n  return x\n}\n", why),
        "scope: rejected inside an arrow closure body");
  check(rejects("class C {\n  export const K = 1\n}\n", why),
        "scope: rejected inside a class body");
  check(rejects("class C {\n  __init__(self)[] {\n    export const K = 1\n  }\n}\n",
                why),
        "scope: rejected inside a method body");
  // A control-flow block at global scope is not global scope either: the export
  // pre-pass walks the top level only, so an export inside a block would be
  // compiled and silently dropped from the table (SPEC §bytecodes "keyword
  // export" makes it an error instead).
  check(rejects("if 1 {\n  export const K = 1\n}\n", why),
        "scope: rejected inside a global-scope if body");
  check(rejects("if 1 {\n} else {\n  export K\n}\n", why),
        "scope: rejected inside an else body");
  check(rejects("for i = 1 to 3 {\n  export const K = 1\n}\n", why),
        "scope: rejected inside a for body");
  check(rejects("while 0 {\n  export K\n}\n", why),
        "scope: rejected inside a while body");
  check(rejects("switch (1) {\n  case 1 : export K\n}\n", why),
        "scope: rejected inside a switch case");
}

// ---------------------------------------------------------------------------
// The rules the table implies
// ---------------------------------------------------------------------------

static void testExportRules() {
  // Only names that bind a global can be exported.
  check(rejects("export 1\n", "export"), "rules: a literal cannot be exported");
  check(rejects("export println(\"x\")\n", "unexpected token"),
        "rules: a call cannot be exported");
  check(rejects("export if 1 {\n}\n", "export can only prefix"),
        "rules: a control statement cannot be exported");
  check(rejects("export nope\n", "export of undeclared name 'nope'"),
        "rules: an undeclared name is rejected");
  // F4 (Q42) resolved: a class name IS exportable now — kind `Class` joins
  // the kind domain (testClassExports below pins the table it produces).
  {
    QLangContext flipCtx;
    CompiledProgram flipProg;
    std::string flipErr;
    check(compile(flipCtx, "class C {\n}\nexport C\n", flipProg, flipErr),
          "rules: a class name is exportable (F4)");
  }
  check(rejects("enum E {\n  A\n}\nexport E\n", "export of undeclared name"),
        "rules: an enum name is not exportable");
  check(rejects("export def f()\n  return 1\nend\nexport def g()\n  return 2\nend\n"
                "export f\n",
                "already exported"),
        "rules: prefix then standalone is a duplicate");
  check(rejects("export const A = 1\nexport const A = 2\n", "already exported"),
        "rules: two prefix exports of one name are a duplicate");
  check(rejects("local v = 1\nexport v\nexport v\n", "already exported"),
        "rules: two standalone exports of one name are a duplicate");

  // An exported function's name holds a function index in the table, so
  // rebinding the name would leave the export pointing at the old function
  // while callers using the name got the new one (SPEC §bytecodes "keyword
  // export": exported functions are not modifiable).
  check(rejects("export def f()\n  return 1\nend\nf = 2\n",
                "cannot assign to exported function 'f'"),
        "rules: assigning to an exported function name is rejected");
  check(rejects("export def f()\n  return 1\nend\nf += 2\n",
                "exported function"),
        "rules: compound assignment to an exported function name is rejected");
  // A bare `f++` on an exported `def` is already gone: the name is a function,
  // not a variable, so the ordinary "use of undeclared variable" fires first.
  // The guard earns its keep when the name is bound again as a variable — the
  // register is writable, but writing it would detach the export.
  check(rejects("export def f()\n  return 1\nend\nf++\n",
                "use of undeclared variable 'f'"),
        "rules: incrementing an exported function name is rejected");
  check(rejects("export def f()\n  return 1\nend\nlocal f = 1\nf++\n",
                "cannot increment/decrement exported function 'f'"),
        "rules: incrementing a name rebound over an exported function is rejected");
  // The guard is about exported *functions*: a variable export stays writable
  // in its own module (SPEC §bytecodes "technical specifications" 3), and an
  // unexported function stays rebindable.
  QLangContext ctx;
  CompiledProgram prog;
  std::string err;
  if (check(compile(ctx, "export local n = 1\nn = 2\nprintln(n)\n", prog, err),
            "rules: a variable export stays writable (" + err + ")"))
    check(runText(prog, ctx) == "2\n", "rules: the write is visible");
  if (check(compile(ctx, "def f()\n  return 1\nend\nf = 2\nprintln(f)\n", prog,
                    err),
            "rules: an unexported def stays rebindable (" + err + ")"))
    check(!runText(prog, ctx).empty(), "rules: the unexported program runs");
  // A local (function-scope) name may shadow nothing and stays writable: the
  // guard is global-scope provenance, not a global name ban.
  if (check(compile(ctx,
                    "export def f()\n  return 1\nend\ndef g()\n  local f = 9\n"
                    "  return f\nend\nprintln(g())\n",
                    prog, err),
            "rules: a function-local may share an exported name (" + err + ")"))
    check(runText(prog, ctx) == "9\n", "rules: the local shadows inside the def");
}

// ---------------------------------------------------------------------------
// The table survives the .qlc round trip (v9 §4.6)
// ---------------------------------------------------------------------------

static void testQlcRoundTrip() {
  QLangContext ctx;
  CompiledProgram prog;
  std::string err;
  const std::string src = R"(
export const MODULE_NAME = "greeter"
export const AUTHOR = "Louis"
export def greet(name)
  return "hello " + name
end
export counter = 0
println(greet(AUTHOR))
)";
  if (!check(compile(ctx, src, prog, err),
             "qlc: compiles (" + err + ")"))
    return;

  std::ostringstream os(std::ios::binary);
  if (!check(writeQlc(os, prog, {}), "qlc: writeQlc accepted the program"))
    return;

  CompiledProgram back;
  std::vector<std::pair<int, std::string>> slots;
  std::string why;
  std::istringstream is(os.str(), std::ios::binary);
  if (!check(readQlc(is, back, slots, why),
             "qlc: readQlc accepted the image (" + why + ")"))
    return;

  check(back.moduleName == prog.moduleName,
        "qlc: the module name survives (got '" + back.moduleName + "')");
  check(exportListing(back) == exportListing(prog),
        "qlc: the export table survives entry for entry");
  check(back.exports.size() == 4, "qlc: four entries on the far side");

  // The refs are handles into the image, so they have to still mean the same
  // thing after a load: the function index names the function, the register is
  // in range of the loaded register count.
  for (const ExportEntry &e : back.exports) {
    if (e.kind == ExportKind::Func) {
      check(e.ref >= 0 && static_cast<size_t>(e.ref) < back.functions.size() &&
                back.functions[e.ref].name == e.name,
            "qlc: exported function ref still names its function (" + e.name +
                ")");
    } else {
      check(e.ref >= 0 && e.ref < back.registerCount,
            "qlc: exported register ref still in range (" + e.name + ")");
    }
  }
  // And the loaded image still runs.
  QLangContext ctx2;
  check(runText(back, ctx2) == "hello Louis\n",
        "qlc: the loaded module runs with identical output");
}

// Q42 (plan docs/plans/archive/class-export-plan.md P3a): `export class` is legal and
// the entry carries the class meta. Construction across the boundary is P3b;
// this round pins the table and the image only.
static void testClassExports() {
  QLangContext ctx;
  CompiledProgram prog;
  std::string err;
  const std::string positive = R"(
export class Point {
  x = 0
  y = 0
  __init__(self, x, y) [ ] {
    self.x = x
    self.y = y
  }
}
class Plain {
  tag = 1
}
export Plain
local p = Point(1, 2)
println(p.x)
)";
  if (!check(compile(ctx, positive, prog, err),
             "class: `export class` and standalone class export compile (" +
                 err + ")"))
    return;

  const ExportEntry *point = findExport(prog, "Point");
  if (check(point != nullptr && point->kind == ExportKind::Class,
            "class: `export class Point` enters the table as a Class")) {
    check(point->initArity == 3,
          "class: Point's initArity is the declared arity incl. self (got " +
              std::to_string(point->initArity) + ")");
    auto it = prog.vars.find("__proto_Point");
    check(it != prog.vars.end() && it->second == point->ref,
          "class: Point's ref is the __proto_Point cell");
  }
  const ExportEntry *plain = findExport(prog, "Plain");
  if (check(plain != nullptr && plain->kind == ExportKind::Class,
            "class: standalone `export Plain` enters the table"))
    check(plain->initArity == -1,
          "class: a class without __init__ carries initArity -1");

  // The class name stays a non-value even when exported.
  check(rejects(positive + "\nlocal q = Point\n", "Point"),
        "class: the exported class name is still not a value");

  // v11 image: the kind and the meta survive writeQlc/readQlc.
  std::ostringstream os(std::ios::binary);
  if (check(writeQlc(os, prog, {}), "class: writeQlc accepts a Class export")) {
    CompiledProgram back;
    std::vector<std::pair<int, std::string>> slots;
    std::string why;
    std::istringstream is(os.str(), std::ios::binary);
    if (check(readQlc(is, back, slots, why),
              "class: readQlc accepts the Class export (" + why + ")")) {
      const ExportEntry *p2 = findExport(back, "Point");
      check(p2 && p2->kind == ExportKind::Class && p2->initArity == 3 &&
                p2->ref == point->ref,
            "class: kind, ref and initArity survive the image");
      const ExportEntry *q2 = findExport(back, "Plain");
      check(q2 && q2->initArity == -1,
            "class: the -1 meta survives too");
    }
  }

  // Rules unchanged around the new kind.
  check(rejects("export class C { x = 1 }\nexport C\n", "already exported"),
        "class: a class cannot be exported twice");
  check(rejects("if 1 { export class C { x = 1 }\n}\n",
                "export is only valid at global scope"),
        "class: `export class` inside a block is rejected");
  check(rejects("export class MODULE_NAME { v = 1 }\n", "MODULE_NAME"),
        "class: a class cannot name the module");
}

// F4 P3b (plan docs/plans/archive/class-export-plan.md): CONSTRUCTION of an imported
// class. The library is compiled to an image and loaded (the `qlcc app.ql
// lib.qlc` path), the app is compiled against it and linked; the run pins
// both spellings constructing, the method call through a receiver variable,
// `type()`, a captured module constant inside `__init__` (the capture spec
// must name the merged global slot, not a frame register), field defaults
// shared by handle, and the zero-`__init__` rule. The rejects pin the
// compile-time half: a class import is a constructor, never a value, and its
// arity is checked against the defining image's `initArity`.
static void testClassConstruction() {
  const char *kLeaf = R"(
export const MODULE_NAME = "leafc"
export const GREET = "hi"
export class Pt {
  x = 0
  hi = ""
  shared = []
  __init__(self, ax) [GREET] {
    self.x = ax
    self.hi = GREET
  }
  ax(self)[] { return self.x }
}
export class Zero {
  flag = true
}
)";
  QLangContext ctx;
  std::string err;
  {
    CompiledProgram lib;
    std::map<std::string, int> libIds;
    if (!compileImage(ctx, kLeaf, lib, err, libIds)) {
      check(false, "construct: the library compiled (" + err + ")");
      return;
    }
    std::ostringstream os(std::ios::binary);
    if (!check(writeQlc(os, lib, libIds),
               "construct: the library image was written"))
      return;
    std::istringstream is(os.str(), std::ios::binary);
    if (!check(ctx.loadProgram(is, "memory.qlc") != nullptr,
               "construct: the library loaded"))
      return;
  }

  // Compile-and-link helper (linker_test's pattern): entry `source`, module
  // "app"; false with the error text when the compile or the link throws.
  auto build = [&](const std::string &source, CompiledProgram &merged) {
    CompiledProgram entry;
    std::map<std::string, int> entryIds;
    if (!compileImage(ctx, source, entry, err, entryIds, "app")) return false;
    try {
      QLangLinker linker(ctx.loadedModules());
      merged = linker.link(entry, entryIds);
      err.clear();
      return true;
    } catch (const QLangError &e) {
      err = e.what();
      return false;
    }
  };
  // The compile-time rejects, against the loaded library.
  auto rejectsLinked = [&](const std::string &source,
                           const std::string &fragment) {
    CompiledProgram merged;
    if (build(source, merged)) return false;
    return err.find(fragment) != std::string::npos;
  };

  check(rejectsLinked("from leafc import Pt\nlet f = Pt\n",
                      "must be constructed"),
        "construct: reading a flat class import is rejected");
  check(
      rejectsLinked("import leafc\nlet g = leafc.Pt\n", "must be constructed"),
      "construct: reading a qualified class import is rejected");
  check(rejectsLinked("from leafc import Pt\nlet p = Pt(1, 2)\n",
                      "constructor of 'Pt' expects 1 argument, got 2"),
        "construct: arity is checked against the image's initArity");
  check(rejectsLinked("from leafc import Zero\nlet z = Zero(1)\n",
                      "has no constructor"),
        "construct: a zero-`__init__` class rejects arguments");

  const std::string app = R"(
from leafc import Pt, Zero
let a = Pt(3)
println(a.x)
println(a.ax())
println(a.hi)
println(type(a))
let b = Zero()
println(b.flag)
let sa = a.shared
sa[] = 9
let c = Pt(7)
println(len(c.shared))
)";
  CompiledProgram merged;
  if (!check(build(app, merged), "construct: the app linked (" + err + ")"))
    return;
  // The class-name read `Pt(7)` in the last line is the flat call form
  // again; the shared default must show the earlier append (handle sharing).
  check(runText(merged, ctx) == "3\n3\nhi\nPt\ntrue\n1\n",
        "construct: the linked run constructs and dispatches");

  const std::string qualified = R"(
import leafc
let b = leafc.Pt(4)
println(b.ax())
println(type(b))
let z = leafc.Zero()
println(z.flag)
)";
  CompiledProgram merged2;
  if (check(build(qualified, merged2),
            "construct: the qualified app linked (" + err + ")"))
    check(runText(merged2, ctx) == "4\nPt\ntrue\n",
          "construct: the qualified spelling constructs too");
}

int main() {
  testPrefixForms();
  testStandaloneForm();
  testModuleName();
  testLibraryMode();
  testGlobalScopeOnly();
  testExportRules();
  testQlcRoundTrip();
  testClassExports();
  testClassConstruction();

  std::cout << (fails == 0 ? "ALL PASS" : "FAILURES") << ": " << passes
            << " passed, " << fails << " failed\n";
  return fails == 0 ? 0 : 1;
}
