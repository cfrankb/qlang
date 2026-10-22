#include "qlang/QLangCompiler.h"

#include <algorithm>
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
  fprintf(stderr, "[DBG-C] m_program.size()=%zu\n", m_program.size());
  // Compile the top-level program into the main (scratch) function context.
  for (auto &s : m_program) {
    compileStmt(s);
  }
  // Implicit `return nil` at the end of the main program.
  emit(Opcode::RETURN, 0);
  fprintf(stderr, "[DBG-C] after compile: m_code.instructions().size()=%zu m_nextReg=%d\n", m_code.instructions().size(), m_nextReg);

  // Wrap the main scratch stream into functions[0].
  Bytecode mainScratch = std::move(m_code);
  fprintf(stderr, "[DBG-C] mainScratch.instructions().size()=%zu\n", mainScratch.instructions().size());
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
                                 const std::vector<std::string> &captures,
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
  auto savedConstVars = m_constVars;
  auto savedVarTypes = m_varTypes;
  auto savedUserFuncs = m_userFuncs;
  auto savedClosureVars = m_closureVars;
  auto savedCaptureRegs = m_captureRegs;
  int savedNextReg = m_nextReg;

  // Fresh function context: params occupy registers 0..paramCount-1 and must
  // reserve those registers so temporaries never collide with a parameter.
  m_code = Bytecode();
  m_regs.clear();
  m_constVars.clear();
  m_varTypes.clear();
  m_userFuncs.clear();
  m_closureVars.clear();
  m_captureRegs.clear();
  m_labels.clear();
  m_nextReg = 0;
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
    // This is a free variable — capture it.
    if (std::find(updatedCaptures.begin(), updatedCaptures.end(), id) == updatedCaptures.end())
      updatedCaptures.push_back(id);
  }

  // Register the explicit capture list (name -> refId).
  for (size_t i = 0; i < updatedCaptures.size(); i++)
    m_captureRegs[updatedCaptures[i]] = static_cast<int>(i);

  // Compile the body.
  for (auto &s : body)
    compileStmt(s);

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
    fn.captures[i].reg = m_captureRegs[updatedCaptures[i]];
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
  m_nextReg = savedNextReg;

  return funcIndex;
}

void QLangCompiler::compileStmt(const StmtPtr &s) {
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
    if (r < 0)
      r = declareVar(s->name); // bare assignment auto-declares an implicit local
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
      fprintf(stderr, "[DBG-ASSIGN] Compiling assign to '%s' value kind=%d\n", s->name.c_str(), s->assignValue ? (int)s->assignValue->kind : -1);
      if (s->assignValue && s->assignValue->kind == ExprKind::Literal && s->assignValue->literal.type() != QLangValue::Type::String)
        fprintf(stderr, "[DBG-ASSIGN]   Literal value=%lld\n", (long long)s->assignValue->literal.asInt());
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
    int r = regOf(e->name);
    if (r < 0)
      throw QLangError(QLangErrorKind::Parse, 0, 0,
                       "use of undeclared variable '" + e->name + "'");
    emit(Opcode::MOV, dest, r);
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
    emit(Opcode::CALL, dest, funcId, argCount);
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
