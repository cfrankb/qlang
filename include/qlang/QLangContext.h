#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "QLangParser.h"
#include "QLangValue.h"

namespace qlang {

// Host for the interpreter. Owns the variable scope and drives lexing,
// parsing and (in later versions) execution. The first version exposes
// parsing of source text plus a variable lookup API.
class QLangContext {
public:
  QLangContext();

  // Parse source text into a list of statements. Throws QLangError on
  // lex/parse failure.
  std::vector<StmtPtr> parse(const std::string &source);

  // Parse, compile to bytecode, and execute. Final variable state is written
  // back into this context's scope. Throws QLangError on lex/parse/compile/run
  // failure.
  void run(const std::string &source);

  // Variable scope access.
  bool has(const std::string &name) const;
  QLangValue get(const std::string &name) const;
  void set(const std::string &name, const QLangValue &value);
  void remove(const std::string &name);
  void clear();

private:
  std::unordered_map<std::string, QLangValue> m_vars;
};

} // namespace qlang
