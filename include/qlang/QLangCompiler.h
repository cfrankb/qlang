#pragma once

#include <set>
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

  // Register names injected as writable host variables (not consts). Used
  // only to give `delete` a precise error (SPEC: delete of an external
  // variable is an error).
  void addExternalVars(const std::unordered_set<std::string> &names);

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
  // A temp register is dead: return it to the pool so a later tmpRegister()
  // reuses it (keeps registerCount flat as the program grows). The old value
  // is released when the register is next written; if never rewritten, frame
  // teardown releases it.
  void freeRegister(int r);
  // Clear a dead statement-level temp (releases its reference) and return
  // it to the pool. Sub-expression temps use freeRegister() (never cleared,
  // as before).
  void releaseTemp(int r);
  // Drop freed registers in [lo, hi) — those slots are reserved for call
  // args/template, so tmpRegister() must not hand one back out.
  void dropFreeRegs(int lo, int hi);
  // Reserve dest+1 .. dest+argCount as live call-arg slots (frame covers
  // them; freed registers can't be handed out into the range).
  void reserveArgSlots(int dest, int argCount);
  // Call-arg slot quarantine: while a call's slots (dest+1 .. dest+nSlots)
  // are live, registers in that range are kept out of the free pool so
  // tmpRegister() can't alias a slot with a live temp. Ranges nest (f(g(x)))
  // and are all restored at the statement boundary (endOfStatement).
  void beginArgSlots(int dest, int nSlots);
  void endArgSlots();
  // Highest live temp, or -1. The free pool must stay strictly above this:
  // a call's arg slots (dest+1..dest+n, dest = temp) must never alias a
  // live temp (loop-header temps survive across statements).
  int maxLiveTmp() const;
  void promoteDeferred();
  void returnToPool(int r);
  int declareVar(const std::string &name, bool blockScoped = false);
  int regOf(const std::string &name) const;
  int emitConst(const QLangValue &value);

  void emit(Opcode op, int a, int b = 0, int c = 0);
  // Emit an instruction with a 32-bit immediate in the i32 slot (MOVI).
  // Separate from emit(): a 3-arg emit() overload would be ambiguous with
  // the defaulted b/c parameters.
  void emitImm(Opcode op, int a, int32_t imm);
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
  std::unordered_set<std::string> m_externalVars; // injected writable host vars
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
    // Static members (new extension beyond v1): live in the class-level
    // `__statics__` table (shared by the prototype and every instance), NOT in
    // the instance field schema. Static methods are compiled with a hidden
    // first parameter (the `__statics__` table) so they need no self.
    std::unordered_set<std::string> staticFields;
    std::unordered_set<std::string> staticConstFields;
    std::unordered_map<std::string, int> staticMethodFuncIndices;
    // Static method arity = declared user param count (NO hidden anchor).
    std::unordered_map<std::string, int> staticMethodArities;
  };
  std::unordered_map<std::string, ClassRecord> m_classes;
  // User-defined enums: name -> member values in declaration order. The enum
  // itself is stored at runtime in a hidden variable `__enum_<Name>` holding
  // an immutable table (name -> value); member access folds to the constant.
  std::unordered_map<std::string, std::vector<std::pair<std::string, QLangValue>>> m_enums;
  // Names of enums declared at top level (visible everywhere, like global
  // consts). Used to keep global enums visible when entering a function body.
  std::unordered_set<std::string> m_globalEnums;
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
  // Dense allocation for named registers (params, locals, globals): they
  // occupy the low end of the frame, temporaries stay above them so a
  // call's arg slots (dest+1..dest+argCount, dest = temp) can't clobber a
  // live named variable. Sharing m_nextReg with temps made every `local` in
  // a recompiled loop body land above all prior temps, purging the pool.
  int m_nextNamedReg = 0;
  // Allocated named registers (params, locals, globals): the dense
  // allocator's skip loop must avoid these too, not just live temps —
  // without it, declarations after a live-temp run all land on the same
  // register.
  std::set<int> m_namedRegs;
  // Named registers freed by dead block scopes (loop/if bodies): reused by
  // later dense allocations so registerCount doesn't grow with the number of
  // loop bodies in the program.
  std::vector<int> m_freeNamedRegs;
  // Pop the innermost block scope, returning its named registers to
  // m_freeNamedRegs. Replaces raw m_blockScopes.pop_back() sites.
  void popBlockScope();
  // Pool of dead temp registers, reused by tmpRegister() before allocating a
  // new one. Scoped per function body (saved/cleared/restored in
  // buildFunction and class compilation) because register indices are
  // per-frame.
  std::vector<int> m_freeRegs;
  // All currently-live temp registers. Temps allocated during a statement are
  // freed at the statement boundary (endOfStatement) so registerCount is the
  // high-water mark of simultaneously-live temps. Temps that must survive
  // across statement boundaries (loop-header temps, re-read on the back
  // edge) are protected: at each statement start every live temp is marked
  // protected, so only temps allocated *within* the statement get freed.
  std::vector<int> m_liveTmps;
  std::set<int> m_protectedTmps;
  // Per-statement entry snapshot of m_protectedTmps. endOfStatement must
  // free against the set as of ITS OWN entry, not the current one — nested
  // statements restore their own (larger) sets, and using the current set
  // would keep loop-header temps alive forever.
  std::vector<std::set<int>> m_protectStack;
  // Active call-arg slot ranges (nested) and the registers quarantined from
  // the free pool on their behalf. Restored to the pool at the statement
  // boundary, skipping registers still inside an enclosing active range.
  std::vector<std::pair<int, int>> m_slotRanges;
  std::set<int> m_quarantined;
  // Freed temps BELOW the highest live temp (e.g. loop-header temps): kept
  // out of the pool so tmpRegister() stays above every live temp — a call's
  // arg slots (dest+1..) must never alias a live temp. Promoted to the pool
  // once the high temp dies (tmpRegister).
  std::set<int> m_deferred;
  void endOfStatement();
  // Source position of the statement currently being compiled; stamped into
  // every instruction by emit() so runtime errors report line/col (SPEC #
  // error reporting).
  int m_curLine = 0;
  int m_curCol = 0;
  // True when compiling inside a function body (global keyword is only valid there).
  bool m_inFunction = false;
  // Static-member compile context: static field + method names of the class
  // currently being compiled, so method bodies can resolve a bare static name
  // through the class-level `__statics__` table. Empty outside class bodies.
  std::unordered_set<std::string> m_classStaticNames;
  // True while compiling a static method body: the hidden anchor parameter
  // (register 0) IS the `__statics__` table, so static names TABGET directly
  // from reg 0. In an instance method body reg 0 is `self`; statics are
  // reached via `self.__statics__`.
  bool m_inStaticMethod = false;
  // Register holding the current method's `__statics__` table (emits the
  // `self.__statics__` TABGET for instance methods; reg 0 for static methods).
  int staticsTableReg();
};

} // namespace qlang
