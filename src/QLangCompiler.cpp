#include "qlang/QLangCompiler.h"

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
  return m_code.addConstant(value);
}

void QLangCompiler::emit(Opcode op, int a, int b, int c) {
  m_code.add(op, a, b, c);
}

CompiledProgram QLangCompiler::compile() {
  for (auto &s : m_program) {
    compileStmt(s);
  }
  CompiledProgram out;
  out.code = std::move(m_code);
  out.vars = std::move(m_regs);
  out.registerCount = m_nextReg;
  return out;
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

    for (auto &body : s->body)
      compileStmt(body);

    emit(Opcode::INC, x, x);
    emit(Opcode::JMP, testLabel);
    m_code.instructions()[jumpFalseIdx].b = static_cast<int>(m_code.size());
    break;
  }
  case StmtKind::While: {
    int testLabel = static_cast<int>(m_code.size());
    int condReg = tmpRegister();
    compileExpr(s->cond, condReg);
    emit(Opcode::JMPIFFALSE, condReg, 0);
    int jumpFalseIdx = static_cast<int>(m_code.size()) - 1;

    for (auto &body : s->whileBody)
      compileStmt(body);

    emit(Opcode::JMP, testLabel);
    m_code.instructions()[jumpFalseIdx].b = static_cast<int>(m_code.size());
    break;
  }
  case StmtKind::If: {
    int condReg = tmpRegister();
    compileExpr(s->ifCond, condReg);
    emit(Opcode::JMPIFFALSE, condReg, 0);
    int jumpFalseIdx = static_cast<int>(m_code.size()) - 1;

    for (auto &body : s->ifBody)
      compileStmt(body);

    m_code.instructions()[jumpFalseIdx].b = static_cast<int>(m_code.size());
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
    int funcId = builtinId(e->callee);
    int argCount = static_cast<int>(e->args.size());
    std::vector<int> argRegs(argCount);
    for (int i = 0; i < argCount; i++)
      argRegs[i] = tmpRegister();
    for (int i = 0; i < argCount; i++)
      compileExpr(e->args[i], argRegs[i]);
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
    TableValue kv;
    for (auto &item : e->items)
      kv[item.first] = evalConstExpr(item.second);
    emit(Opcode::CONST, dest, emitConst(QLangValue::Table(std::move(kv))));
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
  if (name == "toStr") return static_cast<int>(Builtin::ToStr);
  if (name == "toInt") return static_cast<int>(Builtin::ToInt);
  if (name == "type") return static_cast<int>(Builtin::Type);
  if (name == "print") return static_cast<int>(Builtin::Print);
  if (name == "println") return static_cast<int>(Builtin::Println);
  if (name == "printf") return static_cast<int>(Builtin::Printf);
  if (name == "len") return static_cast<int>(Builtin::Len);
  if (name == "blob") return static_cast<int>(Builtin::Blob);
  throw QLangError(QLangErrorKind::Parse, 0, 0, "unknown function '" + name + "'");
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
    int baseR = regOf(baseName);
    if (baseR < 0)
      baseR = declareVar(baseName); // bare `ab[1]=5` auto-declares ab
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
    int baseR = regOf(baseName);
    if (baseR < 0)
      baseR = declareVar(baseName);
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
  throw QLangError(QLangErrorKind::Parse, 0, 0, "unsupported operator '" + text + "'");
}

Opcode QLangCompiler::unOp(const std::string &text) const {
  if (text == "--") return Opcode::DEC;
  if (text == "++") return Opcode::INC;
  if (text == "-") return Opcode::NEG;
  throw QLangError(QLangErrorKind::Parse, 0, 0, "unsupported unary operator '" + text + "'");
}

void QLangCompiler::guardNotConst(const std::string &name,
                                  const char *op) const {
  if (m_constVars.count(name))
    throw QLangError(QLangErrorKind::Parse, 0, 0,
                     std::string("cannot ") + op + " to constant '" + name + "'");
}

} // namespace qlang
