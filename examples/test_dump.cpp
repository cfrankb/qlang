#include <iostream>
#include <string>
#include "qlang/QLangBytecode.h"
#include "qlang/QLangCompiler.h"
#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"
#include "qlang/QLangParser.h"
using namespace qlang;

int main() {
  const char *src =
    "ar = [1,2,3]\n"
    "ar += [4,5]\n"
    "for i in ar {\n"
    "  printf(\"%d\", i)\n"
    "}\n"
    "println(\"\")\n"
    "printf(\"%s %d\", join(\",\", ar), len(ar))\n"
    ;
  QLangContext ctx;
  try {
    auto program = ctx.parse(src);
    CompiledProgram cp = QLangCompiler(program).compile();
    const auto &ins = cp.code.instructions();
    const auto &consts = cp.code.constants();
    std::cout << "Bytecode (" << ins.size() << " insts, " << cp.registerCount << " regs, " << consts.size() << " consts):\n";
    for (size_t i = 0; i < ins.size(); i++) {
      const Instruction &inst = ins[i];
      std::cout << "  " << i << ": " << Bytecode::name(inst.op);
      if (inst.op == Opcode::CONST)
        std::cout << " r[" << inst.a << "] = const[" << inst.b << "] (" << consts[inst.b].typeName() << ")";
      else if (inst.op == Opcode::JMP)
        std::cout << " -> " << inst.a;
      else if (inst.op == Opcode::JMPIFFALSE)
        std::cout << " r[" << inst.a << "] -> " << inst.b;
      else
        std::cout << " r[" << inst.a << "] r[" << inst.b << "] r[" << inst.c << "]";
      std::cout << "\n";
    }
    std::cout << "\nConstants:\n";
    for (size_t i = 0; i < consts.size(); i++)
      std::cout << "  [" << i << "] " << consts[i].typeName() << "\n";
    std::cout << "\nVars:\n";
    for (auto &p : cp.vars)
      std::cout << "  " << p.first << " -> r[" << p.second << "]\n";
    std::cout << "\n--- run ---\n";
    ctx.run(src);
  } catch (const QLangError &e) {
    std::cerr << e.what() << "\n";
    return 1;
  }
  return 0;
}
