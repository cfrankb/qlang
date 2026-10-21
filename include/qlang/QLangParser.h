#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "QLangLexer.h"
#include "QLangValue.h"

namespace qlang {

// ---------------------------------------------------------------------------
// Expression AST
// ---------------------------------------------------------------------------
enum class ExprKind { Literal, Variable, Binary, Unary };

// Base expression node. Concrete nodes are allocated on the heap and held by
// shared_ptr so the tree can be traversed/mutated freely.
struct Expr {
  ExprKind kind;

  // Literal: the constant value (number, string, char-as-byte, ...).
  QLangValue literal;

  // Variable: reference to a named variable.
  std::string name;

  // Binary: op token plus left/right operands.
  Token op;
  std::shared_ptr<Expr> left;
  std::shared_ptr<Expr> right;

  // Unary: single operand and the op token (e.g. --x).
  Token uop;
  std::shared_ptr<Expr> operand;
};

using ExprPtr = std::shared_ptr<Expr>;

// ---------------------------------------------------------------------------
// Statement AST
// ---------------------------------------------------------------------------
enum class StmtKind { Declaration, Assignment, For, While, If, ExprStmt };

// Base statement node. Forward-declared so StmtPtr can be used inside Stmt.
struct Stmt;
using StmtPtr = std::shared_ptr<Stmt>;

// Base statement node.
struct Stmt {
  StmtKind kind;

  // Declaration: local name[:type] = value
  std::string name;
  std::optional<QLangValue::Type> declaredType;
  ExprPtr value;

  // Assignment: name op value  (op includes compound forms like +=)
  Token assignOp;
  ExprPtr assignValue;

  // For: for x = start to end ... next x
  std::string loopVar;
  ExprPtr forStart;
  ExprPtr forEnd;
  std::vector<StmtPtr> body;

  // While: while (cond) ... end
  ExprPtr cond;
  std::vector<StmtPtr> whileBody;

  // If: if (cond) then ... end
  ExprPtr ifCond;
  std::vector<StmtPtr> ifBody;
};

using StmtPtr = std::shared_ptr<Stmt>;

// Recursive-descent parser over the token stream. Produces a top-level list of
// statements covering declarations, assignments, for/while/if control flow and
// bare expressions, matching the grammar in spec.md.
class QLangParser {
public:
  explicit QLangParser(std::vector<Token> tokens);

  // Parse the whole program. Throws QLangError on any grammar violation.
  std::vector<StmtPtr> parseProgram();

private:
  StmtPtr parseStatement();
  StmtPtr parseDeclaration();
  StmtPtr parseFor();
  StmtPtr parseWhile();
  StmtPtr parseIf();
  ExprPtr parseExpression();
  ExprPtr parseBinary(int minPrecedence);
  ExprPtr parseUnary();
  ExprPtr parsePostfix();
  ExprPtr parsePrimary();

  // Helpers
  const Token &peek(int offset = 0) const;
  const Token &current() const;
  bool check(TokenType t) const;
  bool match(TokenType t);
  const Token &consume(TokenType t, const char *what);
  bool atEnd() const;

  std::vector<Token> m_tokens;
  size_t m_pos = 0;
};

} // namespace qlang
