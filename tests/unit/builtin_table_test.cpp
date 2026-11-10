// Builtin-function name table (SPEC §bytecodes "table for build in function
// (qlang base)", plan docs/plans/archive/external-modules-plan-v4.md Phase 1).
//
// The table is the single source of truth for id <-> name: the compiler uses it
// to classify a callee and to fill `CALL`'s operand b, the disassembler uses it
// to name that operand, and Phase 3's external-module resolution will use it to
// reject a module function that shadows a builtin. This suite pins
//
//   * the id -> name mapping (hardcoded below, so a reordering of `Builtin`
//     without the matching table edit fails here rather than silently renaming
//     a builtin in the disassembly),
//   * the round trip through both lookup APIs, including the `str`/`int`
//     aliases and the -1-vs-throw split between `findBuiltin` and `builtinId`,
//   * the compiler integration: the ids the compiler actually emits for
//     builtin calls, the negative-slot path for native/module functions (which
//     is why the emit path classifies with `findBuiltin` and must not throw),
//     and the two classification sites (free-variable scan, `deepcopy`
//     lowering) that the refactor re-pointed at the library table.
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

#include "qlang/QLangBytecode.h"
#include "qlang/QLangCompiler.h"
#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"
#include "qlang/QLangValue.h"

using namespace qlang;

static int passes = 0, fails = 0;

static void check(bool cond, const std::string &what) {
  if (cond) {
    passes++;
  } else {
    fails++;
    std::cout << "  FAIL: " << what << "\n";
  }
}

// The expected table, written out independently of k_builtin_names: this is the
// oracle. It must match `Builtin`'s declaration order in qlang/QLangBytecode.h.
struct Expected {
  int id;
  const char *name;
};

static const Expected kExpected[] = {{0, "toStr"},
                                     {1, "toInt"},
                                     {2, "type"},
                                     {3, "print"},
                                     {4, "println"},
                                     {5, "printf"},
                                     {6, "len"},
                                     {7, "blob"},
                                     {8, "substr"},
                                     {9, "slice"},
                                     {10, "range"},
                                     {11, "join"},
                                     {12, "isset"},
                                     {13, "sprintf"},
                                     {14, "format"},
                                     {15, "assert"},
                                     {16, "next"},
                                     {17, "set_max_steps"},
                                     {18, "get_max_steps"},
                                     {19, "set"},
                                     {20, "release"},
                                     {21, "deepcopy"}};

static constexpr size_t kExpectedCount =
    sizeof(kExpected) / sizeof(kExpected[0]);

namespace {

// One `CALL` found in the main function of a compiled program: operand b (the
// builtin id when >= 0, the negative native slot otherwise) and the argument
// count.
struct CallSite {
  int b;
  int argc;
};

// Compile `source` exactly as `qlang_run` does (addExternalConsts /
// addExternalFuncs / addModules from the context), then decode the main
// function's instruction stream and collect every CALL. Returns false with the
// error text when compilation fails.
bool compileCalls(QLangContext &ctx, const std::string &source,
                  std::vector<CallSite> &calls, std::string &err) {
  try {
    std::vector<StmtPtr> program = ctx.parse(source);
    QLangCompiler compiler(std::move(program), &ctx);
    compiler.addExternalConsts({});
    std::unordered_set<std::string> nativeNames;
    for (const auto &[name, _] : ctx.nativeFuncs()) nativeNames.insert(name);
    compiler.addExternalFuncs(nativeNames);
    compiler.addModules(ctx.modules());
    CompiledProgram compiled = compiler.compile();
    if (compiled.functions.empty()) {
      err = "program compiled to no functions";
      return false;
    }
    const Function &main = compiled.functions[0];
    for (size_t off : main.instrOffsets) {
      DecodedInstruction ins;
      decodeInstruction(main.code.data(), off, main.code.size(), ins);
      if (ins.op == Opcode::CALL) calls.push_back({ins.b, ins.c});
    }
    return true;
  } catch (const QLangError &e) {
    err = e.what();
    return false;
  }
}

// True when `calls` contains a call to builtin `id`.
bool callsBuiltin(const std::vector<CallSite> &calls, int id) {
  for (const CallSite &c : calls)
    if (c.b == id) return true;
  return false;
}

} // namespace

// ---------------------------------------------------------------------------
// id -> name
// ---------------------------------------------------------------------------

static void testIdToName() {
  check(builtinCount() == static_cast<int>(kExpectedCount),
        "builtinCount() is " + std::to_string(kExpectedCount) + " (got " +
            std::to_string(builtinCount()) + ")");
  check(builtinCount() == static_cast<int>(Builtin::Deepcopy) + 1,
        "builtinCount() covers every Builtin id");

  std::unordered_set<std::string> seen;
  for (const Expected &e : kExpected) {
    const char *name = builtinName(e.id);
    check(name != nullptr,
          std::string("builtinName(") + std::to_string(e.id) + ") is not null");
    if (name == nullptr) continue;
    check(std::string(name) == e.name, "builtinName(" + std::to_string(e.id) +
                                           ") is \"" + e.name + "\" (got \"" +
                                           name + "\")");
    check(seen.insert(name).second,
          std::string("name \"") + name + "\" is unique in the table");
  }

  // Out of range answers nullptr, it does not read past the array.
  check(builtinName(-1) == nullptr, "builtinName(-1) is null");
  check(builtinName(builtinCount()) == nullptr,
        "builtinName(builtinCount()) is null");
  check(builtinName(1 << 20) == nullptr, "builtinName(huge) is null");
}

// ---------------------------------------------------------------------------
// name -> id (both lookup APIs, aliases, misses)
// ---------------------------------------------------------------------------

static void testNameToId() {
  for (const Expected &e : kExpected) {
    const std::string name = e.name;
    check(findBuiltin(name) == e.id,
          "findBuiltin(\"" + name + "\") is " + std::to_string(e.id) +
              " (got " + std::to_string(findBuiltin(name)) + ")");
    check(builtinId(name) == e.id,
          "builtinId(\"" + name + "\") is " + std::to_string(e.id));
  }

  // The documented short spellings of the two converters (SPEC §lexical).
  check(findBuiltin("str") == static_cast<int>(Builtin::ToStr),
        "findBuiltin(\"str\") is ToStr");
  check(builtinId("str") == static_cast<int>(Builtin::ToStr),
        "builtinId(\"str\") is ToStr");
  check(findBuiltin("int") == static_cast<int>(Builtin::ToInt),
        "findBuiltin(\"int\") is ToInt");
  check(builtinId("int") == static_cast<int>(Builtin::ToInt),
        "builtinId(\"int\") is ToInt");

  // Misses: the lookup is exact and case-sensitive, and an alias is not a name.
  check(findBuiltin("nope") == -1, "findBuiltin of an unknown name is -1");
  check(findBuiltin("") == -1, "findBuiltin of the empty name is -1");
  check(findBuiltin("ToStr") == -1, "findBuiltin is case-sensitive");
  check(findBuiltin("tostr") == -1, "findBuiltin does not fold case");
  check(findBuiltin("toStr ") == -1, "findBuiltin does not trim");
  check(findBuiltin("strin") == -1, "findBuiltin does not prefix-match");

  // `builtinId` is the emit-path API: an unknown name is a bug, so it throws.
  bool threw = false;
  std::string text;
  try {
    builtinId("nope");
  } catch (const QLangError &e) {
    threw = true;
    text = e.what();
  }
  check(threw, "builtinId of an unknown name throws QLangError");
  check(text.find("unknown builtin function") != std::string::npos &&
            text.find("nope") != std::string::npos,
        "the throw names the builtin and the bad name (got \"" + text + "\")");
}

// ---------------------------------------------------------------------------
// compiler integration: the ids the compiler emits
// ---------------------------------------------------------------------------

static void testCompilerEmitsTableIds() {
  // A builtin call carries the table id in CALL's operand b.
  std::vector<CallSite> calls;
  std::string err;
  QLangContext ctx;
  check(compileCalls(ctx, "local n = len(\"abcd\")\n", calls, err),
        "len() compiles (" + err + ")");
  check(calls.size() == 1, "len() compiles to exactly one CALL");
  check(!calls.empty() && calls[0].b == static_cast<int>(Builtin::Len),
        "len() emits CALL with the table's Len id");
  check(!calls.empty() && calls[0].argc == 1,
        "len() CALL carries one argument");

  // Several builtins at once, including the alias spellings.
  calls.clear();
  QLangContext ctx2;
  const std::string src =
      "local s = toStr(42)\n"
      "local t = str(7)\n"
      "local i = int(\"13\")\n"
      "local j = toInt(\"3\")\n"
      "local k = type(s)\n"
      "println(s)\n";
  check(compileCalls(ctx2, src, calls, err),
        "mixed builtin calls compile (" + err + ")");
  check(callsBuiltin(calls, static_cast<int>(Builtin::ToStr)),
        "toStr() and str() both emit the ToStr id");
  check(callsBuiltin(calls, static_cast<int>(Builtin::ToInt)),
        "toInt() and int() both emit the ToInt id");
  check(callsBuiltin(calls, static_cast<int>(Builtin::Type)),
        "type() emits the Type id");
  check(callsBuiltin(calls, static_cast<int>(Builtin::Println)),
        "println() emits the Println id");
  bool allInRange = true;
  for (const CallSite &c : calls)
    if (c.b < 0 || c.b >= builtinCount()) allInRange = false;
  check(allInRange, "every emitted builtin id is inside the table");

  // The aliases are not just table entries, they still run.
  QLangContext run;
  try {
    run.run("local a = str(42)\nlocal b = int(\"13\")\n");
    check(run.get("a").asString() == "42", "str(42) is \"42\"");
    check(run.get("b").asInt() == 13, "int(\"13\") is 13");
  } catch (const QLangError &e) {
    check(false, std::string("alias spellings run (") + e.what() + ")");
  }
}

// A native (C++) function is not a builtin: the emit path must classify it with
// the non-throwing lookup and take the negative-slot branch. This is the guard
// for the one place where the plan's "use the throwing builtinId" would have
// broken the compiler.
static void testNativeFunctionStillResolves() {
  QLangContext ctx;
  QLangContext::Module mathMod;
  mathMod.name = "math";
  mathMod.funcs["min"] = [](QLangContext &,
                            const std::vector<QLangValue> &args) -> QLangValue {
    int64_t best = args.empty() ? 0 : args[0].asInt();
    for (size_t i = 1; i < args.size(); i++)
      if (args[i].asInt() < best) best = args[i].asInt();
    return QLangValue::Int(static_cast<int32_t>(best));
  };
  ctx.registerModule("math", mathMod);

  std::vector<CallSite> calls;
  std::string err;
  check(compileCalls(ctx, "import math\nlocal a = math.min(3, 1, 4)\n",
                   calls, err),
        "a module function call compiles (" + err + ")");
  check(calls.size() == 1, "the module call compiles to exactly one CALL");
  check(!calls.empty() && calls[0].b < 0,
        "a native function keeps the negative slot space (b = " +
            std::to_string(calls.empty() ? 0 : calls[0].b) + ")");
  check(!calls.empty() && builtinName(calls[0].b) == nullptr,
        "a negative slot has no builtin name");

  QLangContext run;
  run.registerModule("math", mathMod);
  try {
    run.run("import math\nlocal a = math.min(3, 1, 4)\n");
    check(run.get("a").asInt() == 1, "min(3, 1, 4) is still 1");
  } catch (const QLangError &e) {
    check(false, std::string("the module call runs (") + e.what() + ")");
  }
}

// An unknown callee is a compile error with the compiler's own text, not the
// table's throw — the emit path classifies, it does not resolve.
static void testUnknownCalleeKeepsCompilerError() {
  QLangContext ctx;
  std::vector<CallSite> calls;
  std::string err;
  check(!compileCalls(ctx, "local a = nope(1)\n", calls, err),
        "an unknown callee is rejected at compile time");
  check(err.find("unknown function") != std::string::npos &&
            err.find("nope") != std::string::npos,
        "the error is \"unknown function 'nope'\" (got \"" + err + "\")");
}

// ---------------------------------------------------------------------------
// the classification sites the refactor re-pointed at the library table
// ---------------------------------------------------------------------------

// Free-variable / capture scan (QLangCompiler.cpp: a builtin name is not a free
// variable, so it must not become a capture).
static void testBuiltinsAreNotCaptured() {
  QLangContext ctx;
  try {
    ctx.run(
        "def f()\n  return len(\"abcd\")\nend\n"
        "def g()\n  return toStr(7) + \"!\"\nend\n"
        "local a = f()\nlocal b = g()\n");
    check(ctx.get("a").asInt() == 4, "len() inside a closure returns 4");
    check(ctx.get("b").asString() == "7!", "toStr() inside a closure works");
  } catch (const QLangError &e) {
    check(false, std::string("builtins inside closures compile and run (") +
                     e.what() + ")");
  }
}

// `deepcopy` direct lowering (QLangCompiler.cpp): the guard compares against
// Builtin::Deepcopy through the table, so the id must still classify.
static void testDeepcopyClassification() {
  QLangContext ctx;
  try {
    ctx.run(
        "local a = [1, [2, 3]]\nlocal b = deepcopy(a)\n"
        "local inner = b[1]\ninner[0] = 99\n");
    const QLangValue a = ctx.get("a");
    check(a.type() == QLangValue::Type::Array, "deepcopy result is an array");
    check((*a.asArray())[1].asArray() != nullptr &&
              (*(*a.asArray())[1].asArray())[0].asInt() == 2,
          "deepcopy did not share the nested array");
  } catch (const QLangError &e) {
    check(false, std::string("deepcopy lowers (") + e.what() + ")");
  }

  // Its arity guard is on the same branch, so it is checked here too.
  QLangContext bad;
  bool threw = false;
  std::string text;
  try {
    bad.run("local a = [1]\nlocal b = deepcopy(a, a)\n");
  } catch (const QLangError &e) {
    threw = true;
    text = e.what();
  }
  check(threw && text.find("deepcopy expects exactly one argument") !=
                     std::string::npos,
        "deepcopy(a, b) is still a compile error (got \"" + text + "\")");
}

int main() {
  testIdToName();
  testNameToId();
  testCompilerEmitsTableIds();
  testNativeFunctionStillResolves();
  testUnknownCalleeKeepsCompilerError();
  testBuiltinsAreNotCaptured();
  testDeepcopyClassification();

  std::cout << (fails == 0 ? "ALL PASS" : "FAILURES") << ": " << passes
            << " passed, " << fails << " failed\n";
  return fails == 0 ? 0 : 1;
}
