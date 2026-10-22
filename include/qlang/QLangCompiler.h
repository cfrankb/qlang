#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "QLangBytecode.h"
#include "QLangParser.h"

namespace qlang {

// Compile-time context for the innermost loop being compiled. The CONTINUE /
// BREAK targets are baked into those instructions as operand `a`, so the VM
// only needs to load pc = i.a — no runtime loop stack required.
struct LoopCtx {
  int contAddr; // target for `continue` (loop test / increment)
  int breakAddr; // target for `break` (just past the loop)
};

// The output of compilation: a compiled program plus the variable-to-register
// map the VM needs to sync the scope.
struct CompiledProgram {
  Bytecode code;                 // scratch for main; functions() holds all fns
  std::vector<Function> functions; // functions[0] == the implicit "main"
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

  // Compile a function body (a `def` body or an arrow-closure body) into its
  // own instruction stream and constant pool, append it to the program, and
  // return its function index. `captures` is the explicit capture list.
  int buildFunction(const std::string &name,
                    const std::vector<std::string> &params,
                    const std::vector<std::string> &captures,
                    const std::vector<StmtPtr> &body);

  // Compile a statement inside a function body (used by buildFunction for the
  // body and by compileStmt for the main program). The current function's
  // register space, capture table, and instruction stream are the active ones.
  void compileBodyStmt(const StmtPtr &s);

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
  // Compiled user functions (index 0 is reserved for the implicit "main"
  // function and is filled in at the end of compile()). `def` bodies and
  // arrow-closure bodies are appended here as they are compiled.
  std::vector<Function> m_functions;
  // User-defined functions: name -> function index (for `def` calls).
  std::unordered_map<std::string, int> m_userFuncs;
  // Variable names that currently hold an arrow-closure value.
  std::unordered_set<std::string> m_closureVars;
  // Active closure capture table: captured variable name -> refId (index into
  // the current function's explicit capture list).
  std::unordered_map<std::string, int> m_captureRegs;
  // Runtime type tracking: lets the compiler choose the right opcode for
  // `+=` (array append vs. string/number add) and array/table access.
  std::unordered_map<std::string, QLangValue::Type> m_varTypes;
  std::vector<LoopCtx> m_loops; // enclosing-loop targets for continue/break
  std::vector<int> m_switches;  // enclosing-switch end targets for break
  std::vector<int> m_breakJmps; // instruction indices of unpatched break JMPs
  std::unordered_map<std::string, int> m_labels; // label name -> instruction index (per-scope)
  int m_nextReg = 0;
};

} // namespace qlang
