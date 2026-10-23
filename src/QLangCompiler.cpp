#include "qlang/QLangCompiler.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

#include "qlang/QLangErrors.h"

namespace qlang {

// True when an expression is a compile-time array literal (used to decide
// array-append vs. array-extend for `arr += rhs`).
static bool isArrayExpr(const ExprPtr &e) {
  return e && e->kind == ExprKind::Array;
}

QLangCompiler::QLangCompiler(std::vector<StmtPtr> program)
    : m_program(std::move(program)) {}

void QLangCompiler::addExternalConsts(const std::unordered_set<std::string> &names) {
  m_constVars.insert(names.begin(), names.end());
}

void QLangCompiler::addExternalFuncs(const std::unordered_set<std::string> &names) {
  int idx = -1;
  for (const auto &name : names) {
    m_nativeFuncIds[name] = idx--;
  }
}

int QLangCompiler::newRegister() { return m_nextReg++; }
int QLangCompiler::tmpRegister() { return newRegister(); }

int QLangCompiler::declareVar(const std::string &name) {
  auto it = m_regs.find(name);
  if (it != m_regs.end())
    return it->second;
  int r = newRegister();
  m_regs[name] = r;
  return r;
}

int QLangCompiler::regOf(const std::string &name) const {
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
  fprintf(stderr, "[DBG-CONST] Adding constant type=%d value=%s -> index=%d (pool size=%zu)\n",
          (int)value.type(), valStr.c_str(), idx, m_code.constants().size());
  return idx;
}

void QLangCompiler::emit(Opcode op, int a, int b, int c) {
  m_code.add(op, a, b, c);
}

CompiledProgram QLangCompiler::compile() {
// Compile the top-level program into the main (scratch) function context.
for (auto &s : m_program) {
  compileStmt(s, nullptr);
}
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
  std::unordered_set<std::string> paramSet(params.begin(), params.end());

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
  int savedNextReg = m_nextReg;
  bool savedInFunction = m_inFunction;

  // Fresh function context: params occupy registers 0..paramCount-1 and must
  // reserve those registers so temporaries never collide with a parameter.
  m_code = Bytecode();
  m_regs.clear();
  // Keep m_constVars and m_userFuncs populated so global consts and
  // user-defined functions remain visible inside function bodies.
  // m_varTypes is cleared to avoid stale type info from the outer scope.
  m_varTypes.clear();
  m_closureVars.clear();
  m_captureRegs.clear();
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

  // Implicit `return nil`.
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
  m_nextReg = savedNextReg;
  m_inFunction = savedInFunction;

  return funcIndex;
}

void QLangCompiler::compileStmt(const StmtPtr &s,
                                std::vector<std::string> *captures) {
  if (!s)
    return;
  switch (s->kind) {
  case StmtKind::Declaration: {
    int r = declareVar(s->name);
    int t = tmpRegister();
    if (s->value)
      compileExpr(s->value, t);
    else
      emit(Opcode::CONST, t, emitConst(QLangValue())); // `local z` -> nil
    emit(Opcode::MOV, r, t);
    updateVarType(s->name, s->value);
    // Enforce a typed declaration (e.g. local b:byte = 32).
    if (s->declaredType)
      emit(Opcode::CONV, r, r, static_cast<int>(*s->declaredType));
    // Track top-level variable declarations as globals.
    if (!m_inFunction)
      m_globalVars.insert(s->name);
    break;
  }
  case StmtKind::Const: {
    int r = declareVar(s->name);
    m_constVars.insert(s->name);
    int t = tmpRegister();
    if (s->value)
      compileExpr(s->value, t);
    else
      emit(Opcode::CONST, t, emitConst(QLangValue()));
    emit(Opcode::MOV, r, t);
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
    int startReg = tmpRegister();
    compileExpr(s->forStart, startReg);
    emit(Opcode::MOV, x, startReg);

    int endReg = tmpRegister();
    compileExpr(s->forEnd, endReg);

    int testLabel = static_cast<int>(m_code.size());
    int cmpReg = tmpRegister();
    emit(Opcode::LE, cmpReg, x, endReg); // continue while x <= end
    emit(Opcode::JMPIFFALSE, cmpReg, 0); // target fixed up below
    int jumpFalseIdx = static_cast<int>(m_code.size()) - 1;

    // Body jump targets (continue -> increment, break -> exit) live AFTER the
    // body, so patch the CONTINUE/BREAK instructions emitted inside the body
    // once we know their addresses.
    m_loops.push_back({0, 0});
    int bodyStart = static_cast<int>(m_code.instructions().size());
    for (auto &body : s->body)
      compileStmt(body);
    int bodyEnd = static_cast<int>(m_code.instructions().size());

    int incLabel = bodyEnd;
    emit(Opcode::INC, x, x);
    emit(Opcode::JMP, testLabel);
    int exitLabel = static_cast<int>(m_code.size());
    m_code.instructions()[jumpFalseIdx].b = exitLabel;
    for (int j = bodyStart; j < bodyEnd; j++) {
      auto &ci = m_code.instructions()[j];
      if (ci.op == Opcode::CONTINUE) ci.a = incLabel;
      else if (ci.op == Opcode::BREAK) ci.a = exitLabel;
    }
    m_loops.pop_back();
    break;
  }
  case StmtKind::ForIn: {
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
    for (auto &body : s->whileBody)
      compileStmt(body);
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

    for (auto &body : s->ifBody)
      compileStmt(body);

    if (!s->elseBody.empty()) {
      // Emit JMP after if body to skip else body; patch target after else body compiles.
      emit(Opcode::JMP, 0);
      int jumpPastIdx = static_cast<int>(m_code.size()) - 1;
      int elseBodyStart = static_cast<int>(m_code.size());
      for (auto &body : s->elseBody)
        compileStmt(body);
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
    int funcIndex = buildFunction(s->funcName, s->params, {}, s->body);
    m_userFuncs[s->funcName] = funcIndex;
    break;
  }
  case StmtKind::Arrow: {
    // `local name = (params)[captures] { ... }`: an arrow-closure value.
    int funcIndex = buildFunction(s->name, s->params, s->captures, s->body);
    m_closureVars.insert(s->name);
    m_varTypes[s->name] = QLangValue::Type::Function;
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
      for (auto &body : sc.body) {
        fprintf(stderr, "[DBG-CASE] Case %zu body stmt kind=%d\n", i, (int)body->kind);
        if (body->kind == StmtKind::Assignment) {
          fprintf(stderr, "[DBG-CASE]   Assign op=%s value=", body->assignOp.text.c_str());
          if (body->assignValue && body->assignValue->kind == ExprKind::Literal)
            fprintf(stderr, "Literal:%d (type=%d)\n", (int)body->assignValue->literal.asInt(), (int)body->assignValue->literal.type());
          else
            fprintf(stderr, "non-literal\n");
        }
        compileStmt(body);
      }
      emit(Opcode::JMP, 0); // implicit exit: patched after default compiles
      caseExitJmpIdx.push_back(static_cast<int>(m_code.size()) - 1);
    }

    // Compile the default body (if any).
    int defaultBodyStart = static_cast<int>(m_code.size());
    for (auto &db : s->defaultBody)
      compileStmt(db);
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
    fprintf(stderr, "[DBG-SW] switchEnd=%d instructions.size()=%zu\n", switchEnd, m_code.instructions().size());
    for (size_t i = 0; i < m_code.instructions().size(); i++) {
      auto &inst = m_code.instructions()[i];
      fprintf(stderr, "[DBG-SW]   [%zu] op=%d a=%d b=%d c=%d\n", i, (int)inst.op, inst.a, inst.b, inst.c);
    }
    fprintf(stderr, "[DBG-SW] constants.size()=%zu\n", m_code.constants().size());
    for (size_t i = 0; i < m_code.constants().size(); i++) {
      const auto &c = m_code.constants()[i];
      std::string cv;
      if (c.type() == QLangValue::Type::Int)
        cv = std::to_string(c.asInt());
      else if (c.type() == QLangValue::Type::String)
        cv = c.asString();
      else
        cv = std::string("(type=") + std::to_string((int)c.type()) + ")";
      fprintf(stderr, "[DBG-SW]   const[%zu] type=%d str=%s\n", i, (int)c.type(), cv.c_str());
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
    // Allocate the result slot first, then compile each arg into a temp
    // register. After all args are in place, move them to the VM's expected
    // arg slots (dest+1 .. dest+argCount) and emit CALL.
    int argBase = dest + 1;
    std::vector<int> argRegs(argCount);
    for (int i = 0; i < argCount; i++)
      argRegs[i] = tmpRegister();
    for (int i = 0; i < argCount; i++)
      compileExpr(e->args[i], argRegs[i]);
    // Move each arg into the slot the VM reads from.
    for (int i = 0; i < argCount; i++)
      emit(Opcode::MOV, argBase + i, argRegs[i], 0);

    // Method call: obj.method(args) -> load the member (closure) first.
    if (e->calleeMember) {
      int funcReg = tmpRegister();
      compileExpr(e->calleeMember, funcReg);
      emit(Opcode::MOV, dest, funcReg);
      emit(Opcode::CALLCLOS, dest, argCount, argCount);
      break;
    }

    // User `def` function: funcIndex known at compile time.
    auto uit = m_userFuncs.find(e->callee);
    if (uit != m_userFuncs.end()) {
      emit(Opcode::CALLFN, dest, uit->second, argCount);
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
    // Built-in function.
    int funcId = builtinId(e->callee);
    // Native (C++) function — use negative index.
    if (funcId < 0) {
      auto nit = m_nativeFuncIds.find(e->callee);
      if (nit != m_nativeFuncIds.end()) {
        emit(Opcode::CALL, dest, nit->second, argCount);
        break;
      }
    }
    emit(Opcode::CALL, dest, funcId, argCount);
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
    // Check if all elements are constant-evaluable.
    bool allConst = true;
    for (auto &item : e->items) {
      try {
        evalConstExpr(item.second);
      } catch (...) {
        allConst = false;
        break;
      }
    }
    if (allConst) {
      TableValue kv;
      for (auto &item : e->items)
        kv[item.first] = evalConstExpr(item.second);
      emit(Opcode::CONST, dest, emitConst(QLangValue::Table(std::move(kv))));
      break;
    }
    // Runtime table construction.
    int baseR = tmpRegister();
    emit(Opcode::TABCREATE, baseR);
    for (auto &item : e->items) {
      int keyR = tmpRegister();
      emit(Opcode::CONST, keyR, emitConst(QLangValue::String(item.first)));
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
    TableValue kv;
    for (auto &item : e->items)
      kv[item.first] = evalConstExpr(item.second);
    return QLangValue::Table(std::move(kv));
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
  throw QLangError(QLangErrorKind::Parse, 0, 0, "unsupported unary operator '" + text + "'");
}

void QLangCompiler::guardNotConst(const std::string &name,
                                  const char *op) const {
  if (m_constVars.count(name))
    throw QLangError(QLangErrorKind::Parse, 0, 0,
                     std::string("cannot ") + op + " to constant '" + name + "'");
}

} // namespace qlang
