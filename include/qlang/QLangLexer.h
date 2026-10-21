#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace qlang {

// Token categories produced by the lexer.
enum class TokenType {
  // Literals and names
  Integer,
  Float,
  String,
  Char,   // single-quoted literal, e.g. ' ' -> byte
  Ident,

  // Keywords
  Local,
  Const,   // const
  Delete,  // delete
  For,
  To,
  Next,
  If,
  Then,
  While,
  End,
  Def,      // def
  Return,   // return

  // Type keywords (used in typed declarations: local b:byte = 32). The range
  // TypeByte..TypeTable maps 1:1 onto QLangValue::Type Byte..Table.
  TypeByte,
  TypeWord,
  TypeShort,
  TypeInt,
  TypeUnsigned,
  TypeString,
  TypeBlob,
  TypeArray,
  TypeFloat,   // float
  TypeTable,   // table
  StrType,     // str (alias for string; kept outside the range above)

  // Keywords / operators
  In,          // in (membership / for-in)
  Continue,    // continue
  Break,       // break
  True,        // true
  False,       // false

  // Operators
  Dot,           // .  (table member access: t.name)
  Assign,        // =
  Equal,         // ==
  NotEqual,      // !=
  Not,           // !  (logical not)
  Less,          // <
  Greater,       // >
  LessEqual,     // <=
  GreaterEqual,  // >=
  ShiftLeft,     // <<
  ShiftRight,    // >>
  Xor,           // ^
  And,           // &
  Or,            // |
  LogicalAnd,    // &&
  LogicalOr,     // ||
  LogicalOr3,    // |||  (logical or, three bars)
  Modulo,        // %
  Plus,          // +
  Minus,         // -
  Star,          // *
  Slash,         // /
  Increment,     // ++
  Decrement,     // --

  // Punctuation
  LParen,   // (
  RParen,   // )
  LBrace,   // {
  RBrace,   // }
  LBracket, // [
  RBracket, // ]
  Semicolon, // ;
  Colon,     // :
  Comma,     // ,

  EndOfFile
};

// A single lexical token with source location for error reporting.
struct Token {
  TokenType type = TokenType::EndOfFile;
  std::string text;   // raw source text of the token
  std::string value;  // semantic value (number text, string content, ident name)
  int line = 1;
  int col = 1;
};

// Turns source text into a flat token stream. Handles whitespace, `#` line
// comments, single/double/triple-quoted strings, char literals, numbers and
// every operator/punctuation in the spec.
class QLangLexer {
public:
  explicit QLangLexer(std::string source);

  std::vector<Token> tokenize();

private:
  char peek(int offset = 0) const;
  char advance();
  bool atEnd() const;
  void skipWhitespaceAndComments();
  Token number();
  Token stringLiteral(bool triple, char delim, bool singleQuote);
  Token identifier();
  Token operatorToken();

  std::string m_src;
  size_t m_pos = 0;
  int m_line = 1;
  int m_col = 1;
};

// Maps an identifier text to its keyword token type (falls back to Ident).
TokenType keywordType(const std::string &text);

} // namespace qlang
