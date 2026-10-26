#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace qlang {

// Classification of a failure, useful for callers that want to react
// differently to, say, a lex error vs. a type error.
enum class QLangErrorKind {
  Lex,   // bad characters / unterminated string
  Parse, // grammar violations
  Type,  // incompatible value operations
  Runtime, // execution-time failure
  Syntax, // generic syntax problem
  Io     // .qlc read/write failure (bad magic, truncation, malformed data)
};

// All QLang failures derive from this. Line/column are 1-based and are
// attached to every error so messages can point at the offending source.
class QLangError : public std::runtime_error {
public:
  QLangError(QLangErrorKind kind, int line, int col, std::string msg)
      : std::runtime_error(format(kind, line, col, std::move(msg))),
        m_kind(kind), m_line(line), m_col(col) {}

  QLangErrorKind kind() const { return m_kind; }
  int line() const { return m_line; }
  int col() const { return m_col; }

private:
  static std::string format(QLangErrorKind kind, int line, int col,
                            std::string msg) {
    const char *kindName = "error";
    switch (kind) {
    case QLangErrorKind::Lex: kindName = "lex error"; break;
    case QLangErrorKind::Parse: kindName = "parse error"; break;
    case QLangErrorKind::Type: kindName = "type error"; break;
    case QLangErrorKind::Runtime: kindName = "runtime error"; break;
    case QLangErrorKind::Syntax: kindName = "syntax error"; break;
    case QLangErrorKind::Io: kindName = "io error"; break;
    }
    return std::string(kindName) + " at line " + std::to_string(line) +
           "," + std::to_string(col) + ": " + msg;
  }

  QLangErrorKind m_kind;
  int m_line;
  int m_col;
};

} // namespace qlang
