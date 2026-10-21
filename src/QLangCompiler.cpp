#include "qlang/QLangCompiler.h"

#include <stdexcept>

#include "qlang/QLangErrors.h"

namespace qlang {

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
    compileExpr(s->value, t);
    emit(Opcode::MOV, r, t);
    // Enforce a typed declaration (e.g. local b:byte = 32).
    if (s->declaredType)
      emit(Opcode::CONV, r, r, static_cast<int>(*s->declaredType));
    break;
  }
  case StmtKind::Assignment: {
    int r = regOf(s->name);
    if (r < 0)
      throw QLangError(QLangErrorKind::Parse, 0, 0,
                       "assignment to undeclared variable '" + s->name + "'");
    const std::string &op = s->assignOp.text;
    if (op == "=") {
      int t = tmpRegister();
      compileExpr(s->assignValue, t);
      emit(Opcode::MOV, r, t);
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
  if (text == "||") return Opcode::LOR;
  throw QLangError(QLangErrorKind::Parse, 0, 0, "unsupported operator '" + text + "'");
}

Opcode QLangCompiler::unOp(const std::string &text) const {
  if (text == "--") return Opcode::DEC;
  if (text == "++") return Opcode::INC;
  if (text == "-") return Opcode::NEG;
  throw QLangError(QLangErrorKind::Parse, 0, 0, "unsupported unary operator '" + text + "'");
}

} // namespace qlang
