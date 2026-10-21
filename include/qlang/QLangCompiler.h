#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
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

  // Throw if `name` was declared with `const` (guards against reassignment).
  void guardNotConst(const std::string &name, const char *op) const;

  // Resolve a builtin function name to its Builtin id (throws if unknown).
  int builtinId(const std::string &name) const;
  // Fold a constant array/table element expression into a QLangValue.
  QLangValue evalConstExpr(const ExprPtr &e);
  // Infer the runtime type of an expression (Array/Table/... or Unknown).
  QLangValue::Type inferType(const ExprPtr &e) const;
  // Update the tracked type of a variable from an assigned RHS.
  void updateVarType(const std::string &name, const ExprPtr &e);
  // Compile an index/member assignment target (a[i]=v, t.name=v).
  void compileAssignTarget(const ExprPtr &target, const Token &op,
                           const ExprPtr &value);

  Bytecode m_code;
  std::vector<StmtPtr> m_program;
  std::unordered_map<std::string, int> m_regs;
  std::unordered_set<std::string> m_constVars; // names declared with `const`
  // Runtime type tracking: lets the compiler choose the right opcode for
  // `+=` (array append vs. string/number add) and array/table access.
  std::unordered_map<std::string, QLangValue::Type> m_varTypes;
  int m_nextReg = 0;
};

} // namespace qlang
