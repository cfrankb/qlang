#include "qlang/QLangParser.h"

#include <stdexcept>

#include "qlang/QLangErrors.h"

namespace qlang {

// ---------------------------------------------------------------------------
// Construction / token stream helpers
// ---------------------------------------------------------------------------

QLangParser::QLangParser(std::vector<Token> tokens)
    : m_tokens(std::move(tokens)) {}

bool QLangParser::atEnd() const {
  return current().type == TokenType::EndOfFile;
}

const Token &QLangParser::peek(int offset) const {
  size_t p = m_pos + static_cast<size_t>(offset);
  if (p >= m_tokens.size()) return m_tokens.back();
  return m_tokens[p];
}

const Token &QLangParser::current() const { return m_tokens[m_pos]; }

bool QLangParser::check(TokenType t) const { return current().type == t; }

bool QLangParser::match(TokenType t) {
  if (check(t)) {
    m_pos++;
    return true;
  }
  return false;
}

const Token &QLangParser::consume(TokenType t, const char *what) {
  if (!check(t)) {
    const Token &cur = current();
    throw QLangError(QLangErrorKind::Parse, cur.line, cur.col,
                     std::string("expected ") + what + " but found '" +
                     cur.text + "'");
  }
  const Token &result = m_tokens[m_pos];
  m_pos++;
  return result;
}

// ---------------------------------------------------------------------------
// Precedence
// ---------------------------------------------------------------------------

static int precedence(TokenType t) {
  switch (t) {
  case TokenType::LogicalOr: return 1;
  case TokenType::LogicalAnd: return 2;
  case TokenType::Or: return 3;
  case TokenType::Xor: return 4;
  case TokenType::And: return 5;
  case TokenType::Equal:
  case TokenType::NotEqual: return 6;
  case TokenType::Less:
  case TokenType::Greater:
  case TokenType::LessEqual:
  case TokenType::GreaterEqual: return 7;
  case TokenType::ShiftLeft:
  case TokenType::ShiftRight: return 8;
  case TokenType::Plus:
  case TokenType::Minus: return 9;
  case TokenType::Star:
  case TokenType::Slash: return 10;
  default: return -1;
  }
}

// ---------------------------------------------------------------------------
// Program / statements
// ---------------------------------------------------------------------------

std::vector<StmtPtr> QLangParser::parseProgram() {
  std::vector<StmtPtr> program;
  while (!atEnd()) {
    program.push_back(parseStatement());
  }
  return program;
}

StmtPtr QLangParser::parseStatement() {
  if (check(TokenType::Local)) return parseDeclaration();
  if (check(TokenType::For)) return parseFor();
  if (check(TokenType::While)) return parseWhile();
  if (check(TokenType::If)) return parseIf();

  // Expression statement or assignment.
  if (check(TokenType::Ident)) {
    const Token &idt = current();
    size_t save = m_pos;
    m_pos++; // consume the identifier
    TokenType nxt = current().type;
    bool isAssign =
        nxt == TokenType::Assign || nxt == TokenType::Plus ||
        nxt == TokenType::Minus || nxt == TokenType::Star ||
        nxt == TokenType::Slash || nxt == TokenType::And || nxt == TokenType::Or ||
        nxt == TokenType::Xor || nxt == TokenType::ShiftLeft ||
        nxt == TokenType::ShiftRight || nxt == TokenType::Increment ||
        nxt == TokenType::Decrement;
    if (isAssign) {
      auto stmt = std::make_shared<Stmt>();
      stmt->kind = StmtKind::Assignment;
      stmt->name = idt.value;
      stmt->assignOp = current();
      m_pos++; // consume the operator
      stmt->assignValue = parseExpression();
      match(TokenType::Semicolon);
      return stmt;
    }
    // Not an assignment: rewind and parse an expression statement.
    m_pos = save;
    auto expr = parseExpression();
    match(TokenType::Semicolon);
    auto stmt = std::make_shared<Stmt>();
    stmt->kind = StmtKind::ExprStmt;
    stmt->value = expr;
    return stmt;
  }

  const Token &cur = current();
  throw QLangError(QLangErrorKind::Parse, cur.line, cur.col,
                   std::string("unexpected token '") + cur.text +
                       "' at start of statement");
}

StmtPtr QLangParser::parseDeclaration() {
  consume(TokenType::Local, "'local'");
  auto stmt = std::make_shared<Stmt>();
  stmt->kind = StmtKind::Declaration;
  stmt->name = consume(TokenType::Ident, "variable name").value;

  // Optional typed declaration: local b:byte = 32
  if (match(TokenType::Colon)) {
    TokenType tt = current().type;
    if (tt >= TokenType::TypeByte && tt <= TokenType::TypeArray) {
      stmt->declaredType = static_cast<QLangValue::Type>(
          static_cast<int>(tt) - static_cast<int>(TokenType::TypeByte) +
          static_cast<int>(QLangValue::Type::Byte));
      m_pos++;
    } else {
      throw QLangError(QLangErrorKind::Parse, current().line, current().col,
                       "unknown type after ':'");
    }
  }

  // Optional compound-assignment declaration is unusual; require plain '='.
  if (!check(TokenType::Assign) || current().text != "=") {
    throw QLangError(QLangErrorKind::Parse, current().line, current().col,
                     "expected '=' in declaration");
  }
  m_pos++; // consume '='
  stmt->value = parseExpression();
  match(TokenType::Semicolon);
  return stmt;
}

StmtPtr QLangParser::parseFor() {
  int line = current().line, col = current().col;
  consume(TokenType::For, "'for'");
  auto stmt = std::make_shared<Stmt>();
  stmt->kind = StmtKind::For;
  stmt->loopVar = consume(TokenType::Ident, "loop variable").value;
  consume(TokenType::Assign, "'='");
  stmt->forStart = parseExpression();
  consume(TokenType::To, "'to'");
  stmt->forEnd = parseExpression();

  while (!check(TokenType::Next) && !atEnd()) {
    stmt->body.push_back(parseStatement());
  }
  if (check(TokenType::Next)) {
    consume(TokenType::Next, "'next'");
    Token var = consume(TokenType::Ident, "loop variable in 'next'");
    if (var.value != stmt->loopVar) {
      throw QLangError(QLangErrorKind::Parse, var.line, var.col,
                       std::string("'next '") + var.value +
                           "' does not match loop variable '" +
                           stmt->loopVar + "'");
    }
  } else if (atEnd()) {
    throw QLangError(QLangErrorKind::Parse, line, col,
                     "expected 'next' to close 'for' loop");
  }
  return stmt;
}

StmtPtr QLangParser::parseWhile() {
  consume(TokenType::While, "'while'");
  auto stmt = std::make_shared<Stmt>();
  stmt->kind = StmtKind::While;
  stmt->cond = parseExpression();
  while (!check(TokenType::End) && !atEnd()) {
    stmt->whileBody.push_back(parseStatement());
  }
  consume(TokenType::End, "'end' to close 'while'");
  return stmt;
}

StmtPtr QLangParser::parseIf() {
  consume(TokenType::If, "'if'");
  auto stmt = std::make_shared<Stmt>();
  stmt->kind = StmtKind::If;
  stmt->ifCond = parseExpression();
  match(TokenType::Then);
  while (!check(TokenType::End) && !atEnd()) {
    stmt->ifBody.push_back(parseStatement());
  }
  consume(TokenType::End, "'end' to close 'if'");
  return stmt;
}

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

ExprPtr QLangParser::parseExpression() { return parseBinary(1); }

ExprPtr QLangParser::parseBinary(int minPrec) {
  ExprPtr left = parseUnary();
  while (true) {
    TokenType t = current().type;
    int prec = precedence(t);
    if (prec < 0 || prec < minPrec) break;
    Token op = current();
    m_pos++;
    ExprPtr right = parseBinary(prec + 1);
    auto node = std::make_shared<Expr>();
    node->kind = ExprKind::Binary;
    node->op = op;
    node->left = std::move(left);
    node->right = std::move(right);
    left = std::move(node);
  }
  return left;
}

ExprPtr QLangParser::parseUnary() {
  if (check(TokenType::Decrement) || check(TokenType::Increment) ||
      check(TokenType::Minus)) {
    Token op = current();
    m_pos++;
    auto node = std::make_shared<Expr>();
    node->kind = ExprKind::Unary;
    node->uop = op;
    node->operand = parseUnary();
    return node;
  }
  return parsePostfix();
}

ExprPtr QLangParser::parsePostfix() {
  ExprPtr e = parsePrimary();
  while (check(TokenType::Increment) || check(TokenType::Decrement)) {
    Token op = current();
    m_pos++;
    auto node = std::make_shared<Expr>();
    node->kind = ExprKind::Unary;
    node->uop = op;
    node->operand = e;
    e = std::move(node);
  }
  return e;
}

ExprPtr QLangParser::parsePrimary() {
  const Token &tok = current();
  switch (tok.type) {
  case TokenType::Integer: {
    m_pos++;
    auto node = std::make_shared<Expr>();
    node->kind = ExprKind::Literal;
    node->literal = QLangValue::Int(static_cast<int32_t>(std::stoll(tok.value)));
    return node;
  }
  case TokenType::Float: {
    m_pos++;
    auto node = std::make_shared<Expr>();
    node->kind = ExprKind::Literal;
    // No float type in the spec yet: truncate to int for this version.
    node->literal = QLangValue::Int(static_cast<int32_t>(std::stod(tok.value)));
    return node;
  }
  case TokenType::String: {
    m_pos++;
    auto node = std::make_shared<Expr>();
    node->kind = ExprKind::Literal;
    node->literal = QLangValue::String(tok.value);
    return node;
  }
  case TokenType::Char: {
    m_pos++;
    auto node = std::make_shared<Expr>();
    node->kind = ExprKind::Literal;
    uint8_t v = tok.value.empty() ? 0
                                  : static_cast<uint8_t>(tok.value.front());
    node->literal = QLangValue::Byte(v);
    return node;
  }
  case TokenType::Ident: {
    m_pos++;
    auto node = std::make_shared<Expr>();
    node->kind = ExprKind::Variable;
    node->name = tok.value;
    return node;
  }
  case TokenType::LParen: {
    m_pos++;
    ExprPtr inner = parseExpression();
    consume(TokenType::RParen, "')'");
    return inner;
  }
  default:
    break;
  }

  throw QLangError(QLangErrorKind::Parse, tok.line, tok.col,
                   std::string("unexpected token '") + tok.text +
                       "' in expression");
}

} // namespace qlang
