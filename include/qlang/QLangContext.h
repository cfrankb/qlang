#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "QLangParser.h"
#include "QLangValue.h"

namespace qlang
{

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

    // Variable scope access.
    bool isset(const std::string &name) const;
    bool has(const std::string &name) const;
    QLangValue get(const std::string &name) const;
    void set(const std::string &name, const QLangValue &value, const bool isConst = false);
    void remove(const std::string &name);
    void clear();

    // Call a QLang or native function by name from C++.
    // Looks up the function in native funcs first, then in compiled user
    // functions. Returns the function's return value (nil if none).
    // Compiles the program on first use; subsequent calls reuse the cached
    // compilation.
    QLangValue call(const std::string &name, const std::vector<QLangValue> &args = {});

    // Call a closure value (Function type) with the given arguments.
    // Used by the ObjectOps dispatch layer for method calls on tables.
    QLangValue callClosure(const QLangValue &closure, const std::vector<QLangValue> &args);

    // Run a class instance's `__destroy__` finalizer (Phase 7). Called from
    // the instance table's shared_ptr custom deleter at final release: the
    // table map `tbl` is still valid here but about to be destroyed by the
    // caller. Dispatches `closure` in a fresh VM bound to the program at
    // `programId` in the live-program list. All errors are swallowed — the
    // deleter may run during stack unwinding, where a throw would abort.
    void runFinalizer(std::map<std::string, QLangValue> *tbl,
                      const QLangValue &closure, int programId);

    // Index into the live-program list of the most recently run() program
    // (-1 if none). Captured by instance finalizers at attach time so the
    // closure dispatches against the program that compiled it.
    int currentProgramId() const;

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

    // Infinite-loop step limit for the VM. The VM reads this live at every
    // step, so it can be lowered from QLang via set_max_steps(n) (or raised)
    // and the change takes effect immediately — useful for tests that want a
    // fast failure instead of spinning to the default. Default 100000.
    void setMaxSteps(std::size_t n) { m_maxSteps = n; }
    std::size_t maxSteps() const { return m_maxSteps; }

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

  private:
    // Pimpl: holds the compiled program without creating a circular include.
    struct Pimpl;
    std::unique_ptr<Pimpl> m_pimpl;
    std::unordered_map<std::string, QLangValue> m_vars;
    std::unordered_set<std::string> m_injectedConsts;
    std::unordered_map<std::string, NativeFunc> m_nativeFuncs;
    int m_verboseLevel = 0;
    std::size_t m_maxSteps = 100000; // live infinite-loop guard (see setMaxSteps)
    std::vector<Module> m_modules;
  };

} // namespace qlang
