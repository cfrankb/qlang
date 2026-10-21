#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "QLangBytecode.h"
#include "QLangParser.h"

namespace qlang {

// The output of compilation: a compiled program plus the variable-to-register
// map the VM needs to sync the scope.
struct CompiledProgram {
  Bytecode code;
  std::unordered_map<std::string, int> vars; // script name -> register index
  int registerCount = 0;
};

// Walks a parsed AST and emits register-machine bytecode. Each script variable
// is assigned a stable register; control flow is lowered to jumps.
class QLangCompiler {
public:
  explicit QLangCompiler(std::vector<StmtPtr> program);

  CompiledProgram compile();

private:
  int newRegister();
  int tmpRegister();
  int declareVar(const std::string &name);
  int regOf(const std::string &name) const;
  int emitConst(const QLangValue &value);

  void emit(Opcode op, int a, int b = 0, int c = 0);
  void compileStmt(const StmtPtr &s);
  void compileExpr(const ExprPtr &e, int dest);
  Opcode binOp(const std::string &text) const;
  Opcode unOp(const std::string &text) const;

  Bytecode m_code;
  std::vector<StmtPtr> m_program;
  std::unordered_map<std::string, int> m_regs;
  int m_nextReg = 0;
};

} // namespace qlang
