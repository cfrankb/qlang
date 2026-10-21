#include "qlang/QLangContext.h"

#include <utility>

#include "qlang/QLangCompiler.h"
#include "qlang/QLangErrors.h"
#include "qlang/QLangLexer.h"
#include "qlang/QLangVM.h"

namespace qlang {

QLangContext::QLangContext() = default;

std::vector<StmtPtr> QLangContext::parse(const std::string &source) {
  QLangLexer lexer(source);
  std::vector<Token> tokens = lexer.tokenize();
  QLangParser parser(std::move(tokens));
  return parser.parseProgram();
}

void QLangContext::run(const std::string &source) {
  // Pipeline: source -> AST -> bytecode -> execute.
  std::vector<StmtPtr> program = parse(source);
  CompiledProgram compiled = QLangCompiler(std::move(program)).compile();
  QLangVM vm(compiled, *this);
  vm.run();
}

bool QLangContext::has(const std::string &name) const {
  return m_vars.find(name) != m_vars.end();
}

QLangValue QLangContext::get(const std::string &name) const {
  auto it = m_vars.find(name);
  if (it == m_vars.end())
    throw QLangError(QLangErrorKind::Runtime, 0, 0,
                     "undefined variable '" + name + "'");
  return it->second;
}

void QLangContext::set(const std::string &name, const QLangValue &value) {
  m_vars[name] = value;
}

void QLangContext::remove(const std::string &name) { m_vars.erase(name); }

void QLangContext::clear() { m_vars.clear(); }

} // namespace qlang
