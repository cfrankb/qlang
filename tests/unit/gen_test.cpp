#include <iostream>
#include <string>
#include <vector>
#include "qlang/QLangBytecode.h"
#include "qlang/QLangCompiler.h"
#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"
using namespace qlang;

static int passes = 0, fails = 0;

static void expectVal(const std::string &name, const std::string &src,
                      const std::string &var, int expect) {
  QLangContext ctx;
  try {
    ctx.run(src);
    int got = toInt64(ctx.get(var));
    if (got == expect) { std::cout << "[PASS] val:  " << name << " (" << var << "=" << got << ")\n"; passes++; }
    else { std::cout << "[FAIL] val:  " << name << " (" << var << " got " << got << " exp " << expect << ")\n"; fails++; }
  } catch (const QLangError &e) { std::cout << "[FAIL] val:  " << name << " -> " << e.what() << "\n"; fails++; }
}

// A generator's pool closure spec carries the function's capture spec.
// `buildFunction` used to read `fn.captures` AFTER moving `fn` into the function
// table, so the spec landed in the pool with no captures at all: the VM's MAKEGEN
// fallback (`bindCaptures` on the spec) became a silent no-op, and a linked
// program's synthesized `makegen` closure could not fold with it (external-modules
// plan v4 Q33). `expectCaptures` is the capture count the function itself has.
static void expectGenSpec(const std::string &name, const std::string &src,
                          const std::string &fnName, size_t expectCaptures) {
  QLangContext ctx;
  try {
    std::vector<StmtPtr> program = ctx.parse(src);
    QLangCompiler compiler(std::move(program), &ctx);
    CompiledProgram p = compiler.compile();
    int idx = -1;
    for (size_t i = 0; i < p.functions.size(); i++)
      if (p.functions[i].name == fnName) idx = static_cast<int>(i);
    if (idx < 0 || !p.functions[static_cast<size_t>(idx)].isGenerator) {
      std::cout << "[FAIL] spec: " << name << " -> no generator named " << fnName << "\n";
      fails++;
      return;
    }
    const Function &fn = p.functions[static_cast<size_t>(idx)];
    if (fn.captures.size() != expectCaptures) {
      std::cout << "[FAIL] spec: " << name << " -> function has " << fn.captures.size()
                << " captures, expected " << expectCaptures << "\n";
      fails++;
      return;
    }
    const Closure *spec = nullptr;
    // Read the spec the `makegen` actually names, not just any closure
    // constant: a top-level `def` also emits a def-time `makefunc` spec whose
    // captures are frame-relative, and that one is a different value by design.
    for (const Function &f : p.functions)
      for (size_t i = 0; i < f.instrOffsets.size(); i++) {
        DecodedInstruction k;
        decodeInstruction(f.code.data(), f.instrOffsets[i], f.code.size(), k);
        if (k.op != Opcode::MAKEGEN || k.b < 0 ||
            static_cast<size_t>(k.b) >= p.constants.size())
          continue;
        const QLangValue &v = p.constants[static_cast<size_t>(k.b)];
        if (v.type() == QLangValue::Type::Function &&
            v.asFunction().funcIndex == idx)
          spec = &v.asFunction();
      }
    if (!spec) {
      std::cout << "[FAIL] spec: " << name << " -> no makegen names the generator\n";
      fails++;
      return;
    }
    if (spec->captures.size() != fn.captures.size()) {
      std::cout << "[FAIL] spec: " << name << " -> pool spec has " << spec->captures.size()
                << " captures, function has " << fn.captures.size() << "\n";
      fails++;
      return;
    }
    for (size_t i = 0; i < spec->captures.size(); i++)
      if (spec->captures[i].reg != fn.captures[i].reg) {
        std::cout << "[FAIL] spec: " << name << " -> capture " << i << " is "
                  << spec->captures[i].reg << ", function says " << fn.captures[i].reg << "\n";
        fails++;
        return;
      }
    std::cout << "[PASS] spec: " << name << " (" << expectCaptures << " captures)\n";
    passes++;
  } catch (const QLangError &e) {
    std::cout << "[FAIL] spec: " << name << " -> " << e.what() << "\n";
    fails++;
  }
}

int main() {
  expectVal("gen basic yield",
    "def gen_range(mi, mx) { for i = mi to mx { yield i } }\n"
    "local gen = gen_range(1, 3)\n"
    "local r = gen();",
    "r", 1);
  expectVal("gen second yield",
    "def gen_range(mi, mx) { for i = mi to mx { yield i } }\n"
    "local gen = gen_range(1, 3)\n"
    "gen(); gen(); local r = gen();",
    "r", 3);
  expectVal("gen exhausted returns nil",
    "def gen_range(mi, mx) { for i = mi to mx { yield i } }\n"
    "local gen = gen_range(1, 1)\n"
    "gen(); gen(); local r = gen();",
    "r", 0);
  expectVal("gen empty yield",
    "def gen_once() { yield; yield 42 }\n"
    "local gen = gen_once()\n"
    "local r = gen();",
    "r", 0);
  expectVal("gen capture outer",
    "def make_gen(start) { for i = start to start+2 { yield i } }\n"
    "local gen = make_gen(10)\n"
    "local r = gen();",
    "r", 10);

  // The pool spec a `makegen` reads must describe the generator's captures.
  expectGenSpec("gen pool spec carries a global capture",
    "local base = 10\n"
    "def steps(n)\n"
    "  global base\n"
    "  for i = 1 to n { yield base + i }\n"
    "end\n"
    "local g = steps(2)\n",
    "steps", 1);
  expectGenSpec("gen pool spec of a capture-free generator is empty",
    "def gen_range(mi, mx) { for i = mi to mx { yield i } }\n"
    "local g = gen_range(1, 3)\n",
    "gen_range", 0);

  std::cout << "\n==== " << passes << " passed, " << fails << " failed ====\n";
  return fails ? 1 : 0;
}
