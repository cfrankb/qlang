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
      case QLangValue::Type::Qword: return static_cast<int>(static_cast<long long>(v.asQword()));
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

  // QLang-callable: set_max_steps(n) / get_max_steps(). Lowers/raises the VM's
  // live infinite-loop step limit; a value of 0 (or a missing arg) restores the
  // default (100000). set_max_steps returns the new limit; get_max_steps
  // returns the current limit. Tests use this to fail fast on a runaway loop.
  QLangValue setMaxStepsFunc(QLangContext &ctx, const std::vector<QLangValue> &args) {
    std::size_t n = 0;
    if (!args.empty()) {
      const QLangValue &v = args[0];
      switch (v.type()) {
        case QLangValue::Type::Int:   n = static_cast<std::size_t>(v.asInt());   break;
        case QLangValue::Type::Qword: n = static_cast<std::size_t>(v.asQword()); break;
        case QLangValue::Type::Float: n = static_cast<std::size_t>(v.asFloat()); break;
        default:                      n = 0; break;
      }
    }
    if (n == 0) n = QLangVM::kMaxSteps; // 0 / missing -> default
    ctx.setMaxSteps(n);
    return QLangValue::Int(static_cast<int>(n));
  }
  QLangValue getMaxStepsFunc(QLangContext &ctx, const std::vector<QLangValue> &) {
    return QLangValue::Int(static_cast<int>(ctx.maxSteps()));
  }
}

  // Pimpl definition — breaks the circular include with QLangCompiler.h.
  struct QLangContext::Pimpl
  {
    std::optional<CompiledProgram> compiled;
    // Every program compiled by run() stays alive here so instance
    // finalizers (`__destroy__`) can still dispatch their closures after the
    // run() VM is gone (Phase 7). Entries are shared_ptrs; cleared by
    // clear() only AFTER m_vars (which owns the instances) is cleared.
    std::vector<std::shared_ptr<CompiledProgram>> livePrograms;
  };

  QLangContext::QLangContext()
  {
    m_pimpl = std::make_unique<Pimpl>();
    registerBuiltinModules(*this);
    // Application-wide verbose level, adjustable from QLang via set_verbose().
    setFunc("set_verbose", setVerboseFunc);
    // VM step limit, adjustable from QLang via set_max_steps(n) / get_max_steps().
    setFunc("set_max_steps", setMaxStepsFunc);
    setFunc("get_max_steps", getMaxStepsFunc);
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
    QLangCompiler compiler(std::move(program), this);
    compiler.addExternalConsts(m_injectedConsts);
    std::unordered_set<std::string> externalVars;
    for (const auto &[name, _] : m_vars)
      if (m_injectedConsts.find(name) == m_injectedConsts.end())
        externalVars.insert(name);
    compiler.addExternalVars(externalVars);
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
    // Cache the compiled program for subsequent call() invocations, and keep
    // it alive in livePrograms so instance finalizers can dispatch against
    // it later (Phase 7).
    auto live = std::make_shared<CompiledProgram>(std::move(compiled));
    m_pimpl->compiled = *live;
    m_pimpl->livePrograms.push_back(std::move(live));
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
    // m_vars first: releasing the instances may fire `__destroy__`
    // finalizers, which need livePrograms still intact (Phase 7).
    m_vars.clear();
    m_injectedConsts.clear();
    m_nativeFuncs.clear();
    m_modules.clear();
    m_pimpl->livePrograms.clear();
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

  QLangValue QLangContext::callClosure(const QLangValue &closure, const std::vector<QLangValue> &args)
  {
    // Ensure we have a compiled program.
    if (!m_pimpl->compiled.has_value())
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "no program compiled; call run() first");

    // Verify the closure is a Function type.
    if (closure.type() != QLangValue::Type::Function)
      throw QLangError(QLangErrorKind::Type, 0, 0,
                       "expected closure value");

    QLangVM vm(m_pimpl->compiled.value(), *this);
    return vm.callClosure(closure, args);
  }

  int QLangContext::currentProgramId() const
  {
    return static_cast<int>(m_pimpl->livePrograms.size()) - 1;
  }

  void QLangContext::runFinalizer(std::map<std::string, QLangValue> *tbl,
                                  const QLangValue &closure, int programId)
  {
    // Guard: if the context was cleared (or is being destroyed) the program
    // is gone — skip the finalizer rather than dispatch into a dead program.
    if (programId < 0 ||
        programId >= static_cast<int>(m_pimpl->livePrograms.size()))
      return;
    const auto &prog = m_pimpl->livePrograms[static_cast<size_t>(programId)];
    if (!prog)
      return;
    // Own the table transiently with a no-op deleter so the finalizer body
    // can read/write `self`; the caller's custom deleter destroys the map
    // after this returns. The transient ref also keeps the refcount > 0
    // during the call, so the finalizer cannot recursively re-enter it.
    QLangValue instance = QLangValue::Table(
        std::shared_ptr<std::map<std::string, QLangValue>>(
            tbl, [](std::map<std::string, QLangValue> *) {}));
    try {
      QLangVM vm(*prog, *this);
      vm.callClosure(closure, {instance});
    } catch (const std::exception &) {
      // Finalizer errors are swallowed: the deleter may run during stack
      // unwinding, where a rethrow would abort.
    } catch (...) {
    }
  }

} // namespace qlang
