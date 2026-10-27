#include "qlang/QLangLexer.h"

#include <algorithm>
#include <cctype>

#include "qlang/QLangErrors.h"

namespace qlang {

TokenType keywordType(const std::string &text) {
  if (text == "local") return TokenType::Local;
  if (text == "let") return TokenType::Let;
  if (text == "const") return TokenType::Const;
  if (text == "delete") return TokenType::Delete;
  if (text == "for") return TokenType::For;
  if (text == "to") return TokenType::To;
  if (text == "next") return TokenType::Next;
  if (text == "step") return TokenType::Step;
  if (text == "if") return TokenType::If;
  if (text == "then") return TokenType::Then;
  if (text == "while") return TokenType::While;
  if (text == "end") return TokenType::End;
  if (text == "def") return TokenType::Def;
  if (text == "return") return TokenType::Return;
  if (text == "else") return TokenType::Else;
  if (text == "in") return TokenType::In;
  if (text == "continue") return TokenType::Continue;
  if (text == "break") return TokenType::Break;
  if (text == "goto") return TokenType::Goto;
  if (text == "range") return TokenType::Range;
  if (text == "switch") return TokenType::Switch;
  if (text == "case") return TokenType::Case;
  if (text == "default") return TokenType::Default;
  if (text == "true") return TokenType::True;
  if (text == "false") return TokenType::False;
  if (text == "bool") return TokenType::TypeBool;
  if (text == "char") return TokenType::TypeChar;
  if (text == "byte") return TokenType::TypeByte;
  if (text == "word") return TokenType::TypeWord;
  if (text == "short") return TokenType::TypeShort;
  if (text == "int") return TokenType::TypeInt;
  if (text == "unsigned") return TokenType::TypeUnsigned;
  if (text == "string") return TokenType::TypeString;
  if (text == "str") return TokenType::StrType;
  if (text == "blob") return TokenType::TypeBlob;
  if (text == "array") return TokenType::TypeArray;
  if (text == "float") return TokenType::TypeFloat;
  if (text == "table") return TokenType::TypeTable;
  if (text == "dword") return TokenType::TypeDword;
  if (text == "type") return TokenType::Type;
  if (text == "global") return TokenType::Global;
  if (text == "assert") return TokenType::Assert;
  if (text == "isset") return TokenType::Isset;
  if (text == "import") return TokenType::Import;
  if (text == "from") return TokenType::From;
  if (text == "as") return TokenType::As;
  if (text == "class") return TokenType::Class;
  if (text == "static") return TokenType::Static;
  if (text == "nil") return TokenType::Nil;
  if (text == "eof") return TokenType::Eof;
  if (text == "yield") return TokenType::Yield;
  return TokenType::Ident;
}

QLangLexer::QLangLexer(std::string source) : m_src(std::move(source)) {}

bool QLangLexer::atEnd() const { return m_pos >= m_src.size(); }

char QLangLexer::peek(int offset) const {
  size_t p = m_pos + static_cast<size_t>(offset);
  if (p >= m_src.size()) return '\0';
  return m_src[p];
}

char QLangLexer::advance() {
  char c = m_src[m_pos++];
  if (c == '\n') {
    m_line++;
    m_col = 1;
  } else {
    m_col++;
  }
  return c;
}

void QLangLexer::skipWhitespaceAndComments() {
  while (!atEnd()) {
    char c = peek();
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      advance();
    } else if (c == '#') {
      // Line comment: consume to end of line.
      while (!atEnd() && peek() != '\n')
        advance();
    } else {
      break;
    }
  }
}

Token QLangLexer::number() {
  int line = m_line, col = m_col;
  std::string text;
  bool isFloat = false;

  // Hexadecimal integer literal: 0x[0-9a-fA-F]+
  if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
    advance(); // '0'
    advance(); // 'x'
    std::string digits;
    while (!atEnd() && std::isxdigit(static_cast<unsigned char>(peek())))
      digits.push_back(advance());
    Token tok;
    tok.type = TokenType::Integer;
    tok.text = "0x" + digits;
    tok.value = tok.text;
    tok.line = line;
    tok.col = col;
    return tok;
  }

  while (!atEnd()) {
    char c = peek();
    if (std::isdigit(static_cast<unsigned char>(c))) {
      text.push_back(advance());
    } else if (c == '.' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
      isFloat = true;
      text.push_back(advance());
    } else {
      break;
    }
  }
  Token tok;
  tok.type = isFloat ? TokenType::Float : TokenType::Integer;
  tok.text = text;
  tok.value = text;
  tok.line = line;
  tok.col = col;
  return tok;
}

Token QLangLexer::stringLiteral(bool triple, char delim, bool singleQuote) {
  int line = m_line, col = m_col;
  if (triple) {
    advance(); advance(); advance(); // opening delimiters (three quote chars)
  } else {
    advance(); // opening delimiter
  }
  std::string text;

  if (triple) {
    // Read until the closing triple delimiter (or EOF).
    bool closed = false;
    while (!atEnd()) {
      if (peek() == delim && peek(1) == delim && peek(2) == delim) {
        advance();
        advance();
        advance();
        closed = true;
        break;
      }
      text.push_back(advance());
    }
    // Only unterminated if we ran off EOF without finding the closing delimiter.
    if (!closed)
      throw QLangError(QLangErrorKind::Lex, line, col,
                       "unterminated multiline string");
  } else {
    // Single-line string or char literal.
    bool closed = false;
    while (!atEnd()) {
      char c = advance();
      if (c == '\\') {
        // Escape sequence: take next char literally.
        if (atEnd()) break;
        char e = advance();
        switch (e) {
        case 'n': text.push_back('\n'); break;
        case 't': text.push_back('\t'); break;
        case 'r': text.push_back('\r'); break;
        case '\\': text.push_back('\\'); break;
        case '"': text.push_back('"'); break;
        case '\'': text.push_back('\''); break;
        default: text.push_back(e); break;
        }
      } else if (c == delim) {
        closed = true;
        break;
      } else {
        text.push_back(c);
      }
    }
    if (!closed)
      throw QLangError(QLangErrorKind::Lex, line, col,
                       "unterminated string literal");
  }

  Token tok;
  tok.text = text;
  tok.value = text;
  tok.line = line;
  tok.col = col;
  // A single-quoted, non-triple literal is a char (byte); triple and
  // double-quoted are strings.
  tok.type = (triple || !singleQuote) ? TokenType::String : TokenType::Char;
  return tok;
}

// f-string: f"..." or f'...' — parses content with ${...} interpolation.
// The raw template (with ${...} intact) is stored in token.value.
// Escapes are processed; ${...} sections are left as-is for the parser.
Token QLangLexer::fstringLiteral(bool triple, char delim, bool singleQuote) {
  int line = m_line, col = m_col;
  if (triple) {
    advance(); advance(); advance(); // opening delimiters (three quote chars)
  } else {
    advance(); // opening delimiter (the quote char)
  }
  std::string text;

  if (triple) {
    bool closed = false;
    while (!atEnd()) {
      if (peek() == delim && peek(1) == delim && peek(2) == delim) {
        advance(); advance(); advance();
        closed = true;
        break;
      }
      // Check for ${...} interpolation — preserve it verbatim.
      if (peek() == '$' && peek(1) == '{') {
        // Read the ${...} block verbatim.
        text.push_back('$');
        text.push_back('{');
        advance(); advance(); // skip ${
        int braceDepth = 1;
        while (!atEnd() && braceDepth > 0) {
          char c = advance();
          text.push_back(c);
          if (c == '{') braceDepth++;
          else if (c == '}') braceDepth--;
        }
        if (braceDepth > 0)
          throw QLangError(QLangErrorKind::Lex, line, col,
                           "unterminated interpolation in f-string");
      } else {
        text.push_back(advance());
      }
    }
    if (!closed)
      throw QLangError(QLangErrorKind::Lex, line, col,
                       "unterminated multiline f-string");
  } else {
    bool closed = false;
    while (!atEnd()) {
      char c = advance();
      if (c == '\\') {
        if (atEnd()) break;
        char e = advance();
        switch (e) {
        case 'n': text.push_back('\n'); break;
        case 't': text.push_back('\t'); break;
        case 'r': text.push_back('\r'); break;
        case '\\': text.push_back('\\'); break;
        case '"': text.push_back('"'); break;
        case '\'': text.push_back('\''); break;
        default: text.push_back(e); break;
        }
      } else if (c == '$' && peek() == '{') {
        // Preserve ${...} verbatim.
        text.push_back('$');
        text.push_back('{');
        advance(); // skip '{'
        int braceDepth = 1;
        while (!atEnd() && braceDepth > 0) {
          char ec = advance();
          text.push_back(ec);
          if (ec == '{') braceDepth++;
          else if (ec == '}') braceDepth--;
        }
        if (braceDepth > 0)
          throw QLangError(QLangErrorKind::Lex, line, col,
                           "unterminated interpolation in f-string");
      } else if (c == delim) {
        closed = true;
        break;
      } else {
        text.push_back(c);
      }
    }
    if (!closed)
      throw QLangError(QLangErrorKind::Lex, line, col,
                       "unterminated f-string literal");
  }

  Token tok;
  tok.text = "f" + std::string(1, delim) + text + std::string(1, delim);
  tok.value = text;
  tok.line = line;
  tok.col = col;
  tok.type = TokenType::FString;
  return tok;
}

Token QLangLexer::identifier() {
  int line = m_line, col = m_col;
  std::string text;
  while (!atEnd()) {
    char c = peek();
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
      text.push_back(advance());
    } else {
      break;
    }
  }
  Token tok;
  tok.type = keywordType(text);
  tok.text = text;
  tok.value = text;
  tok.line = line;
  tok.col = col;
  return tok;
}

Token QLangLexer::operatorToken() {
  int line = m_line, col = m_col;
  char c = peek();
  char n = peek(1);
  char t = peek(2);
  auto make = [&](TokenType tokType, std::string text, int count) {
    for (int i = 0; i < count; i++)
      advance();
    Token tok;
    tok.type = tokType;
    tok.text = std::move(text);
    tok.value = tok.text;
    tok.line = line;
    tok.col = col;
    return tok;
  };

  // Compound assignment operators (check before their bare forms).
  if (c == '<' && n == '<' && t == '=')
    return make(TokenType::ShiftLeft, "<<=", 3);
  if (c == '>' && n == '>' && t == '=')
    return make(TokenType::ShiftRight, ">>=", 3);
  if ((c == '+' || c == '-' || c == '*' || c == '/' || c == '&' || c == '|' ||
       c == '^') &&
      n == '=') {
    std::string text;
    text.push_back(c);
    text.push_back('=');
    return make(TokenType::Assign, text, 2);
  }

  // Two-character operators.
  if (c == '=' && n == '=') return make(TokenType::Equal, "==", 2);
  if (c == '!' && n == '=') return make(TokenType::NotEqual, "!=", 2);
  if (c == '!' && n != '=') return make(TokenType::Not, "!", 1);
  if (c == '<' && n == '=') return make(TokenType::LessEqual, "<=", 2);
  if (c == '>' && n == '=') return make(TokenType::GreaterEqual, ">=", 2);
  if (c == '<' && n == '<') return make(TokenType::ShiftLeft, "<<", 2);
  if (c == '>' && n == '>') return make(TokenType::ShiftRight, ">>", 2);
  if (c == '&' && n == '&') return make(TokenType::LogicalAnd, "&&", 2);
  if (c == '|' && n == '|' && t == '|')
    return make(TokenType::LogicalOr3, "|||", 3);
  if (c == '|' && n == '|') return make(TokenType::LogicalOr, "||", 2);
  if (c == '*' && n == '*') return make(TokenType::Exponential, "**", 2);
  if (c == '+' && n == '+') return make(TokenType::Increment, "++", 2);
  if (c == '-' && n == '-') return make(TokenType::Decrement, "--", 2);
  // :: scope resolution (ClassName::staticMember) - before single ':'
  if (c == ':' && n == ':') return make(TokenType::ScopeRes, "::", 2);
  // $$ operator (indirect access) - must be checked before $
  if (c == '$' && n == '$') return make(TokenType::DollarDollar, "$$", 2);
  // $ operator (dynamic key resolution) - must be checked after $$
  if (c == '$') return make(TokenType::Dollar, "$", 1);

  // Single-character operators / punctuation.
  switch (c) {
  case '=': return make(TokenType::Assign, "=", 1);
  case '<': return make(TokenType::Less, "<", 1);
  case '>': return make(TokenType::Greater, ">", 1);
  case '^': return make(TokenType::Xor, "^", 1);
  case '&': return make(TokenType::And, "&", 1);
  case '|': return make(TokenType::Or, "|", 1);
  case '+': return make(TokenType::Plus, "+", 1);
  case '-': return make(TokenType::Minus, "-", 1);
  case '*': return make(TokenType::Star, "*", 1);
  case '/': return make(TokenType::Slash, "/", 1);
  case '%': return make(TokenType::Modulo, "%", 1);
  case '(': return make(TokenType::LParen, "(", 1);
  case ')': return make(TokenType::RParen, ")", 1);
  case '{': return make(TokenType::LBrace, "{", 1);
  case '}': return make(TokenType::RBrace, "}", 1);
  case '[': return make(TokenType::LBracket, "[", 1);
  case ']': return make(TokenType::RBracket, "]", 1);
  case ';': return make(TokenType::Semicolon, ";", 1);
  case ':': return make(TokenType::Colon, ":", 1);
  case ',': return make(TokenType::Comma, ",", 1);
  case '.': return make(TokenType::Dot, ".", 1);
  }

  throw QLangError(QLangErrorKind::Lex, line, col,
                   std::string("unexpected character '") + c + "'");
}

std::vector<Token> QLangLexer::tokenize() {
  std::vector<Token> tokens;
  while (!atEnd()) {
    skipWhitespaceAndComments();
    if (atEnd()) break;

    char c = peek();

    if (std::isdigit(static_cast<unsigned char>(c))) {
      tokens.push_back(number());
      continue;
    }

    if (c == '"' || c == '\'') {
      // Triple-quoted?
      if (peek(1) == c && peek(2) == c) {
        tokens.push_back(stringLiteral(true, c, false));
      } else {
        tokens.push_back(stringLiteral(false, c, c == '\''));
      }
      continue;
    }

    // f-string: f"..." or f'...'  (must be before identifier check)
    if (c == 'f' && !atEnd() && (peek(1) == '"' || peek(1) == '\'')) {
      advance(); // consume 'f'
      char delim = peek();  // peek() now returns the quote char
      bool triple = (peek(1) == delim && peek(2) == delim);
      if (triple) {
        tokens.push_back(fstringLiteral(true, delim, false));
      } else {
        tokens.push_back(fstringLiteral(false, delim, delim == '\''));
      }
      continue;
    }

    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      tokens.push_back(identifier());
      continue;
    }

    // Operators, punctuation and multi-char literals.
    // `++`/`--` are handled by operatorToken; a lone `+`/`-` too.
    tokens.push_back(operatorToken());
  }

  Token eof;
  eof.type = TokenType::EndOfFile;
  eof.text = "";
  eof.line = m_line;
  eof.col = m_col;
  tokens.push_back(eof);
  return tokens;
}

} // namespace qlang
