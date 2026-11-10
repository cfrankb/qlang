#pragma once

#include <cassert>
#include <cstddef>
#include <functional>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "QLangParser.h"
#include "QLangTypes.h"
#include "QLangValue.h"

namespace qlang
{

  // Forward declaration (defined in QLangCompiler.h): liveProgram() and
  // runFinalizer() pass program shared_ptrs without including the compiler
  // header (the pimpl keeps the dependency one-way).
  struct CompiledProgram;

  // Forward declaration (defined in QLangBytecodeIO.h, which includes this
  // header): the loaded-module registry stores LoadedModule shared_ptrs in
  // the pimpl, so the public API needs the name only.
  struct LoadedModule;

  // Gated trace-output tiers, used by QLangVM::trace / QLangCompiler::trace.
  // 0 = silent (default); 1 = frame/call events; 2 = VM loop + opcodes +
  // returns; 3 = full detail (generators, table ops, compile internals).
  // Program output (print/println) is never affected.
  enum
  {
    TRACE_OFF,
    TRACE_FRAME,
    TRACE_VM_LOOP,
    TRACE_FULL
  };

  // The single home for every top-level (global) variable (Phase 2).
  // Owned by QLangContext; the VM's MAIN frame register file ALIASES
  // `slots`, so script reads/writes of top-level names and host get/set
  // touch the same storage. There is no sync in at run start and no sync
  // out at run end (or on throw) — the table is always current.
  //
  // `slots` is indexed by slot index, which is also the main-frame
  // register index the compiler assigns to that name (`prog.vars[name]`).
  // `erased` is per-slot (not per-register-index): a `delete` in a nested
  // frame never touches it (P2).
  struct GlobalTable
  {
    std::vector<QLangValue> slots; // slot index == main-frame register index
    std::unordered_map<std::string, size_t> names; // name -> slot
    std::unordered_set<size_t> erased; // per-slot, not per-register-index
  };

  // Host for the interpreter. Owns the variable scope and drives lexing,
  // parsing and (in later versions) execution. The first version exposes
  // parsing of source text plus a variable lookup API.
  class QLangContext
  {
  public:
    QLangContext();
    ~QLangContext();

    // Parse source text into a list of statements. Throws QLangError on
    // lex/parse failure.
    std::vector<StmtPtr> parse(const std::string &source);

    // Parse, compile to bytecode, and execute. Final variable state is written
    // back into this context's scope. Throws QLangError on lex/parse/compile/run
    // failure.
    void run(const std::string &source);

    // Variable scope access. All top-level variables live in the context's
    // global table (Phase 2); these accessors operate on it directly.
    bool isset(const std::string &name) const;
    bool has(const std::string &name) const;
    QLangValue get(const std::string &name) const;
    void set(const std::string &name, const QLangValue &value, const bool isConst = false);
    void remove(const std::string &name);
    void clear();

    // Inject an enum into the global context (SPEC §enum: "C++ api needed to
    // inject enums to the global context"). Mirrors a top-level QLang
    // `enum Name:baseType { ... }`: the members are stored in the hidden
    // const table `__enum_<Name>` and the name is registered with the
    // compiler, so `Name::Member` / `Name.Member` fold to the member value
    // and `Name` resolves to the table (`len(Name)` = member count). The
    // enum is global: visible inside functions, like a global const.
    // `members` is declaration order; values are converted to `baseType`
    // (numeric; default int) exactly as a declared enum's members are.
    // Throws QLangError when the name or a member name is not an identifier
    // lexeme (or a reserved word), `members` is empty, a member name is
    // duplicated, `baseType` is not numeric, or the name is already in use
    // (a live global, a previous injection, or a declared enum).
    bool registerEnum(const std::string &name,
                      const std::vector<std::pair<std::string, int64_t>> &members,
                      QLangValue::Type baseType = QLangValue::Type::Int);

    // Global-table access (Phase 2). The compiler assigns top-level names
    // to table slots (globalSlotFor) so the main frame can alias them;
    // the VM's main frame aliases `globalSlots()` directly.
    GlobalTable &globalTable();
    const GlobalTable &globalTable() const;
    // The table's slot vector: the main frame's register file aliases this
    // storage, so top-level names live in the table with no sync.
    std::vector<QLangValue> &globalSlots();
    const std::vector<QLangValue> &globalSlots() const;
    // High-water mark of assigned slots: the main program's non-global
    // register space (top-level locals, temps) starts above this.
    std::size_t globalCount() const;
    // Look up the slot for `name`, appending a new slot if absent. Used by
    // set() and the compiler's top-level declareVar.
    std::size_t globalSlotFor(const std::string &name);
    // Slot index of `name`, or SIZE_MAX if absent.
    std::size_t globalSlotOf(const std::string &name) const;
    // True when `name` is present and not erased (isset semantics).
    bool hasGlobal(const std::string &name) const;
    // Mark a slot erased (top-level `delete`). Unnamed slots may be marked
    // too; it is a no-op for lookups, which go name -> slot -> erased.
    void eraseGlobalSlot(std::size_t slot);
    // Drop a name and its slot (host remove()).
    void removeGlobal(const std::string &name);

    // Call a QLang or native function by name from C++.
    // Looks up the function in native funcs first, then in compiled user
    // functions. Returns the function's return value (nil if none).
    // Compiles the program on first use; subsequent calls reuse the cached
    // compilation.
    QLangValue call(const std::string &name, const std::vector<QLangValue> &args = {});

    // Call a closure value (Function type) with the given arguments.
    // Used by the ObjectOps dispatch layer for method calls on tables.
    QLangValue callClosure(const QLangValue &closure, const std::vector<QLangValue> &args);

    // Drain pending instance finalizers (`__destroy__`) (Phase 3, H3). The
    // instance table's shared_ptr custom deleter no longer dispatches
    // script — it only enqueues (table, closure, program) on the context's
    // finalizer queue, from any thread. The queue is drained at safe points:
    // the start of run()/call()/callClosure(), inside clear(), or explicitly
    // from the host (e.g. an engine tick). Drains to a fixpoint — a
    // finalizer that releases more instances enqueues them, and they run in
    // the same call. Finalizer errors are swallowed (a finalizer must never
    // take down the drain). No-op while the context is destroying or a drain
    // is already in progress (re-entrancy guard).
    void processFinalizers();

    // Enqueue a finalizer from the instance table's shared_ptr deleter
    // (Phase 3). Public because the VM's attachFinalizer deleter calls it;
    // thread-safe (the deleter may fire on any thread) and never throws
    // (the deleter may run during stack unwinding, where a throw would
    // abort).
    void enqueueFinalizer(TableValue table,
                          QLangValue closure,
                          std::shared_ptr<const CompiledProgram> prog);

    // Shared pointer to the most recently run() program (null if none).
    // Captured by instance finalizers at attach time so the closure
    // dispatches against the program that compiled it.
    std::shared_ptr<const CompiledProgram> liveProgram() const;

    // Inject a C++ function callable from QLang. Called like a builtin:
    //   result = myFunc(arg1, arg2)
    // The first argument is the context (like Lua's lua_State*), followed by
    // the QLang arguments. Return a QLangValue (nil if no result).
    using NativeFunc = std::function<QLangValue(QLangContext &, const std::vector<QLangValue> &)>;
    void setFunc(const std::string &name, NativeFunc func);
    const std::unordered_map<std::string, NativeFunc> &nativeFuncs() const { return m_nativeFuncs; }

    // Application-wide verbose level for diagnostic trace output.
    //   0 = off (default) — no trace emitted.
    //   1 = frame/call events.
    //   2 = VM loop + opcodes.
    //   3 = full detail (generators, table ops).
    // Adjust at runtime from QLang via set_verbose(level) or from C++ via this
    // setter. Program output (print/println/printf) is unaffected.
    void setVerboseLevel(int level) { m_verboseLevel = level; }
    int verboseLevel() const { return m_verboseLevel; }
    // True when the current level is >= minLevel (used to gate trace output).
    bool isAt(int minLevel) const { return m_verboseLevel >= minLevel; }

    // Gated diagnostic emit (mirrors QLangVM::trace / QLangCompiler::trace):
    // printf-style output to stderr only when the level is >= minLevel. Used
    // by the context's own diagnostics — module resolution (plan C8a).
    template <typename... Args>
    void trace(int minLevel, const char *fmt, Args &&...args) const {
      if (!isAt(minLevel)) return;
      char buf[2048];
      int n = std::snprintf(buf, sizeof(buf), fmt, std::forward<Args>(args)...);
      if (n > 0) std::fputs(buf, stderr);
    }

    // Infinite-loop step limit for the VM. The VM reads this live at every
    // step, so it can be lowered from QLang via set_max_steps(n) (or raised)
    // and the change takes effect immediately — useful for tests that want a
    // fast failure instead of spinning to the default. Default 100000.
    void setMaxSteps(std::size_t n) { m_maxSteps = n; }
    std::size_t maxSteps() const { return m_maxSteps; }

    // Step budget (Phase 5, F6): the context owns the CUMULATIVE instruction
    // counter. Every VM increments it per instruction (via step()) and never
    // resets it, so the count survives across run()/call()/callClosure()
    // dispatches — the engine reads stepsUsed() before and after a dispatch
    // to compute the per-dispatch cost (EVENT_HANDLERS.md §8.1). budget() is
    // the live limit (== maxSteps()). resetSteps() zeroes the counter for a
    // host-side budget reset (e.g. per event tick).
    std::size_t stepsUsed() const { return m_stepsUsed; }
    std::size_t budget() const { return m_maxSteps; }
    std::size_t step() { return ++m_stepsUsed; }
    void resetSteps() { m_stepsUsed = 0; }

    // Module registration. A module is a named collection of native functions.
    // Modules are resolved at compile time by `import` statements.
    struct Module
    {
      std::string name;
      std::unordered_map<std::string, NativeFunc> funcs;
    };
    void registerModule(const std::string &name, const Module &module);
    const std::vector<Module> &modules() const { return m_modules; }

    // Convenience: find an already-registered module by name and register it
    // under `alias` (or `name` if alias is empty). Equivalent to QLang
    // `import <name>` / `import <alias>`. Throws if the source module is not
    // found.
    void importModule(const std::string &name, const std::string &alias = "");

    // Reload the default built-in modules (math: min, max, sum). Call after
    // clear() if you need the built-in modules again.
    void loadBuiltinModules();

    // Name of the module this context runs. The context parses source into
    // the `main` module (SPEC # modules); error reports reference it.
    const char *moduleName() const { return "main"; }

    // Loaded `.qlc` modules (SPEC §bytecodes "external modules"; plan v4
    // Phase 3b).
    //
    // A compiled library image is loaded once and kept in the context's module
    // registry, keyed by the name `import` binds it under (its
    // `program.moduleName`). Loading re-resolves the image's native-slot names
    // against this context's natives and its registered C++ modules, so a
    // library's `import math` works without the host repeating the mapping.
    // Both loaders throw QLangError(Io) on an unreadable or malformed image,
    // an unresolvable native name, or a module name that is already loaded.
    std::shared_ptr<LoadedModule> loadProgram(std::istream &is,
                                              const std::string &path = "");
    std::shared_ptr<LoadedModule> loadProgramFile(const std::string &path);
    // The loaded module importable as `name`, or nullptr. Invalidated by
    // clear().
    const LoadedModule *loadedModule(const std::string &name) const;
    // Every loaded module, in load order.
    std::vector<std::shared_ptr<LoadedModule>> loadedModules() const;

    // Module search paths (SPEC §api "module search paths"; plan C8a).
    //
    // Where the engine looks for a module nobody loaded. The list defaults to
    // the working directory at construction; `addModuleSearchPath` appends.
    // A path is a directory: one that does not exist (or is not a directory)
    // is skipped rather than an error, and an empty entry means the working
    // directory. Paths are host configuration, so clear() keeps them.
    const std::vector<std::string> &moduleSearchPaths() const;
    void setModuleSearchPaths(std::vector<std::string> paths);
    void addModuleSearchPath(std::string dir); // append

    // External search callback (SPEC §api "external search callback"): lets a
    // host supply modules from a non-standard filesystem (a Qt `:/...` resource,
    // an archive, an embedded table). The callback is consulted between the
    // registry and the search paths, so it can supply or shadow a file module
    // but never a C++ module the host registered. It must not throw — a
    // failure is reported through `error`:
    //
    //   module          resolved, registered under its own `program.moduleName`
    //   nullptr + ""    not found: resolution continues with the search paths
    //   nullptr + msg   found but unusable: resolution stops and reports msg
    //
    // Assigning nullptr restores the default (filesystem search only).
    using ModuleSearchCallback = std::function<std::shared_ptr<LoadedModule>(
        const std::string &modulename, std::string &error)>;
    void setModuleSearchCallback(ModuleSearchCallback callback);
    const ModuleSearchCallback &moduleSearchCallback() const;

    // Resolve `name` to a loaded module (SPEC §api "module search paths";
    // plan C8a + C8b). The order is the spec's: a registered C++ module
    // (answered nullptr — the caller binds it as natives, not as an image), an
    // entry already in the registry, the search callback, then the search paths
    // in order, trying `<name>.qlc` before `<name>.ql` within each directory.
    // A `.qlc` hit is taken in by loadProgramFile; a `.ql` hit is COMPILED into
    // an image and taken in by the same path (compileModuleSource), so the
    // spec's `[modulename].ql` search works with no host help.
    //
    // Resolution happens once per context: the result is cached under the
    // requested name, so a second request for the same name — or for the
    // module's own declared name — reuses it even when the two differ. The
    // image's own imports are resolved transitively through the same order, so
    // a discovered library's dependencies arrive with it; a dependency that
    // cannot be found is an error naming the module that asked for it. A source
    // that is itself still being compiled further up the same resolution is an
    // import cycle, reported with the chain that produced it.
    //
    // Returns nullptr when nothing matched (a C++ module, or simply absent).
    // Throws QLangError(Io) when a candidate was found but could not be used:
    // the callback reported a reason, an image is malformed, a discovered
    // source does not compile, or a transitive dependency is missing.
    std::shared_ptr<LoadedModule> findModule(const std::string &name);

    // External C++ type registration (docs/EXTERNAL_TYPES.md).
    //
    // Validates the meta (empty/duplicate names, property-vs-method name
    // clashes, duplicate type names) and, when a constructor is present,
    // registers the type name as a native function so `T(args)` constructs.
    // The type name is also seeded as a script variable (a TypeRef value)
    // so `T.Constant` resolves — see kTypeRefOps. The returned handle is a
    // shared_ptr (Phase 3): it stays valid as long as the host (or any
    // script value) holds it, including across clear().
    std::shared_ptr<TypeDescriptor> registerType(const TypeMeta &meta);
    template <class T>
    std::shared_ptr<TypeDescriptor> import()
    {
      return registerType(T::qlang_meta());
    }

    // Signal API (docs/spec/17-signals.md). Signals let an external (C++)
    // type notify script code of state changes. v1: external types only.
    //
    // One live connection. The sender is held WEAK (a connection never keeps
    // its sender alive); the handler (and, for the string-slot form, the
    // target) is held STRONG. `handle` is the monotonically increasing int
    // returned by connect (its order is the delivery order).
    struct SignalConnection {
      int handle = 0;
      std::weak_ptr<void> sender;
      std::string signal;
      QLangValue handler;  // Function or BoundMethod (nil for the slot form)
      QLangValue target;   // string-slot form: the method's target (nil otherwise)
      std::string method;  // string-slot form: the method name (empty otherwise)
    };

    // Event-handler call surface (docs/EVENT_HANDLERS.md §4): thin wrappers
    // over ObjectOps, generic — they know nothing about handlers, levels, or
    // types. Host-only: the VM does not call ops->callMethod in v1 (method
    // calls are TABGET -> BoundMethod -> CALLCLOS).

    // True when `obj` resolves `name` to a callable member (a Function or a
    // BoundMethod). Values without ObjectOps (numbers, strings, arrays, ...)
    // and non-callable members are false; never throws.
    bool hasMethod(const QLangValue &obj, const std::string &name) const;
    // Dispatch `obj.name(args)` through the value's ObjectOps::callMethod and
    // return the result: the method receives the script-visible first
    // argument (self / the statics anchor), then `args` — the same convention
    // as `obj.name(args)` in script. Throws QLangError when the value has no
    // object ops or `name` is not a callable member (mirrors emitSignal's
    // failure mode); errors raised inside the method propagate.
    QLangValue callMethod(const QLangValue &obj, const std::string &name,
                          const std::vector<QLangValue> &args);

    // True when the external object's type declares a signal named `name`
    // (mirrors hasMethod above, which this pair predates).
    bool hasSignal(const QLangValue &obj, const std::string &name) const;
    // C++-side emit: same checks and delivery rules as script `emit`.
    void emitSignal(const QLangValue &obj, const std::string &name,
                    const std::vector<QLangValue> &args);
    // connect(sender, "signal", handler) -> handle. `handler` is a Function
    // value or a BoundMethod.
    int connectSignal(const QLangValue &sender, const std::string &signal,
                      const QLangValue &handler);
    // connect(sender, "signal", target, "method") -> handle (string-slot
    // form; the method is resolved on `target` at emit time).
    int connectSignalSlot(const QLangValue &sender, const std::string &signal,
                          const QLangValue &target, const std::string &method);
    // disconnect(handle): drop one connection (a stale handle is a silent
    // no-op).
    void disconnectSignal(int handle);
    // disconnect(sender, "signal"): drop every connection for that signal.
    void disconnectSignalAll(const QLangValue &sender, const std::string &signal);
    // The live connections for (sender, signal), in connection (handle)
    // order. A connection whose sender has expired is skipped (it is dropped
    // by the next finalizer drain).
    std::vector<SignalConnection> signalConnections(const QLangValue &sender,
                                                    const std::string &signal) const;
    // Drop every connection whose sender has expired. Called from
    // processFinalizers() (the spec's "dropped via the finalizer queue").
    void dropExpiredConnections();

  private:
    // Pimpl: holds the compiled program without creating a circular include.
    struct Pimpl;
    std::unique_ptr<Pimpl> m_pimpl;
    // One queued instance finalizer (Phase 3). The table is owned — the
    // shared_ptr deleter moved its last reference in — so the instance
    // stays alive until the drain runs the finalizer.
    struct FinalizerEntry
    {
      TableValue table;
      QLangValue closure;
      std::shared_ptr<const CompiledProgram> prog;
    };
    // Run one queued finalizer in a fresh VM bound to its program. Errors
    // are swallowed. Called only from processFinalizers().
    void runFinalizer(const FinalizerEntry &entry);
    // Finalizer queue (Phase 3, H3): `__destroy__` dispatch is deferred
    // from the shared_ptr deleter to a safe point. Declared BEFORE
    // m_globals so the queue outlives the global table — a deleter firing
    // during m_globals teardown (context destruction) must find a live
    // queue and mutex.
    std::mutex m_finalizerMutex;
    std::vector<FinalizerEntry> m_finalizerQueue;
    bool m_drainingFinalizers = false;
    // The single home for top-level variables (Phase 2). Replaces the old
    // name->value map: slots are what the main frame aliases, `erased`
    // carries delete state per slot.
    GlobalTable m_globals;
    std::unordered_set<std::string> m_injectedConsts;
    // Host-injected enums (registerEnum): name -> the member values handed
    // to the compiler (m_enums shape) + the hidden table var holding them at
    // runtime. remove()/clear() drop both the name and the hidden var.
    struct InjectedEnum {
      std::vector<std::pair<std::string, QLangValue>> members;
      std::string hiddenVar;
    };
    std::unordered_map<std::string, InjectedEnum> m_injectedEnums;
    std::unordered_map<std::string, NativeFunc> m_nativeFuncs;
    // Registered external C++ types live in the Pimpl's TypeRegistry
    // (Phase 3, H1): shared handles stay valid across clear() because the
    // registry drops only its own reference.
    int m_verboseLevel = 0;
    std::size_t m_maxSteps = 100000; // live infinite-loop guard (see setMaxSteps)
    std::size_t m_stepsUsed = 0; // cumulative instruction counter (Phase 5, F6)
    // Threading contract (Phase 5, H5): one context, one thread. The first
    // use records the owning thread; in debug builds every subsequent use
    // asserts it is the same thread. `mutable` so const accessors can bind
    // too. enqueueFinalizer is exempt (the shared_ptr deleter may fire on
    // any thread, by design).
    mutable std::thread::id m_thread;
    void bindThread() const;
    // The host function a native-slot name asks for: bare, or `module.func`
    // (docs/SERIALIZATION.md §5, SPEC §modules "import statement").
    NativeFunc nativeNamed(const std::string &name) const;
    std::vector<Module> m_modules;
    // Module search paths + the external search callback (SPEC §api "module
    // search paths", "external search callback"; plan C8a). Host configuration:
    // clear() drops the module registry but keeps both.
    std::vector<std::string> m_moduleSearchPaths;
    ModuleSearchCallback m_moduleSearchCallback;
    // Compiles one discovered `.ql` source into an image and takes it in
    // (SPEC §api "module search paths": a `[modulename].ql` hit "compiles into
    // an image and takes in by the same path"; plan C8b). Called by findModule,
    // on a scratch context rather than this one, so a library's top level never
    // touches this context's global area.
    std::shared_ptr<LoadedModule> compileModuleSource(const std::string &path);
    // The `.ql` files whose compile is in progress on this context, outermost
    // first (plan C8b). An image exists only once its compile finishes, so the
    // import graph's own cycle check cannot see a nested cycle; this chain is
    // the guard, and every nested compile is reached through this context's
    // findModule, so one chain covers the whole tree.
    std::vector<std::string> m_compilingSources;
    // Signal connection registry (docs/spec/17-signals.md): handle ->
    // connection. The handle is a monotonically increasing int (its order is
    // the delivery order); it does not wrap in practice.
    std::unordered_map<int, SignalConnection> m_signalConnections;
    int m_nextSignalHandle = 1;
    // Set in the destructor before m_globals is torn down: finalizer drains
    // must not build a VM against a half-destroyed context (SIGBUS in
    // adoptGlobals). Deleters firing during teardown only enqueue; the
    // queued entries die with the context undrained.
    bool m_destroying = false;
  };

} // namespace qlang
