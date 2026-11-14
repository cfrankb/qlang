#include "qlang/QLangContext.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <utility>

#include "qlang/QLangBytecodeIO.h"
#include "qlang/QLangCompiler.h"
#include "qlang/QLangErrors.h"
#include "qlang/QLangLexer.h"
#include "qlang/QLangManifest.h"
#include "qlang/QLangObjects.h"
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
      case QLangValue::Type::Long:  return static_cast<int>(static_cast<long long>(v.asLong()));
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
        case QLangValue::Type::Long:  n = static_cast<std::size_t>(v.asLong());  break;
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

  // The weak sender reference of an External value: the owned shared_ptr when
  // the script constructed the object, else the borrowed weak_ptr. A
  // connection stores this so it never keeps its sender alive.
  std::weak_ptr<void> senderWeakRef(const QLangValue &sender) {
    const auto &ref = sender.asExternal();
    if (ref.owned)
      return ref.owned;
    return ref.borrowed;
  }

  // The sender's object pointer (for identity matching), or null when the
  // borrow has expired.
  void *senderObjectPtr(const QLangValue &sender) {
    const auto &ref = sender.asExternal();
    if (ref.owned)
      return ref.owned.get();
    return ref.borrowed.lock().get();
  }

  // QLang-callable: connect(sender, "signal", handler) or
  // connect(sender, "signal", target, "method"). Returns a connection handle
  // (int). The arity dispatch is the only difference between the two forms.
  QLangValue connectFunc(QLangContext &ctx, const std::vector<QLangValue> &args) {
    if (args.size() == 3)
      return QLangValue::Int(
          ctx.connectSignal(args[0], args[1].asString(), args[2]));
    if (args.size() == 4)
      return QLangValue::Int(ctx.connectSignalSlot(args[0], args[1].asString(),
                                                   args[2], args[3].asString()));
    throw QLangError(QLangErrorKind::Type, 0, 0,
                     "connect expects 3 or 4 arguments");
  }

  // QLang-callable: disconnect(handle) or disconnect(sender, "signal").
  QLangValue disconnectFunc(QLangContext &ctx, const std::vector<QLangValue> &args) {
    if (args.size() == 1) {
      ctx.disconnectSignal(static_cast<int>(toInt64(args[0])));
      return QLangValue();
    }
    if (args.size() == 2) {
      ctx.disconnectSignalAll(args[0], args[1].asString());
      return QLangValue();
    }
    throw QLangError(QLangErrorKind::Type, 0, 0,
                     "disconnect expects 1 or 2 arguments");
  }
}

  // Program cache (Phase 3, H1): the compiled program(s) for a context,
  // split out of QLangContext so the context is a thin public API. The
  // cached program is the last entry of livePrograms (run() stores the same
  // shared_ptr in both); livePrograms keeps every run() program alive so
  // instance finalizers (`__destroy__`) can dispatch their closures after
  // the run() VM is gone (Phase 7). Entries are shared_ptrs (Phase 2:
  // finalizer deleters capture the shared_ptr directly — no int-index
  // protocol); cleared by clear() only AFTER m_globals (which owns the
  // instances) is cleared.
  class ProgramCache
  {
  public:
    void store(CompiledProgram prog)
    {
      auto live = std::make_shared<CompiledProgram>(std::move(prog));
      m_compiled = *live;
      m_livePrograms.push_back(std::move(live));
    }
    bool has() const { return m_compiled.has_value(); }
    const CompiledProgram &compiled() const { return m_compiled.value(); }
    std::shared_ptr<const CompiledProgram> live() const
    {
      if (m_livePrograms.empty())
        return nullptr;
      return m_livePrograms.back();
    }
    void clear()
    {
      m_livePrograms.clear();
      m_compiled.reset();
    }

  private:
    std::optional<CompiledProgram> m_compiled;
    std::vector<std::shared_ptr<CompiledProgram>> m_livePrograms;
  };

  // Type registry (Phase 3, H1): registered external C++ types, split out of
  // QLangContext. Shared handles: the registry holds one reference, the host
  // may hold others; clear() drops only the registry's, so host-held handles
  // stay valid.
  class TypeRegistry
  {
  public:
    std::shared_ptr<TypeDescriptor> add(std::shared_ptr<TypeDescriptor> d)
    {
      m_types.push_back(std::move(d));
      return m_types.back();
    }
    const TypeDescriptor *find(const std::string &name) const
    {
      for (const auto &t : m_types)
        if (t->name == name)
          return t.get();
      return nullptr;
    }
    bool contains(const std::string &name) const
    {
      return find(name) != nullptr;
    }
    void clear() { m_types.clear(); }

  private:
    std::vector<std::shared_ptr<TypeDescriptor>> m_types;
  };

  // Module registry (external-modules plan v4 Phase 3b): the `.qlc` library
  // images a context has loaded, keyed by the name `import` binds them under.
  // Lives in the pimpl for the same reason as ProgramCache — LoadedModule
  // embeds a CompiledProgram. Entries are shared_ptrs so a host or the linker
  // can keep an image alive past clear(). Duplicate names are rejected: two
  // images for one name would make `import <name>` ambiguous.
  class ModuleRegistry
  {
  public:
    std::shared_ptr<LoadedModule> add(std::shared_ptr<LoadedModule> m)
    {
      const std::string &name = m->program.moduleName;
      for (const auto &e : m_modules)
        if (e->program.moduleName == name)
          throw QLangError(QLangErrorKind::Io, 0, 0,
                           "module '" + name + "' is already loaded");
      m_modules.push_back(std::move(m));
      return m_modules.back();
    }
    std::shared_ptr<LoadedModule> find(const std::string &name) const
    {
      for (const auto &e : m_modules)
        if (e->program.moduleName == name)
          return e;
      return nullptr;
    }
    // Resolutions already performed, keyed by the name that was ASKED FOR
    // (SPEC §api "module search paths": "a module is resolved once per
    // context"). The registry keys on the module's own declared name, which a
    // file's name may disagree with (`foo.qlc` declaring MODULE_NAME = "bar"),
    // so the requested name needs its own entry or every later `import foo`
    // would search again and collide with the loaded `bar`.
    void remember(const std::string &requested,
                  const std::shared_ptr<LoadedModule> &m)
    {
      m_resolved[requested] = m;
    }
    std::shared_ptr<LoadedModule> resolved(const std::string &requested) const
    {
      const auto it = m_resolved.find(requested);
      return it == m_resolved.end() ? nullptr : it->second;
    }
    const std::vector<std::shared_ptr<LoadedModule>> &all() const
    {
      return m_modules;
    }
    // Undo an add()/remember() pair when taking the module in failed midway
    // (a dependency of the newly loaded image resolved nowhere): the registry
    // must not keep a module the caller was told about as an error. Cache
    // entries pointing at the removed image go too, whatever name they were
    // requested under.
    void remove(const std::string &name)
    {
      for (auto it = m_modules.begin(); it != m_modules.end(); ++it)
        if ((*it)->program.moduleName == name)
        {
          m_modules.erase(it);
          break;
        }
      for (auto it = m_resolved.begin(); it != m_resolved.end();)
        if (it->second && it->second->program.moduleName == name)
          it = m_resolved.erase(it);
        else
          ++it;
    }
    void forget(const std::string &requested) { m_resolved.erase(requested); }
    void clear()
    {
      m_modules.clear();
      m_resolved.clear();
    }

  private:
    std::vector<std::shared_ptr<LoadedModule>> m_modules;
    std::unordered_map<std::string, std::shared_ptr<LoadedModule>> m_resolved;
  };

  // Pimpl definition — breaks the circular include with QLangCompiler.h.
  // Phase 3 (H1): the context's program cache and type registry are separate
  // classes; the context itself is a thin public API.
  struct QLangContext::Pimpl
  {
    ProgramCache programs;
    TypeRegistry types;
    ModuleRegistry modules;
  };

  QLangContext::QLangContext()
  {
    m_pimpl = std::make_unique<Pimpl>();
    // The default search path is the working directory AT CONSTRUCTION
    // (SPEC §api "module search paths") — snapshotted, not re-read per lookup,
    // so a host that chdir()s keeps resolving against where it started. An
    // empty list (a context built where the cwd is unreadable, or a host that
    // calls setModuleSearchPaths({})) simply disables file discovery.
    std::error_code ec;
    const std::string cwd = std::filesystem::current_path(ec).string();
    if (!ec && !cwd.empty())
      m_moduleSearchPaths.push_back(cwd);
    registerBuiltinModules(*this);
    // Application-wide verbose level, adjustable from QLang via set_verbose().
    setFunc("set_verbose", setVerboseFunc);
    // VM step limit, adjustable from QLang via set_max_steps(n) / get_max_steps().
    setFunc("set_max_steps", setMaxStepsFunc);
    setFunc("get_max_steps", getMaxStepsFunc);
    // Signal connect/disconnect (docs/spec/17-signals.md): arity-dispatched
    // natives (3/4 args for connect, 1/2 for disconnect).
    setFunc("connect", connectFunc);
    setFunc("disconnect", disconnectFunc);
  }

  // Threading contract (Phase 5, H5): one context, one thread. The first use
  // records the owning thread; in debug builds (NDEBUG undefined) every
  // subsequent use asserts it is the same thread. In release builds the
  // assert compiles out, so this is a zero-cost no-op beyond the id compare.
  // enqueueFinalizer is deliberately NOT guarded: the instance table's
  // shared_ptr deleter may fire on any thread (even during unwinding) and
  // only enqueues — it never drives the VM.
  void QLangContext::bindThread() const
  {
    std::thread::id cur = std::this_thread::get_id();
    if (m_thread == std::thread::id()) {
      m_thread = cur;
      return;
    }
    assert(m_thread == cur && "QLangContext used from multiple threads");
  }

  std::vector<StmtPtr> QLangContext::parse(const std::string &source)
  {
    bindThread();
    QLangLexer lexer(source);
    std::vector<Token> tokens = lexer.tokenize();
    QLangParser parser(std::move(tokens));
    return parser.parseProgram();
  }

  void QLangContext::run(const std::string &source)
  {
    bindThread();
    // Safe point (Phase 3): drain instance finalizers enqueued since the
    // last dispatch before starting a new run.
    processFinalizers();
    try {
    // Pipeline: source -> AST -> bytecode -> execute.
    std::vector<StmtPtr> program = parse(source);
    QLangCompiler compiler(std::move(program), this);
    compiler.addExternalConsts(m_injectedConsts);
    // Host-injected enums (registerEnum): the names must resolve at compile
    // time (member folding, bare-enum reads); the runtime table is already
    // in the hidden const var.
    if (!m_injectedEnums.empty()) {
      std::unordered_map<std::string,
                         std::vector<std::pair<std::string, QLangValue>>> enums;
      for (const auto &[ename, ie] : m_injectedEnums)
        enums[ename] = ie.members;
      compiler.addExternalEnums(enums);
    }
    // External vars are the table's LIVE names: erased (deleted) slots are
    // not visible to the program, matching the old m_vars behavior.
    std::unordered_set<std::string> externalVars;
    for (const auto &[name, slot] : m_globals.names)
      if (m_injectedConsts.find(name) == m_injectedConsts.end() &&
          m_globals.erased.find(slot) == m_globals.erased.end())
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
    // it alive in the program cache so instance finalizers can dispatch
    // against it later (Phase 7).
    m_pimpl->programs.store(std::move(compiled));
    QLangVM vm(m_pimpl->programs.compiled(), *this);
    vm.run();
    } catch (const QLangError &e) {
      // Attach the full report (module + line/col + innermost function +
      // frame chain) so callers see one formatted message (SPEC # error
      // reporting).
      throw QLangError::report(e);
    }
  }

  // The host function an image's native-slot name asks for
  // (docs/SERIALIZATION.md §5). A bare name is a `registerFunc` native or any
  // registered module's function of that name, which is the spelling a flat
  // `from m import f` stores. A `module.func` name is what `import m` stores
  // (SPEC §modules "import statement"): the module that owns the qualifier
  // answers it, so a same-named function of another module cannot take the
  // slot.
  QLangContext::NativeFunc
  QLangContext::nativeNamed(const std::string &name) const {
    const size_t dot = name.find('.');
    if (dot != std::string::npos) {
      // A native literally registered under the dotted name wins; otherwise the
      // prefix is a module qualifier and only that module's function answers
      // it.
      const auto nit = m_nativeFuncs.find(name);
      if (nit != m_nativeFuncs.end())
        return nit->second;
      for (const auto &m : m_modules)
        if (m.name == name.substr(0, dot)) {
          const auto fit = m.funcs.find(name.substr(dot + 1));
          return fit == m.funcs.end() ? NativeFunc() : fit->second;
        }
      return NativeFunc();
    }
    const auto it = m_nativeFuncs.find(name);
    if (it != m_nativeFuncs.end())
      return it->second;
    for (const auto &m : m_modules) {
      const auto mit = m.funcs.find(name);
      if (mit != m.funcs.end())
        return mit->second;
    }
    return NativeFunc();
  }

  // Load a `.qlc` image into the context's module registry
  // (SPEC §bytecodes "external modules"; plan v4 Phase 3b). The image stores
  // native function NAMES only (docs/SERIALIZATION.md §4.3), so every slot is
  // re-resolved here against this context's natives and its registered C++
  // modules — a name that resolves nowhere is a load-time error rather than a
  // runtime nil call. `path` is used for diagnostics only.
  std::shared_ptr<LoadedModule> QLangContext::loadProgram(std::istream &is,
                                                          const std::string &path)
  {
    bindThread();
    auto mod = std::make_shared<LoadedModule>();
    mod->path = path;
    const std::string where = path.empty() ? std::string() : path + ": ";
    std::string err;
    if (!readQlc(is, mod->program, mod->nativeSlots, err))
      throw QLangError(QLangErrorKind::Io, 0, 0, where + err);
    // docs/SERIALIZATION.md §5.2: every missing name is reported in ONE error,
    // not fail-on-first — a library image typically imports a whole module, and
    // a host that forgot to register it wants the full list, not one name per
    // load attempt.
    std::vector<std::pair<int, NativeFunc>> resolved;
    std::vector<std::string> missing;
    resolved.reserve(mod->nativeSlots.size());
    for (const auto &[slot, name] : mod->nativeSlots) {
      const NativeFunc fn = nativeNamed(name);
      if (!fn) {
        missing.push_back(name);
        continue;
      }
      resolved.emplace_back(slot, fn);
    }
    if (!missing.empty()) {
      std::string list;
      for (const std::string &n : missing) {
        if (!list.empty())
          list += ", ";
        list += "'" + n + "'";
      }
      throw QLangError(QLangErrorKind::Io, 0, 0,
                       where + "missing native function(s): " + list);
    }
    auto &nf = mod->program.nativeFuncs;
    for (const auto &[slot, fn] : resolved) {
      while (nf.size() <= static_cast<size_t>(slot))
        nf.push_back(nullptr);
      nf[static_cast<size_t>(slot)] = fn;
    }
    return m_pimpl->modules.add(std::move(mod));
  }

  std::shared_ptr<LoadedModule> QLangContext::loadProgramFile(
      const std::string &path)
  {
    bindThread();
    std::ifstream in(path, std::ios::binary);
    if (!in)
      throw QLangError(QLangErrorKind::Io, 0, 0,
                       "cannot open module file '" + path + "'");
    return loadProgram(in, path);
  }

  // A `QPM1` package is fully parsed before the handshake and the handshake is
  // fully done before a single image is loaded: a package the host cannot serve
  // must leave the context exactly as it was, not half a module registry. The
  // fingerprint comparison is byte equality over the canonical entry form, so
  // host and manifest agree by construction (QLangManifest.h).
  std::shared_ptr<LoadedModule> QLangContext::loadPortableModuleFile(
      const std::string &path)
  {
    bindThread();
    std::ifstream in(path, std::ios::binary);
    if (!in)
      throw QLangError(QLangErrorKind::Io, 0, 0,
                       "cannot open portable module file '" + path + "'");
    const QpmPackage pkg = readQpm(in);

    for (const ManifestType &t : pkg.manifest)
    {
      const TypeDescriptor *d = m_pimpl->types.find(t.name);
      if (!d)
        throw QLangError(QLangErrorKind::Type, 0, 0,
                         "missing type '" + t.name +
                             "' required by portable module");
      if (serializeTypeEntry(manifestTypeFrom(*d)) != serializeTypeEntry(t))
        throw QLangError(QLangErrorKind::Type, 0, 0,
                         "external type '" + t.name +
                             "' does not match the portable module");
    }

    // The images are the package's own payloads, so no search path, callback or
    // source is consulted: a portable module is self-contained. Each one goes
    // through loadProgram, which is what re-resolves its native slots and keys
    // the registry by its declared module name.
    auto loadOne = [this, &path](const QpmImage &img) {
      std::istringstream is(img.bytes, std::ios::binary);
      return loadProgram(is, path + ":" + img.name);
    };
    std::shared_ptr<LoadedModule> entry;
    for (const QpmImage &img : pkg.images)
      if (img.name != pkg.entryName)
        loadOne(img);  // the libraries first, so the entry's imports resolve
    for (const QpmImage &img : pkg.images)
      if (img.name == pkg.entryName)
        entry = loadOne(img);
    return entry;
  }

  const LoadedModule *QLangContext::loadedModule(const std::string &name) const
  {
    bindThread();
    return m_pimpl->modules.find(name).get();
  }

  std::vector<std::shared_ptr<LoadedModule>> QLangContext::loadedModules() const
  {
    bindThread();
    return m_pimpl->modules.all();
  }

  // --- Module search paths + external search callback ---
  // SPEC §api "module search paths" and §api "external search callback";
  // plan docs/plans/missing-features-plan.md §C8a. The context owns the path
  // list and the callback; `findModule` is the one resolution order both the
  // compiler and a host go through.

  const std::vector<std::string> &QLangContext::moduleSearchPaths() const
  {
    bindThread();
    return m_moduleSearchPaths;
  }

  void QLangContext::setModuleSearchPaths(std::vector<std::string> paths)
  {
    bindThread();
    m_moduleSearchPaths = std::move(paths);
  }

  void QLangContext::addModuleSearchPath(std::string dir)
  {
    bindThread();
    m_moduleSearchPaths.push_back(std::move(dir));
  }

  void QLangContext::setModuleSearchCallback(ModuleSearchCallback callback)
  {
    bindThread();
    m_moduleSearchCallback = std::move(callback);
  }

  const QLangContext::ModuleSearchCallback &
  QLangContext::moduleSearchCallback() const
  {
    bindThread();
    return m_moduleSearchCallback;
  }

  void QLangContext::setOutputCallback(OutputCallback callback)
  {
    bindThread();
    m_outputCallback = std::move(callback);
  }

  const QLangContext::OutputCallback &
  QLangContext::outputCallback() const
  {
    bindThread();
    return m_outputCallback;
  }

  void QLangContext::output(int level, const std::string &message,
                            const std::string &file, int line) const
  {
    if (m_outputCallback)
    {
      // The contract says the callback must not throw. Output has no
      // reporting channel that is not the same broken callback, so a
      // violation is swallowed rather than unwound through the engine
      // (SPEC §api "output callback (universal)").
      try
      {
        m_outputCallback(message, level, file, line);
      }
      catch (...)
      {
      }
      return;
    }
    // Default routing, byte-identical to the pre-callback engine: program
    // output to stdout (the old `std::cout <<` sites), diagnostics to stderr
    // (the old `std::fputs(stderr)` sites).
    if (level == OUT_INFO)
      std::cout << message;
    else
      std::fputs(message.c_str(), stderr);
  }

  // Every module `mod` itself imports, each name resolved through findModule
  // (SPEC §api "module search paths"; plan C8a). An image's import table names
  // the DEFINING module of every name it bound, and only `.ql` imports appear
  // there — a C++ module's names become native slots — so the table is the
  // image's complete dependency list. Each dependency goes through the same
  // order (registry, callback, paths); one that resolves nowhere is reported
  // against the module that asked for it, because the link would otherwise fail
  // later saying only that a module is not loaded. Recursion terminates on the
  // registry: `mod` is registered before its dependencies are walked, so a cycle
  // finds it already resolved (and checkImportCycles still rejects the graph).
  static void resolveModuleDependencies(QLangContext &ctx,
                                        const LoadedModule &mod)
  {
    std::unordered_set<std::string> seen;
    for (const ImportEntry &imp : mod.program.imports)
    {
      if (imp.module.empty() || !seen.insert(imp.module).second)
        continue;
      if (ctx.findModule(imp.module))
        continue;
      std::string from = "module '" + mod.program.moduleName + "'";
      if (!mod.path.empty())
        from += " (found at " + mod.path + ")";
      throw QLangError(QLangErrorKind::Io, 0, 0,
                       from + " imports module '" + imp.module +
                           "', which could not be found in the module search "
                           "paths");
    }
  }

  // Compiles a discovered `.ql` source into an image and takes it in — the
  // other half of SPEC §api "module search paths" ("...before `[modulename].ql`,
  // which it compiles into an image and takes in by the same path"; plan C8b).
  //
  // The compile runs on a PRIVATE context, never here, for two reasons: a
  // module's top level declares globals and the global table belongs to a
  // context, so compiling here would leak the library's names into the entry's
  // global area; and a module image numbers its registers from 0 because
  // QLangLinker re-bases them at link time, while a compile here would start
  // them above this context's existing globals. The scratch borrows everything
  // that is not a global area — this context's natives and C++ modules — and
  // answers its own imports by asking `this`, so a dependency is discovered,
  // registered and cached ONCE here instead of once per scratch (and a cycle is
  // caught by one chain). Host-injected consts and enums are not carried over:
  // a library sees the same names a `.qlc` image of it would.
  //
  // The image goes out through the codec and back in through `loadProgram`, so
  // a compiled source and a `.qlc` file on disk share one path: natives are
  // stored as NAMES and re-resolved against this context (docs/SERIALIZATION.md
  // §4.3), and the registry keeps the same provenance either way.
  std::shared_ptr<LoadedModule>
  QLangContext::compileModuleSource(const std::string &path)
  {
    // Every diagnostic names the file it came from (SPEC §12 error reporting):
    // the compiler reports a failed resolution at the `import` statement, so
    // only the source file's own path can say which module is broken.
    const std::string where = path + ": ";

    // Cycles. No image exists until a module's compile finishes, so the import
    // graph the compiler checks (checkImportCycles) cannot see a nested one:
    // `a.ql` importing `b.ql` importing `a.ql` would recurse until the stack
    // gave up. The key is the FILE, not the requested name — a cycle is a file
    // reached again, whatever name each import spelled it as.
    std::error_code ec;
    std::string key = std::filesystem::absolute(path, ec).lexically_normal().string();
    if (ec || key.empty())
      key = path;
    for (const std::string &running : m_compilingSources)
      if (running == key)
      {
        std::string chain = "import cycle:";
        for (const std::string &c : m_compilingSources)
          chain += " " + c + " ->";
        throw QLangError(QLangErrorKind::Io, 0, 0,
                         chain + " " + key + " is already being compiled");
      }
    m_compilingSources.push_back(key);
    // A compile that threw must not leave its file marked: the next import of
    // an unrelated module would be told it is a cycle.
    struct InFlight
    {
      std::vector<std::string> &chain;
      ~InFlight() { chain.pop_back(); }
    } inFlight{m_compilingSources};

    std::ifstream in(path, std::ios::binary);
    if (!in)
      throw QLangError(QLangErrorKind::Io, 0, 0,
                       "cannot open module source '" + path + "'");
    std::ostringstream text;
    text << in.rdbuf();

    QLangContext scratch;
    scratch.setVerboseLevel(verboseLevel());
    scratch.setModuleSearchPaths({}); // resolution is answered by `this`, below
    for (const auto &[name, func] : m_nativeFuncs)
      scratch.setFunc(name, func);
    for (const auto &mod : m_modules)
    {
      bool have = false;
      for (const auto &existing : scratch.modules())
        have = have || existing.name == mod.name;
      if (!have)
        scratch.registerModule(mod.name, mod);
    }
    scratch.setModuleSearchCallback(
        [this](const std::string &name, std::string &error)
            -> std::shared_ptr<LoadedModule>
        {
          try
          {
            return findModule(name);
          }
          catch (const std::exception &e) // the callback must not throw
          {
            error = e.what();
            return nullptr;
          }
        });

    CompiledProgram prog;
    std::map<std::string, int> nativeIds; // the slot table writeQlc wants
    try
    {
      std::vector<StmtPtr> ast = scratch.parse(text.str());
      QLangCompiler compiler(std::move(ast), &scratch);
      compiler.addExternalConsts({});
      std::unordered_set<std::string> nativeNames;
      for (const auto &[name, _] : scratch.nativeFuncs())
        nativeNames.insert(name);
      compiler.addExternalFuncs(nativeNames);
      compiler.addModules(scratch.modules());
      // The module's identity (SPEC §bytecodes "keyword export"): what it
      // declares wins; a source that declares nothing takes the filename stem
      // (plan Q37), which is also what its import name is expected to be. A
      // declared name that disagrees with the file is still what the registry
      // keys on (Q36 — findModule's `accept`, which traces the substitution).
      compiler.setModuleName(std::filesystem::path(path).stem().string());
      compiler.setLibraryMode(true);
      prog = compiler.compile();
      // Only the natives the module's code calls go in the image: it is loaded right
      // here, and a name it never calls would still have to be resolvable
      // (docs/SERIALIZATION.md §5.2, plan native-slot-portability P1).
      nativeIds = compiler.nativeSlotIds();
    }
    catch (const QLangError &e)
    {
      throw QLangError(e.kind(), e.line(), e.col(), where + e.message());
    }

    std::ostringstream image(std::ios::binary);
    if (!writeQlc(image, prog, nativeIds))
      throw QLangError(QLangErrorKind::Io, 0, 0,
                       where + "cannot build an image for module '" +
                           prog.moduleName + "'");
    std::istringstream bytes(image.str(), std::ios::binary);
    return loadProgram(bytes, path);
  }

  std::shared_ptr<LoadedModule>
  QLangContext::findModule(const std::string &name)
  {
    bindThread();
    if (name.empty())
      return nullptr;

    // 1. A registered C++ module outranks every file (SPEC §api "module search
    //    paths", order step 1). It is not an image, so the answer is "nothing
    //    to load" — the caller binds the module's natives instead. This is also
    //    why the callback cannot shadow a C++ module.
    for (const auto &m : m_modules)
      if (m.name == name)
        return nullptr;

    // 2. Already resolved under this name, or already in the registry under its
    //    own (step 2 — the host may have loaded it with loadProgramFile).
    if (auto loaded = m_pimpl->modules.resolved(name))
      return loaded;
    if (auto loaded = m_pimpl->modules.find(name))
      return loaded;

    // What a successful resolution does, however the image was found: register
    // it under its own declared name, cache it under the name that was asked
    // for, and bring in whatever it imports.
    // `hostSupplied` marks the callback's image: one the host hands over that
    // is ALREADY registered stays registered if this resolution fails — the
    // host put it there, not us.
    auto accept = [this, &name](std::shared_ptr<LoadedModule> mod,
                                bool hostSupplied = false)
    {
      if (mod->program.moduleName.empty())
        throw QLangError(
            QLangErrorKind::Io, 0, 0,
            "module '" + name + "': the loaded image declares no module name");
      const auto existing = m_pimpl->modules.find(mod->program.moduleName);
      if (existing && existing != mod)
        throw QLangError(QLangErrorKind::Io, 0, 0,
                         "module '" + mod->program.moduleName +
                             "' is already loaded");
      if (!existing)
        m_pimpl->modules.add(mod);
      if (mod->program.moduleName != name)
        trace(TRACE_FULL, "[MODULE] '%s' resolved as '%s'\n", name.c_str(),
              mod->program.moduleName.c_str());
      m_pimpl->modules.remember(name, mod);
      try
      {
        resolveModuleDependencies(*this, *mod);
      }
      catch (...)
      {
        // A dependency resolved nowhere: the module is not left half-taken-in.
        // `remove` undoes the registration (which `loadProgram` performs as it
        // reads the image) and every cache entry pointing at it, under this or
        // any other requested name; a host-supplied image that was already
        // registered only loses this name's cache entry. Its own resolved
        // dependencies stay registered — they are valid modules; only this
        // resolution is undone.
        if (hostSupplied && existing)
          m_pimpl->modules.forget(name);
        else
          m_pimpl->modules.remove(mod->program.moduleName);
        throw;
      }
      return mod;
    };

    // 3. The host's callback (SPEC §api "external search callback"), consulted
    //    after the registry and before the paths so it can shadow a file.
    if (m_moduleSearchCallback)
    {
      std::string err;
      std::shared_ptr<LoadedModule> mod;
      try
      {
        mod = m_moduleSearchCallback(name, err);
      }
      catch (const std::exception &e)
      {
        // The contract says the callback must not throw. A host that breaks it
        // gets the exception reported as a resolution failure rather than
        // unwound through the caller.
        mod = nullptr;
        err = std::string("the module search callback threw: ") + e.what();
      }
      catch (...)
      {
        mod = nullptr;
        err = "the module search callback threw an unknown exception";
      }
      if (mod)
      {
        trace(TRACE_FULL, "[MODULE] '%s' supplied by the search callback\n",
              name.c_str());
        return accept(mod, /*hostSupplied=*/true);
      }
      if (!err.empty())
        throw QLangError(QLangErrorKind::Io, 0, 0,
                         "module '" + name + "': " + err);
      // nullptr + empty error: not found, continue with the paths.
    }

    // 4. The search paths, in order; within a directory `.qlc` before `.ql`.
    std::string sourceHit; // the first `<name>.ql` seen, reported below
    for (const std::string &dir : m_moduleSearchPaths)
    {
      const std::string base = dir.empty() ? std::string(".") : dir;
      std::error_code ec;
      if (!std::filesystem::is_directory(base, ec))
        continue; // a directory that is not there is skipped, not an error
      const std::string image =
          (std::filesystem::path(base) / (name + ".qlc")).string();
      std::error_code iec;
      if (std::filesystem::is_regular_file(image, iec))
      {
        // The first match wins, so an image that is there but cannot be taken
        // in is reported, not skipped: silently falling through to a
        // same-named file further down the list would run something else.
        trace(TRACE_FULL, "[MODULE] '%s' found at %s\n", name.c_str(),
              image.c_str());
        return accept(loadProgramFile(image));
      }
      const std::string source =
          (std::filesystem::path(base) / (name + ".ql")).string();
      std::error_code sec;
      if (sourceHit.empty() && std::filesystem::is_regular_file(source, sec))
        sourceHit = source;
    }

    // A source file and no image: compile it and take the result in (SPEC §api
    // "module search paths", plan C8b). A source that is there but does not
    // compile is reported, never skipped for a same-named file further down the
    // list — the same rule the `.qlc` hit above follows.
    if (!sourceHit.empty())
    {
      trace(TRACE_FULL, "[MODULE] '%s' found at %s (compiling)\n", name.c_str(),
            sourceHit.c_str());
      return accept(compileModuleSource(sourceHit));
    }

    trace(TRACE_FULL, "[MODULE] '%s' not found\n", name.c_str());
    return nullptr;
  }

  bool QLangContext::has(const std::string &name) const
  {
    return hasGlobal(name);
  }

  bool QLangContext::isset(const std::string &name) const
  {
    return hasGlobal(name);
  }

  QLangValue QLangContext::get(const std::string &name) const
  {
    bindThread();
    if (!hasGlobal(name))
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "undefined variable '" + name + "'");
    return m_globals.slots[globalSlotOf(name)];
  }

  void QLangContext::set(const std::string &name, const QLangValue &value, const bool isConst)
  {
    bindThread();
    // Write straight into the table (the main frame aliases it), and
    // revive an erased slot: re-assigning a deleted variable re-creates it.
    std::size_t slot = globalSlotFor(name);
    m_globals.slots[slot] = value;
    m_globals.erased.erase(slot);
    if (isConst)
      m_injectedConsts.insert(name);
    else
      m_injectedConsts.erase(name);
  }

  void QLangContext::remove(const std::string &name)
  {
    bindThread();
    removeGlobal(name);
    m_injectedConsts.erase(name);
    // An injected enum dies with its name: drop the compiler registration
    // and the hidden table var (`__enum_<Name>` is a separate global slot).
    auto eit = m_injectedEnums.find(name);
    if (eit != m_injectedEnums.end()) {
      removeGlobal(eit->second.hiddenVar);
      m_injectedConsts.erase(eit->second.hiddenVar);
      m_injectedEnums.erase(eit);
    }
  }

  bool QLangContext::registerEnum(const std::string &name,
                                  const std::vector<std::pair<std::string, int64_t>> &members,
                                  const QLangValue::Type baseType)
  {
    bindThread();
    // A name is injectable when it lexes as a plain identifier: non-empty,
    // starting with a letter or underscore, and keywordType() mapping it to
    // Ident (which also rejects reserved words and type keywords).
    auto validIdent = [](const std::string &s) {
      if (s.empty())
        return false;
      const unsigned char c = static_cast<unsigned char>(s[0]);
      if (!(std::isalpha(c) || c == '_'))
        return false;
      return keywordType(s) == TokenType::Ident;
    };
    if (!validIdent(name))
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "cannot inject enum '" + name +
                       "': not a valid identifier");
    if (members.empty())
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "cannot inject enum '" + name +
                       "': an enum requires at least one member");
    std::unordered_set<std::string> seen;
    for (const auto &[mname, value] : members) {
      (void)value;
      if (!validIdent(mname))
        throw QLangError(QLangErrorKind::Runtime, 0, 0,
                         "cannot inject enum '" + name + "': member '" +
                         mname + "' is not a valid identifier");
      if (!seen.insert(mname).second)
        throw QLangError(QLangErrorKind::Runtime, 0, 0,
                         "cannot inject enum '" + name +
                         "': duplicate member '" + mname + "'");
    }
    switch (baseType) {
    case QLangValue::Type::Char:
    case QLangValue::Type::Byte:
    case QLangValue::Type::Word:
    case QLangValue::Type::Short:
    case QLangValue::Type::Int:
    case QLangValue::Type::Unsigned:
    case QLangValue::Type::Qword:
    case QLangValue::Type::Long:
      break;
    default:
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "cannot inject enum '" + name +
                       "': base type must be numeric");
    }
    const std::string hvar = "__enum_" + name;
    if (hasGlobal(name) || m_injectedEnums.count(name) || hasGlobal(hvar))
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "cannot inject enum '" + name + "': name already in use");
    // Type the members exactly as compileEnum types a declared enum's.
    std::vector<std::pair<std::string, QLangValue>> typed;
    typed.reserve(members.size());
    TableValue kv = std::make_shared<std::map<std::string, QLangValue>>();
    for (const auto &[mname, value] : members) {
      QLangValue v = QLangValue::Int(static_cast<int32_t>(value));
      switch (baseType) {
      case QLangValue::Type::Char: v = QLangValue::Char(static_cast<int8_t>(value)); break;
      case QLangValue::Type::Byte: v = QLangValue::Byte(static_cast<uint8_t>(value)); break;
      case QLangValue::Type::Word: v = QLangValue::Word(static_cast<uint16_t>(value)); break;
      case QLangValue::Type::Short: v = QLangValue::Short(static_cast<int16_t>(value)); break;
      case QLangValue::Type::Unsigned: v = QLangValue::Unsigned(static_cast<uint32_t>(value)); break;
      case QLangValue::Type::Qword: v = QLangValue::Qword(static_cast<uint64_t>(value)); break;
      case QLangValue::Type::Long: v = QLangValue::Long(value); break;
      default: break;
      }
      typed.push_back({mname, v});
      (*kv)[mname] = v;
    }
    // Runtime: the hidden const table (the main frame aliases the global
    // slots, so no VM change is needed). The hidden name is registered as an
    // injected const too — that is how the compiler loads it (declareVar on
    // the const path) and keeps it out of the writable-external-var set.
    set(hvar, QLangValue::Table(kv), /*isConst=*/true);
    InjectedEnum ie;
    ie.members = std::move(typed);
    ie.hiddenVar = hvar;
    m_injectedEnums[name] = std::move(ie);
    m_injectedConsts.insert(name);
    return true;
  }

  // -----------------------------------------------------------------------
  // Global table (Phase 2)
  // -----------------------------------------------------------------------

  GlobalTable &QLangContext::globalTable()
  {
    return m_globals;
  }

  const GlobalTable &QLangContext::globalTable() const
  {
    return m_globals;
  }

  std::vector<QLangValue> &QLangContext::globalSlots()
  {
    return m_globals.slots;
  }

  const std::vector<QLangValue> &QLangContext::globalSlots() const
  {
    return m_globals.slots;
  }

  std::size_t QLangContext::globalCount() const
  {
    return m_globals.slots.size();
  }

  std::size_t QLangContext::globalSlotFor(const std::string &name)
  {
    bindThread();
    auto it = m_globals.names.find(name);
    if (it != m_globals.names.end())
      return it->second;
    std::size_t slot = m_globals.slots.size();
    m_globals.slots.emplace_back();
    m_globals.names[name] = slot;
    return slot;
  }

  std::size_t QLangContext::globalSlotOf(const std::string &name) const
  {
    auto it = m_globals.names.find(name);
    if (it == m_globals.names.end())
      return SIZE_MAX;
    return it->second;
  }

  bool QLangContext::hasGlobal(const std::string &name) const
  {
    auto it = m_globals.names.find(name);
    if (it == m_globals.names.end())
      return false;
    return m_globals.erased.find(it->second) == m_globals.erased.end();
  }

  void QLangContext::eraseGlobalSlot(std::size_t slot)
  {
    bindThread();
    m_globals.erased.insert(slot);
  }

  void QLangContext::removeGlobal(const std::string &name)
  {
    bindThread();
    auto it = m_globals.names.find(name);
    if (it == m_globals.names.end())
      return;
    std::size_t slot = it->second;
    m_globals.names.erase(it);
    m_globals.erased.erase(slot);
    m_globals.slots[slot] = QLangValue();
  }

  void QLangContext::clear()
  {
    bindThread();
    // m_globals first: releasing the instances enqueues `__destroy__`
    // finalizers (Phase 3), which are drained below while livePrograms is
    // still intact (each entry also holds its own program shared_ptr).
    m_globals = GlobalTable();
    processFinalizers();
    m_injectedConsts.clear();
    m_injectedEnums.clear(); // their hidden vars died with m_globals above
    m_nativeFuncs.clear();
    m_modules.clear();
    // Signal connections hold closures from the program being cleared; drop
    // them all (a host must re-connect after clear()).
    m_signalConnections.clear();
    m_nextSignalHandle = 1;
    m_pimpl->programs.clear();
    // External type descriptors, their constructor natives, and the TypeRef
    // variables all die together; host code must re-run import<T>() after
    // clear().
    m_pimpl->types.clear();
    // Loaded `.qlc` libraries die with the C++ modules they were resolved
    // against (their native slots point at those functions), and so does the
    // resolution cache that keyed them by requested name. The search paths,
    // the search callback and the output callback do NOT: they are host
    // configuration, not program state, so a host does not have to re-point
    // the context at its libraries or re-arm its message sink after every
    // clear() (SPEC §api "module search paths", "output callback
    // (universal)").
    m_pimpl->modules.clear();
  }

  void QLangContext::setFunc(const std::string &name, NativeFunc func)
  {
    bindThread();
    m_nativeFuncs[name] = std::move(func);
  }

  std::shared_ptr<TypeDescriptor> QLangContext::registerType(const TypeMeta &meta)
  {
    bindThread();
    if (meta.name.empty())
      throw QLangError(QLangErrorKind::Type, 0, 0,
                       "external type name is empty");
    for (const auto &m : meta.members) {
      if (m.name.empty())
        throw QLangError(QLangErrorKind::Type, 0, 0,
                         "external type '" + meta.name +
                             "' has a member with an empty name");
    }
    // Duplicate member names, and a name used as both property and method.
    std::unordered_map<std::string, int> memberKinds;
    for (const auto &m : meta.members) {
      auto it = memberKinds.find(m.name);
      if (it != memberKinds.end())
        throw QLangError(QLangErrorKind::Type, 0, 0,
                         "duplicate member name '" + m.name + "' on type '" +
                             meta.name + "'");
      memberKinds[m.name] = m.kind;
    }
    // Signals: non-empty name, no clash with a member name, no duplicates.
    std::unordered_map<std::string, bool> signalNames;
    for (const auto &s : meta.signals) {
      if (s.name.empty())
        throw QLangError(QLangErrorKind::Type, 0, 0,
                         "external type '" + meta.name +
                             "' has a signal with an empty name");
      if (memberKinds.count(s.name))
        throw QLangError(QLangErrorKind::Type, 0, 0,
                         "signal name '" + s.name +
                             "' clashes with a member on type '" + meta.name +
                             "'");
      if (!signalNames.emplace(s.name, true).second)
        throw QLangError(QLangErrorKind::Type, 0, 0,
                         "duplicate signal name '" + s.name + "' on type '" +
                             meta.name + "'");
    }
    // Duplicate type name: re-import is supported only after clear(), which
    // also clears the type registry and the constructor native.
    if (m_pimpl->types.contains(meta.name))
      throw QLangError(QLangErrorKind::Type, 0, 0,
                       "type '" + meta.name + "' is already registered");
    // A type name clashing with an existing native function is only a
    // problem when the type also registers a constructor native under the
    // same name.
    if (meta.construct && m_nativeFuncs.count(meta.name))
      throw QLangError(QLangErrorKind::Type, 0, 0,
                       "type name '" + meta.name +
                           "' clashes with an existing native function");

    auto desc = std::make_shared<TypeDescriptor>();
    desc->name = meta.name;
    desc->members = meta.members;
    desc->toString = meta.toString;
    desc->construct = meta.construct;
    desc->enums = meta.enums;
    desc->signals = meta.signals;
    desc->typeInfo = meta.typeInfo;
    desc->typeCheck = meta.typeCheck;
    desc->hasStr = meta.hasStr;
    desc->hasCtor = meta.hasCtor;
    desc->ctorArgTags = meta.ctorArgTags;

    if (meta.construct) {
      // `T(args)` constructs: the native wraps the construct result in an
      // owned External value. The lambda captures the shared handle (Phase
      // 3) — a copied NativeFunc stays valid across clear().
      setFunc(meta.name, [d = desc](QLangContext &c,
                                    const std::vector<QLangValue> &a) {
        ExternalRef ref;
        ref.type = d;
        ref.owned = d->construct(c, a);
        ref.expected = d->typeInfo;
        ref.check = d->typeCheck;
        return QValue::External(std::move(ref));
      });
    }
    // Seed the type-level value so `T.Constant` compiles: the name becomes
    // a (const) script variable holding a TypeRef. The native table above
    // keeps `T(args)` working; `T = ...` is rejected (const).
    set(meta.name, QValue::TypeRef(desc), true);

    return m_pimpl->types.add(std::move(desc));
  }

  // -----------------------------------------------------------------------
  // Event-handler call surface (docs/EVENT_HANDLERS.md §4)
  // -----------------------------------------------------------------------

  bool QLangContext::hasMethod(const QLangValue &obj,
                               const std::string &name) const
  {
    bindThread();
    const ObjectOps *ops = getObjectOps(obj);
    if (!ops)
      return false;
    QLangValue member;
    try
    {
      // A missing member throws on class instances and external values; a
      // plain table reads it as nil. Both are "no method".
      member = ops->getField(obj, name, *const_cast<QLangContext *>(this));
    }
    catch (const QLangError &)
    {
      return false;
    }
    return member.type() == QLangValue::Type::Function ||
           member.type() == QLangValue::Type::BoundMethod;
  }

  QLangValue QLangContext::callMethod(
      const QLangValue &obj, const std::string &name,
      const std::vector<QLangValue> &args)
  {
    bindThread();
    // Safe point (Phase 3): drain pending instance finalizers first, like
    // emitSignal()/callClosure().
    processFinalizers();
    const ObjectOps *ops = getObjectOps(obj);
    if (!ops)
      throw QLangError(QLangErrorKind::Type, 0, 0,
                       std::string("callMethod on a ") + obj.typeName() +
                           " value");
    if (!hasMethod(obj, name))
      throw QLangError(QLangErrorKind::Type, 0, 0,
                       "no method '" + name + "' on " + obj.typeName() +
                           " value");
    return ops->callMethod(obj, name, args, *this);
  }

  // -----------------------------------------------------------------------
  // Signals (docs/spec/17-signals.md)
  // -----------------------------------------------------------------------

  bool QLangContext::hasSignal(const QLangValue &obj, const std::string &name) const
  {
    bindThread();
    if (obj.type() != QLangValue::Type::External)
      return false;
    const auto &ref = obj.asExternal();
    if (!ref.type)
      return false;
    for (const auto &s : ref.type->signals)
      if (s.name == name)
        return true;
    return false;
  }

  void QLangContext::emitSignal(const QLangValue &obj, const std::string &name,
                                const std::vector<QLangValue> &args)
  {
    bindThread();
    // Safe point (Phase 3): drain pending instance finalizers first.
    processFinalizers();
    // Delivery runs in a VM (the same deliverSignal the EMIT opcode calls),
    // so a compiled program is required.
    if (!m_pimpl->programs.has())
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "no program compiled; call run() first");
    QLangVM vm(m_pimpl->programs.compiled(), *this);
    vm.deliverSignal(obj, name, args);
  }

  int QLangContext::connectSignal(const QLangValue &sender, const std::string &signal,
                                  const QLangValue &handler)
  {
    bindThread();
    if (sender.type() != QLangValue::Type::External)
      throw QLangError(QLangErrorKind::Type, 0, 0,
                       "connect on a non-external object");
    if (!hasSignal(sender, signal))
      throw QLangError(QLangErrorKind::Type, 0, 0,
                       "unknown signal '" + signal + "' on type '" +
                           sender.asExternal().type->name + "'");
    if (handler.type() != QLangValue::Type::Function &&
        handler.type() != QLangValue::Type::BoundMethod)
      throw QLangError(QLangErrorKind::Type, 0, 0,
                       "connect handler is not a function");
    SignalConnection conn;
    conn.handle = m_nextSignalHandle++;
    conn.sender = senderWeakRef(sender);
    conn.signal = signal;
    conn.handler = handler;
    m_signalConnections[conn.handle] = std::move(conn);
    return conn.handle;
  }

  int QLangContext::connectSignalSlot(const QLangValue &sender,
                                      const std::string &signal,
                                      const QLangValue &target,
                                      const std::string &method)
  {
    bindThread();
    if (sender.type() != QLangValue::Type::External)
      throw QLangError(QLangErrorKind::Type, 0, 0,
                       "connect on a non-external object");
    if (!hasSignal(sender, signal))
      throw QLangError(QLangErrorKind::Type, 0, 0,
                       "unknown signal '" + signal + "' on type '" +
                           sender.asExternal().type->name + "'");
    SignalConnection conn;
    conn.handle = m_nextSignalHandle++;
    conn.sender = senderWeakRef(sender);
    conn.signal = signal;
    conn.target = target;
    conn.method = method;
    m_signalConnections[conn.handle] = std::move(conn);
    return conn.handle;
  }

  void QLangContext::disconnectSignal(int handle)
  {
    bindThread();
    m_signalConnections.erase(handle); // stale handle: silent no-op
  }

  void QLangContext::disconnectSignalAll(const QLangValue &sender,
                                         const std::string &signal)
  {
    bindThread();
    if (sender.type() != QLangValue::Type::External)
      return;
    void *ptr = senderObjectPtr(sender);
    if (!ptr)
      return;
    for (auto it = m_signalConnections.begin();
         it != m_signalConnections.end();) {
      if (it->second.signal == signal &&
          it->second.sender.lock().get() == ptr)
        it = m_signalConnections.erase(it);
      else
        ++it;
    }
  }

  std::vector<QLangContext::SignalConnection>
  QLangContext::signalConnections(const QLangValue &sender,
                                  const std::string &signal) const
  {
    bindThread();
    std::vector<SignalConnection> out;
    if (sender.type() != QLangValue::Type::External)
      return out;
    void *ptr = senderObjectPtr(sender);
    if (!ptr)
      return out;
    for (const auto &kv : m_signalConnections) {
      const SignalConnection &conn = kv.second;
      if (conn.signal != signal)
        continue;
      auto locked = conn.sender.lock();
      if (!locked || locked.get() != ptr)
        continue; // expired, or a distinct object of the same type
      out.push_back(conn);
    }
    // Connection order == handle order (the monotonically increasing connect
    // order); the registry is a hash map, so sort explicitly.
    std::sort(out.begin(), out.end(),
              [](const SignalConnection &a, const SignalConnection &b) {
                return a.handle < b.handle;
              });
    return out;
  }

  void QLangContext::dropExpiredConnections()
  {
    bindThread();
    for (auto it = m_signalConnections.begin();
         it != m_signalConnections.end();) {
      if (it->second.sender.expired())
        it = m_signalConnections.erase(it);
      else
        ++it;
    }
  }

  void QLangContext::registerModule(const std::string &name, const Module &module)
  {
    bindThread();
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
    bindThread();
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
    bindThread();
    registerBuiltinModules(*this);
  }

  QLangContext::~QLangContext()
  {
    // Members destroy in reverse declaration order, so m_globals (which owns
    // the instances) is torn down while m_pimpl and the finalizer queue are
    // still alive. Releasing an instance fires its shared_ptr deleter, which
    // now only ENQUEUES the `__destroy__` finalizer (Phase 3) — the queued
    // entries die with the context undrained, so no VM is ever built against
    // this half-destroyed context (SIGBUS in adoptGlobals). The flag also
    // guards processFinalizers() against a drain started just before
    // destruction.
    m_destroying = true;
  }

  QLangValue QLangContext::call(const std::string &name, const std::vector<QLangValue> &args)
  {
    bindThread();
    // Safe point (Phase 3): drain pending instance finalizers first.
    processFinalizers();
    // Check native funcs first.
    auto it = m_nativeFuncs.find(name);
    if (it != m_nativeFuncs.end())
      return it->second(*this, args);

    // Ensure we have a compiled program.
    if (!m_pimpl->programs.has())
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "no program compiled; call run() first");

    // Look up the function by name in the compiled program.
    const auto &functions = m_pimpl->programs.compiled().functions;
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

    QLangVM vm(m_pimpl->programs.compiled(), *this);
    return vm.callFunction(funcIndex, args);
  }

  QLangValue QLangContext::callClosure(const QLangValue &closure, const std::vector<QLangValue> &args)
  {
    bindThread();
    // Safe point (Phase 3): drain pending instance finalizers first.
    processFinalizers();
    // Ensure we have a compiled program.
    if (!m_pimpl->programs.has())
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "no program compiled; call run() first");

    // Verify the closure is a Function type.
    if (closure.type() != QLangValue::Type::Function)
      throw QLangError(QLangErrorKind::Type, 0, 0,
                       "expected closure value");

    QLangVM vm(m_pimpl->programs.compiled(), *this);
    return vm.callClosure(closure, args);
  }

  std::shared_ptr<const CompiledProgram> QLangContext::liveProgram() const
  {
    // The cached program is the last entry of the program cache (run() stores
    // the same shared_ptr in both); share it so finalizer deleters keep the
    // program alive for as long as their instances do (Phase 2).
    return m_pimpl->programs.live();
  }

  void QLangContext::enqueueFinalizer(TableValue table,
                                      QLangValue closure,
                                      std::shared_ptr<const CompiledProgram> prog)
  {
    // The deleter may run on any thread and during stack unwinding: lock,
    // push, and swallow allocation failures (losing a finalizer under OOM
    // is acceptable; throwing here would abort).
    try {
      std::lock_guard<std::mutex> lock(m_finalizerMutex);
      m_finalizerQueue.push_back(
          FinalizerEntry{std::move(table), std::move(closure), std::move(prog)});
    } catch (...) {
    }
  }

  void QLangContext::processFinalizers()
  {
    bindThread();
    // Re-entrancy guard: a finalizer that re-enters the context (e.g. via a
    // native that calls call()) must not start a nested drain — the outer
    // fixpoint loop below picks up whatever it enqueues.
    if (m_destroying || m_drainingFinalizers)
      return;
    m_drainingFinalizers = true;
    for (;;) {
      std::vector<FinalizerEntry> batch;
      {
        std::lock_guard<std::mutex> lock(m_finalizerMutex);
        if (m_finalizerQueue.empty())
          break;
        batch.swap(m_finalizerQueue);
      }
      for (const auto &entry : batch)
        runFinalizer(entry);
    }
    // A finalizer that released the last reference to a signal sender drops
    // that sender's connections (docs/spec/17-signals.md: "dropped via the
    // finalizer queue").
    dropExpiredConnections();
    m_drainingFinalizers = false;
  }

  void QLangContext::runFinalizer(const FinalizerEntry &entry)
  {
    // If the program is null the context was cleared before the attach —
    // skip the finalizer rather than dispatch into a dead program. A
    // context that is mid-destruction must also be skipped: building a VM
    // against it would touch the half-destroyed global table (SIGBUS in
    // adoptGlobals).
    if (!entry.prog || m_destroying)
      return;
    // The queue entry owns the table, so it stays alive for the whole call
    // even if the finalizer drops its `self` reference.
    QLangValue instance = QLangValue::Table(entry.table);
    try {
      QLangVM vm(*entry.prog, *this);
      // The finalizer runs in a pushed function frame — the main frame
      // (the only table alias, Phase 2) is never created, so the global
      // table is untouched by the dispatch itself.
      vm.callClosure(entry.closure, {instance});
    } catch (...) {
      // Finalizer errors are swallowed: a finalizer must never take down
      // the drain (the drain may run during clear() or destruction).
    }
  }

} // namespace qlang
