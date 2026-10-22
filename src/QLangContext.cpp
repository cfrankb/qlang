#include "qlang/QLangContext.h"

#include <utility>

#include "qlang/QLangCompiler.h"
#include "qlang/QLangErrors.h"
#include "qlang/QLangLexer.h"
#include "qlang/QLangVM.h"

namespace qlang
{

  QLangContext::QLangContext() = default;

  std::vector<StmtPtr> QLangContext::parse(const std::string &source)
  {
    QLangLexer lexer(source);
    std::vector<Token> tokens = lexer.tokenize();
    QLangParser parser(std::move(tokens));
    return parser.parseProgram();
  }

  void QLangContext::run(const std::string &source)
  {
    // Pipeline: source -> AST -> bytecode -> execute.
    std::vector<StmtPtr> program = parse(source);
    QLangCompiler compiler(std::move(program));
    compiler.addExternalConsts(m_injectedConsts);
    std::unordered_set<std::string> nativeNames;
    for (const auto &[name, _] : m_nativeFuncs)
      nativeNames.insert(name);
    compiler.addExternalFuncs(nativeNames);
    CompiledProgram compiled = compiler.compile();
    // Populate the compiled program's native function table.
    for (const auto &[name, func] : m_nativeFuncs) {
      auto it = compiler.nativeFuncIds().find(name);
      if (it != compiler.nativeFuncIds().end()) {
        int idx = -1 - it->second;
        while (compiled.nativeFuncs.size() <= static_cast<size_t>(idx))
          compiled.nativeFuncs.push_back(nullptr);
        compiled.nativeFuncs[static_cast<size_t>(idx)] = func;
      }
    }
    QLangVM vm(compiled, *this);
    vm.run();
  }

  bool QLangContext::has(const std::string &name) const
  {
    return m_vars.find(name) != m_vars.end();
  }

  bool QLangContext::isset(const std::string &name) const
  {
    return m_vars.find(name) != m_vars.end();
  }

  QLangValue QLangContext::get(const std::string &name) const
  {
    auto it = m_vars.find(name);
    if (it == m_vars.end())
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "undefined variable '" + name + "'");
    return it->second;
  }

  void QLangContext::set(const std::string &name, const QLangValue &value, const bool isConst)
  {
    m_vars[name] = value;
    if (isConst)
      m_injectedConsts.insert(name);
    else
      m_injectedConsts.erase(name);
  }

  void QLangContext::remove(const std::string &name)
  {
    m_vars.erase(name);
    m_injectedConsts.erase(name);
  }

  void QLangContext::clear()
  {
    m_vars.clear();
    m_injectedConsts.clear();
    m_nativeFuncs.clear();
  }

  void QLangContext::setFunc(const std::string &name, NativeFunc func)
  {
    m_nativeFuncs[name] = std::move(func);
  }

} // namespace qlang
