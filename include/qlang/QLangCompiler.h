#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <cstdio>
#include <utility>

#include "QLangBytecode.h"
#include "QLangContext.h"
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
  std::unordered_set<std::string> constVars; // names that are const (cannot be reassigned)
  // Native (C++) functions registered via QLangContext::setFunc(), indexed
  // by the negative opcode operand (index = -1 - opcode_b).
  std::vector<QLangContext::NativeFunc> nativeFuncs;
  int registerCount = 0;
};

// Walks a parsed AST and emits register-machine bytecode. Each script variable
// is assigned a stable register; control flow is lowered to jumps.
class QLangCompiler {
public:
  explicit QLangCompiler(std::vector<StmtPtr> program, QLangContext *scope = nullptr);

  // Register names that are const because they were injected from the host
  // (not declared with `const` in QLang source). The compiler treats them
  // the same as source-level const vars for guardNotConst purposes.
  void addExternalConsts(const std::unordered_set<std::string> &names);

  // Register names of C++ native functions. The compiler assigns them
  // negative indices so the VM can dispatch them at runtime.
  void addExternalFuncs(const std::unordered_set<std::string> &names);

  // Register module functions. Each function in each module is registered as
  // an external func with the same name (module qualification is compile-time
  // only for flat import; qualified access math.min() is a future extension).
  void addModules(const std::vector<QLangContext::Module> &modules);
  const std::unordered_map<std::string, int> &nativeFuncIds() const { return m_nativeFuncIds; }
  const std::vector<QLangContext::NativeFunc> &nativeFuncsResolved() const { return m_nativeFuncsResolved; }

  CompiledProgram compile();

  // Gated diagnostic emit (mirrors QLangVM::trace). Prints printf-style
  // output to stderr only when the owning context's verbose level is >=
  // minLevel (see the TRACE_* levels in QLangContext). A compiler built
  // without a context emits nothing. Program output (print/println) is
  // unaffected.
  template <typename... Args>
  void trace(int minLevel, const char *fmt, Args &&...args) const {
    if (!m_scope || !m_scope->isAt(minLevel)) return;
    char buf[2048];
    int n = std::snprintf(buf, sizeof(buf), fmt, std::forward<Args>(args)...);
    if (n > 0) std::fputs(buf, stderr);
  }

private:
 // Owning context, used to gate trace() output on its verbose level.
 // Null when the compiler is constructed standalone (trace() is then a no-op).
 QLangContext *m_scope = nullptr;

 int newRegister();
  int tmpRegister();
  int declareVar(const std::string &name, bool blockScoped = false);
  int regOf(const std::string &name) const;
  int emitConst(const QLangValue &value);

  void emit(Opcode op, int a, int b = 0, int c = 0);
  void compileStmt(const StmtPtr &s,
                   std::vector<std::string> *captures = nullptr);
  void compileExpr(const ExprPtr &e, int dest);
  Opcode binOp(const std::string &text) const;
  Opcode unOp(const std::string &text) const;

  // Compile a function body (a `def` body or an arrow-closure body) into its
  // own instruction stream and constant pool, append it to the program, and
  // return its function index. `captures` is the explicit capture list.
  int buildFunction(const std::string &name,
                    const std::vector<std::string> &params,
                    std::vector<std::string> captures,
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
  // Recursively check if a statement (or any of its children) contains a yield.
  static bool containsYield(const StmtPtr &s);

  Bytecode m_code;
  std::vector<StmtPtr> m_program;
  std::unordered_map<std::string, int> m_regs;
  // Block-scope name maps, innermost last. Only explicit `local`/`let`/`const`
  // land here (SPEC # block scope); bare assignments and loop vars stay in
  // m_regs so they cross block boundaries.
  std::vector<std::unordered_map<std::string, int>> m_blockScopes;
  std::unordered_set<std::string> m_constVars; // names declared with `const`
  // External C++ native functions: name -> negative opcode index.
  // Negative indices (-1, -2, ...) signal the VM to dispatch via the
  // context's native function registry.
  std::unordered_map<std::string, int> m_nativeFuncIds;
  // Resolved native function indices: index -> function (filled at compile time).
  std::vector<QLangContext::NativeFunc> m_nativeFuncsResolved;
  // Set of names that are module functions (imported from registered modules).
  std::unordered_set<std::string> m_moduleFuncNames;
  // Registered modules (populated by addModules, used by import compilation).
  std::vector<QLangContext::Module> m_moduleFuncs;
  // Compiled user functions (index 0 is reserved for the implicit "main"
  // function and is filled in at the end of compile()). `def` bodies and
  // arrow-closure bodies are appended here as they are compiled.
  std::vector<Function> m_functions;
  // User-defined functions: name -> function index (for `def` calls).
  std::unordered_map<std::string, int> m_userFuncs;
  // User-defined classes: name -> prototype-creation function index + method info.
  struct ClassRecord {
    int prototypeFunc = -1; // function index that creates and returns the prototype table
    std::vector<std::string> methodNames;
    std::unordered_map<std::string, int> methodFuncIndices;
    // Fixed arity (Phase 4): method name -> declared parameter count
    // (INCLUDING `self`). Call sites are checked against this at compile
    // time: a call must pass exactly arity-1 arguments.
    std::unordered_map<std::string, int> methodArities;
    // Closed field set (Phase 1): declared fields in declaration order, plus
    // name -> index. Emitted into the prototype's `__fields__` table and
    // copied onto each instance so GETFIELD/SETFIELD can enforce the set.
    std::vector<std::string> fieldNames;
    std::unordered_map<std::string, int> fieldIndices;
    // Const fields (Phase 2): the subset of declared fields that are `const`
    // (read-only). Emitted into the prototype's `__const_fields__` table and
    // copied onto each instance so SETFIELD can reject assignment to them.
    std::unordered_set<std::string> constFields;
    // Field type inference (Phase 5): field name -> type inferred from the
    // field's default value. Emitted into the prototype's `__field_types__`
    // table (name -> type-name string) and copied onto each instance so
    // SETFIELD can reject values that violate the inferred type. A field with
    // a non-constant (or `nil`) default has no entry -> no constraint.
    std::unordered_map<std::string, QLangValue::Type> fieldTypes;
  };
  std::unordered_map<std::string, ClassRecord> m_classes;
  // Variable names that currently hold an arrow-closure value.
  std::unordered_set<std::string> m_closureVars;
  // Active closure capture table: captured variable name -> refId (index into
  // the current function's explicit capture list).
  std::unordered_map<std::string, int> m_captureRegs;
  // Variable names declared at the top level (visible everywhere as globals).
  // Functions only see them when explicitly imported via `global`.
  std::unordered_set<std::string> m_globalVars;
  // Variables explicitly imported into function scope via `global`. These are
  // NOT in m_regs (so they compile as REFLOAD/REFSTORE) but ARE assignable.
  std::unordered_set<std::string> m_capturedVars;
  // Variable names known to hold a generator (arrow-closure containing yield).
  // Assignments/calls to these variables inside nested functions use CALLCLOS.
  std::unordered_set<std::string> m_generatorVars;
  // Function indices known to be generator functions (contain `yield`).
  // Persists across buildFunction calls so nested def calls can check without
  // accessing m_functions (which is restored after each buildFunction).
  std::unordered_set<int> m_generatorFuncIndices;
  // Maps generator funcIndex → constant pool index of the Closure constant
  // used by MAKEGEN to create a GeneratorValue.
  std::unordered_map<int, int> m_generatorConstIndices;
  // Caller's register mapping saved at function-entry for global keyword support.
  std::unordered_map<std::string, int> m_savedRegs;
  // Mapping from global var name to caller's register number (for capture spec).
  std::unordered_map<std::string, int> m_globalRegMap;
  // Runtime type tracking: lets the compiler choose the right opcode for
  // `+=` (array append vs. string/number add) and array/table access.
  std::unordered_map<std::string, QLangValue::Type> m_varTypes;
  std::vector<LoopCtx> m_loops; // enclosing-loop targets for continue/break
  std::vector<int> m_switches;  // enclosing-switch end targets for break
  std::vector<int> m_breakJmps; // instruction indices of unpatched break JMPs
  std::unordered_map<std::string, int> m_labels; // label name -> instruction index (per-scope)
  int m_nextReg = 0;
  // True when compiling inside a function body (global keyword is only valid there).
  bool m_inFunction = false;
};

} // namespace qlang
