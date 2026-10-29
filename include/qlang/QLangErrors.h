#pragma once

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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

// One stack frame of an execution trace (SPEC # error reporting): the
// function name and the source line of the instruction executing in that
// frame. The VM fills these innermost-first when an error is thrown.
struct QLangFrameRef {
  std::string name;
  int line = 0;
};

// All QLang failures derive from this. Line/column are 1-based and are
// attached to every error so messages can point at the offending source.
// `module` and `frames` are optional context (SPEC # error reporting): the
// owning module name and the execution trace, innermost first.
class QLangError : public std::runtime_error {
public:
  QLangError(QLangErrorKind kind, int line, int col, std::string msg,
             std::string module = "", std::vector<QLangFrameRef> frames = {})
      : std::runtime_error(format(kind, line, col, msg)),
        m_kind(kind), m_line(line), m_col(col), m_msg(std::move(msg)),
        m_module(std::move(module)), m_frames(std::move(frames)) {}

  QLangErrorKind kind() const { return m_kind; }
  int line() const { return m_line; }
  int col() const { return m_col; }
  const std::string &module() const { return m_module; }
  const std::vector<QLangFrameRef> &frames() const { return m_frames; }
  // The raw failure message, without the kind/line/col prefix.
  const std::string &message() const { return m_msg; }

  static std::string kindName(QLangErrorKind kind) {
    switch (kind) {
    case QLangErrorKind::Lex: return "lex error";
    case QLangErrorKind::Parse: return "parse error";
    case QLangErrorKind::Type: return "type error";
    case QLangErrorKind::Runtime: return "runtime error";
    case QLangErrorKind::Syntax: return "syntax error";
    case QLangErrorKind::Io: return "io error";
    }
    return "error";
  }

  // The fully-formatted report (SPEC # error reporting):
  //   module 'main' line 12, col 5 in function 'foo': <kind>: <message>
  //   at line 12 in foo
  //   at line 3 in main
  // Missing pieces (no line, no frames) are omitted.
  static std::string formatReport(const QLangError &e) {
    std::string s;
    if (!e.m_module.empty())
      s += "module '" + e.m_module + "' ";
    if (e.m_line > 0)
      s += "line " + std::to_string(e.m_line) + ", col " +
           std::to_string(e.m_col) + " ";
    if (!e.m_frames.empty())
      s += "in function '" + e.m_frames.front().name + "' ";
    s += kindName(e.m_kind) + ": " + e.m_msg;
    for (const auto &f : e.m_frames)
      s += "\nat line " + std::to_string(f.line) + " in " + f.name;
    return s;
  }

  // Re-throw helper used by QLangContext::run(): a new error whose what()
  // is the fully-formatted report. Kind/line/col/module/frames are kept.
  static QLangError report(const QLangError &e) {
    return QLangError(ReportTag{}, e.m_kind, e.m_line, e.m_col, e.m_msg,
                      e.m_module, e.m_frames, formatReport(e));
  }

private:
  struct ReportTag {};
  QLangError(ReportTag, QLangErrorKind kind, int line, int col,
             std::string msg, std::string module,
             std::vector<QLangFrameRef> frames, std::string what)
      : std::runtime_error(std::move(what)),
        m_kind(kind), m_line(line), m_col(col), m_msg(std::move(msg)),
        m_module(std::move(module)), m_frames(std::move(frames)) {}

  static std::string format(QLangErrorKind kind, int line, int col,
                            const std::string &msg) {
    return kindName(kind) + " at line " + std::to_string(line) + "," +
           std::to_string(col) + ": " + msg;
  }

  QLangErrorKind m_kind;
  int m_line;
  int m_col;
  std::string m_msg;
  std::string m_module;
  std::vector<QLangFrameRef> m_frames;
};

} // namespace qlang
