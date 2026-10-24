#include "qlang/QLangContext.h"

#include <utility>

#include "qlang/QLangCompiler.h"
#include "qlang/QLangErrors.h"
#include "qlang/QLangLexer.h"
#include "qlang/QLangVM.h"
#include "qlang/QLangModules.h"

namespace qlang
{

namespace {
  // Convert a QLangValue into an integer verbosity level (0 for non-numeric /
  // nil). Used by the set_verbose() builtin below.
  int levelFromValue(const QLangValue &v) {
    switch (v.type()) {
      case QLangValue::Type::Int:   return static_cast<int>(v.asInt());
      case QLangValue::Type::Dword: return static_cast<int>(static_cast<long long>(v.asDword()));
      case QLangValue::Type::Float: return static_cast<int>(v.asFloat());
      default:                      return 0;
    }
  }

  // QLang-callable: set_verbose(level). Adjusts the application-wide verbose
  // level used to gate diagnostic trace output; returns the new level.
  QLangValue setVerboseFunc(QLangContext &ctx, const std::vector<QLangValue> &args) {
    int lvl = levelFromValue(args.empty() ? QLangValue() : args[0]);
    ctx.setVerboseLevel(lvl);
    return QLangValue::Int(lvl);
  }
}

  // Pimpl definition — breaks the circular include with QLangCompiler.h.
  struct QLangContext::Pimpl
  {
    std::optional<CompiledProgram> compiled;
  };

  QLangContext::QLangContext()
  {
    m_pimpl = std::make_unique<Pimpl>();
    registerBuiltinModules(*this);
    // Application-wide verbose level, adjustable from QLang via set_verbose().
    setFunc("set_verbose", setVerboseFunc);
  }

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
    compiler.addModules(m_modules);
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
    // Also copy module-resolved native functions into compiled.nativeFuncs.
    const auto &resolved = compiler.nativeFuncsResolved();
    for (size_t i = 0; i < resolved.size(); i++) {
      while (compiled.nativeFuncs.size() <= i)
        compiled.nativeFuncs.push_back(nullptr);
      if (resolved[i])
        compiled.nativeFuncs[i] = resolved[i];
    }
    // Cache the compiled program for subsequent call() invocations.
    m_pimpl->compiled = std::move(compiled);
    QLangVM vm(m_pimpl->compiled.value(), *this);
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
    m_modules.clear();
    m_pimpl->compiled.reset();
  }

  void QLangContext::setFunc(const std::string &name, NativeFunc func)
  {
    m_nativeFuncs[name] = std::move(func);
  }

  void QLangContext::registerModule(const std::string &name, const Module &module)
  {
    for (const auto &[fname, _] : module.funcs) {
      // Check for name collision with existing native funcs.
      if (m_nativeFuncs.count(fname) && m_nativeFuncs.count(fname) /* already registered */) {
        // Silently allow override — last registration wins.
      }
    }
    m_modules.push_back(module);
    if (m_modules.back().name != name)
      m_modules.back().name = name;
  }

  void QLangContext::importModule(const std::string &name, const std::string &alias)
  {
    // Find the module by its registered name.
    const Module *found = nullptr;
    for (auto &mod : m_modules) {
      if (mod.name == name) { found = &mod; break; }
    }
    if (!found)
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "unknown module '" + name + "'");
    // Register under the requested name (alias if given, otherwise original).
    Module copy = *found;
    copy.name = alias.empty() ? name : alias;
    registerModule(copy.name, copy);
  }

  void QLangContext::loadBuiltinModules()
  {
    registerBuiltinModules(*this);
  }

  QLangContext::~QLangContext() = default;

  QLangValue QLangContext::call(const std::string &name, const std::vector<QLangValue> &args)
  {
    // Check native funcs first.
    auto it = m_nativeFuncs.find(name);
    if (it != m_nativeFuncs.end())
      return it->second(*this, args);

    // Ensure we have a compiled program.
    if (!m_pimpl->compiled.has_value())
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "no program compiled; call run() first");

    // Look up the function by name in the compiled program.
    const auto &functions = m_pimpl->compiled.value().functions;
    int funcIndex = -1;
    for (int i = static_cast<int>(functions.size()) - 1; i >= 1; --i) {
      if (functions[static_cast<size_t>(i)].name == name) {
        funcIndex = i;
        break;
      }
    }
    if (funcIndex < 0)
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "undefined function '" + name + "'");

    QLangVM vm(m_pimpl->compiled.value(), *this);
    return vm.callFunction(funcIndex, args);
  }

} // namespace qlang
