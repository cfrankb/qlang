#include "qlang/QLangContext.h"

#include <algorithm>
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
    bool contains(const std::string &name) const
    {
      for (const auto &t : m_types)
        if (t->name == name)
          return true;
      return false;
    }
    void clear() { m_types.clear(); }

  private:
    std::vector<std::shared_ptr<TypeDescriptor>> m_types;
  };

  // Pimpl definition — breaks the circular include with QLangCompiler.h.
  // Phase 3 (H1): the context's program cache and type registry are separate
  // classes; the context itself is a thin public API.
  struct QLangContext::Pimpl
  {
    ProgramCache programs;
    TypeRegistry types;
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
