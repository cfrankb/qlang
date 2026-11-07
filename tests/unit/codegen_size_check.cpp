// codegen_size_check — emitted-instruction budget gate (SPEC §bytecodes
// "encoding guidelines": no dead stores, no redundant temporary copies;
// AGENTS.md: a measured instruction-count growth is a regression to root-cause,
// never a number to re-baseline silently).
//
// The `test-hello-*.qla` dumps of tests/scripts/hello.ql have been compared by
// hand across commits, which is how the 229 -> 243 growth was found. This suite
// makes that comparison mechanical: every budgeted script is compiled exactly
// the way `qlang_run` compiles it (no external constants, the context's natives
// and modules registered) and the instruction count of the entry function — the
// number `qlang_run --profile` prints — must stay within its budget.
//
// A budget is a ceiling, not a target: shrinking a stream is a win and the
// ceiling comes down in the same commit; raising one needs a reason recorded in
// docs/compliance/.
//
// exit code: 0 every budget met
//            1 a budget exceeded, a script unreadable, or a compile error

#include <cstddef>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "qlang/QLangCompiler.h"
#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"

#ifndef QLANG_TEST_SCRIPTS_DIR
#error "QLANG_TEST_SCRIPTS_DIR must be defined by the build (tests/scripts)"
#endif

using namespace qlang;

static int passes = 0, fails = 0;

static void check(bool ok, const std::string &name) {
  if (ok) {
    std::cout << "[PASS] " << name << "\n";
    passes++;
  } else {
    std::cout << "[FAIL] " << name << "\n";
    fails++;
  }
  std::cout.flush();
}

// One scripted program and the instruction ceiling of its entry function. The
// corpus is the perf corpus (tests/perf/perf_bench.cpp): the four scripts whose
// compile stage is already tracked, so a codegen change shows up in both gates.
// hello.ql 215 -> 211 and perf_stress.ql 522 -> 521: comparison conditions with
// a non-Int operand now fuse to a flag compare + flag jump instead of a value
// compare + temp + JMPIFFALSE (plan docs/plans/m3-flag-flow-plan.md S5).
struct Budget {
  const char *script;
  size_t entry;
};

static const Budget kBudgets[] = {
    {"hello.ql", 211},
    {"money.ql", 151},
    {"cast.ql", 163},
    {"perf_stress.ql", 521},
};

static std::string readScript(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::string();
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Compile `source` as `qlang_run` does. `entry` receives the entry function's
// instruction count, `total` the count over every function (both are printed so
// a failure says where the growth is).
static bool measure(const std::string &source, size_t &entry, size_t &total,
                    std::string &err) {
  try {
    QLangContext ctx;
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
    entry = compiled.functions[0].instrOffsets.size();
    total = 0;
    for (const auto &f : compiled.functions) total += f.instrOffsets.size();
    return true;
  } catch (const QLangError &e) {
    err = e.what();
    return false;
  }
}

int main() {
  for (const Budget &b : kBudgets) {
    const std::string script = b.script;
    const std::string source =
        readScript(std::string(QLANG_TEST_SCRIPTS_DIR) + "/" + script);
    if (source.empty()) {
      check(false, "budget: " + script + " is readable");
      continue;
    }
    size_t entry = 0, total = 0;
    std::string err;
    if (!measure(source, entry, total, err)) {
      check(false, "budget: " + script + " compiles (" + err + ")");
      continue;
    }
    std::ostringstream name;
    name << "budget: " << script << " entry function <= " << b.entry
         << " instructions (measured " << entry << ", program " << total << ")";
    check(entry <= b.entry, name.str());
  }
  std::cout << (fails == 0 ? "all codegen size checks passed\n"
                           : "codegen size checks FAILED\n");
  return fails == 0 ? 0 : 1;
}
