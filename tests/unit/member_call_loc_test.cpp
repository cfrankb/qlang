// member_call_loc_test — member calls and keyword builtins must stamp the
// CALL they emit with the callee token's source location (bug report
// docs/bugs/member-call-missing-callline.md: parsePostfix built the
// `obj.method(args)` Call node without callLine/callCol, so the CALL landed
// at (0,0) — and inside an f-string, shiftExprLines turned that 0 into a
// phantom `stmtLine - 1`, garbling the disassembly listing's L<n> headers).
//
// exit code: 0 all locs stamped, 1 otherwise

#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

#include "qlang/QLangBytecode.h"
#include "qlang/QLangCompiler.h"
#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"

using namespace qlang;

static int fails = 0;

static void check(bool ok, const std::string &name) {
  std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << "\n";
  if (!ok) fails++;
}

int main() {
  // Line numbers matter: the f-string at line 5 is what previously produced
  // the phantom `4:0` CALL loc.
  const std::string source =
      "import math\n"
      "\n"
      "local a = math.min(66,1,7)\n"
      "print(a)\n"
      "println(f\"m {math.min(3,4)}\")\n";
  CompiledProgram compiled;
  try {
    QLangContext ctx;
    std::vector<StmtPtr> program = ctx.parse(source);
    QLangCompiler compiler(std::move(program), &ctx);
    compiler.addExternalConsts({});
    std::unordered_set<std::string> nativeNames;
    for (const auto &[name, _] : ctx.nativeFuncs()) nativeNames.insert(name);
    compiler.addExternalFuncs(nativeNames);
    compiler.addModules(ctx.modules());
    compiled = compiler.compile();
  } catch (const QLangError &e) {
    std::cout << "[FAIL] compile: " << e.what() << "\n";
    return 1;
  }
  const Function &fn = compiled.functions[0];
  // Collect the native CALLs (b < 0) with their stamped locations.
  std::vector<std::pair<int, int>> nativeCallLocs;
  bool hasZeroLoc = false;
  for (size_t j = 0; j < fn.instrOffsets.size(); j++) {
    if (j < fn.loc.size() && fn.loc[j].first == 0) hasZeroLoc = true;
    DecodedInstruction ins;
    decodeInstruction(fn.code.data(), fn.instrOffsets[j], fn.code.size(), ins);
    if (ins.op == Opcode::CALL && ins.b < 0 && j < fn.loc.size())
      nativeCallLocs.emplace_back(fn.loc[j].first, fn.loc[j].second);
  }
  check(!nativeCallLocs.empty(), "program contains native CALLs");
  check(!hasZeroLoc, "no instruction is stamped with loc line 0");
  // The math.min CALL on line 3 sits at column 16 (the 'min' token); the
  // f-string CALL (line 5) keeps the sub-parse's column but must be stamped
  // with the statement's own line, not line 4.
  bool line3 = false, line5 = false;
  for (const auto &[line, col] : nativeCallLocs) {
    if (line == 3 && col == 16) line3 = true;
    if (line == 5) line5 = true;
  }
  check(line3, "math.min CALL on line 3 carries the callee loc (3:16)");
  check(line5, "f-string math.min CALL carries its statement's line 5");
  std::cout << (fails == 0 ? "all member-call loc checks passed\n"
                           : "member-call loc checks FAILED\n");
  return fails == 0 ? 0 : 1;
}
