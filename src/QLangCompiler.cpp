#include "qlang/QLangCompiler.h"

#include <execinfo.h>

#include <algorithm>
#include <iostream>
#include <optional>
#include <stdexcept>

#include "qlang/QLangErrors.h"

namespace qlang {

// Reserved prototype/instance key holding the closed-field schema: a table
// mapping each declared field name to its index (Phase 1). The prototype
// function emits it and the constructor copies it onto each instance so the
// VM can enforce the closed field set in GETFIELD/SETFIELD.
static const char *kFieldMetaKey = "__fields__";
// Reserved instance key holding the class name, stamped by the constructor for
// closed-field-set error messages (Phase 1.3).
static const char *kClassMetaKey = "__class__";
// Reserved prototype/instance key holding the const-field set: a table mapping
// each `const` field name to 1 (Phase 2). Emitted into the prototype and copied
// onto each instance so SETFIELD can reject assignment to read-only fields.
static const char *kConstFieldMetaKey = "__const_fields__";
// Reserved prototype/instance key holding the inferred field types: a table
// mapping each type-constrained field name to its type-name string (Phase 5).
// Emitted into the prototype and copied onto each instance so SETFIELD can
// reject values that violate the type inferred from the field's default.
static const char *kFieldTypeMetaKey = "__field_types__";

// True when an expression is a compile-time array literal (used to decide
// array-append vs. array-extend for `arr += rhs`).
static bool isArrayExpr(const ExprPtr &e) {
  return e && e->kind == ExprKind::Array;
}

// True when e is a call to the builtin range(a, b) with exactly two arguments.
// On success, outStart/outEnd are set to the two argument expressions so the
// compiler can emit a counter loop or bounds check without materializing an
// array (spec §range).
static bool isRangeCall(const ExprPtr &e, ExprPtr &outStart, ExprPtr &outEnd) {
  if (!e || e->kind != ExprKind::Call || e->callee != "range" ||
      e->args.size() != 2)
    return false;
  outStart = e->args[0];
  outEnd = e->args[1];
  return true;
}

// Fold an expression to a compile-time integer when it is a numeric literal
// (Int/Float) or a unary-minus of such a literal. Returns std::nullopt for
// anything else (variable, binary op, call, ...). Used to decide at compile
// time whether a `step` value and a range's bounds are constants, which
// determines whether the loop can be emitted with a fixed comparison (the
// common, tight case) or needs a runtime direction/path selection.
static std::optional<int64_t> foldConst(const ExprPtr &e) {
  if (!e) return std::nullopt;
  if (e->kind == ExprKind::Literal) {
    const QLangValue &v = e->literal;
    if (v.type() == QLangValue::Type::Int)
      return static_cast<int64_t>(v.asInt());
    if (v.type() == QLangValue::Type::Float)
      return static_cast<int64_t>(v.asFloat());
    return std::nullopt;
  }
  if (e->kind == ExprKind::Unary && e->uop.type == TokenType::Minus) {
    auto inner = foldConst(e->operand);
    if (inner) return -*inner;
  }
  return std::nullopt;
}

// Evaluate a table key expression to a string.
// Static identifier keys are Variable expressions; dynamic keys ($expr) are
// other expression forms that must be constant-evaluable at compile time.
static std::string evalConstKey(const ExprPtr &key) {
  if (!key)
    throw QLangError(QLangErrorKind::Parse, 0, 0, "null table key");
  if (key->kind == ExprKind::Variable)
    return key->name;
  if (key->kind == ExprKind::Literal)
    return key->literal.asString();
  throw QLangError(QLangErrorKind::Parse, 0, 0,
                   "non-constant table key");
}

QLangCompiler::QLangCompiler(std::vector<StmtPtr> program, QLangContext *scope)
    : m_program(std::move(program)), m_scope(scope) {}

void QLangCompiler::addExternalConsts(const std::unordered_set<std::string> &names) {
  m_constVars.insert(names.begin(), names.end());
}

void QLangCompiler::addExternalFuncs(const std::unordered_set<std::string> &names) {
  int idx = -1;
  for (const auto &name : names) {
    m_nativeFuncIds[name] = idx--;
  }
}

void QLangCompiler::addModules(const std::vector<QLangContext::Module> &modules) {
  std::unordered_set<std::string> allNames;
  for (const auto &mod : modules) {
    for (const auto &[name, _] : mod.funcs) {
      allNames.insert(name);
      m_moduleFuncNames.insert(name);
    }
  }
  addExternalFuncs(allNames);
  m_moduleFuncs = modules;
}

int QLangCompiler::newRegister() { return m_nextReg++; }
int QLangCompiler::tmpRegister() { return newRegister(); }

int QLangCompiler::declareVar(const std::string &name, bool blockScoped) {
  if (blockScoped && !m_blockScopes.empty()) {
    auto &top = m_blockScopes.back();
    auto it = top.find(name);
    if (it != top.end())
      return it->second;
    int r = newRegister();
    top[name] = r;
    return r;
  }
  auto it = m_regs.find(name);
  if (it != m_regs.end())
    return it->second;
  int r = newRegister();
  m_regs[name] = r;
  return r;
}

int QLangCompiler::regOf(const std::string &name) const {
  for (auto it = m_blockScopes.rbegin(); it != m_blockScopes.rend(); ++it) {
    auto f = it->find(name);
    if (f != it->end())
      return f->second;
  }
  auto it = m_regs.find(name);
  return it == m_regs.end() ? -1 : it->second;
}

int QLangCompiler::emitConst(const QLangValue &value) {
  int idx = m_code.addConstant(value);
  std::string valStr;
  if (value.type() == QLangValue::Type::Int)
    valStr = std::to_string(value.asInt());
  else if (value.type() == QLangValue::Type::String)
    valStr = value.asString();
  else
    valStr = std::string("(type=") + std::to_string((int)value.type()) + ")";
  trace(TRACE_FULL, "[DBG-CONST] Adding constant type=%d value=%s -> index=%d (pool size=%zu)\n",
        (int)value.type(), valStr.c_str(), idx, m_code.constants().size());
  return idx;
}

void QLangCompiler::emit(Opcode op, int a, int b, int c) {
  m_code.add(op, a, b, c);
}

CompiledProgram QLangCompiler::compile() {
  // Compile the top-level program into the main (scratch) function context.
  for (auto &s : m_program)
    compileStmt(s, nullptr);
  // Implicit `return nil` at the end of the main program.
  emit(Opcode::RETURN, 0);

  // Wrap the main scratch stream into functions[0].
  Bytecode mainScratch = std::move(m_code);
  Function mainFn;
  mainFn.name = "main";
  mainFn.paramCount = 0;
  mainFn.registerCount = m_nextReg;
  mainFn.instructions = std::move(mainScratch.instructions());
  mainFn.constants = std::move(mainScratch.constants());

  CompiledProgram out;
  out.functions.push_back(std::move(mainFn));
  for (auto &fn : m_functions)
    out.functions.push_back(std::move(fn));


  Bytecode outCode;
  outCode.setInstructions(out.functions[0].instructions);
  outCode.setConstants(out.functions[0].constants);
  // Copy (not move) so out.functions retains its instruction streams for the VM.
  for (auto &fn : out.functions)
    outCode.addFunction(fn.name, fn.paramCount, fn.registerCount,
                        fn.instructions, fn.constants);
  out.code = std::move(outCode);
  out.vars = std::move(m_regs);
  out.constVars = std::move(m_constVars);
  out.registerCount = m_nextReg;
  return out;
}

// Compile a function body (def body or arrow-closure body) into its own
// instruction stream and constant pool, append it as a new function, and return
// its function index. The enclosing function's compilation state is saved and
// restored so nested definitions work.
// Collect all variable names referenced in an expression.
static void collectLocalVars(const StmtPtr &s, std::unordered_set<std::string> &locals) {
  if (!s) return;
  if (s->kind == StmtKind::Declaration || s->kind == StmtKind::Const) {
    locals.insert(s->name);
  } else if (s->kind == StmtKind::For) {
    locals.insert(s->loopVar);
    for (auto &b : s->body) collectLocalVars(b, locals);
  } else if (s->kind == StmtKind::ForIn) {
    locals.insert(s->loopVar);
    for (auto &b : s->body) collectLocalVars(b, locals);
  } else if (s->kind == StmtKind::If) {
    for (auto &b : s->ifBody) collectLocalVars(b, locals);
    for (auto &b : s->elseBody) collectLocalVars(b, locals);
  } else if (s->kind == StmtKind::While) {
    for (auto &b : s->whileBody) collectLocalVars(b, locals);
  } else if (s->kind == StmtKind::Def) {
    for (auto &b : s->body) collectLocalVars(b, locals);
  }
}

static void collectStmtIds(const StmtPtr &s, std::unordered_set<std::string> &ids,
                           const std::unordered_set<std::string> *skip);
static void collectIds(const ExprPtr &e, std::unordered_set<std::string> &ids,
                       const std::unordered_set<std::string> *skip = nullptr) {
  if (!e) return;
  if (e->kind == ExprKind::Variable) {
    if (!skip || !skip->count(e->name))
      ids.insert(e->name);
    return;
  }
  if (e->kind == ExprKind::Binary) {
    collectIds(e->left, ids, skip);
    collectIds(e->right, ids, skip);
  } else if (e->kind == ExprKind::Unary) {
    collectIds(e->operand, ids, skip);
  } else if (e->kind == ExprKind::Call) {
    for (auto &a : e->args) collectIds(a, ids, skip);
  } else if (e->kind == ExprKind::Index) {
    collectIds(e->indexBase, ids, skip);
    if (e->indexExpr) collectIds(e->indexExpr, ids, skip);
  } else if (e->kind == ExprKind::Array) {
    for (auto &el : e->elements) collectIds(el, ids, skip);
  } else if (e->kind == ExprKind::Table) {
    for (auto &item : e->items) {
      collectIds(item.first, ids, skip);
      collectIds(item.second, ids, skip);
    }
  } else if (e->kind == ExprKind::Closure) {
    for (auto &st : e->body) collectStmtIds(st, ids, skip);
  }
}

static void collectStmtIds(const StmtPtr &s, std::unordered_set<std::string> &ids,
                           const std::unordered_set<std::string> *skip = nullptr) {
  if (!s) return;
  if (s->kind == StmtKind::Declaration || s->kind == StmtKind::Const) {
    if (s->value) collectIds(s->value, ids, skip);
  } else if (s->kind == StmtKind::Assignment) {
    if (s->assignValue) collectIds(s->assignValue, ids, skip);
    if (s->target) collectIds(s->target, ids, skip);
  } else if (s->kind == StmtKind::If) {
    collectIds(s->ifCond, ids, skip);
    for (auto &b : s->ifBody) collectStmtIds(b, ids, skip);
    for (auto &b : s->elseBody) collectStmtIds(b, ids, skip);
  } else if (s->kind == StmtKind::For) {
    if (s->forStart) collectIds(s->forStart, ids, skip);
    if (s->forEnd) collectIds(s->forEnd, ids, skip);
    for (auto &b : s->body) collectStmtIds(b, ids, skip);
  } else if (s->kind == StmtKind::ForIn) {
    if (s->forEnd) collectIds(s->forEnd, ids, skip);
    for (auto &b : s->body) collectStmtIds(b, ids, skip);
  } else if (s->kind == StmtKind::While) {
    collectIds(s->cond, ids, skip);
    for (auto &b : s->whileBody) collectStmtIds(b, ids, skip);
  } else if (s->kind == StmtKind::Return) {
    if (s->returnValue) collectIds(s->returnValue, ids, skip);
  } else if (s->kind == StmtKind::Yield) {
    if (s->yieldValue) collectIds(s->yieldValue, ids, skip);
  } else if (s->kind == StmtKind::ExprStmt) {
    if (s->value) collectIds(s->value, ids, skip);
  } else if (s->kind == StmtKind::Def) {
    // Skip params of this nested def (and recursively deeper nested defs).
    std::unordered_set<std::string> nestedParams(s->params.begin(), s->params.end());
    for (auto &b : s->body) collectStmtIds(b, ids, &nestedParams);
  }
}

int QLangCompiler::buildFunction(const std::string &name,
                                 const std::vector<std::string> &params,
                                 std::vector<std::string> captures,
                                 const std::vector<StmtPtr> &body) {
  // Auto-detect free variables: variables referenced in the body that are
  // not parameters and not yet declared in this function's scope.
  std::unordered_set<std::string> allIds;
  for (auto &s : body)
    collectStmtIds(s, allIds);
  // Also collect all locally-declared variables (params + local decls + loop vars)
  // so we don't mistakenly capture them as free variables.
  std::unordered_set<std::string> paramSet(params.begin(), params.end());
  std::unordered_set<std::string> localVars = paramSet;
  for (auto &s : body)
    collectLocalVars(s, localVars);

  // Save the enclosing function's compilation state.
  Bytecode savedCode = m_code;
  auto savedRegs = m_regs;
  // Save caller's register mapping for global keyword use.
  m_savedRegs = m_regs;
  auto savedConstVars = m_constVars;
  auto savedVarTypes = m_varTypes;
  auto savedUserFuncs = m_userFuncs;
  auto savedClosureVars = m_closureVars;
  auto savedCaptureRegs = m_captureRegs;
  auto savedGlobalVars = m_globalVars;
  auto savedCapturedVars = m_capturedVars;
  auto savedGeneratorVars = m_generatorVars;
  int savedNextReg = m_nextReg;
  bool savedInFunction = m_inFunction;

  // Fresh function context: params occupy registers 0..paramCount-1 and must
  // reserve those registers so temporaries never collide with a parameter.
  trace(TRACE_FULL, "[DBG-BF] Resetting m_code (buildFunction for '%s') this=%p\n", name.c_str(), (void*)this);
  m_code = Bytecode();
  m_regs.clear();
  // Keep m_constVars and m_userFuncs populated so global consts and
  // user-defined functions remain visible inside function bodies.
  // m_varTypes is cleared to avoid stale type info from the outer scope.
  m_varTypes.clear();
  m_closureVars.clear();
  m_captureRegs.clear();
  m_generatorVars.clear();
  m_labels.clear();
  m_nextReg = 0;
  m_inFunction = true;
  for (size_t i = 0; i < params.size(); i++) {
    int r = static_cast<int>(i);
    m_regs[params[i]] = r;
    if (r >= m_nextReg)
      m_nextReg = r + 1;
  }

  // Now detect free variables in the fresh context.
  std::vector<std::string> updatedCaptures = captures;
  for (auto &id : allIds) {
    if (paramSet.count(id)) continue;
    if (localVars.count(id)) continue; // locally declared (loop var, local, etc.)
    if (m_regs.count(id)) continue; // already declared in this function
    if (m_userFuncs.count(id)) continue; // user function name
    if (builtinId(id) >= 0) continue; // builtin
    // Global consts are visible everywhere — capture them like free variables.
    if (m_constVars.count(id)) {
      if (std::find(updatedCaptures.begin(), updatedCaptures.end(), id) == updatedCaptures.end())
        updatedCaptures.push_back(id);
      continue;
    }
    // Skip top-level globals: they are invisible in function scope unless
    // explicitly imported via `global`.
    if (m_globalVars.count(id)) continue;
    // This is a free variable from an enclosing function — capture it.
    if (std::find(updatedCaptures.begin(), updatedCaptures.end(), id) == updatedCaptures.end())
      updatedCaptures.push_back(id);
  }

  // Register the explicit capture list (name -> enclosing-frame register).
  for (size_t i = 0; i < updatedCaptures.size(); i++) {
    const std::string &var = updatedCaptures[i];
    // Use the enclosing frame's register mapping if available; otherwise
    // fall back to the capture index (for nested-function captures).
    auto it = savedRegs.find(var);
    m_captureRegs[var] = (it != savedRegs.end()) ? it->second : static_cast<int>(i);
  }

  // Also populate m_regs for captured globals so assignments work correctly.
  for (auto &name : updatedCaptures) {
    if (m_capturedVars.count(name) && regOf(name) < 0)
      declareVar(name);
  }

  // Compile the body.
  for (auto &s : body)
    compileStmt(s, &updatedCaptures);

  // Sync captures (modified by global statements) back to updatedCaptures.
  // This ensures fn.captures is populated correctly.

  // Implicit `return nil`. The RETURN opcode returns reg 0, but a prior
  // statement may have left a value there; force reg 0 to nil so a function
  // with no return statement returns nil (spec: return value defaults to nil).
  emit(Opcode::CONST, 0, emitConst(QLangValue())); // reg 0 = nil
  emit(Opcode::RETURN, 0);

  Function fn;
  fn.name = name;
  fn.paramCount = static_cast<int>(params.size());
  fn.registerCount = m_nextReg;
  // Populate the capture spec so the VM can bind enclosing-frame registers
  // when this function is called.
  fn.captures.resize(updatedCaptures.size());
  for (size_t i = 0; i < updatedCaptures.size(); i++) {
    const std::string &var = updatedCaptures[i];
    // Use the caller's register number if available (for globals), otherwise 0.
    int callerReg = m_globalRegMap.count(var) ? m_globalRegMap[var] : 0;
    fn.captures[i].reg = callerReg;
    fn.captures[i].frame = nullptr; // bound at call time by the VM
  }

  int funcIndex = static_cast<int>(m_functions.size()) + 1; // +1 for prepended mainFn
  Bytecode fnCode = std::move(m_code);
  fn.instructions = std::move(fnCode.instructions());
  fn.constants = std::move(fnCode.constants());
  // Detect generator: any `yield` statement (at any nesting depth) makes this
  // a generator function.
  for (auto &s : body) {
    if (s && containsYield(s)) {
      fn.isGenerator = true;
      break;
    }
  }
  if (fn.isGenerator) {
    m_generatorFuncIndices.insert(funcIndex);
  }
  m_functions.push_back(std::move(fn));

  // Restore the enclosing function's compilation state.
  m_code = std::move(savedCode);
  m_regs = std::move(savedRegs);
  m_constVars = std::move(savedConstVars);
  m_varTypes = std::move(savedVarTypes);
  m_userFuncs = std::move(savedUserFuncs);
  m_closureVars = std::move(savedClosureVars);
  m_captureRegs = std::move(savedCaptureRegs);
  m_savedRegs.clear();
  m_globalRegMap.clear();
  m_globalVars = std::move(savedGlobalVars);
  m_capturedVars = std::move(savedCapturedVars);
  m_generatorVars = std::move(savedGeneratorVars);
  m_nextReg = savedNextReg;
  m_inFunction = savedInFunction;

  // Emit generator closure constant into the MAIN bytecode (not the temporary
  // function bytecode that was just moved into fn). MAKEGEN in compileExpr
  // references constants in the current m_code.
  if (fn.isGenerator) {
    auto closure = std::make_shared<Closure>();
    closure->funcIndex = funcIndex;
    closure->captures = fn.captures;
    int genConstIdx = emitConst(QLangValue::Function(closure));
    m_generatorConstIndices[funcIndex] = genConstIdx;
  }

  return funcIndex;
}

// Register holding the current method's `__statics__` table. In a static
// method the hidden anchor parameter (register 0) IS the table. In an
// instance method register 0 is `self`, so emit a TABGET of `self.__statics__`.
int QLangCompiler::staticsTableReg() {
  if (m_inStaticMethod)
    return 0;
  int r = tmpRegister();
  int keyR = tmpRegister();
  emit(Opcode::CONST, keyR, emitConst(QLangValue::String("__statics__")));
  emit(Opcode::TABGET, r, 0, keyR);
  return r;
}

void QLangCompiler::compileStmt(const StmtPtr &s,
                                std::vector<std::string> *captures) {
  if (!s)
    return;
  switch (s->kind) {
  case StmtKind::Declaration: {
    int r = declareVar(s->name, true);
    int t = tmpRegister();
    if (s->value)
      compileExpr(s->value, t);
    else
      emit(Opcode::CONST, t, emitConst(QLangValue())); // `local z` -> nil
    emit(Opcode::MOV, r, t);
    // Clear the RHS temp so it doesn't hold an extra reference past the
    // declaration (matters for tables with finalizers: a lingering temp
    // reference keeps the instance alive until frame end).
    emit(Opcode::CONST, t, emitConst(QLangValue()));
    updateVarType(s->name, s->value);
    // Track generator variables: if RHS calls a generator function, mark LHS.
    if (s->value && s->value->kind == ExprKind::Call) {
      auto *e = s->value.get();
      trace(TRACE_FULL, "[DBG-GENVAR] Tracking gen var '%s' from call to '%s': userFuncs=%d genVars=%d\n",
            s->name.c_str(), e->callee.c_str(),
            (int)m_userFuncs.count(e->callee), (int)m_generatorVars.count(e->callee));
      if (m_userFuncs.count(e->callee) && m_generatorVars.count(e->callee))
        m_generatorVars.insert(s->name);
      trace(TRACE_FULL, "[DBG-GENVAR] m_generatorVars after insert: {");
      for (auto &g : m_generatorVars) trace(TRACE_FULL, " %s", g.c_str());
      trace(TRACE_FULL, " }\n");
    }
    // Enforce a typed declaration (e.g. local b:byte = 32).
    if (s->declaredType)
      emit(Opcode::CONV, r, r, static_cast<int>(*s->declaredType));
    // Track top-level variable declarations as globals.
    if (!m_inFunction)
      m_globalVars.insert(s->name);
    break;
  }
  case StmtKind::Const: {
    int r = declareVar(s->name, true);
    m_constVars.insert(s->name);
    int t = tmpRegister();
    if (s->value)
      compileExpr(s->value, t);
    else
      emit(Opcode::CONST, t, emitConst(QLangValue()));
    emit(Opcode::MOV, r, t);
    // Clear the RHS temp (extra reference would keep finalized tables alive
    // until frame end).
    emit(Opcode::CONST, t, emitConst(QLangValue()));
    updateVarType(s->name, s->value);
    // Enforce a typed declaration (e.g. local b:byte = 32).
    if (s->declaredType)
      emit(Opcode::CONV, r, r, static_cast<int>(*s->declaredType));
    // Global consts are visible everywhere.
    if (!m_inFunction)
      m_globalVars.insert(s->name);
    break;
  }
  case StmtKind::Assignment: {
    // Bare static member name in a method body of a class with statics:
    // write through the shared `__statics__` table.
    if (!s->target && !m_classStaticNames.empty() &&
        m_classStaticNames.count(s->name)) {
      int staticsR = staticsTableReg();
      int keyR = tmpRegister();
      emit(Opcode::CONST, keyR, emitConst(QLangValue::String(s->name)));
      int t = tmpRegister();
      compileExpr(s->assignValue, t);
      const std::string &op = s->assignOp.text;
      if (op == "=") {
        emit(Opcode::TABPUT, staticsR, keyR, t);
      } else {
        int cur = tmpRegister();
        emit(Opcode::TABGET, cur, staticsR, keyR);
        Opcode opCode = (op == "++") ? Opcode::INC
                                     : (op == "--") ? Opcode::DEC
                                                    : binOp(op.substr(0, op.size() - 1));
        emit(opCode, cur, cur, t);
        emit(Opcode::TABPUT, staticsR, keyR, cur);
      }
      break;
    }
    // Direct assignment to a captured variable (write-back / reference).
    auto cit = m_captureRegs.find(s->name);
    if (cit != m_captureRegs.end() && !s->target) {
      const std::string &op = s->assignOp.text;
      if (op == "=") {
        int t = tmpRegister();
        compileExpr(s->assignValue, t);
        emit(Opcode::REFSTORE, t, cit->second, 0);
      } else {
        int cur = tmpRegister();
        emit(Opcode::REFLOAD, cur, cit->second, 0);
        int t = tmpRegister();
        compileExpr(s->assignValue, t);
        Opcode opCode = (op == "++") ? Opcode::INC : Opcode::DEC;
        emit(opCode, cur, cur, t);
        emit(Opcode::REFSTORE, cur, cit->second, 0);
      }
      break;
    }
    // Index/member assignment target (e.g. a[i] = v, t.name = v).
    if (s->target) {
      compileAssignTarget(s->target, s->assignOp, s->assignValue);
      break;
    }
    int r = regOf(s->name);
    if (r < 0) {
      // Bare assignment auto-declares an implicit local, unless it's a captured
      // global (which should have been declared via `global` first).
      if (!m_capturedVars.count(s->name))
        r = declareVar(s->name); // bare assignment auto-declares an implicit local
    }
    // Track top-level bare assignments as globals.
    if (!m_inFunction)
      m_globalVars.insert(s->name);
    guardNotConst(s->name, "assignment");
    const std::string &op = s->assignOp.text;
    // Array append: `name += value` where name is an array.
    if (op == "+=" && m_varTypes[s->name] == QLangValue::Type::Array) {
      int t = tmpRegister();
      compileExpr(s->assignValue, t);
      emit(isArrayExpr(s->assignValue) ? Opcode::ARRAPPV : Opcode::ARRAPP,
           r, t, 0);
      break;
    }
    if (op == "=") {
      int t = tmpRegister();
      compileExpr(s->assignValue, t);
      emit(Opcode::MOV, r, t);
      // Clear the RHS temp (extra reference would keep finalized tables
      // alive until frame end).
      emit(Opcode::CONST, t, emitConst(QLangValue()));
      updateVarType(s->name, s->assignValue);
    } else {
      int t = tmpRegister();
      compileExpr(s->assignValue, t);
      Opcode opCode = (op == "++") ? Opcode::INC
                                  : (op == "--") ? Opcode::DEC
                                                 : binOp(op.substr(0, op.size() - 1));
      emit(opCode, r, r, t);
    }
    break;
  }
  case StmtKind::Delete: {
    int r = regOf(s->delName);
    if (r < 0)
      throw QLangError(QLangErrorKind::Parse, 0, 0,
                       "delete of undeclared variable '" + s->delName + "'");
    emit(Opcode::DELETE, r);
    break;
  }
  case StmtKind::For: {
    int x = declareVar(s->loopVar);
    m_blockScopes.emplace_back(); // body is a block scope (SPEC # block scope)
    int startReg = tmpRegister();
    compileExpr(s->forStart, startReg);
    emit(Opcode::MOV, x, startReg);
    int endReg = tmpRegister();
    compileExpr(s->forEnd, endReg);

    bool hasStep = s->stepExpr != nullptr;
    auto stepVal = hasStep ? foldConst(s->stepExpr) : std::nullopt;
    auto startFold = foldConst(s->forStart);
    auto endFold = foldConst(s->forEnd);
    bool boundsFoldable = startFold && endFold;
    int64_t correctDir = boundsFoldable
                             ? ((endFold.value() > startFold.value()) ? 1 : -1)
                             : 0;

    if (hasStep && stepVal && boundsFoldable) {
      // Constant step + constant bounds: fully resolved at compile time.
      int64_t sv = *stepVal;
      if (sv == 0) {
        // step 0 -> infinite loop, x frozen at start.
        m_loops.push_back({0, 0});
        int bodyStart = static_cast<int>(m_code.instructions().size());
        for (auto &body : s->body)
          compileStmt(body);
        int bodyEnd = static_cast<int>(m_code.instructions().size());
        int incLabel = bodyEnd;
        emit(Opcode::JMP, bodyStart);
        int exitLabel = static_cast<int>(m_code.size());
        for (int j = bodyStart; j < bodyEnd; j++) {
          auto &ci = m_code.instructions()[j];
          if (ci.op == Opcode::CONTINUE) ci.a = incLabel;
          else if (ci.op == Opcode::BREAK) ci.a = exitLabel;
        }
        m_loops.pop_back();
      } else if ((sv > 0) != (correctDir > 0)) {
        // User step points the wrong way: run the body exactly once.
        m_loops.push_back({0, 0});
        int bodyStart = static_cast<int>(m_code.instructions().size());
        for (auto &body : s->body)
          compileStmt(body);
        int bodyEnd = static_cast<int>(m_code.instructions().size());
        int exitLabel = static_cast<int>(m_code.size());
        for (int j = bodyStart; j < bodyEnd; j++) {
          auto &ci = m_code.instructions()[j];
          if (ci.op == Opcode::CONTINUE) ci.a = exitLabel;
          else if (ci.op == Opcode::BREAK) ci.a = exitLabel;
        }
        m_loops.pop_back();
      } else {
        // Guard loop in the natural direction with the user's step.
        m_loops.push_back({0, 0});
        int stepReg = tmpRegister();
        compileExpr(s->stepExpr, stepReg);
        int guardLabel = static_cast<int>(m_code.size());
        int cmpReg = tmpRegister();
        // QLang ranges are always inclusive: forward LE (x <= end),
        // backward GE (x >= end).
        Opcode fwdCmp = (correctDir > 0) ? Opcode::LE : Opcode::GE;
        emit(fwdCmp, cmpReg, x, endReg);
        emit(Opcode::JMPIFFALSE, cmpReg, 0);
        int jumpFalseIdx = static_cast<int>(m_code.size()) - 1;
        int bodyStart = static_cast<int>(m_code.instructions().size());
        for (auto &body : s->body)
          compileStmt(body);
        int bodyEnd = static_cast<int>(m_code.instructions().size());
        int incLabel = bodyEnd;
        emit(Opcode::ADD, x, x, stepReg);
        emit(Opcode::JMP, guardLabel);
        int exitLabel = static_cast<int>(m_code.size());
        for (int j = bodyStart; j < bodyEnd; j++) {
          auto &ci = m_code.instructions()[j];
          if (ci.op == Opcode::CONTINUE) ci.a = incLabel;
          else if (ci.op == Opcode::BREAK) ci.a = exitLabel;
        }
        m_code.instructions()[jumpFalseIdx].b = exitLabel;
        m_loops.pop_back();
      }
    } else {
      // Runtime path: variable step and/or variable bounds. The loop's natural
      // direction is sign(end - start), computed at runtime. A zero step is
      // normalized to that direction's unit step (so a `step 0` loop advances,
      // matching the spec's for-step semantics). The user step's sign then
      // selects one of two bodies:
      //   1. sign(step) == sign(dir)  -> bounded guard loop
      //   2. otherwise (wrong direction) -> run the body exactly once
      m_loops.push_back({0, 0});

      int stepReg = tmpRegister();
      if (hasStep) {
        compileExpr(s->stepExpr, stepReg);
      } else {
        emit(Opcode::CONST, stepReg, emitConst(QLangValue::Int(1)));
      }
      int zeroReg = tmpRegister();
      emit(Opcode::CONST, zeroReg, emitConst(QLangValue::Int(0)));

      // direction = (end > start) ? 1 : -1
      int subReg = tmpRegister();
      emit(Opcode::SUB, subReg, endReg, startReg);
      int posReg = tmpRegister();
      emit(Opcode::GT, posReg, subReg, zeroReg);
      int dirReg = tmpRegister();
      emit(Opcode::CONST, dirReg, emitConst(QLangValue::Int(-1)));
      int dPosLabel = static_cast<int>(m_code.size());
      emit(Opcode::JMPIFTRUE, posReg, 0);
      int dDoneLabel = static_cast<int>(m_code.size());
      emit(Opcode::JMP, 0);
      m_code.instructions()[dPosLabel].b = dPosLabel + 2;
      emit(Opcode::CONST, dirReg, emitConst(QLangValue::Int(1)));
      m_code.instructions()[dDoneLabel].a = dPosLabel + 3;

      // No explicit step: advance in the loop's natural direction (unit step).
      // Without this a backward no-step loop would get step +1 vs dir -1 and
      // take the run-once path.
      if (!hasStep)
        emit(Opcode::MOV, stepReg, dirReg);

      // step == 0 -> natural-direction unit step (so a `step 0` loop advances).
      int zeroCmp = tmpRegister();
      emit(Opcode::EQ, zeroCmp, stepReg, zeroReg);
      int zeroStepLabel = static_cast<int>(m_code.size());
      emit(Opcode::JMPIFFALSE, zeroCmp, 0);      // step != 0 -> skip normalization
      emit(Opcode::MOV, stepReg, dirReg);        // step == 0 -> step = dir (+1 / -1)
      m_code.instructions()[zeroStepLabel].b = zeroStepLabel + 2;

      // runtime conditions
      int posStep = tmpRegister();
      emit(Opcode::GT, posStep, stepReg, zeroReg);
      int posDir = tmpRegister();
      emit(Opcode::GT, posDir, dirReg, zeroReg);
      int sameSign = tmpRegister();
      emit(Opcode::EQ, sameSign, posStep, posDir);

      // Decision: sameSign -> guard loop; else fall through to run-once.
      int guardLabel = static_cast<int>(m_code.size());
      emit(Opcode::JMPIFTRUE, sameSign, 0);

      // Run-once body (wrong direction): execute exactly once, then exit.
      emit(Opcode::MOV, x, startReg);
      int runOnceBodyStart = static_cast<int>(m_code.instructions().size());
      for (auto &body : s->body)
        compileStmt(body);
      int runOnceBodyEnd = static_cast<int>(m_code.instructions().size());
      int runOnceJmpIdx = static_cast<int>(m_code.size());
      emit(Opcode::JMP, 0);                 // patched to loopExit below
      for (int j = runOnceBodyStart; j < runOnceBodyEnd; j++) {
        auto &ci = m_code.instructions()[j];
        if (ci.op == Opcode::CONTINUE) ci.a = 0;   // patched below
        else if (ci.op == Opcode::BREAK) ci.a = 0; // patched below
      }

      // Guard loop (natural direction, bounded by a guard check). Direction is
      // fixed for the whole loop, so the direction branch is a one-time decision
      // that is cheaply re-evaluated each iteration.
      //   forward (dir > 0):  `to` bound inclusive -> LE guard (x <= end continues)
      //   backward (dir <= 0): `to` bound inclusive -> GE guard (x >= end continues)
      int guardLoopStart = static_cast<int>(m_code.size());
      emit(Opcode::MOV, x, startReg);
      int guardOnceLabel = static_cast<int>(m_code.size());
      emit(Opcode::JMPIFTRUE, posDir, 0);          // forward -> LT guard path
      int guardCmp = tmpRegister();
      int guardGeIdx = static_cast<int>(m_code.size());
      emit(Opcode::GE, guardCmp, x, endReg);       // backward guard (x >= end)
      emit(Opcode::JMPIFFALSE, guardCmp, 0);       // exit when x < end
      int guardSkipIdx = static_cast<int>(m_code.size());
      emit(Opcode::JMP, 0);                        // backward -> loop body
      int guardFwdIdx = static_cast<int>(m_code.size());
      emit(Opcode::LE, guardCmp, x, endReg);       // forward guard (x <= end)
      emit(Opcode::JMPIFFALSE, guardCmp, 0);       // exit when x > end
      int guardLoopEntry = static_cast<int>(m_code.size());
      for (auto &body : s->body)
        compileStmt(body);
      int guardBodyEnd = static_cast<int>(m_code.instructions().size());
      emit(Opcode::ADD, x, x, stepReg);
      emit(Opcode::JMP, guardOnceLabel);           // loop back to direction branch
      int loopExit = static_cast<int>(m_code.size());

      // Patch the guard loop's branches.
      m_code.instructions()[guardOnceLabel].b = guardFwdIdx;   // forward -> guard
      m_code.instructions()[guardGeIdx + 1].b = loopExit;     // backward exit
      m_code.instructions()[guardFwdIdx + 1].b = loopExit;    // forward exit
      m_code.instructions()[guardSkipIdx].a = guardLoopEntry; // backward -> body (JMP reads a)
      for (int j = guardLoopEntry; j < guardBodyEnd; j++) {
        auto &ci = m_code.instructions()[j];
        if (ci.op == Opcode::CONTINUE) ci.a = guardBodyEnd;   // -> ADD (still increment)
        else if (ci.op == Opcode::BREAK) ci.a = loopExit;
      }
      // Patch the run-once body to exit (continue/break both leave the loop).
      m_code.instructions()[runOnceJmpIdx].a = loopExit;
      for (int j = runOnceBodyStart; j < runOnceBodyEnd; j++) {
        auto &ci = m_code.instructions()[j];
        if (ci.op == Opcode::CONTINUE) ci.a = loopExit;
        else if (ci.op == Opcode::BREAK) ci.a = loopExit;
      }
      // Patch the decision jump.
      m_code.instructions()[guardLabel].b = guardLoopStart;  // sameSign -> guard loop
      m_loops.pop_back();
    }
    m_blockScopes.pop_back();
    break;
  }
  case StmtKind::ForIn: {
    // for i in range(a, b) -> counter loop: i = a; while (i <= b) { ...; ++i }
    // No array is materialized (spec §range).
    m_blockScopes.emplace_back(); // body is a block scope (SPEC # block scope)
    ExprPtr ra, rb;
    if (isRangeCall(s->forEnd, ra, rb)) {
      int x = declareVar(s->loopVar);
      int startReg = tmpRegister();
      compileExpr(ra, startReg);
      emit(Opcode::MOV, x, startReg);
      int endReg = tmpRegister();
      compileExpr(rb, endReg);

      bool hasStep = s->stepExpr != nullptr;
      auto stepVal = hasStep ? foldConst(s->stepExpr) : std::nullopt;
      auto startFold = foldConst(ra);
      auto endFold = foldConst(rb);
      bool boundsFoldable = startFold && endFold;
      int64_t correctDir = boundsFoldable
                               ? ((endFold.value() > startFold.value()) ? 1 : -1)
                               : 0;

      if (hasStep && stepVal && boundsFoldable) {
        int64_t sv = *stepVal;
        if (sv == 0) {
          m_loops.push_back({0, 0});
          int bodyStart = static_cast<int>(m_code.instructions().size());
          for (auto &body : s->body)
            compileStmt(body);
          int bodyEnd = static_cast<int>(m_code.instructions().size());
          int incLabel = bodyEnd;
          emit(Opcode::JMP, bodyStart);
          int exitLabel = static_cast<int>(m_code.size());
          for (int j = bodyStart; j < bodyEnd; j++) {
            auto &ci = m_code.instructions()[j];
            if (ci.op == Opcode::CONTINUE) ci.a = incLabel;
            else if (ci.op == Opcode::BREAK) ci.a = exitLabel;
          }
          m_loops.pop_back();
        } else if ((sv > 0) != (correctDir > 0)) {
          m_loops.push_back({0, 0});
          int bodyStart = static_cast<int>(m_code.instructions().size());
          for (auto &body : s->body)
            compileStmt(body);
          int bodyEnd = static_cast<int>(m_code.instructions().size());
          int exitLabel = static_cast<int>(m_code.size());
          for (int j = bodyStart; j < bodyEnd; j++) {
            auto &ci = m_code.instructions()[j];
            if (ci.op == Opcode::CONTINUE) ci.a = exitLabel;
            else if (ci.op == Opcode::BREAK) ci.a = exitLabel;
          }
          m_loops.pop_back();
        } else {
          m_loops.push_back({0, 0});
          int stepReg = tmpRegister();
          compileExpr(s->stepExpr, stepReg);
          int guardLabel = static_cast<int>(m_code.size());
          int cmpReg = tmpRegister();
          emit(correctDir > 0 ? Opcode::LE : Opcode::GE, cmpReg, x, endReg);
          emit(Opcode::JMPIFFALSE, cmpReg, 0);
          int jumpFalseIdx = static_cast<int>(m_code.size()) - 1;
          int bodyStart = static_cast<int>(m_code.instructions().size());
          for (auto &body : s->body)
            compileStmt(body);
          int bodyEnd = static_cast<int>(m_code.instructions().size());
          int incLabel = bodyEnd;
          emit(Opcode::ADD, x, x, stepReg);
          emit(Opcode::JMP, guardLabel);           // loop back to the guard
          int exitLabel = static_cast<int>(m_code.size());
          for (int j = bodyStart; j < bodyEnd; j++) {
            auto &ci = m_code.instructions()[j];
            if (ci.op == Opcode::CONTINUE) ci.a = incLabel;
            else if (ci.op == Opcode::BREAK) ci.a = exitLabel;
          }
          m_code.instructions()[jumpFalseIdx].b = exitLabel;
          m_loops.pop_back();
        }
      } else {
        m_loops.push_back({0, 0});
        int stepReg = tmpRegister();
        if (hasStep) {
          compileExpr(s->stepExpr, stepReg);
        } else {
          emit(Opcode::CONST, stepReg, emitConst(QLangValue::Int(1)));
        }
        int zeroReg = tmpRegister();
        emit(Opcode::CONST, zeroReg, emitConst(QLangValue::Int(0)));
        // Natural direction from the (runtime) bounds.
        int subReg = tmpRegister();
        emit(Opcode::SUB, subReg, endReg, startReg);
        int posReg = tmpRegister();
        emit(Opcode::GT, posReg, subReg, zeroReg);
        int dirReg = tmpRegister();
        emit(Opcode::CONST, dirReg, emitConst(QLangValue::Int(-1)));
        int dPosLabel = static_cast<int>(m_code.size());
        emit(Opcode::JMPIFTRUE, posReg, 0);
        int dDoneLabel = static_cast<int>(m_code.size());
        emit(Opcode::JMP, 0);
        m_code.instructions()[dPosLabel].b = dPosLabel + 2;
        emit(Opcode::CONST, dirReg, emitConst(QLangValue::Int(1)));
        m_code.instructions()[dDoneLabel].a = dPosLabel + 3;

        // step == 0 -> infinite loop: body + increment forever, no guard.
        int zeroCmp = tmpRegister();
        emit(Opcode::EQ, zeroCmp, stepReg, zeroReg);
        int zeroJumpIdx = static_cast<int>(m_code.size());
        emit(Opcode::JMPIFTRUE, zeroCmp, 0);
        int infExitLabel = static_cast<int>(m_code.size());
        int posStep = tmpRegister();
        emit(Opcode::GT, posStep, stepReg, zeroReg);
        int posDir = tmpRegister();
        emit(Opcode::GT, posDir, dirReg, zeroReg);
        int sameSign = tmpRegister();
        emit(Opcode::EQ, sameSign, posStep, posDir);
        int ssJumpIdx = static_cast<int>(m_code.size());
        emit(Opcode::JMPIFTRUE, sameSign, 0);
        // Run-once: step and direction disagree -> body executes a single time.
        emit(Opcode::MOV, x, startReg);
        int runOnceBodyStart = static_cast<int>(m_code.instructions().size());
        for (auto &body : s->body)
          compileStmt(body);
        int runOnceBodyEnd = static_cast<int>(m_code.instructions().size());
        int runOnceExitJmpIdx = static_cast<int>(m_code.size());
        emit(Opcode::JMP, 0); // placeholder, backpatched to guardExit below
        // Infinite-loop (step == 0) body: x advances forever, no guard.
        // Emitted before the guarded loop so the guarded loop's exit
        // (guardExit) falls past it.
        emit(Opcode::MOV, x, startReg);
        int infBodyStart = static_cast<int>(m_code.instructions().size());
        for (auto &body : s->body)
          compileStmt(body);
        int infBodyEnd = static_cast<int>(m_code.instructions().size());
        emit(Opcode::ADD, x, x, stepReg);
        emit(Opcode::JMP, infBodyStart);
        for (int j = infBodyStart; j < infBodyEnd; j++) {
          auto &ci = m_code.instructions()[j];
          if (ci.op == Opcode::CONTINUE) ci.a = infBodyEnd;
          else if (ci.op == Opcode::BREAK) ci.a = infExitLabel;
        }
        // Guarded loop: init x once, then re-check the direction-selected guard
        // every pass (forward: x <= end; backward: x >= end).
        int guardInit = static_cast<int>(m_code.size());
        emit(Opcode::MOV, x, startReg);
        int guardLabel = static_cast<int>(m_code.size());
        int posDir2 = tmpRegister();
        emit(Opcode::GT, posDir2, dirReg, zeroReg);
        int fwdGuardIdx = static_cast<int>(m_code.size());
        emit(Opcode::JMPIFTRUE, posDir2, 0);
        int cmpReg = tmpRegister();
        emit(Opcode::GE, cmpReg, x, endReg);
        int backExitIdx = static_cast<int>(m_code.size());
        emit(Opcode::JMPIFFALSE, cmpReg, 0);
        int skipFwdIdx = static_cast<int>(m_code.size());
        emit(Opcode::JMP, 0);
        m_code.instructions()[fwdGuardIdx].b = skipFwdIdx + 1;
        emit(Opcode::LE, cmpReg, x, endReg);
        int fwdExitIdx = static_cast<int>(m_code.size());
        emit(Opcode::JMPIFFALSE, cmpReg, 0);
        int bodyStart = static_cast<int>(m_code.instructions().size());
        for (auto &body : s->body)
          compileStmt(body);
        int bodyEnd = static_cast<int>(m_code.instructions().size());
        emit(Opcode::ADD, x, x, stepReg);
        emit(Opcode::JMP, guardLabel);
        int guardExit = static_cast<int>(m_code.size());
        for (int j = bodyStart; j < bodyEnd; j++) {
          auto &ci = m_code.instructions()[j];
          if (ci.op == Opcode::CONTINUE) ci.a = bodyEnd; // jump to increment
          else if (ci.op == Opcode::BREAK) ci.a = guardExit;
        }
        m_code.instructions()[backExitIdx].b = guardExit;
        m_code.instructions()[fwdExitIdx].b = guardExit;
        m_code.instructions()[zeroJumpIdx].b = infBodyStart;
        m_code.instructions()[ssJumpIdx].b = guardInit;
        m_code.instructions()[runOnceExitJmpIdx].a = guardExit;
        for (int j = runOnceBodyStart; j < runOnceBodyEnd; j++) {
          auto &ci = m_code.instructions()[j];
          if (ci.op == Opcode::CONTINUE) ci.a = guardExit;
          else if (ci.op == Opcode::BREAK) ci.a = guardExit;
        }
        m_loops.pop_back();
      }
      m_blockScopes.pop_back();
      break;
    }
    // `step` is only meaningful for numeric ranges, not array/table
    // iteration (whose stride is fixed at 1). Reject it here.
    if (s->stepExpr != nullptr) {
      throw QLangError(
          QLangErrorKind::Type, 0, 0,
          "step is not supported over array literals in for-in loops");
    }
    // for i in <collection> ... next i : iterate array elements / table keys.
    int x = declareVar(s->loopVar); // loopVar holds each element/key
    int idx = tmpRegister();        // index counter
    emit(Opcode::CONST, idx, emitConst(QLangValue::Int(0)));
    int colReg = tmpRegister();
    compileExpr(s->forEnd, colReg); // load the collection once
    int lenReg = tmpRegister();
    emit(Opcode::LEN, lenReg, colReg, 0);

    int testLabel = static_cast<int>(m_code.size());
    int cmpReg = tmpRegister();
    emit(Opcode::LT, cmpReg, idx, lenReg); // idx < len(collection)
    emit(Opcode::JMPIFFALSE, cmpReg, 0);
    int jumpFalseIdx = static_cast<int>(m_code.size()) - 1;
    // Assign loopVar = collection[idx] at the top of each iteration.
    emit(Opcode::SEQGET, x, colReg, idx);

    m_loops.push_back({0, 0});
    int bodyStart = static_cast<int>(m_code.instructions().size());
    for (auto &body : s->body)
      compileStmt(body);
    int bodyEnd = static_cast<int>(m_code.instructions().size());

    int incLabel = bodyEnd;
    emit(Opcode::INC, idx, idx);
    emit(Opcode::JMP, testLabel);
    int exitLabel = static_cast<int>(m_code.size());
    m_code.instructions()[jumpFalseIdx].b = exitLabel;
    for (int j = bodyStart; j < bodyEnd; j++) {
      auto &ci = m_code.instructions()[j];
      if (ci.op == Opcode::CONTINUE) ci.a = incLabel;
      else if (ci.op == Opcode::BREAK) ci.a = exitLabel;
    }
    m_loops.pop_back();
    m_blockScopes.pop_back();
    break;
  }
  case StmtKind::While: {
    int testLabel = static_cast<int>(m_code.size());
    int condReg = tmpRegister();
    compileExpr(s->cond, condReg);
    emit(Opcode::JMPIFFALSE, condReg, 0);
    int jumpFalseIdx = static_cast<int>(m_code.size()) - 1;

    // continue -> re-evaluate the condition (testLabel is known before the body);
    // break -> exit (post-body), so patch BREAK instructions after the body.
    m_loops.push_back({testLabel, 0});
    int bodyStart = static_cast<int>(m_code.instructions().size());
    m_blockScopes.emplace_back(); // body is a block scope (SPEC # block scope)
    for (auto &body : s->whileBody)
      compileStmt(body);
    m_blockScopes.pop_back();
    int bodyEnd = static_cast<int>(m_code.instructions().size());

    emit(Opcode::JMP, testLabel);
    int exitLabel = static_cast<int>(m_code.size());
    m_code.instructions()[jumpFalseIdx].b = exitLabel;
    for (int j = bodyStart; j < bodyEnd; j++)
      if (m_code.instructions()[j].op == Opcode::BREAK)
        m_code.instructions()[j].a = exitLabel;
    m_loops.pop_back();
    break;
  }
  case StmtKind::If: {
    int condReg = tmpRegister();
    compileExpr(s->ifCond, condReg);
    emit(Opcode::JMPIFFALSE, condReg, 0);
    int jumpFalseIdx = static_cast<int>(m_code.size()) - 1;

    m_blockScopes.emplace_back(); // if body is a block scope (SPEC # block scope)
    for (auto &body : s->ifBody)
      compileStmt(body);
    m_blockScopes.pop_back();

    if (!s->elseBody.empty()) {
      // Emit JMP after if body to skip else body; patch target after else body compiles.
      emit(Opcode::JMP, 0);
      int jumpPastIdx = static_cast<int>(m_code.size()) - 1;
      int elseBodyStart = static_cast<int>(m_code.size());
      m_blockScopes.emplace_back(); // else body is a separate block scope
      for (auto &body : s->elseBody)
        compileStmt(body);
      m_blockScopes.pop_back();
      m_code.instructions()[jumpPastIdx].a = static_cast<int>(m_code.size());
      m_code.instructions()[jumpFalseIdx].b = elseBodyStart;
    } else {
      m_code.instructions()[jumpFalseIdx].b = static_cast<int>(m_code.size());
    }
    break;
  }
  case StmtKind::GotoIf: {
    int condReg = tmpRegister();
    compileExpr(s->gotoIfCond, condReg);
    auto lit = m_labels.find(s->gotoIfLabel);
    if (lit == m_labels.end())
      throw QLangError(QLangErrorKind::Parse, 0, 0,
                       "undefined label '" + s->gotoIfLabel + "'");
    emit(Opcode::JMPIFTRUE, condReg, lit->second);
    break;
  }
  case StmtKind::ExprStmt: {
    // Statement-form prefix/postfix inc/dec on a variable must write back to
    // that variable's register (e.g. --a, a++). Otherwise the inc/dec result
    // would land in a throwaway temp and the variable would be unchanged.
    if (s->value && s->value->kind == ExprKind::Unary &&
        (s->value->uop.text == "++" || s->value->uop.text == "--")) {
      const ExprPtr &opnd = s->value->operand;
      if (opnd && opnd->kind == ExprKind::Variable) {
        int r = regOf(opnd->name);
        if (r < 0)
          throw QLangError(QLangErrorKind::Parse, 0, 0,
                           "use of undeclared variable '" + opnd->name + "'");
        guardNotConst(opnd->name, "increment/decrement");
        emit(s->value->uop.text == "++" ? Opcode::INC : Opcode::DEC, r, r);
        break;
      }
    }
    int t = tmpRegister();
    compileExpr(s->value, t);
    break;
  }
  case StmtKind::Def: {
    // `def name(params) ... end` (or brace form): a global function.
    trace(TRACE_FULL, "[DBG-GENVAR] Def stmt for '%s', m_generatorVars={", s->funcName.c_str());
    for (auto &g : m_generatorVars) trace(TRACE_FULL, " %s", g.c_str());
    trace(TRACE_FULL, " }, m_functions.size=%d\n", (int)m_functions.size());
    int funcIndex = buildFunction(s->funcName, s->params, {}, s->body);
    m_userFuncs[s->funcName] = funcIndex;
    // Track generator functions at the outer scope so assignments from them
    // are recognized as producing generator values.
    trace(TRACE_FULL, "[DBG-GENVAR] After buildFunction: m_functions.size=%d, back.isGenerator=%d\n",
          (int)m_functions.size(), m_functions.empty() ? -1 : (int)m_functions.back().isGenerator);
    if (!m_functions.empty() && m_functions.back().isGenerator)
      m_generatorVars.insert(s->funcName);
    trace(TRACE_FULL, "[DBG-GENVAR] After insert: m_generatorVars={");
    for (auto &g : m_generatorVars) trace(TRACE_FULL, " %s", g.c_str());
    trace(TRACE_FULL, " }\n");
    break;
  }
  case StmtKind::Arrow: {
    // `local name = (params)[captures] { ... }`: an arrow-closure value.
    int funcIndex = buildFunction(s->name, s->params, s->captures, s->body);
    m_closureVars.insert(s->name);
    m_varTypes[s->name] = QLangValue::Type::Function;
    // If the closure contains yield (at any nesting depth), mark it as a
    // generator variable so nested functions emit CALLCLOS for calls to it.
    for (auto &st : s->body) {
      if (st && containsYield(st)) {
        m_generatorVars.insert(s->name);
        break;
      }
    }
    int r = declareVar(s->name);
    int t = tmpRegister();
    // Constant describing the closure: funcIndex + enclosing capture registers.
    auto closure = std::make_shared<Closure>();
    closure->funcIndex = funcIndex;
    for (auto &cap : s->captures) {
      int reg = regOf(cap);
      if (reg < 0)
        throw QLangError(QLangErrorKind::Parse, 0, 0,
                         "capture of undeclared variable '" + cap + "'");
      CaptureRef cr;
      cr.reg = reg;
      cr.frame = nullptr; // filled in by MAKEFUNC at runtime
      closure->captures.push_back(cr);
    }
    int constIdx = emitConst(QLangValue::Function(closure));
    emit(Opcode::MAKEFUNC, t, constIdx);
    emit(Opcode::MOV, r, t);
    break;
  }
  case StmtKind::Return: {
    // Return value goes into register 0 (the frame's return register).
    if (s->returnValue) {
      int t = tmpRegister();
      compileExpr(s->returnValue, t);
      emit(Opcode::MOV, 0, t);
    } else {
      emit(Opcode::CONST, 0, emitConst(QLangValue())); // nil
    }
    emit(Opcode::RETURN, 0);
    break;
  }
  case StmtKind::Yield: {
    // Yield value goes into register 0 (the generator's yield register).
    if (s->yieldValue) {
      int t = tmpRegister();
      compileExpr(s->yieldValue, t);
      emit(Opcode::MOV, 0, t);
    } else {
      emit(Opcode::CONST, 0, emitConst(QLangValue())); // nil
    }
    emit(Opcode::YIELD, 0);
    break;
  }
  case StmtKind::Continue: {
    if (m_loops.empty())
      throw QLangError(QLangErrorKind::Parse, 0, 0,
                       "'continue' outside of a loop");
    emit(Opcode::CONTINUE, m_loops.back().contAddr);
    break;
  }
  case StmtKind::Break: {
    if (!m_loops.empty()) {
      emit(Opcode::BREAK, m_loops.back().breakAddr);
      break;
    }
    if (!m_switches.empty()) {
      // Record this break JMP for later patching once we know the switch end.
      m_breakJmps.push_back(static_cast<int>(m_code.size()));
      emit(Opcode::JMP, 0); // placeholder, patched in switch compilation
      break;
    }
    throw QLangError(QLangErrorKind::Parse, 0, 0,
                     "'break' outside of a loop or switch");
    break;
  }
  case StmtKind::Label: {
    // Record the current instruction index as the target for this label.
    m_labels[s->labelName] = static_cast<int>(m_code.size());
    break;
  }
  case StmtKind::Goto: {
    auto lit = m_labels.find(s->gotoLabel);
    if (lit == m_labels.end())
      throw QLangError(QLangErrorKind::Parse, 0, 0,
                       "undefined label '" + s->gotoLabel + "'");
    emit(Opcode::JMP, lit->second);
    break;
  }
  case StmtKind::Import: {
    // Resolve module functions at compile time and register them as external
    // funcs in the current scope. Supports:
    //   import math                          — flat import all
    //   import math.min                      — flat import single func (legacy)
    //   import math as m                     — register under alias, flat import all
    //   from math import *                   — flat import all
    //   from math import min, max            — flat import specific funcs
    if (!m_inFunction) {
      // Find the module.
      const QLangContext::Module *mod = nullptr;
      for (const auto &m : m_moduleFuncs) {
        if (m.name == s->importModule) { mod = &m; break; }
      }
      if (!mod)
        throw QLangError(QLangErrorKind::Parse, 0, 0,
                         "undefined module '" + s->importModule + "'");

      // If alias specified, register the module under the alias name.
      if (!s->importAlias.empty()) {
        QLangContext::Module aliased;
        aliased.name = s->importAlias;
        aliased.funcs = mod->funcs;
        m_moduleFuncs.push_back(aliased);
        // Now import from the alias.
        mod = &m_moduleFuncs.back();
      }

      // Collect function names to import.
      std::vector<std::string> names;
      if (!s->importFunc.empty()) {
        // Legacy single-function import.
        auto fit = mod->funcs.find(s->importFunc);
        if (fit == mod->funcs.end())
          throw QLangError(QLangErrorKind::Parse, 0, 0,
                           "undefined function '" + s->importFunc + "' in module '" + s->importModule + "'");
        names.push_back(s->importFunc);
      } else if (s->importAll) {
        // `from math import *` — import all.
        for (const auto &[fname, func] : mod->funcs)
          names.push_back(fname);
      } else if (!s->importFuncs.empty()) {
        // `from math import min, max` — import specific functions.
        for (const auto &fname : s->importFuncs) {
          if (mod->funcs.find(fname) == mod->funcs.end())
            throw QLangError(QLangErrorKind::Parse, 0, 0,
                             "undefined function '" + fname + "' in module '" + s->importModule + "'");
          names.push_back(fname);
        }
      } else {
        // `import math` — flat import all (v1).
        for (const auto &[fname, func] : mod->funcs)
          names.push_back(fname);
      }

      // Register each function as an external func in the current scope.
      for (const auto &fname : names) {
        int idx = -1 - m_nativeFuncIds[fname];
        while (m_nativeFuncsResolved.size() <= static_cast<size_t>(idx))
          m_nativeFuncsResolved.push_back(nullptr);
        auto it = mod->funcs.find(fname);
        if (it != mod->funcs.end())
          m_nativeFuncsResolved[static_cast<size_t>(idx)] = it->second;
      }
    }
    break;
  }
  case StmtKind::Class: {
    // class Name { fields + methods end }
    // Compile body into a function that creates the prototype table and returns it.
    // Store the function index; at construction time we CALLFN it to get the prototype.
    Bytecode savedCode = m_code;
    auto savedRegs = m_regs;
    auto savedConstVars = m_constVars;
    auto savedVarTypes = m_varTypes;
    auto savedUserFuncs = m_userFuncs;
    auto savedClosureVars = m_closureVars;
    auto savedCaptureRegs = m_captureRegs;
    auto savedGlobalVars = m_globalVars;
    auto savedCapturedVars = m_capturedVars;
    auto savedGeneratorVars = m_generatorVars;
    int savedNextReg = m_nextReg;
    bool savedInFunction = m_inFunction;

    m_code = Bytecode();
    m_regs.clear();
    m_constVars.clear();
    m_varTypes.clear();
    m_userFuncs.clear();
    m_closureVars.clear();
    m_captureRegs.clear();
    m_generatorVars.clear();
    m_globalVars.clear();
    m_capturedVars.clear();
    m_labels.clear();
    m_nextReg = 0;
    m_inFunction = false;

    // Emit: TABCREATE proto, then compile body, then RETURN proto.
    emit(Opcode::TABCREATE, 0);
    m_nextReg = 1; // temporaries must not collide with prototype at reg 0

    std::vector<std::string> methodNames;
    std::unordered_map<std::string, int> methodFuncIndices;
    // Fixed arity (Phase 4): method name -> declared param count (incl. self).
    std::unordered_map<std::string, int> methodArities;
    // Closed field set (Phase 1): declared fields in declaration order.
    std::vector<std::string> fieldNames;
    std::unordered_map<std::string, int> fieldIndices;
    // Const fields (Phase 2): declared fields marked `const` (read-only).
    std::vector<std::string> constNames;
    // Field type inference (Phase 5): field name -> type inferred from the
    // field's default value (name -> type-name string in `__field_types__`).
    std::unordered_map<std::string, QLangValue::Type> fieldTypes;
    // Static members (new extension beyond v1). Collected in the loop and
    // compiled into the shared `__statics__` table AFTER the loop: method
    // bodies must see the full static name set, so static methods are deferred.
    std::unordered_set<std::string> allStatics;
    std::vector<std::pair<std::string, ExprPtr>> staticFieldDecls; // (name, value)
    std::vector<std::string> staticConstNames;
    struct StaticMethodDecl {
      std::string name;
      std::vector<std::string> params;
      std::vector<std::string> captures;
      std::vector<StmtPtr> body;
      int funcIndex = -1; // filled in the post-loop statics build
    };
    std::vector<StaticMethodDecl> staticMethodDecls;
    for (auto &st : s->classBody) {
      if (!st->isStatic)
        continue;
      if (st->kind == StmtKind::Arrow || st->kind == StmtKind::Assignment ||
          st->kind == StmtKind::Const)
        allStatics.insert(st->name);
    }
    for (auto &st : s->classBody) {
      if (st->kind == StmtKind::Assignment || st->kind == StmtKind::Const) {
        if (st->isStatic) {
          // Static field: stored in the `__statics__` table, NOT the instance
          // field schema. Value is compiled in the post-loop statics build.
          staticFieldDecls.push_back(
              {st->name, st->kind == StmtKind::Assignment ? st->assignValue
                                                          : st->value});
          if (st->kind == StmtKind::Const)
            staticConstNames.push_back(st->name);
          continue;
        }
        // A field: `name = value` or `const name = value`.
        int keyR = tmpRegister();
        emit(Opcode::CONST, keyR, emitConst(QLangValue::String(st->name)));
        int valR = tmpRegister();
        compileExpr(st->kind == StmtKind::Assignment ? st->assignValue
                                                     : st->value,
                    valR);
        emit(Opcode::TABPUT, 0, keyR, valR);
        // Record the declared field (name -> index) for the closed field set.
        fieldIndices[st->name] = static_cast<int>(fieldNames.size());
        fieldNames.push_back(st->name);
        if (st->kind == StmtKind::Const)
          constNames.push_back(st->name);
        // Field type inference (Phase 5): infer the field's type from its
        // default value. Only constant defaults yield a constraint; a
        // non-constant (or `nil`) default leaves the field unconstrained.
        try {
          QLangValue def =
              evalConstExpr(st->kind == StmtKind::Assignment ? st->assignValue
                                                             : st->value);
          if (def.type() != QLangValue::Type::Nil)
            fieldTypes[st->name] = def.type();
        } catch (...) {
          // Non-constant default: no compile-time type constraint.
        }
      } else if (st->kind == StmtKind::Arrow) {
        if (st->isStatic) {
          // Static method: compiled (with a hidden anchor param) in the
          // post-loop statics build, after all static names are known.
          staticMethodDecls.push_back(
              {st->name, st->params, st->captures, st->body});
          continue;
        }
        // Instance method: compile now, with the class's static names visible
        // so the body can reference statics via `self.__statics__`.
        m_classStaticNames = allStatics;
        m_inStaticMethod = false;
        int funcIndex = buildFunction(st->name, st->params, st->captures, st->body);
        m_classStaticNames.clear();
        auto closure = std::make_shared<Closure>();
        closure->funcIndex = funcIndex;
        int constIdx = emitConst(QLangValue::Function(closure));
        int keyR = tmpRegister();
        emit(Opcode::CONST, keyR, emitConst(QLangValue::String(st->name)));
        int funcR = tmpRegister();
        emit(Opcode::MAKEFUNC, funcR, constIdx);
        emit(Opcode::TABPUT, 0, keyR, funcR);
        methodNames.push_back(st->name);
        methodFuncIndices[st->name] = funcIndex;
        // Fixed arity (Phase 4): record the declared parameter count
        // (includes `self`) for call-site checking.
        methodArities[st->name] = static_cast<int>(st->params.size());
      } else {
        compileStmt(st);
      }
    }

    // Static members (new extension): build the shared `__statics__` table and
    // install it into the prototype BEFORE `__fields__` (same metadata-ordering
    // constraint as `__const_fields__` above). Every instance copies this table
    // by reference, so instance writes mutate class-level state.
    if (!staticFieldDecls.empty() || !staticMethodDecls.empty()) {
      int staticsReg = tmpRegister();
      emit(Opcode::TABCREATE, staticsReg);
      for (auto &sf : staticFieldDecls) {
        int keyR = tmpRegister();
        emit(Opcode::CONST, keyR, emitConst(QLangValue::String(sf.first)));
        int valR = tmpRegister();
        compileExpr(sf.second, valR);
        emit(Opcode::TABPUT, staticsReg, keyR, valR);
      }
      for (auto &sm : staticMethodDecls) {
        // Hidden anchor parameter (the `__statics__` table) as param 0, so a
        // static method needs no self and can read/write statics directly.
        std::vector<std::string> params = {"__anchor__"};
        for (auto &p : sm.params)
          params.push_back(p);
        m_classStaticNames = allStatics;
        m_inStaticMethod = true;
        int funcIndex = buildFunction(sm.name, params, sm.captures, sm.body);
        m_inStaticMethod = false;
        m_classStaticNames.clear();
        sm.funcIndex = funcIndex;
        auto closure = std::make_shared<Closure>();
        closure->funcIndex = funcIndex;
        int constIdx = emitConst(QLangValue::Function(closure));
        int keyR = tmpRegister();
        emit(Opcode::CONST, keyR, emitConst(QLangValue::String(sm.name)));
        int funcR = tmpRegister();
        emit(Opcode::MAKEFUNC, funcR, constIdx);
        emit(Opcode::TABPUT, staticsReg, keyR, funcR);
      }
      // `__static_const__` set (name -> 1) for static const fields, so
      // SETFIELD can reject assignment to them.
      if (!staticConstNames.empty()) {
        int sconstReg = tmpRegister();
        emit(Opcode::TABCREATE, sconstReg);
        for (const auto &cn : staticConstNames) {
          int keyR = tmpRegister();
          emit(Opcode::CONST, keyR, emitConst(QLangValue::String(cn)));
          int valR = tmpRegister();
          emit(Opcode::CONST, valR, emitConst(QLangValue::Int(1)));
          emit(Opcode::TABPUT, sconstReg, keyR, valR);
        }
        int sckeyR = tmpRegister();
        emit(Opcode::CONST, sckeyR,
             emitConst(QLangValue::String("__static_const__")));
        emit(Opcode::TABPUT, staticsReg, sckeyR, sconstReg);
      }
      int skeyR = tmpRegister();
      emit(Opcode::CONST, skeyR, emitConst(QLangValue::String("__statics__")));
      emit(Opcode::TABPUT, 0, skeyR, staticsReg);
    }

    // Const fields (Phase 2): emit `__const_fields__` metadata (name -> 1) into
    // the prototype BEFORE installing `__fields__`, so the prototype is not yet
    // a "class instance" (enforcement key present) while metadata is written —
    // setField would otherwise reject the non-declared metadata key.
    if (!constNames.empty()) {
      int cfieldsReg = tmpRegister();
      emit(Opcode::TABCREATE, cfieldsReg);
      for (const auto &cn : constNames) {
        int keyR = tmpRegister();
        emit(Opcode::CONST, keyR, emitConst(QLangValue::String(cn)));
        int valR = tmpRegister();
        emit(Opcode::CONST, valR, emitConst(QLangValue::Int(1)));
        emit(Opcode::TABPUT, cfieldsReg, keyR, valR);
      }
      int cfkeyR = tmpRegister();
      emit(Opcode::CONST, cfkeyR,
           emitConst(QLangValue::String(kConstFieldMetaKey)));
      emit(Opcode::TABPUT, 0, cfkeyR, cfieldsReg);
    }

    // Field types (Phase 5): emit `__field_types__` metadata (name ->
    // type-name string) into the prototype BEFORE installing `__fields__` for
    // the same reason as the const-field metadata above.
    if (!fieldTypes.empty()) {
      int ftypesReg = tmpRegister();
      emit(Opcode::TABCREATE, ftypesReg);
      for (const auto &ft : fieldTypes) {
        int keyR = tmpRegister();
        emit(Opcode::CONST, keyR, emitConst(QLangValue::String(ft.first)));
        int valR = tmpRegister();
        QLangValue tv(ft.second);
        emit(Opcode::CONST, valR, emitConst(QLangValue::String(tv.typeName())));
        emit(Opcode::TABPUT, ftypesReg, keyR, valR);
      }
      int ftkeyR = tmpRegister();
      emit(Opcode::CONST, ftkeyR,
           emitConst(QLangValue::String(kFieldTypeMetaKey)));
      emit(Opcode::TABPUT, 0, ftkeyR, ftypesReg);
    }

    // Closed field set (Phase 1): emit `__fields__` metadata (name -> index)
    // into the prototype table LAST so instances inherit the closed field
    // schema. Installing `__fields__` is what makes the prototype a "class
    // instance" for setField/getField enforcement.
    if (!fieldNames.empty()) {
      int fieldsReg = tmpRegister();
      emit(Opcode::TABCREATE, fieldsReg);
      for (size_t i = 0; i < fieldNames.size(); i++) {
        int keyR = tmpRegister();
        emit(Opcode::CONST, keyR, emitConst(QLangValue::String(fieldNames[i])));
        int valR = tmpRegister();
        emit(Opcode::CONST, valR,
             emitConst(QLangValue::Int(static_cast<int32_t>(i))));
        emit(Opcode::TABPUT, fieldsReg, keyR, valR);
      }
      int fkeyR = tmpRegister();
      emit(Opcode::CONST, fkeyR, emitConst(QLangValue::String(kFieldMetaKey)));
      emit(Opcode::TABPUT, 0, fkeyR, fieldsReg);
    }

    emit(Opcode::RETURN, 0);

    int prototypeFuncIdx = static_cast<int>(m_functions.size()) + 1; // +1 for mainFn prepend
    m_functions.emplace_back(Function{
        s->className + "_prototype", 0, m_nextReg,
        std::move(m_code.instructions()), std::move(m_code.constants()), {}});
    m_code = std::move(savedCode);
    m_regs = std::move(savedRegs);
    m_constVars = std::move(savedConstVars);
    m_varTypes = std::move(savedVarTypes);
    m_userFuncs = std::move(savedUserFuncs);
    m_closureVars = std::move(savedClosureVars);
    m_captureRegs = std::move(savedCaptureRegs);
    m_globalVars = std::move(savedGlobalVars);
    m_capturedVars = std::move(savedCapturedVars);
    m_generatorVars = std::move(savedGeneratorVars);
    m_nextReg = savedNextReg;
    m_inFunction = savedInFunction;

    ClassRecord rec;
    rec.prototypeFunc = prototypeFuncIdx;
    rec.methodNames = std::move(methodNames);
    rec.methodFuncIndices = std::move(methodFuncIndices);
    rec.methodArities = std::move(methodArities);
    rec.fieldNames = std::move(fieldNames);
    rec.fieldIndices = std::move(fieldIndices);
    for (const auto &cn : constNames)
      rec.constFields.insert(cn);
    rec.fieldTypes = std::move(fieldTypes);
    // Static members (new extension): recorded so `C::name` codegen and call
    // sites can resolve them and check arity.
    for (auto &sf : staticFieldDecls)
      rec.staticFields.insert(sf.first);
    for (const auto &cn : staticConstNames)
      rec.staticConstFields.insert(cn);
    for (auto &sm : staticMethodDecls) {
      rec.staticMethodFuncIndices[sm.name] = sm.funcIndex;
      rec.staticMethodArities[sm.name] = static_cast<int>(sm.params.size());
    }
    m_classes[s->className] = std::move(rec);
    // Cache the prototype table in a hidden variable (`__proto_<Name>`) so
    // every instance and `C::name` access shares ONE prototype — and with it
    // the one shared `__statics__` map.
    int protoVar = declareVar("__proto_" + s->className);
    emit(Opcode::CALLFN, protoVar, prototypeFuncIdx, 0);
    break;
  }
  case StmtKind::Global: {
    if (!m_inFunction)
      throw QLangError(QLangErrorKind::Parse, 0, 0,
                       "'global' is only valid inside a function");
    for (const auto &name : s->globalNames) {
      // Allow global to override local declarations — global wins.
      if (regOf(name) >= 0) {
        m_regs.erase(name);
      }
      if (!m_globalVars.count(name)) {
        throw QLangError(QLangErrorKind::Parse, 0, 0,
                         "global variable '" + name + "' is not declared");
      }
      // Add to capture list so the VM binds it to the enclosing frame.
      if (captures && std::find(captures->begin(), captures->end(), name) == captures->end()) {
        captures->push_back(name);
      }
      // Mark as captured so compileExpr routes through m_captureRegs.
      m_capturedVars.insert(name);
      // Set up the capture register immediately so subsequent references
      // compile as REFLOAD/REFSTORE instead of MOV/MOV.
      // m_captureRegs stores the capture INDEX (0, 1, 2...) for the VM.
      if (captures) {
        int captureIdx = static_cast<int>(captures->size()) - 1;
        m_captureRegs[name] = captureIdx;
        // Save the caller's register number for later use in fn.captures.
        m_globalRegMap[name] = m_savedRegs.count(name) ? m_savedRegs[name] : 0;
      }
    }
    break;
  }
  case StmtKind::Switch: {
    // Compile the switch expression into a register.
    int valReg = tmpRegister();
    compileExpr(s->switchExpr, valReg);

    // Push switch context so `break` inside case bodies jumps here.
    // We'll patch the exit target after we know where switchEnd is.
    m_switches.push_back(0); // placeholder, patched later

    // Emit a comparison + conditional-jump chain for each case.
    // For each case we emit:
    //   [match check: EQ/IN] -> JMPIFTRUE -> [JMP to skip] -> [case body]
    // After all cases, compile default, then emit JMP to switch end.
    // The JMP after each case jumps to the next case's check (or switchEnd for last).
    std::vector<int> caseMatchStarts; // instruction index of each case's match start (const emit)
    std::vector<int> caseJmpiIfTrueIdx; // instruction index of each JMPIFTRUE
    std::vector<int> caseSkipJmpIdx;    // JMP after JMPIFTRUE (skip to next case)
    std::vector<int> caseExitJmpIdx;    // implicit JMP after case body (exit to switchEnd)
    std::vector<int> caseBodyStarts;    // instruction index of each case body
    for (size_t i = 0; i < s->cases.size(); i++) {
      const auto &sc = s->cases[i];
      int matchCheckIdx = static_cast<int>(m_code.size());
      caseMatchStarts.push_back(matchCheckIdx);
      // Emit the match check for this case.
      int cmpReg = tmpRegister();
      if (sc.isIn) {
        int exprReg = tmpRegister();
        compileExpr(sc.value, exprReg);
        emit(Opcode::IN, cmpReg, valReg, exprReg);
      } else {
        int exprReg = tmpRegister();
        compileExpr(sc.value, exprReg);
        emit(Opcode::EQ, cmpReg, valReg, exprReg);
      }
      emit(Opcode::JMPIFTRUE, cmpReg, 0); // target to patch: jump to case body
      caseJmpiIfTrueIdx.push_back(static_cast<int>(m_code.size()) - 1);
      // Emit JMP to skip this case body (patched to next case or switchEnd).
      int skipJmpIdx = static_cast<int>(m_code.size());
      emit(Opcode::JMP, 0);
      caseSkipJmpIdx.push_back(skipJmpIdx);
      // Compile the case body, then emit an implicit JMP to skip remaining cases.
      caseBodyStarts.push_back(static_cast<int>(m_code.size()));
      m_blockScopes.emplace_back(); // each case body is a block scope
      for (auto &body : sc.body) {
        trace(TRACE_FULL, "[DBG-CASE] Case %zu body stmt kind=%d\n", i, (int)body->kind);
        if (body->kind == StmtKind::Assignment) {
          trace(TRACE_FULL, "[DBG-CASE]   Assign op=%s value=", body->assignOp.text.c_str());
          if (body->assignValue && body->assignValue->kind == ExprKind::Literal)
            trace(TRACE_FULL, "Literal:%d (type=%d)\n", (int)body->assignValue->literal.asInt(), (int)body->assignValue->literal.type());
          else
            trace(TRACE_FULL, "non-literal\n");
        }
        compileStmt(body);
      }
      m_blockScopes.pop_back();
      emit(Opcode::JMP, 0); // implicit exit: patched after default compiles
      caseExitJmpIdx.push_back(static_cast<int>(m_code.size()) - 1);
    }

    // Compile the default body (if any).
    int defaultBodyStart = static_cast<int>(m_code.size());
    m_blockScopes.emplace_back(); // default body is a block scope
    for (auto &db : s->defaultBody)
      compileStmt(db);
    m_blockScopes.pop_back();
    int defaultBodyEnd = static_cast<int>(m_code.size());

    // Now we know where switchEnd is (after all cases and default).
    int switchEnd = static_cast<int>(m_code.size());
    // Update the switch context with the real exit target.
    m_switches.back() = switchEnd;
    // Patch any break JMPs that were emitted with placeholder target 0.
    for (int idx : m_breakJmps) {
      m_code.at(idx).a = switchEnd;
    }
    m_breakJmps.clear();

    // Patch all JMPIFTRUE targets to point at their case body start,
    // skip-JMP targets to next case (or default body for last),
    // and exit-JMP targets to switchEnd.
    for (size_t i = 0; i < s->cases.size(); i++) {
      m_code.at(caseJmpiIfTrueIdx[i]).b = caseBodyStarts[i];
      if (i + 1 < s->cases.size())
        m_code.at(caseSkipJmpIdx[i]).a = caseMatchStarts[i + 1];
      else
        m_code.at(caseSkipJmpIdx[i]).a = defaultBodyStart;
      m_code.at(caseExitJmpIdx[i]).a = switchEnd;
    }

    // Pop switch context after patching is complete.
    m_switches.pop_back();

    // Debug: dump switch bytecode
    trace(TRACE_FULL, "[DBG-SW] switchEnd=%d instructions.size()=%zu\n", switchEnd, m_code.instructions().size());
    for (size_t i = 0; i < m_code.instructions().size(); i++) {
      auto &inst = m_code.instructions()[i];
      trace(TRACE_FULL, "[DBG-SW]   [%zu] op=%d a=%d b=%d c=%d\n", i, (int)inst.op, inst.a, inst.b, inst.c);
    }
    trace(TRACE_FULL, "[DBG-SW] constants.size()=%zu\n", m_code.constants().size());
    for (size_t i = 0; i < m_code.constants().size(); i++) {
      const auto &c = m_code.constants()[i];
      std::string cv;
      if (c.type() == QLangValue::Type::Int)
        cv = std::to_string(c.asInt());
      else if (c.type() == QLangValue::Type::String)
        cv = c.asString();
      else
        cv = std::string("(type=") + std::to_string((int)c.type()) + ")";
      trace(TRACE_FULL, "[DBG-SW]   const[%zu] type=%d str=%s\n", i, (int)c.type(), cv.c_str());
    }
    break;
  }
  }
}

void QLangCompiler::compileExpr(const ExprPtr &e, int dest) {
  if (!e)
    throw QLangError(QLangErrorKind::Parse, 0, 0, "null expression");

  switch (e->kind) {
  case ExprKind::Literal:
    emit(Opcode::CONST, dest, emitConst(e->literal));
    break;
  case ExprKind::Variable: {
    // Bare static member name inside a method body of a class with statics:
    // read from the class-level `__statics__` table (shared across instances).
    if (!m_classStaticNames.empty() && m_classStaticNames.count(e->name)) {
      int staticsR = staticsTableReg();
      int keyR = tmpRegister();
      emit(Opcode::CONST, keyR, emitConst(QLangValue::String(e->name)));
      emit(Opcode::TABGET, dest, staticsR, keyR);
      break;
    }
    // Captured variable: load through the closure reference (live binding).
    auto cit = m_captureRegs.find(e->name);
    if (cit != m_captureRegs.end()) {
      emit(Opcode::REFLOAD, dest, cit->second, 0);
      break;
    }
    // Explicitly captured global var (declared with `global`): route through
    // m_captureRegs so it uses REFLOAD instead of MOV.
    if (m_capturedVars.count(e->name) && m_captureRegs.count(e->name)) {
      emit(Opcode::REFLOAD, dest, m_captureRegs[e->name], 0);
      break;
    }
    int r = regOf(e->name);
    if (r >= 0) {
      emit(Opcode::MOV, dest, r);
      break;
    }
    // Global const: emit a CONST instruction to load its value.
    if (m_constVars.count(e->name)) {
      // Global consts are captured via the closure mechanism. If we get here,
      // it means the const was not captured (e.g., used without being in scope).
      // This shouldn't happen for properly captured consts.
      throw QLangError(QLangErrorKind::Parse, 0, 0,
                       "use of undeclared variable '" + e->name + "'");
    }
    throw QLangError(QLangErrorKind::Parse, 0, 0,
                     "use of undeclared variable '" + e->name + "'");
    break;
  }
  case ExprKind::Binary:
  case ExprKind::Unary: {
    if (e->kind == ExprKind::Binary) {
      // `l in range(a, b)` -> (l >= a) && (l <= b): inclusive bounds check,
      // no array materialized (spec §range).
      if (e->op.text == "in") {
        ExprPtr ra, rb;
        if (isRangeCall(e->right, ra, rb)) {
          int l = tmpRegister();
          compileExpr(e->left, l);
          int aReg = tmpRegister();
          compileExpr(ra, aReg);
          int geReg = tmpRegister();
          emit(Opcode::GE, geReg, l, aReg);
          int bReg = tmpRegister();
          compileExpr(rb, bReg);
          int leReg = tmpRegister();
          emit(Opcode::LE, leReg, l, bReg);
          emit(Opcode::LAND, dest, geReg, leReg);
          break;
        }
      }
      int l = tmpRegister();
      compileExpr(e->left, l);
      int rr = tmpRegister();
      compileExpr(e->right, rr);
      emit(binOp(e->op.text), dest, l, rr);
    } else {
      int o = tmpRegister();
      compileExpr(e->operand, o);
      emit(unOp(e->uop.text), dest, o);
    }
    break;
  }
  case ExprKind::Call: {
    int argCount = static_cast<int>(e->args.size());
    trace(TRACE_FRAME, "[DBG-CALL] Entering call expr for '%s' with %d args\n", e->callee.c_str(), argCount);
    trace(TRACE_FULL, "[DBG-CALL] &m_code=%p m_code.size=%zu\n", (void*)&m_code, m_code.instructions().size());
    // Allocate the result slot first, then compile each arg into a temp
    // register. After all args are in place, move them to the VM's expected
    // arg slots (dest+1 .. dest+argCount) and emit CALL.
    int argBase = dest + 1;
    std::vector<int> argRegs(argCount);
    for (int i = 0; i < argCount; i++)
      argRegs[i] = tmpRegister();
    for (int i = 0; i < argCount; i++) {
      trace(TRACE_FULL, "[DBG-CALL] Compiling arg %d\n", i);
      compileExpr(e->args[i], argRegs[i]);
      trace(TRACE_FULL, "[DBG-CALL] After arg %d: m_code=%zu\n", i, m_code.instructions().size());
    }
    // Move each arg into the slot the VM reads from.
    for (int i = 0; i < argCount; i++)
      emit(Opcode::MOV, argBase + i, argRegs[i], 0);

    // Method call: obj.method(args) -> load the member (closure) first.
    if (e->calleeMember) {
      // Static method call: C::name(args). The closure lives in the class-level
      // `__statics__` table and is called with that table as a hidden first
      // argument (the anchor) — no self.
      if (e->calleeMember->scopeStatic) {
        if (e->calleeMember->memberBase->kind != ExprKind::Variable)
          throw QLangError(QLangErrorKind::Parse, e->callLine, e->callCol,
                           "'::' requires a class name on the left");
        const std::string &cname = e->calleeMember->memberBase->name;
        auto cit = m_classes.find(cname);
        if (cit == m_classes.end())
          throw QLangError(QLangErrorKind::Parse, e->callLine, e->callCol,
                           "'" + cname + "' is not a class");
        const ClassRecord &rec = cit->second;
        if (!rec.staticMethodFuncIndices.count(e->callee))
          throw QLangError(QLangErrorKind::Parse, e->callLine, e->callCol,
                           "'" + cname + "' has no static method '" +
                               e->callee + "'");
        int declaredArity = rec.staticMethodArities.at(e->callee);
        if (argCount != declaredArity)
          throw QLangError(
              QLangErrorKind::Parse, e->callLine, e->callCol,
              "static method '" + e->callee + "' expects " +
                  std::to_string(declaredArity) + " argument" +
                  (declaredArity == 1 ? "" : "s") + ", got " +
                  std::to_string(argCount));
        int baseR = tmpRegister();
        emit(Opcode::MOV, baseR, regOf("__proto_" + cname));
        int staticsR = tmpRegister();
        int skR = tmpRegister();
        emit(Opcode::CONST, skR, emitConst(QLangValue::String("__statics__")));
        emit(Opcode::TABGET, staticsR, baseR, skR);
        int keyR = tmpRegister();
        emit(Opcode::CONST, keyR, emitConst(QLangValue::String(e->callee)));
        int funcReg = tmpRegister();
        emit(Opcode::TABGET, funcReg, staticsR, keyR);
        // Shift user args down by one to make room for the anchor at argBase.
        for (int i = argCount - 1; i >= 0; i--)
          emit(Opcode::MOV, argBase + i + 1, argRegs[i], 0);
        int minReg = argBase + argCount + 1;
        if (m_nextReg < minReg) m_nextReg = minReg;
        emit(Opcode::MOV, argBase, staticsR, 0); // anchor = statics table
        emit(Opcode::MOV, dest, funcReg);
        emit(Opcode::CALLCLOS, dest, argCount + 1, argCount + 1);
        break;
      }
      // Prepend self only for known class methods (whose signature includes self).
      // Table-stored closures are called as-is.
      bool prependSelf = false;
      int declaredArity = -1; // param count incl. self; -1 = unknown
      for (const auto &kv : m_classes) {
        if (kv.second.methodFuncIndices.count(e->callee)) {
          prependSelf = true;
          auto ait = kv.second.methodArities.find(e->callee);
          if (ait != kv.second.methodArities.end())
            declaredArity = ait->second;
          break;
        }
      }
      // Fixed arity (Phase 4.2): a known class method must be called with
      // exactly (declared params - 1) arguments.
      if (prependSelf && declaredArity > 0 &&
          argCount + 1 != declaredArity) {
        int expected = declaredArity - 1;
        throw QLangError(
            QLangErrorKind::Parse, e->callLine, e->callCol,
            "method '" + e->callee + "' expects " +
            std::to_string(expected) + " argument" +
            (expected == 1 ? "" : "s") + ", got " +
            std::to_string(argCount));
      }
      if (prependSelf) {
        // Shift existing args down by one FIRST, before allocating new temps
        // that might collide with the shifted slots.
        for (int i = argCount - 1; i >= 0; i--)
          emit(Opcode::MOV, argBase + i + 1, argRegs[i], 0);
        // Advance past shifted slots so selfReg won't collide.
        int minReg = argBase + argCount + 1;
        if (m_nextReg < minReg) m_nextReg = minReg;
        // Now argBase is free. Compile base into a fresh temp.
        int selfReg = tmpRegister();
        compileExpr(e->calleeMember->memberBase, selfReg);
        emit(Opcode::MOV, argBase, selfReg, 0);
        int funcReg = tmpRegister();
        compileExpr(e->calleeMember, funcReg);
        emit(Opcode::MOV, dest, funcReg);
        emit(Opcode::CALLCLOS, dest, argCount + 1, argCount + 1);
      } else {
        int funcReg = tmpRegister();
        compileExpr(e->calleeMember, funcReg);
        emit(Opcode::MOV, dest, funcReg);
        emit(Opcode::CALLCLOS, dest, argCount, argCount);
      }
      break;
    }

    // Class constructor call: ClassName(args) -> create instance, copy methods,
    // call __init__(self, args...).
    auto cit = m_classes.find(e->callee);
    if (cit != m_classes.end()) {
      const ClassRecord &rec = cit->second;
      // Create instance table.
      int instanceReg = tmpRegister();
      emit(Opcode::TABCREATE, instanceReg);
      // Load the cached prototype table (created once at class definition).
      int protoReg = tmpRegister();
      emit(Opcode::MOV, protoReg, regOf("__proto_" + e->callee));
      // Copy methods from prototype to instance.
      for (const auto &mname : rec.methodNames) {
        int keyR = tmpRegister();
        emit(Opcode::CONST, keyR, emitConst(QLangValue::String(mname)));
        int valR = tmpRegister();
        emit(Opcode::TABGET, valR, protoReg, keyR);
        emit(Opcode::TABPUT, instanceReg, keyR, valR);
      }
      // Copy field defaults from prototype to instance (Phase 6): a
      // `v = 7` in the class body is an initial value applied at
      // construction, not merely a schema entry. Done before the metadata
      // writes below, like the method copies.
      for (const auto &fname : rec.fieldNames) {
        int keyR = tmpRegister();
        emit(Opcode::CONST, keyR, emitConst(QLangValue::String(fname)));
        int valR = tmpRegister();
        emit(Opcode::TABGET, valR, protoReg, keyR);
        emit(Opcode::TABPUT, instanceReg, keyR, valR);
      }
      // Static members (new extension): copy the shared `__statics__` table
      // from the prototype BY REFERENCE (it is a table value), so the instance
      // and every sibling mutate the same class-level map.
      if (!rec.staticFields.empty() || !rec.staticMethodFuncIndices.empty()) {
        int skeyR = tmpRegister();
        emit(Opcode::CONST, skeyR, emitConst(QLangValue::String("__statics__")));
        int svalR = tmpRegister();
        emit(Opcode::TABGET, svalR, protoReg, skeyR);
        emit(Opcode::TABPUT, instanceReg, skeyR, svalR);
      }
      // Closed field set (Phase 1.3): stamp the class name onto the instance
      // BEFORE installing the `__fields__` schema, so the instance is not yet
      // a "class instance" (enforcement key present) while metadata is written.
      {
        int ckeyR = tmpRegister();
        emit(Opcode::CONST, ckeyR, emitConst(QLangValue::String(kClassMetaKey)));
        int cvalR = tmpRegister();
        emit(Opcode::CONST, cvalR, emitConst(QLangValue::String(e->callee)));
        emit(Opcode::TABPUT, instanceReg, ckeyR, cvalR);
      }
      // Const fields (Phase 2): copy the `__const_fields__` set from the
      // prototype BEFORE installing `__fields__`, so the instance is not yet a
      // "class instance" (enforcement key present) while metadata is written —
      // setField would otherwise reject the non-declared metadata key.
      if (!rec.constFields.empty()) {
        int cfkeyR = tmpRegister();
        emit(Opcode::CONST, cfkeyR,
             emitConst(QLangValue::String(kConstFieldMetaKey)));
        int cfvalR = tmpRegister();
        emit(Opcode::TABGET, cfvalR, protoReg, cfkeyR);
        emit(Opcode::TABPUT, instanceReg, cfkeyR, cfvalR);
      }
      // Field types (Phase 5): copy the `__field_types__` table from the
      // prototype BEFORE installing `__fields__` (same ordering constraint as
      // the const-field metadata above).
      if (!rec.fieldTypes.empty()) {
        int ftkeyR = tmpRegister();
        emit(Opcode::CONST, ftkeyR,
             emitConst(QLangValue::String(kFieldTypeMetaKey)));
        int ftvalR = tmpRegister();
        emit(Opcode::TABGET, ftvalR, protoReg, ftkeyR);
        emit(Opcode::TABPUT, instanceReg, ftkeyR, ftvalR);
      }
      // Closed field set (Phase 1): copy the `__fields__` schema from the
      // prototype onto the instance LAST so GETFIELD/SETFIELD can enforce the
      // closed set. Installing `__fields__` is what makes the instance a
      // "class instance".
      if (!rec.fieldNames.empty()) {
        int fkeyR = tmpRegister();
        emit(Opcode::CONST, fkeyR, emitConst(QLangValue::String(kFieldMetaKey)));
        int fvalR = tmpRegister();
        emit(Opcode::TABGET, fvalR, protoReg, fkeyR);
        emit(Opcode::TABPUT, instanceReg, fkeyR, fvalR);
      }
      // Save instance to dest before overwriting instanceReg with args.
      emit(Opcode::MOV, dest, instanceReg);
      // Clear the temp so it does not keep a second reference to the
      // instance alive (Phase 7: the finalizer must fire when the user's
      // last reference goes away, not at frame end).
      {
        int nilC = m_code.addConstant(QLangValue());
        emit(Opcode::CONST, instanceReg, nilC, 0);
      }
      // Phase 7: attach the `__destroy__` finalizer (no-op if the class has
      // none). The deleter keeps the closure and owning program alive so the
      // method can run at final release, even after this run()'s VM is gone.
      emit(Opcode::TABFINALIZE, dest);
      // Prepend instance as first arg (self): move args into place,
      // then move saved instance as arg[0].
      int newArgCount = argCount + 1;
      int newArgBase = dest + 1;
      for (int i = argCount - 1; i >= 0; i--) {
        emit(Opcode::MOV, newArgBase + i + 1, argRegs[i], 0);
      }
      emit(Opcode::MOV, newArgBase, dest, 0);
      // Call __init__ (fixed arity, Phase 4.3/4.4).
      auto initIt = rec.methodFuncIndices.find("__init__");
      if (initIt == rec.methodFuncIndices.end()) {
        // Phase 4.4: no __init__ -> the constructor takes zero arguments.
        if (argCount != 0) {
          throw QLangError(
              QLangErrorKind::Parse, e->callLine, e->callCol,
              "class '" + e->callee + "' has no constructor; expected 0 "
              "argument" + (argCount == 1 ? "" : "s") + ", got " +
              std::to_string(argCount));
        }
        break;
      }
      // Phase 4.3: __init__ exists -> check arg count against its declared
      // arity (params include `self`, so expected = arity - 1).
      auto ait = rec.methodArities.find("__init__");
      if (ait != rec.methodArities.end()) {
        int expected = ait->second - 1;
        if (argCount != expected) {
          throw QLangError(
              QLangErrorKind::Parse, e->callLine, e->callCol,
              "constructor of '" + e->callee + "' expects " +
              std::to_string(expected) + " argument" +
              (expected == 1 ? "" : "s") + ", got " +
              std::to_string(argCount));
        }
      }
      int funcIdx = initIt->second;
      emit(Opcode::CALLFN, dest, funcIdx, newArgCount);
      // __init__ may return nil; instance was passed as self at newArgBase
      // and survives there (callee frame is separate).
      emit(Opcode::MOV, dest, newArgBase);
      // Release the caller-frame copies of the call args (self + ctor args)
      // so they do not extend the instance's lifetime to frame end (Phase 7
      // finalizers need the true final release; same pattern as the MAKEGEN
      // arg-register clearing above).
      {
        int nilC = m_code.addConstant(QLangValue());
        for (int j = 0; j < newArgCount; j++)
          emit(Opcode::CONST, newArgBase + j, nilC, 0);
      }
      break;
    }
    // User `def` function: funcIndex known at compile time.
    auto uit = m_userFuncs.find(e->callee);
    if (uit != m_userFuncs.end()) {
      // Generator functions: emit MAKEGEN to create a generator object,
      // not CALLFN which would run the generator inline.
      if (m_generatorFuncIndices.count(uit->second)) {
        int constIdx = m_generatorConstIndices[uit->second];
        emit(Opcode::MAKEGEN, dest, constIdx, argCount);
        // Clear arg regs so they don't leak into the next instruction.
        for (int j = 0; j < argCount; j++)
          emit(Opcode::CONST, argBase + j, m_code.addConstant(QLangValue()), 0);
      } else {
        emit(Opcode::CALLFN, dest, uit->second, argCount);
      }
      break;
    }
    // Arrow-closure variable: the closure value (with its funcIndex) is loaded
    // from the variable register and dispatched at runtime.
    auto cif = m_closureVars.find(e->callee);
    if (cif != m_closureVars.end()) {
      int cr = regOf(e->callee);
      emit(Opcode::MOV, dest, cr);
      emit(Opcode::CALLCLOS, dest, argCount, argCount);
      break;
    }
    // Generator variable: use CALLCLOS (runtime dispatch to resume generator).
    trace(TRACE_FULL, "[DBG-GENVAR] Call to '%s': in m_generatorVars? %d\n",
          e->callee.c_str(), (int)m_generatorVars.count(e->callee));
    if (m_generatorVars.count(e->callee)) {
      int cr = regOf(e->callee);
      emit(Opcode::MOV, dest, cr);
      emit(Opcode::CALLCLOS, dest, argCount, argCount);
      break;
    }
    // Built-in function.
    int funcId = builtinId(e->callee);
    // Native (C++) function — use negative index.
    if (funcId < 0) {
      auto nit = m_nativeFuncIds.find(e->callee);
      if (nit != m_nativeFuncIds.end()) {
        emit(Opcode::CALL, dest, nit->second, argCount);
        break;
      }
      // Not a user def, closure, generator var, builtin, or native/module
      // function: an unknown call must be a compile error, not a silent
      // negative-index CALL (which would dispatch into nativeFuncs[0]).
      throw QLangError(QLangErrorKind::Parse, e->callLine, e->callCol,
                       "unknown function '" + e->callee + "'");
    }
    emit(Opcode::CALL, dest, funcId, argCount);
    if (e->callee == "range") trace(TRACE_FULL, "[DBG-RANGE] emitted CALL dest=%d b=%d c=%d\n", dest, funcId, argCount);
    // Propagate source position for runtime error reporting.
    m_code.at(m_code.size() - 1).line = e->callLine;
    m_code.at(m_code.size() - 1).col = e->callCol;
    break;
  }
  case ExprKind::Array: {
    ArrayValue elems;
    elems.reserve(e->elements.size());
    for (auto &el : e->elements)
      elems.push_back(evalConstExpr(el));
    emit(Opcode::CONST, dest, emitConst(QLangValue::Array(std::move(elems))));
    break;
  }
  case ExprKind::Table: {
    // Check if all elements and keys are constant-evaluable.
    bool allConst = true;
    for (auto &item : e->items) {
      try {
        evalConstKey(item.first);
        evalConstExpr(item.second);
      } catch (...) {
        allConst = false;
        break;
      }
    }
    if (allConst) {
      TableValue kv = std::make_shared<std::map<std::string, QLangValue>>();
      for (auto &item : e->items)
        kv->operator[](evalConstKey(item.first)) = evalConstExpr(item.second);
      emit(Opcode::CONST, dest, emitConst(QLangValue::Table(kv)));
      break;
    }
    // Runtime table construction.
    int baseR = tmpRegister();
    emit(Opcode::TABCREATE, baseR);
    for (auto &item : e->items) {
      int keyR = tmpRegister();
      // Static identifier keys are Variable expressions - emit as string constant.
      // Dynamic keys ($expr) are compiled as expressions.
      if (item.first->kind == ExprKind::Variable)
        emit(Opcode::CONST, keyR, emitConst(QLangValue::String(item.first->name)));
      else
        compileExpr(item.first, keyR);
      int valR = tmpRegister();
      compileExpr(item.second, valR);
      emit(Opcode::TABPUT, baseR, keyR, valR);
    }
    emit(Opcode::MOV, dest, baseR);
    break;
  }
  case ExprKind::Closure: {
    // Anonymous arrow closure: build the function and emit MAKEFUNC.
    int funcIndex = buildFunction("closure", e->params, e->captures, e->body);
    auto closure = std::make_shared<Closure>();
    closure->funcIndex = funcIndex;
    for (auto &cap : e->captures) {
      int reg = regOf(cap);
      if (reg < 0)
        throw QLangError(QLangErrorKind::Parse, 0, 0,
                         "capture of undeclared variable '" + cap + "'");
      CaptureRef cr;
      cr.reg = reg;
      cr.frame = nullptr;
      closure->captures.push_back(cr);
    }
    int constIdx = emitConst(QLangValue::Function(closure));
    emit(Opcode::MAKEFUNC, dest, constIdx);
    break;
  }
  case ExprKind::Index: {
    // base[index] read -> ARRGET dest, baseR, idxR
    int baseR = tmpRegister();
    compileExpr(e->indexBase, baseR);
    int idxR = tmpRegister();
    compileExpr(e->indexExpr, idxR);
    emit(Opcode::ARRGET, dest, baseR, idxR);
    break;
  }
  case ExprKind::Member: {
    if (e->scopeStatic) {
      // C::name -> TABGET staticsR, C, "__statics__"; TABGET dest, staticsR,
      // name. The base must be a known class and the name one of its static
      // members (compile-time check).
      if (e->memberBase->kind != ExprKind::Variable)
        throw QLangError(QLangErrorKind::Parse, 0, 0,
                         "'::' requires a class name on the left");
      const std::string &cname = e->memberBase->name;
      auto cit = m_classes.find(cname);
      if (cit == m_classes.end())
        throw QLangError(QLangErrorKind::Parse, 0, 0,
                         "'" + cname + "' is not a class");
      const ClassRecord &rec = cit->second;
      if (!rec.staticFields.count(e->memberName) &&
          !rec.staticMethodFuncIndices.count(e->memberName))
        throw QLangError(QLangErrorKind::Parse, 0, 0,
                         "'" + cname + "' has no static member '" +
                             e->memberName + "'");
      int baseR = tmpRegister();
      emit(Opcode::MOV, baseR, regOf("__proto_" + cname));
      int staticsR = tmpRegister();
      int skR = tmpRegister();
      emit(Opcode::CONST, skR, emitConst(QLangValue::String("__statics__")));
      emit(Opcode::TABGET, staticsR, baseR, skR);
      int keyR = tmpRegister();
      emit(Opcode::CONST, keyR, emitConst(QLangValue::String(e->memberName)));
      emit(Opcode::TABGET, dest, staticsR, keyR);
      break;
    }
    // base.name read -> TABGET dest, baseR, keyR
    int baseR = tmpRegister();
    compileExpr(e->memberBase, baseR);
    int keyR = tmpRegister();
    emit(Opcode::CONST, keyR, emitConst(QLangValue::String(e->memberName)));
    emit(Opcode::TABGET, dest, baseR, keyR);
    break;
  }
  case ExprKind::Cast: {
    int src = tmpRegister();
    compileExpr(e->castOperand, src);
    emit(Opcode::CAST, dest, src, static_cast<int>(e->targetType));
    break;
  }
  case ExprKind::FString: {
    // f"literal ${expr} more" -> call format(template, expr1, expr2, ...)
    // CALL convention: args at dest+1..dest+argCount, result at dest.
    // So: reg[dest+1] = template, reg[dest+2..] = expressions.
    // Format specs are encoded into the template placeholder:
    //   no spec     -> %s
    //   ".2f"       -> %.2f
    //   "d"         -> %d
    std::string fmt;
    int exprCount = static_cast<int>(e->exprParts.size());
    for (int i = 0; i < exprCount; i++) {
      if (i < static_cast<int>(e->strParts.size()))
        fmt += e->strParts[i];

      // Build the printf placeholder from the format spec.
      std::string placeholder;
      if (i < static_cast<int>(e->formatSpecs.size())) {
        if (!e->formatSpecs[i].empty()) {
          // Python format spec -> printf spec.
          // ":.2f" -> "%.2f", ":d" -> "%d", ":f" -> "%f", ":10s" -> "%10s"
          const std::string &spec = e->formatSpecs[i];
          // Find where the conversion specifier starts (first alphabetic char).
          // e.g. ".2f" -> convStart=2, conversion="f", modifiers=".2"
          size_t convStart = 0;
          while (convStart < spec.size() && !std::isalpha(static_cast<unsigned char>(spec[convStart])))
            convStart++;
          if (convStart == spec.size()) {
            // No conversion specifier — treat as string.
            placeholder = "%s";
          } else {
            std::string conversion = spec.substr(convStart);
            std::string modifiers = spec.substr(0, convStart);
            // Handle width: ":10s" -> "%10s", ":>10" -> "%10s" (center-align not supported)
            int width = 0;
            bool leftAlign = false;
            bool zeroPad = false;
            bool altForm = false;
            size_t mi = 0;
            // Parse flags: #, 0, -, +, space
            while (mi < modifiers.size()) {
              char flag = modifiers[mi];
              if (flag == '#') {
                altForm = true;
                mi++;
              } else if (flag == '0' && width == 0) {
                // '0' is a zero-pad flag only when it appears before width digits
                zeroPad = true;
                mi++;
              } else if (flag == '-') {
                leftAlign = true;
                mi++;
              } else if (flag == '+' || flag == ' ') {
                // Sign flags — not used in placeholder generation but consume them
                mi++;
              } else {
                break;
              }
            }
            // Parse width digits
            while (mi < modifiers.size() && std::isdigit(static_cast<unsigned char>(modifiers[mi]))) {
              width = width * 10 + (modifiers[mi] - '0');
              mi++;
            }
            // Check for dot+precision (only for float/double)
            int precision = -1;
            if (mi < modifiers.size() && modifiers[mi] == '.') {
              mi++;
              precision = 0;
              while (mi < modifiers.size() && std::isdigit(static_cast<unsigned char>(modifiers[mi]))) {
                precision = precision * 10 + (modifiers[mi] - '0');
                mi++;
              }
            }
            // Build printf placeholder
            if (conversion == "f" || conversion == "e" || conversion == "g" ||
                conversion == "a" || conversion == "F" || conversion == "E" ||
                conversion == "G" || conversion == "A") {
              // Float: %[flags][width][.precision]f
              std::string flags;
              if (zeroPad) flags += '0';
              else if (!leftAlign && width > 0) flags += ' ';
              if (width > 0) {
                char wb[16];
                std::snprintf(wb, sizeof(wb), "%d", width);
                placeholder = "%" + flags + wb;
              } else {
                placeholder = "%" + flags;
              }
              if (precision >= 0) {
                char pb[16];
                std::snprintf(pb, sizeof(pb), ".%d", precision);
                placeholder += pb;
              }
              placeholder += conversion;
            } else if (conversion == "d" || conversion == "i" || conversion == "x" ||
                       conversion == "X" || conversion == "o" || conversion == "u") {
              // Integer: %[flags][width]d
              std::string flags;
              if (altForm) flags += '#';
              if (zeroPad && width > 0) flags += '0';
              else if (!leftAlign && width > 0) flags += ' ';
              if (width > 0) {
                char wb[16];
                std::snprintf(wb, sizeof(wb), "%d", width);
                placeholder = "%" + flags + wb + conversion;
              } else {
                placeholder = "%" + flags + conversion;
              }
            } else if (conversion == "s") {
              // String: %[flags][width]s
              std::string flags;
              if (!leftAlign && width > 0) flags += ' ';
              if (width > 0) {
                char wb[16];
                std::snprintf(wb, sizeof(wb), "%d", width);
                placeholder = "%" + flags + wb + "s";
              } else {
                placeholder = "%" + flags + "s";
              }
            } else {
              // Unknown conversion — fall back to %s
              placeholder = "%s";
            }
          }
        } else {
          // Empty format spec — default to %s.
          placeholder = "%s";
        }
      } else {
        placeholder = "%s";
      }
      fmt += placeholder;
    }
    if (static_cast<int>(e->strParts.size()) > exprCount)
      fmt += e->strParts.back();

    // Ensure enough registers: CALL reads dest+1..dest+argCount.
    int totalArgs = exprCount + 1;
    int maxReg = dest + totalArgs;
    while (m_nextReg <= maxReg)
      tmpRegister();

    // Compile each expression into temp registers.
    std::vector<int> argRegs(exprCount);
    for (int i = 0; i < exprCount; i++)
      argRegs[i] = tmpRegister();
    for (int i = 0; i < exprCount; i++)
      compileExpr(e->exprParts[i], argRegs[i]);

    // Layout: dest+1=template, dest+2..dest+totalArgs=expr args.
    int templateReg = dest + 1;
    int exprStartReg = dest + 2;

    emit(Opcode::CONST, templateReg, emitConst(QLangValue::String(fmt)));

    for (int i = 0; i < exprCount; i++)
      emit(Opcode::MOV, exprStartReg + i, argRegs[i], 0);

    emit(Opcode::CALL, dest, static_cast<int>(Builtin::Format), totalArgs);
    break;
  }
  }
}

int QLangCompiler::builtinId(const std::string &name) const {
  if (name == "toStr" || name == "str") return static_cast<int>(Builtin::ToStr);
  if (name == "toInt" || name == "int") return static_cast<int>(Builtin::ToInt);
  if (name == "type") return static_cast<int>(Builtin::Type);
  if (name == "print") return static_cast<int>(Builtin::Print);
  if (name == "println") return static_cast<int>(Builtin::Println);
  if (name == "printf") return static_cast<int>(Builtin::Printf);
  if (name == "len") return static_cast<int>(Builtin::Len);
  if (name == "blob") return static_cast<int>(Builtin::Blob);
  if (name == "substr") return static_cast<int>(Builtin::Substr);
  if (name == "slice") return static_cast<int>(Builtin::Slice);
  if (name == "range") return static_cast<int>(Builtin::Range);
  if (name == "join") return static_cast<int>(Builtin::Join);
  if (name == "isset") return static_cast<int>(Builtin::IsSet);
  if (name == "sprintf") return static_cast<int>(Builtin::Sprintf);
  if (name == "format") return static_cast<int>(Builtin::Format);
  if (name == "assert") return static_cast<int>(Builtin::Assert);
  if (name == "next") return static_cast<int>(Builtin::Next);
  if (name == "set_max_steps") return static_cast<int>(Builtin::SetMaxSteps);
  if (name == "get_max_steps") return static_cast<int>(Builtin::GetMaxSteps);
  return -1;
}

QLangValue QLangCompiler::evalConstExpr(const ExprPtr &e) {
  if (!e)
    throw QLangError(QLangErrorKind::Parse, 0, 0, "null array/table element");
  switch (e->kind) {
  case ExprKind::Literal:
    return e->literal;
  case ExprKind::Array: {
    ArrayValue elems;
    for (auto &el : e->elements)
      elems.push_back(evalConstExpr(el));
    return QLangValue::Array(std::move(elems));
  }
  case ExprKind::Table: {
    TableValue kv = std::make_shared<std::map<std::string, QLangValue>>();
    for (auto &item : e->items)
      kv->operator[](evalConstKey(item.first)) = evalConstExpr(item.second);
    return QLangValue::Table(kv);
  }
  default:
    throw QLangError(QLangErrorKind::Parse, 0, 0,
                     "non-constant element in array/table literal");
  }
}

QLangValue::Type QLangCompiler::inferType(const ExprPtr &e) const {
  if (!e)
    return QLangValue::Type::Nil;
  switch (e->kind) {
  case ExprKind::Array: return QLangValue::Type::Array;
  case ExprKind::Table: return QLangValue::Type::Table;
  case ExprKind::Literal: return e->literal.type();
  case ExprKind::Variable: {
    auto it = m_varTypes.find(e->name);
    return it == m_varTypes.end() ? QLangValue::Type::Nil : it->second;
  }
  default: return QLangValue::Type::Nil;
  }
}

void QLangCompiler::updateVarType(const std::string &name, const ExprPtr &e) {
  if (!e)
    return;
  QLangValue::Type t = inferType(e);
  if (t == QLangValue::Type::Array || t == QLangValue::Type::Table)
    m_varTypes[name] = t;
}

void QLangCompiler::compileAssignTarget(const ExprPtr &target, const Token &op,
                                        const ExprPtr &value) {
  const bool isCompound = (op.text != "=");
  if (target->kind == ExprKind::Index) {
    // base is a variable; use its register directly so the mutation persists.
    const std::string &baseName = target->indexBase->name;
    int baseR;
    // Base may be a captured variable: load it through the reference so the
    // read-modify-write mutates the shared (enclosing) array.
    auto cit = m_captureRegs.find(baseName);
    if (cit != m_captureRegs.end()) {
      baseR = tmpRegister();
      emit(Opcode::REFLOAD, baseR, cit->second, 0);
    } else {
      baseR = regOf(baseName);
      if (baseR < 0)
        baseR = declareVar(baseName); // bare `ab[1]=5` auto-declares ab
    }
    int idxR = tmpRegister();
    if (target->indexExpr)
      compileExpr(target->indexExpr, idxR);
    else
      emit(Opcode::CONST, idxR, emitConst(QLangValue::Int(0))); // ab[] -> append
    int valR = tmpRegister();
    compileExpr(value, valR);
    if (target->indexExpr) {
      // read-modify-write for compound ops (e.g. a[i] += v)
      int tmpR = tmpRegister();
      emit(Opcode::ARRGET, tmpR, baseR, idxR);
      Opcode opCode = (op.text == "++") ? Opcode::INC
                      : (op.text == "--") ? Opcode::DEC
                                          : binOp(op.text.substr(0, op.text.size() - 1));
      emit(opCode, tmpR, tmpR, valR);
      emit(Opcode::ARRPUT, baseR, idxR, tmpR);
    } else {
      // empty subscript ab[] = v -> append
      emit(Opcode::ARRAPP, baseR, valR, 0);
    }
  } else if (target->kind == ExprKind::Member) {
    const std::string &baseName = target->memberBase->name;
    int baseR;
    auto cit = m_captureRegs.find(baseName);
    if (cit != m_captureRegs.end()) {
      baseR = tmpRegister();
      emit(Opcode::REFLOAD, baseR, cit->second, 0);
    } else {
      baseR = regOf(baseName);
      if (baseR < 0)
        baseR = declareVar(baseName);
    }
    int keyR = tmpRegister();
    emit(Opcode::CONST, keyR, emitConst(QLangValue::String(target->memberName)));
    int valR = tmpRegister();
    compileExpr(value, valR);
    if (isCompound) {
      int tmpR = tmpRegister();
      emit(Opcode::TABGET, tmpR, baseR, keyR);
      Opcode opCode = (op.text == "++") ? Opcode::INC
                      : (op.text == "--") ? Opcode::DEC
                                          : binOp(op.text.substr(0, op.text.size() - 1));
      emit(opCode, tmpR, tmpR, valR);
      emit(Opcode::TABPUT, baseR, keyR, tmpR);
    } else {
      emit(Opcode::TABPUT, baseR, keyR, valR);
    }
  }
}

Opcode QLangCompiler::binOp(const std::string &text) const {
  if (text == "==") return Opcode::EQ;
  if (text == "!=") return Opcode::NE;
  if (text == "<") return Opcode::LT;
  if (text == ">") return Opcode::GT;
  if (text == "<=") return Opcode::LE;
  if (text == ">=") return Opcode::GE;
  if (text == "+") return Opcode::ADD;
  if (text == "-") return Opcode::SUB;
  if (text == "*") return Opcode::MUL;
  if (text == "/") return Opcode::DIV;
  if (text == "&") return Opcode::AND;
  if (text == "|") return Opcode::OR;
  if (text == "^") return Opcode::XOR;
  if (text == "<<") return Opcode::SHL;
  if (text == ">>") return Opcode::SHR;
  if (text == "&&") return Opcode::LAND;
  if (text == "|||" || text == "||") return Opcode::LOR;
  if (text == "**") return Opcode::POW;
  if (text == "%") return Opcode::MOD;
  if (text == "in") return Opcode::IN;
  throw QLangError(QLangErrorKind::Parse, 0, 0, "unsupported operator '" + text + "'");
}

Opcode QLangCompiler::unOp(const std::string &text) const {
  if (text == "--") return Opcode::DEC;
  if (text == "++") return Opcode::INC;
  if (text == "-") return Opcode::NEG;
  if (text == "!") return Opcode::NOT;
  if (text == "$") return Opcode::EVALKEY;
  throw QLangError(QLangErrorKind::Parse, 0, 0, "unsupported unary operator '" + text + "'");
}

void QLangCompiler::guardNotConst(const std::string &name,
                                  const char *op) const {
  if (m_constVars.count(name))
    throw QLangError(QLangErrorKind::Parse, 0, 0,
                     std::string("cannot ") + op + " to constant '" + name + "'");
}

bool QLangCompiler::containsYield(const StmtPtr &s) {
  if (!s)
    return false;
  if (s->kind == StmtKind::Yield)
    return true;
  // Recurse into block-bearing statement types.
  switch (s->kind) {
  case StmtKind::For:
    for (auto &st : s->body)
      if (containsYield(st))
        return true;
    return false;
  case StmtKind::ForIn:
    for (auto &st : s->body)
      if (containsYield(st))
        return true;
    return false;
  case StmtKind::While:
    for (auto &st : s->whileBody)
      if (containsYield(st))
        return true;
    return false;
  case StmtKind::If:
    for (auto &st : s->ifBody)
      if (containsYield(st))
        return true;
    for (auto &st : s->elseBody)
      if (containsYield(st))
        return true;
    return false;
  case StmtKind::Def:
    for (auto &st : s->body)
      if (containsYield(st))
        return true;
    return false;
  case StmtKind::Arrow:
    for (auto &st : s->body)
      if (containsYield(st))
        return true;
    return false;
  case StmtKind::Switch:
    for (auto &c : s->cases)
      for (auto &st : c.body)
        if (containsYield(st))
          return true;
    for (auto &st : s->defaultBody)
      if (containsYield(st))
        return true;
    return false;
  default:
    return false;
  }
}

} // namespace qlang
