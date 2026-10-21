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
  case TokenType::LogicalOr:
  case TokenType::LogicalOr3: return 1;
  case TokenType::Exponential: return 11;
  case TokenType::LogicalAnd: return 2;
  case TokenType::Or: return 3;
  case TokenType::Xor: return 4;
  case TokenType::And: return 5;
  case TokenType::Equal:
  case TokenType::NotEqual: return 6;
  case TokenType::In: return 6;
  case TokenType::Less:
  case TokenType::Greater:
  case TokenType::LessEqual:
  case TokenType::GreaterEqual: return 7;
  case TokenType::ShiftLeft:
  case TokenType::ShiftRight: return 8;
  case TokenType::Plus:
  case TokenType::Minus: return 9;
  case TokenType::Star:
  case TokenType::Slash:
  case TokenType::Modulo: return 10;
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
  if (check(TokenType::Local) || check(TokenType::Let)) return parseDeclaration();
  if (check(TokenType::Const)) return parseConst();
  if (check(TokenType::Delete)) return parseDelete();
  if (check(TokenType::For)) return parseFor();
  if (check(TokenType::While)) return parseWhile();
  if (check(TokenType::If)) return parseIf();
  if (check(TokenType::Continue)) return parseContinue();
  if (check(TokenType::Break)) return parseBreak();
  if (check(TokenType::Goto)) return parseGoto();

  if (check(TokenType::Def)) return parseFunction();
  if (check(TokenType::Return)) return parseReturn();

  // Label statement: :labelname
  if (check(TokenType::Colon)) {
    consume(TokenType::Colon, "':' before label name");
    auto stmt = std::make_shared<Stmt>();
    stmt->kind = StmtKind::Label;
    stmt->labelName = consume(TokenType::Ident, "label name after ':'").value;
    match(TokenType::Semicolon);
    return stmt;
  }

  // Statement-form prefix inc/dec: ++a / --a
  if (check(TokenType::Increment) || check(TokenType::Decrement)) {
    auto expr = parseExpression();
    auto stmt = std::make_shared<Stmt>();
    stmt->kind = StmtKind::ExprStmt;
    stmt->value = expr;
    match(TokenType::Semicolon);
    return stmt;
  }

  // Bare literal expression statement (e.g. a standalone multiline string).
  if (check(TokenType::String) || check(TokenType::Char) ||
      check(TokenType::Integer) || check(TokenType::Float)) {
    auto expr = parseExpression();
    match(TokenType::Semicolon);
    auto stmt = std::make_shared<Stmt>();
    stmt->kind = StmtKind::ExprStmt;
    stmt->value = expr;
    return stmt;
  }

  // Expression statement or assignment.
  if (check(TokenType::Ident)) {
    const Token &idt = current();
    m_pos++; // consume the identifier
    TokenType nxt = current().type;

    // Postfix inc/dec as a statement: a++ / a--
    if (nxt == TokenType::Increment || nxt == TokenType::Decrement) {
      Token op = current();
      m_pos++; // consume ++ / --
      auto node = std::make_shared<Expr>();
      node->kind = ExprKind::Unary;
      node->uop = op;
      auto var = std::make_shared<Expr>();
      var->kind = ExprKind::Variable;
      var->name = idt.value;
      node->operand = var;
      auto stmt = std::make_shared<Stmt>();
      stmt->kind = StmtKind::ExprStmt;
      stmt->value = node;
      match(TokenType::Semicolon);
      return stmt;
    }

    bool isAssign =
        nxt == TokenType::Assign || nxt == TokenType::Plus ||
        nxt == TokenType::Minus || nxt == TokenType::Star ||
        nxt == TokenType::Slash || nxt == TokenType::And || nxt == TokenType::Or ||
        nxt == TokenType::Xor || nxt == TokenType::ShiftLeft ||
        nxt == TokenType::ShiftRight;
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
    // Index assignment target: ab[expr] = value  (or ab[] = value -> append).
    if (nxt == TokenType::LBracket) {
      m_pos++; // consume '['
      ExprPtr idx;
      if (check(TokenType::RBracket))
        idx = nullptr; // empty subscript ab[] = v -> append
      else
        idx = parseExpression();
      consume(TokenType::RBracket, "']' after subscript");
      auto stmt = std::make_shared<Stmt>();
      stmt->kind = StmtKind::Assignment;
      stmt->name = idt.value;
      stmt->assignOp = current();
      m_pos++; // consume the operator (= or +=)
      stmt->assignValue = parseExpression();
      auto idxNode = std::make_shared<Expr>();
      idxNode->kind = ExprKind::Index;
      auto base = std::make_shared<Expr>();
      base->kind = ExprKind::Variable;
      base->name = idt.value;
      idxNode->indexBase = base;
      idxNode->indexExpr = idx;
      stmt->target = idxNode;
      match(TokenType::Semicolon);
      return stmt;
    }
    // Member assignment target: t.name = value.
    // But if the token after the member name is '(' it's a method call, not assignment.
    if (nxt == TokenType::Dot) {
      m_pos++; // consume '.'
      // Peek past the member-name token to check for '(' vs assignment op.
      size_t saved = m_pos;
      m_pos++; // consume the member-name token
      TokenType afterMember = current().type;
      m_pos = saved; // rewind to after '.'
      if (afterMember == TokenType::LParen) {
        // Method call: not an assignment. Rewind fully and let parseExpression handle it.
        m_pos = m_pos - 2; // step back onto the identifier
        auto expr = parseExpression();
        match(TokenType::Semicolon);
        auto stmt = std::make_shared<Stmt>();
        stmt->kind = StmtKind::ExprStmt;
        stmt->value = expr;
        return stmt;
      }
      std::string name = current().value;
      m_pos++; // consume the member-name token
      auto stmt = std::make_shared<Stmt>();
      stmt->kind = StmtKind::Assignment;
      stmt->name = idt.value;
      stmt->assignOp = current();
      m_pos++; // consume the operator (= or +=)
      stmt->assignValue = parseExpression();
      auto mNode = std::make_shared<Expr>();
      mNode->kind = ExprKind::Member;
      auto base = std::make_shared<Expr>();
      base->kind = ExprKind::Variable;
      base->name = idt.value;
      mNode->memberBase = base;
      mNode->memberName = name;
      stmt->target = mNode;
      match(TokenType::Semicolon);
      return stmt;
    }
    // Not an assignment: rewind and parse an expression statement.
    m_pos--; // step back onto the identifier
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
  match(TokenType::Local);
  match(TokenType::Let);
  auto stmt = std::make_shared<Stmt>();
  stmt->kind = StmtKind::Declaration;
  stmt->name = consume(TokenType::Ident, "variable name").value;

  // Optional typed declaration: local b:byte = 32
  if (match(TokenType::Colon)) {
    TokenType tt = current().type;
    bool isType =
        (tt >= TokenType::TypeBool && tt <= TokenType::TypeTable) ||
        tt == TokenType::StrType;
    if (isType) {
      QLangValue::Type vt =
          (tt == TokenType::StrType)
              ? QLangValue::Type::String
              : static_cast<QLangValue::Type>(
                    static_cast<int>(tt) - static_cast<int>(TokenType::TypeBool) +
                    static_cast<int>(QLangValue::Type::Bool));
      stmt->declaredType = vt;
      m_pos++;
    } else {
      throw QLangError(QLangErrorKind::Parse, current().line, current().col,
                        "unknown type after ':'");
    }
  }

  // Optional value: `local z` declares a nil. `local name = (params)...{}`
  // is an arrow-closure assignment; otherwise it is a normal expression.
  if (check(TokenType::Assign) && current().text == "=") {
    m_pos++; // consume '='
    if (check(TokenType::LParen) && looksLikeArrow()) {
      return parseArrowClosure(stmt->name);
    }
    stmt->value = parseExpression();
  }
  match(TokenType::Semicolon);
  return stmt;
}

StmtPtr QLangParser::parseConst() {
  consume(TokenType::Const, "'const'");
  auto stmt = std::make_shared<Stmt>();
  stmt->kind = StmtKind::Const;
  stmt->isConst = true;
  stmt->name = consume(TokenType::Ident, "variable name").value;

  if (match(TokenType::Colon)) {
    TokenType tt = current().type;
    bool isType =
        (tt >= TokenType::TypeBool && tt <= TokenType::TypeTable) ||
        tt == TokenType::StrType;
    if (isType) {
      QLangValue::Type vt =
          (tt == TokenType::StrType)
              ? QLangValue::Type::String
              : static_cast<QLangValue::Type>(
                    static_cast<int>(tt) - static_cast<int>(TokenType::TypeBool) +
                    static_cast<int>(QLangValue::Type::Bool));
      stmt->declaredType = vt;
      m_pos++;
    } else {
      throw QLangError(QLangErrorKind::Parse, current().line, current().col,
                        "unknown type after ':'");
    }
  }

  if (!check(TokenType::Assign) || current().text != "=") {
    throw QLangError(QLangErrorKind::Parse, current().line, current().col,
                     "expected '=' in const declaration");
  }
  m_pos++; // consume '='
  stmt->value = parseExpression();
  match(TokenType::Semicolon);
  return stmt;
}

StmtPtr QLangParser::parseDelete() {
  consume(TokenType::Delete, "'delete'");
  auto stmt = std::make_shared<Stmt>();
  stmt->kind = StmtKind::Delete;
  stmt->delName = consume(TokenType::Ident, "variable name").value;
  match(TokenType::Semicolon);
  return stmt;
}

StmtPtr QLangParser::parseFor() {
  int line = current().line, col = current().col;
  consume(TokenType::For, "'for'");
  auto stmt = std::make_shared<Stmt>();
  stmt->loopVar = consume(TokenType::Ident, "loop variable").value;

  // for i in <collection> ... next i  (sequential iteration)
  if (check(TokenType::In)) {
    m_pos++; // consume 'in'
    stmt->kind = StmtKind::ForIn;
    stmt->forEnd = parseExpression(); // the collection

    // Accept either 'next' or '{' as body delimiter
    if (check(TokenType::LBrace)) {
      m_pos++; // consume '{'
      while (!check(TokenType::RBrace) && !atEnd())
        stmt->body.push_back(parseStatement());
      consume(TokenType::RBrace, "'}' to close 'for' loop");
    } else {
      while (!check(TokenType::Next) && !check(TokenType::End) && !atEnd())
        stmt->body.push_back(parseStatement());
      if (check(TokenType::Next)) {
        consume(TokenType::Next, "'next'");
        Token var = consume(TokenType::Ident, "loop variable in 'next'");
        if (var.value != stmt->loopVar)
          throw QLangError(QLangErrorKind::Parse, var.line, var.col,
                           std::string("'next '") + var.value +
                               "' does not match loop variable '" +
                               stmt->loopVar + "'");
      } else if (atEnd()) {
        throw QLangError(QLangErrorKind::Parse, line, col,
                         "expected 'next' to close 'for' loop");
      }
    }
    return stmt;
  }

  // for x = start to end ... next x  (numeric iteration)
  consume(TokenType::Assign, "'='");
  stmt->kind = StmtKind::For;
  stmt->forStart = parseExpression();
  consume(TokenType::To, "'to'");
  stmt->forEnd = parseExpression();

  // Accept either 'next' or '{' as body delimiter
  if (check(TokenType::LBrace)) {
    m_pos++; // consume '{'
    while (!check(TokenType::RBrace) && !atEnd())
      stmt->body.push_back(parseStatement());
    consume(TokenType::RBrace, "'}' to close 'for' loop");
  } else {
    while (!check(TokenType::Next) && !check(TokenType::End) && !atEnd()) {
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
  }
  return stmt;
}

StmtPtr QLangParser::parseWhile() {
  consume(TokenType::While, "'while'");
  auto stmt = std::make_shared<Stmt>();
  stmt->kind = StmtKind::While;
  stmt->cond = parseExpression();

  // Accept either 'end' or '{' as body delimiter
  if (check(TokenType::LBrace)) {
    m_pos++; // consume '{'
    while (!check(TokenType::RBrace) && !atEnd())
      stmt->whileBody.push_back(parseStatement());
    consume(TokenType::RBrace, "'}' to close 'while' loop");
  } else {
    while (!check(TokenType::End) && !atEnd()) {
      stmt->whileBody.push_back(parseStatement());
    }
    consume(TokenType::End, "'end' to close 'while'");
  }
  return stmt;
}

StmtPtr QLangParser::parseIf() {
  consume(TokenType::If, "'if'");
  auto stmt = std::make_shared<Stmt>();
  stmt->kind = StmtKind::If;
  stmt->ifCond = parseExpression();

  // Short-circuit form: `if cond goto label`
  if (check(TokenType::Goto)) {
    m_pos++; // consume 'goto'
    auto gotoStmt = std::make_shared<Stmt>();
    gotoStmt->kind = StmtKind::GotoIf;
    gotoStmt->gotoIfCond = stmt->ifCond;
    gotoStmt->gotoIfLabel = consume(TokenType::Ident, "label name after 'goto'").value;
    match(TokenType::Semicolon);
    return gotoStmt;
  }

  match(TokenType::Then);

  // Accept either 'end' or '{' as body delimiter
  if (check(TokenType::LBrace)) {
    m_pos++; // consume '{'
    while (!check(TokenType::RBrace) && !atEnd())
      stmt->ifBody.push_back(parseStatement());
    consume(TokenType::RBrace, "'}' to close 'if'");
  } else {
    while (!check(TokenType::End) && !check(TokenType::Else) && !atEnd()) {
      stmt->ifBody.push_back(parseStatement());
    }
    if (check(TokenType::End))
      consume(TokenType::End, "'end' to close 'if'");
  }

  // Optional else / else if
  if (check(TokenType::Else)) {
    m_pos++; // consume 'else'
    if (check(TokenType::If)) {
      // else if: recursively parse the nested if as the else branch
      stmt->elseBody.push_back(parseIf());
    } else {
      // else body
      if (check(TokenType::LBrace)) {
        m_pos++;
        while (!check(TokenType::RBrace) && !atEnd())
          stmt->elseBody.push_back(parseStatement());
        consume(TokenType::RBrace, "'}' to close 'else'");
      } else {
        while (!check(TokenType::End) && !atEnd() && !check(TokenType::Else)) {
          stmt->elseBody.push_back(parseStatement());
        }
        if (check(TokenType::End))
          consume(TokenType::End, "'end' to close 'else'");
      }
    }
  }

  return stmt;
}

StmtPtr QLangParser::parseReturn() {
  consume(TokenType::Return, "'return'");
  auto stmt = std::make_shared<Stmt>();
  stmt->kind = StmtKind::Return;
  // Optional value; a bare `return` (or `return` before a terminator) yields nil.
  if (!check(TokenType::Semicolon) && !check(TokenType::End) &&
      !check(TokenType::RBrace) && !atEnd()) {
    stmt->returnValue = parseExpression();
  }
  match(TokenType::Semicolon);
  return stmt;
}

StmtPtr QLangParser::parseContinue() {
  int line = current().line, col = current().col;
  (void)line;
  (void)col;
  consume(TokenType::Continue, "'continue'");
  auto stmt = std::make_shared<Stmt>();
  stmt->kind = StmtKind::Continue;
  match(TokenType::Semicolon);
  return stmt;
}

StmtPtr QLangParser::parseBreak() {
  int line = current().line, col = current().col;
  (void)line;
  (void)col;
  consume(TokenType::Break, "'break'");
  auto stmt = std::make_shared<Stmt>();
  stmt->kind = StmtKind::Break;
  match(TokenType::Semicolon);
  return stmt;
}

StmtPtr QLangParser::parseGoto() {
  int line = current().line, col = current().col;
  (void)line;
  (void)col;
  consume(TokenType::Goto, "'goto'");
  auto stmt = std::make_shared<Stmt>();
  stmt->kind = StmtKind::Goto;
  stmt->gotoLabel = consume(TokenType::Ident, "label name after 'goto'").value;
  match(TokenType::Semicolon);
  return stmt;
}

StmtPtr QLangParser::parseFunction() {
  consume(TokenType::Def, "'def'");
  auto stmt = std::make_shared<Stmt>();
  stmt->kind = StmtKind::Def;
  stmt->funcName = consume(TokenType::Ident, "function name").value;
  consume(TokenType::LParen, "'(' after function name");
  if (!check(TokenType::RParen)) {
    do {
      stmt->params.push_back(consume(TokenType::Ident, "parameter name").value);
    } while (match(TokenType::Comma));
  }
  consume(TokenType::RParen, "')'");

  if (match(TokenType::LBrace)) {
    // Brace form: def name(params) { ... }
    while (!check(TokenType::RBrace) && !atEnd())
      stmt->body.push_back(parseStatement());
    consume(TokenType::RBrace, "'}' to close function");
  } else {
    // End form: def name(params) ... end
    while (!check(TokenType::End) && !atEnd())
      stmt->body.push_back(parseStatement());
    consume(TokenType::End, "'end' to close function");
  }
  match(TokenType::Semicolon);
  return stmt;
}

// True when the parenthesized group beginning at the current token is an arrow
// function parameter list, i.e. it is followed by an explicit capture list
// '['. The capture list is mandatory. Used to disambiguate `local f = (a)[] { }`
// from `local x = (a + b)`.
bool QLangParser::looksLikeArrow() const {
  int depth = 0;
  size_t j = m_pos;
  while (j < m_tokens.size()) {
    if (m_tokens[j].type == TokenType::LParen)
      depth++;
    else if (m_tokens[j].type == TokenType::RParen) {
      depth--;
      if (depth == 0) {
        j++;
        break;
      }
    }
    j++;
  }
  return j < m_tokens.size() &&
         m_tokens[j].type == TokenType::LBracket;
}

StmtPtr QLangParser::parseArrowClosure(const std::string &varName) {
  int line = current().line, col = current().col;
  (void)line;
  (void)col;
  consume(TokenType::LParen, "'(' of arrow function");
  auto stmt = std::make_shared<Stmt>();
  stmt->kind = StmtKind::Arrow;
  stmt->name = varName;
  if (!check(TokenType::RParen)) {
    do {
      stmt->params.push_back(consume(TokenType::Ident, "parameter name").value);
    } while (match(TokenType::Comma));
  }
  consume(TokenType::RParen, "')' of arrow function");

  // Mandatory explicit capture list: [x, y, ...]
  consume(TokenType::LBracket, "'[' to open capture list");
  if (!check(TokenType::RBracket)) {
    do {
      stmt->captures.push_back(
        consume(TokenType::Ident, "captured variable").value);
    } while (match(TokenType::Comma));
  }
  consume(TokenType::RBracket, "']' to close capture list");

  // Body in braces.
  consume(TokenType::LBrace, "'{' to open arrow function body");
  while (!check(TokenType::RBrace) && !atEnd())
    stmt->body.push_back(parseStatement());
  consume(TokenType::RBrace, "'}' to close arrow function body");
  match(TokenType::Semicolon);
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
    // Right-associative: ** (precedence 11) parses with same precedence
    // so that a**b**c groups as a**(b**c), not (a**b)**c.
    int nextPrec = (t == TokenType::Exponential) ? prec : prec + 1;
    ExprPtr right = parseBinary(nextPrec);
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
      check(TokenType::Minus) || check(TokenType::Not)) {
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
  // Postfix inc/dec binds only to a bare variable (e.g. a++). Restricting to a
  // Variable prevents a following statement-level ++/-- from being greedily
  // swallowed as a prefix on a parenthesized/sub-expression operand.
  while ((check(TokenType::Increment) || check(TokenType::Decrement)) &&
         e->kind == ExprKind::Variable) {
    Token op = current();
    m_pos++;
    auto node = std::make_shared<Expr>();
    node->kind = ExprKind::Unary;
    node->uop = op;
    node->operand = e;
    e = std::move(node);
  }
  // Subscript (array) and member (table) access, which may chain (a.b[0], a[0].b).
  while (true) {
    if (check(TokenType::LBracket)) {
      m_pos++; // consume '['
      ExprPtr idx;
      if (check(TokenType::RBracket))
        idx = nullptr; // empty subscript ab[] (append target)
      else
        idx = parseExpression();
      consume(TokenType::RBracket, "']' after subscript");
      auto node = std::make_shared<Expr>();
      node->kind = ExprKind::Index;
      node->indexBase = e;
      node->indexExpr = idx;
      e = std::move(node);
    } else if (check(TokenType::Dot)) {
      m_pos++; // consume '.'
      const Token &nameTok = current();
      m_pos++; // consume the member-name token (Ident or keyword)
      auto node = std::make_shared<Expr>();
      node->kind = ExprKind::Member;
      node->memberBase = e;
      node->memberName = nameTok.value;
      e = std::move(node);
    } else {
      break;
    }
  }
  // Call on member: obj.method(args) -> wrap in Call with Member as callee.
  if (check(TokenType::LParen)) {
    m_pos++; // consume '('
    auto node = std::make_shared<Expr>();
    node->kind = ExprKind::Call;
    node->callee = e->memberName;
    node->calleeMember = e;
    if (!check(TokenType::RParen)) {
      do {
        node->args.push_back(parseExpression());
      } while (match(TokenType::Comma));
    }
    consume(TokenType::RParen, "')'");
    e = std::move(node);
  }
  return e;
}

ExprPtr QLangParser::parsePrimary() {
  const Token &tok = current();
  switch (tok.type) {
  case TokenType::Integer: {
    m_pos++;
    long long v = std::stoll(tok.value); // decimal
    if (tok.value.size() > 2 && tok.value[0] == '0' &&
        (tok.value[1] == 'x' || tok.value[1] == 'X'))
      v = std::stoll(tok.value, nullptr, 16); // 0x... hexadecimal
    auto node = std::make_shared<Expr>();
    node->kind = ExprKind::Literal;
    node->literal = QLangValue::Int(static_cast<int32_t>(v));
    return node;
  }
  case TokenType::True: {
    m_pos++;
    auto node = std::make_shared<Expr>();
    node->kind = ExprKind::Literal;
    node->literal = QLangValue::Int(1); // bool true -> 1 (spec: 1/0 stored as Int)
    return node;
  }
  case TokenType::False: {
    m_pos++;
    auto node = std::make_shared<Expr>();
    node->kind = ExprKind::Literal;
    node->literal = QLangValue::Int(0);
    return node;
  }
  case TokenType::Float: {
    m_pos++;
    auto node = std::make_shared<Expr>();
    node->kind = ExprKind::Literal;
    node->literal = QLangValue::Float(std::stod(tok.value));
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
    // Call expression: name(args)
    if (check(TokenType::LParen)) {
      m_pos++; // consume '('
      auto node = std::make_shared<Expr>();
      node->kind = ExprKind::Call;
      node->callee = tok.value;
      if (!check(TokenType::RParen)) {
        do {
          node->args.push_back(parseExpression());
        } while (match(TokenType::Comma));
      }
      consume(TokenType::RParen, "')'");
      return node;
    }
    auto node = std::make_shared<Expr>();
    node->kind = ExprKind::Variable;
    node->name = tok.value;
    return node;
  }
  // str() / int() are reserved words (type tokens) but also valid as builtin
  // function calls per spec §236-237. If followed by '(', treat as a call.
  case TokenType::StrType: {
    m_pos++;
    if (check(TokenType::LParen)) {
      m_pos++; // consume '('
      auto node = std::make_shared<Expr>();
      node->kind = ExprKind::Call;
      node->callee = "str";
      if (!check(TokenType::RParen)) {
        do {
          node->args.push_back(parseExpression());
        } while (match(TokenType::Comma));
      }
      consume(TokenType::RParen, "')'");
      return node;
    }
    throw QLangError(QLangErrorKind::Parse, tok.line, tok.col,
                     std::string("unexpected token '") + tok.text +
                     "' in expression");
  }
  case TokenType::TypeInt: {
    m_pos++;
    if (check(TokenType::LParen)) {
      m_pos++; // consume '('
      auto node = std::make_shared<Expr>();
      node->kind = ExprKind::Call;
      node->callee = "int";
      if (!check(TokenType::RParen)) {
        do {
          node->args.push_back(parseExpression());
        } while (match(TokenType::Comma));
      }
      consume(TokenType::RParen, "')'");
      return node;
    }
    throw QLangError(QLangErrorKind::Parse, tok.line, tok.col,
                     std::string("unexpected token '") + tok.text +
                     "' in expression");
  }
  case TokenType::LParen: {
    // Check for anonymous arrow closure: (params)[captures] { ... }
    if (looksLikeArrow()) {
      auto node = std::make_shared<Expr>();
      node->kind = ExprKind::Closure;
      consume(TokenType::LParen, "'(' of arrow function");
      if (!check(TokenType::RParen)) {
        do {
          node->params.push_back(consume(TokenType::Ident, "parameter name").value);
        } while (match(TokenType::Comma));
      }
      consume(TokenType::RParen, "')' of arrow function");
      // Mandatory explicit capture list: [x, y, ...]
      consume(TokenType::LBracket, "'[' to open capture list");
      if (!check(TokenType::RBracket)) {
        do {
          node->captures.push_back(consume(TokenType::Ident, "capture name").value);
        } while (match(TokenType::Comma));
      }
      consume(TokenType::RBracket, "']' after capture list");
      // Body: { ... } or ... end
      if (match(TokenType::LBrace)) {
        while (!check(TokenType::RBrace) && !atEnd())
          node->body.push_back(parseStatement());
        consume(TokenType::RBrace, "'}' to close closure");
      } else {
        while (!check(TokenType::End) && !atEnd())
          node->body.push_back(parseStatement());
        consume(TokenType::End, "'end' to close closure");
      }
      match(TokenType::Semicolon);
      return node;
    }
    m_pos++;
    ExprPtr inner = parseExpression();
    consume(TokenType::RParen, "')'");
    return inner;
  }
  case TokenType::TypeBlob: {
    // blob() call -> create an empty binary blob.
    m_pos++; // consume 'blob' first, then expect '('
    if (check(TokenType::LParen)) {
      m_pos++; // consume '('
      auto node = std::make_shared<Expr>();
      node->kind = ExprKind::Call;
      node->callee = "blob";
      if (!check(TokenType::RParen)) {
        do {
          node->args.push_back(parseExpression());
        } while (match(TokenType::Comma));
      }
      consume(TokenType::RParen, "')'");
      return node;
    }
    throw QLangError(QLangErrorKind::Parse, tok.line, tok.col,
                     std::string("expected '(' after 'blob'"));
  }
  case TokenType::Range: {
    // range() builtin call.
    m_pos++;
    if (check(TokenType::LParen)) {
      m_pos++;
      auto node = std::make_shared<Expr>();
      node->kind = ExprKind::Call;
      node->callee = "range";
      if (!check(TokenType::RParen)) {
        do {
          node->args.push_back(parseExpression());
        } while (match(TokenType::Comma));
      }
      consume(TokenType::RParen, "')'");
      return node;
    }
    throw QLangError(QLangErrorKind::Parse, tok.line, tok.col,
                     std::string("expected '(' after 'range'"));
  }
  case TokenType::Slice: {
    // slice() builtin call.
    m_pos++;
    if (check(TokenType::LParen)) {
      m_pos++;
      auto node = std::make_shared<Expr>();
      node->kind = ExprKind::Call;
      node->callee = "slice";
      if (!check(TokenType::RParen)) {
        do {
          node->args.push_back(parseExpression());
        } while (match(TokenType::Comma));
      }
      consume(TokenType::RParen, "')'");
      return node;
    }
    throw QLangError(QLangErrorKind::Parse, tok.line, tok.col,
                     std::string("expected '(' after 'slice'"));
  }
  case TokenType::LBracket:
    return parseArrayLiteral();
  case TokenType::LBrace:
    return parseTableLiteral();
  default:
    break;
  }

  throw QLangError(QLangErrorKind::Parse, tok.line, tok.col,
                   std::string("unexpected token '") + tok.text +
                       "' in expression");
}

ExprPtr QLangParser::parseArrayLiteral() {
  consume(TokenType::LBracket, "'['");
  auto node = std::make_shared<Expr>();
  node->kind = ExprKind::Array;
  if (!check(TokenType::RBracket)) {
    do {
      node->elements.push_back(parseExpression());
    } while (match(TokenType::Comma));
  }
  consume(TokenType::RBracket, "]''");
  return node;
}

ExprPtr QLangParser::parseTableLiteral() {
  consume(TokenType::LBrace, "'{'");
  auto node = std::make_shared<Expr>();
  node->kind = ExprKind::Table;
  if (!check(TokenType::RBrace)) {
    do {
      std::string key = consume(TokenType::Ident, "table key").value;
      ExprPtr val;
      // Optional :type annotation on the value: key:byte = expr
      if (match(TokenType::Colon)) {
        if (current().type >= TokenType::TypeBool &&
            current().type <= TokenType::TypeTable)
          m_pos++;
        else
          throw QLangError(QLangErrorKind::Parse, current().line, current().col,
                           "unknown type after ':' in table entry");
      }
      consume(TokenType::Assign, "'=' in table entry");
      val = parseExpression();
      node->items.emplace_back(key, val);
    } while (match(TokenType::Comma));
  }
  consume(TokenType::RBrace, "}''");
  return node;
}

} // namespace qlang
