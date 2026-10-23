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
enum class ExprKind {
  Literal,
  Variable,
  Binary,
  Unary,
  Call,
  Array,
  Table,
  Index,   // base[index]  (array subscript read)
  Member,   // base.name    (table member read)
  Closure,   // (params)[captures] { ... }  (anonymous closure expression)
  FString,   // f"hello ${name}"  (interpolated string literal)
  Cast       // cast<Type>(expr)  (runtime type coercion)
};

struct Expr; // forward declaration so ExprPtr can be used inside Expr
struct Stmt; // forward declaration so StmtPtr can be used inside Expr
using ExprPtr = std::shared_ptr<Expr>;

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

  // Call: callee(name) with positional args.
  std::string callee;
  ExprPtr calleeMember; // for method calls: obj.method(args)
  std::vector<ExprPtr> args;
  int callLine = 0;
  int callCol = 0;

  // Array literal: [e0, e1, ...].
  std::vector<ExprPtr> elements;

  // Table literal: { key = value, ... } (value may carry an optional :type).
  std::vector<std::pair<std::string, ExprPtr>> items;

  // Index: base[index]  (array subscript access).
  ExprPtr indexBase;
  ExprPtr indexExpr;

  // Member: base.name  (table member access).
  ExprPtr memberBase;
  std::string memberName;

  // Closure: (params)[captures] { ... }  (anonymous arrow closure expression).
  std::vector<std::string> params;
  std::vector<std::string> captures;
  std::vector<std::shared_ptr<Stmt>> body;

  // FString: f"literal ${expr} more"  (interpolated string).
  // `parts` alternates: even indices = literal string segments,
  // odd indices = expression nodes to interpolate.
  std::vector<std::string> strParts;
  std::vector<ExprPtr> exprParts;
  std::vector<std::string> formatSpecs;  // per-expression format spec (e.g. ".2f")

  // Cast: cast<Type>(expr)  (runtime type coercion).
  QLangValue::Type targetType;
  ExprPtr castOperand;
};

// ---------------------------------------------------------------------------
// Statement AST
// ---------------------------------------------------------------------------
enum class StmtKind {
  Declaration,
  Const,     // const name[:type] = value
  Assignment,
  Delete,    // delete var
  For,
  ForIn,   // for i in <collection> ... next i
  While,
  If,
  ExprStmt,
  Def,       // def name(params) ... end  /  def name(params) { ... }
  Arrow,     // local name = (params)[captures] { ... }  (closure assignment)
  Return,    // return [value]
  Continue,  // continue
  Break,     // break
  Label,     // :labelname (no-op marker, records PC for goto)
  Goto,      // goto labelname
  GotoIf,    // if cond goto label (conditional jump)
  Switch,    // switch (expr) { case ... : ... default: ... }
  Global,    // global a, b (import top-level vars into function scope)
  Import     // import math / import math.min
};

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

  // Const: const name[:type] = value (immutable after init)
  bool isConst = false;

  // Delete: delete var
  std::string delName;

  // Assignment: name op value  (op includes compound forms like +=)
  Token assignOp;
  ExprPtr assignValue;
  // Optional full left-hand expression for index/member assignment
  // (e.g. a[i] = v or t.name = v). When set, `name` holds the base variable
  // name and `target` is an Index or Member expression describing the LHS.
  ExprPtr target;

  // For: for x = start to end ... next x
  std::string loopVar;
  ExprPtr forStart;
  ExprPtr forEnd;
  bool forIn = false; // for i in <collection> (forEnd holds the collection)
  std::vector<StmtPtr> body;

  // While: while (cond) ... end
  ExprPtr cond;
  std::vector<StmtPtr> whileBody;

  // If: if (cond) then ... end
  ExprPtr ifCond;
  std::vector<StmtPtr> ifBody;
  std::vector<StmtPtr> elseBody;

  // Def: def name(params) { ... } / def name(params) ... end  (global fn).
  std::string funcName;
  std::vector<std::string> params;
  // `body` (shared with For) holds the function's statements.

  // Arrow: local name = (params)[captures] { ... }  (closure assignment).
  std::vector<std::string> captures;

  // Return: return [value]  (value optional; defaults to nil).
  ExprPtr returnValue;

  // Label: :labelname (records a PC target for goto).
  std::string labelName;

  // Goto: goto labelname (unconditional jump to a label).
  std::string gotoLabel;

  // GotoIf: if cond goto label (conditional jump).
  ExprPtr gotoIfCond;
  std::string gotoIfLabel;

  // Switch: switch (expr) { case val : ... case in expr : ... default : ... }
  ExprPtr switchExpr;
  struct SwitchCase {
    ExprPtr value;       // the case expression to match
    bool isIn = false;   // true if `case in expr:` (membership test)
    std::vector<StmtPtr> body;
  };
  std::vector<SwitchCase> cases;
  std::vector<StmtPtr> defaultBody;

  // Global: global name1, name2, ...  (import top-level vars into function scope).
  std::vector<std::string> globalNames;

  // Import: import math / import math.min  (bring module names into scope).
  std::string importModule;
  std::string importFunc;  // empty = import all; non-empty = import single func
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
  StmtPtr parseConst();
  StmtPtr parseDelete();
  StmtPtr parseFor();
  StmtPtr parseWhile();
  StmtPtr parseIf();
  StmtPtr parseReturn();
  StmtPtr parseContinue();
  StmtPtr parseBreak();
  StmtPtr parseGoto();
  StmtPtr parseSwitch();
  StmtPtr parseGlobal();
  StmtPtr parseImport();
  StmtPtr parseFunction();
  StmtPtr parseArrowClosure(const std::string &varName);
  bool looksLikeArrow() const;
  ExprPtr parseExpression();
  ExprPtr parseBinary(int minPrecedence);
  ExprPtr parseUnary();
  ExprPtr parsePostfix();
  ExprPtr parsePrimary();
  ExprPtr parseArrayLiteral();
  ExprPtr parseTableLiteral();

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
