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

    // Inject a C++ function callable from QLang. Called like a builtin:
    //   result = myFunc(arg1, arg2)
    // The first argument is the context (like Lua's lua_State*), followed by
    // the QLang arguments. Return a QLangValue (nil if no result).
    using NativeFunc = std::function<QLangValue(QLangContext &, const std::vector<QLangValue> &)>;
    void setFunc(const std::string &name, NativeFunc func);
    const std::unordered_map<std::string, NativeFunc> &nativeFuncs() const { return m_nativeFuncs; }

    // Module registration. A module is a named collection of native functions.
    // Modules are resolved at compile time by `import` statements.
    struct Module {
      std::string name;
      std::unordered_map<std::string, NativeFunc> funcs;
    };
    void registerModule(const std::string &name, const Module &module);
    const std::vector<Module> &modules() const { return m_modules; }

  private:
    // Pimpl: holds the compiled program without creating a circular include.
    struct Pimpl;
    std::unique_ptr<Pimpl> m_pimpl;
    std::unordered_map<std::string, QLangValue> m_vars;
    std::unordered_set<std::string> m_injectedConsts;
    std::unordered_map<std::string, NativeFunc> m_nativeFuncs;
    std::vector<Module> m_modules;
  };

} // namespace qlang
