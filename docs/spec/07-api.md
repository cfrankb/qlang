# Qlang C++ API

The API is defined against **C++20** (the root `CMakeLists.txt` sets
`CMAKE_CXX_STANDARD 20`).

## Module registration

System modules are auto-registered on `QLangContext` construction. Call
`registerBuiltinModules(ctx)` to register additional or override modules before
the first `run()`. Call `ctx.loadBuiltinModules()` to reload the default modules
after `ctx.clear()`:

```cpp
#include "qlang/QLangModules.h"
QLangContext ctx;
registerBuiltinModules(ctx);  // math module (min, max, sum) is now available

ctx.clear();
ctx.loadBuiltinModules();     // math module is available again
```

Two methods control module availability:

### `ctx.registerModule(name, module)`

Defines and stores a new module in the context's registry. This is a **setup**
operation — it does not import anything into QLang scope. QLang code can use
`import <name>` to pull the functions in on the next `run()`.

```cpp
QLangContext::Module myMod;
myMod.name = "math";
myMod.funcs["min"] = [](QLangContext&, const std::vector<QLangValue>& args) -> QLangValue {
    if (args.empty()) return QLangValue::Int(0);
    int64_t v = toInt64(args[0]);
    for (size_t i = 1; i < args.size(); i++) {
        int64_t a = toInt64(args[i]);
        if (a < v) v = a;
    }
    return QLangValue::Int(static_cast<int32_t>(v));
};
myMod.funcs["max"] = [](QLangContext&, const std::vector<QLangValue>& args) -> QLangValue { ... };
ctx.registerModule("math", myMod);
```

Registering a module with a name that already exists appends a new entry; the
last registration wins at compile time.

### `ctx.importModule(name, alias)`

Finds an already-registered module by `name` and re-registers it under
`alias` (or the same `name` if alias is empty). This is the C++ equivalent of
the QLang `import <name>` statement — it makes the module's functions
available for compile-time import resolution.

```cpp
ctx.importModule("math");           // equivalent to QLang `import math`
ctx.importModule("math", "m");      // equivalent to QLang `import m`
```

Throws `runtime error: unknown module '...'` if the source module is not found.
Useful when you want to expose the same module under multiple names or override
the default name.

### QLang import syntax

Modules are resolved at compile time — nothing is looked up while a program runs. For a **C++**
module that stays true of the machine as well: each imported function is a native slot of `CALL`,
keyed by the spelling the importer used (`math.min` for `import math`, `min` for
`from math import min`). A **`.ql`** module is the other half of the contract — its
names are entries of the importing module's import table, reached by `getimport` / `callimport`
([13-bytecodes.md](13-bytecodes.md) `### OpCode getimport / callimport`, implemented Round 65) —
and a loaded `.qlc` is merged by `QLangLinker`; see [09-modules.md](09-modules.md).

```python
import math                   # binds the module: math.min, math.max, math.sum, math.rand

import math as m              # binds the alias only: m.min, m.max

from math import *            # every public name of math, by bare name
from math import min, max     # `min` and `max` only, by bare name
```

| Form | Binds |
|---|---|
| `import math` | `math` — the module; its entities are reached as `math.min(...)` |
| `import math as m` | `m` — the module under an alias; `math` itself is not bound |
| `from math import *` | every public name, flat (`math` is not bound) |
| `from math import min, max` | the listed names, flat (`math` is not bound) |

The parser additionally accepts the legacy single-function form
`import math.min`, which binds that one function flat as `min` (the module is not bound).
A bare name is visible only through a form that binds it flat: after `import math`,
`min(...)` is `unknown function 'min'`. For a C++ module the name is resolved at compile time
and the imported function is registered as an external (native) function of the importing
module; for a loaded `.qlc` it is resolved against that image's export table. The full rules
(what a qualifier may and may not do, where an `import` may appear) are in
[09-modules.md](09-modules.md) `## import statement`.

## ctx.clear()

`ctx.clear()` resets the context state for reuse:

```cpp
ctx.clear();
// equivalent to:
//   m_vars.clear();
//   m_injectedConsts.clear();
//   m_nativeFuncs.clear();
//   m_modules.clear();
//   m_pimpl->compiled.reset();
```

After `clear()`, all injected vars, consts, native funcs, and modules are
removed. The compiled program cache is invalidated. Call
`registerBuiltinModules(ctx)` again if you need the built-in modules.

`ctx.remove(name)` deletes a single variable.
`ctx.has(name)` / `ctx.isset(name)` check whether a variable exists.

`QLangContext::call(name, args)` invokes a QLang or native function by name
from C++. It checks native funcs first, then scans compiled user functions by
name. The compiled program is cached after the first `run()` call so subsequent
`call()` invocations reuse it.

```cpp
ctx.run("def square(x) { return x * x }\n");
QLangValue r = ctx.call("square", {qval_int(7)});  // r.asInt() == 49

ctx.setFunc("dbl", [](QLangContext&, const std::vector<QLangValue>& a) {
  return qval_int(a[0].asInt() * 2);
});
QLangValue r2 = ctx.call("dbl", {qval_int(21)});  // 42
```

TODO: 
- add methods to rerived the bytecodes() from the context 
- upload new bytecodes into the context
- version bytecodes to avoid loading incompatible bytecodes

## verbose level

**Priority one** — an application-wide verbose level that is adjustable with a single function call.

The verbose level gates *diagnostic trace output only* (printf-style lines written to **stderr**). It never affects *program output* (`print` / `println` / `printf` to stdout). The level is process-global: one call changes it for the whole interpreter, and it can be toggled at runtime from either QLang or C++.

### Granularity

| Level | Trace detail emitted |
|-------|----------------------|
| `0` | Off (default). No VM trace emitted. |
| `1` | Frame / call events (`[pushCall] …`). |
| `2` | VM loop + opcodes + returns (`[VM] …`, `[DBG-RET] …`). |
| `3` | Full detail: generators, table ops, bytecode (`[DBG-GEN-EXIT] …`, `[VM-TABCREATE] …`). |

Compiler diagnostics (`[DBG-COMPILER]`, `[DBG-COMP]`, `[DBG-CONST]`, `[DBG-CALL]`, …) are emitted by `QLangCompiler` and are independent of this level.

### QLang API

`set_verbose(level)` sets the level and returns the new value.

```python
set_verbose(1)          # frame/call trace on stderr
println(foo())          # program output on stdout — unaffected by verbose
set_verbose(0)          # turn VM trace back off at runtime
```

```python
def f() { local x = 0; x = 1 + 2; return x }

set_verbose(2)          # print the opcode stream to stderr
println(f())            # >> 3   (stdout, unaffected)
set_verbose(0)
```

```
# stderr (level 2, truncated):
# [VM] func=1 pc=2 op=0 a=3 b=1 c=0 regs=6
# [VM] func=1 pc=3 op=0 a=4 b=2 c=0 regs=6
# [VM] func=1 pc=4 op=6 a=2 b=3 c=4 regs=6
```

### C++ API

The level lives on `QLangContext` and is readable/writable at any time:

```cpp
#include "qlang/QLangContext.h"
using namespace qlang;

QLangContext ctx;
ctx.setVerboseLevel(2);        // 0 = off, 1 = frames, 2 = VM loop, 3 = full
int lvl = ctx.verboseLevel();  // read back the current level
bool on = ctx.isAt(2);         // true when current level >= 2 (used to gate trace)

QLangValue r = ctx.run("println(1)");
```

### Implementation notes

- `QLangContext` owns the level (`m_verboseLevel`, default `0`) and exposes `setVerboseLevel(int)`, `verboseLevel()`, and `isAt(int)`.
- `set_verbose` is registered as a native function (`QLangContext.cpp`). It accepts an int/qword/float argument and returns the resulting level.
- `QLangVM` holds a gated emit helper `trace(minLevel, fmt, …)` (declared inline in `QLangVM.h`). Every diagnostic call site in `QLangVM.cpp` was converted from `fprintf(stderr, …)` to `trace(T, …)` with its tier `T` (1/2/3); sites inside the free function `callBuiltin` call `vm->trace(T, …)`.
- `trace()` returns immediately when `m_scope.isAt(minLevel)` is false, so no formatting cost is incurred when a tier is disabled.

### Example session

```
$ ./vtest prog.lang            # prog.lang: set_verbose(1); print(foo())
42                             # <- stdout: program output
[pushCall] calleeIndex=1 …     # <- stderr: trace, only when level >= 1
```

## step budget

The context owns a **cumulative** VM instruction counter (Phase 5, F6). Every
VM increments it per instruction and never resets it, so the count survives
across `run()`/`call()`/`callClosure()` dispatches. This makes the
infinite-loop budget reachable from C++ (the engine's per-dispatch budget,
`docs/EVENT_HANDLERS.md` §8.1):

```cpp
std::size_t before = ctx.stepsUsed();   // read the cumulative count
ctx.call("handler", {args});            // dispatch
std::size_t cost = ctx.stepsUsed() - before;  // per-dispatch instruction cost
std::size_t limit = ctx.budget();       // live limit (== ctx.maxSteps())
ctx.resetSteps();                       // optional host-side budget reset
```

- `ctx.stepsUsed()` — cumulative instruction count since construction (or the
  last `resetSteps()`).
- `ctx.budget()` — the live step limit (== `ctx.maxSteps()`, default 100000).
- `ctx.step()` — internal: increments and returns the new count (called by
  the VM per instruction; not for host use).
- `ctx.resetSteps()` — zeroes the counter (host-side budget reset, e.g. per
  event tick).

The limit is still enforced by the VM: a runaway loop throws
`runtime error: infinite loop (step limit)` when the cumulative count exceeds
`budget()`.

## threading contract

**One context, one thread.** A `QLangContext` is **not** thread-safe: it must
be created, driven, and destroyed on a **single thread**. The VM is
single-threaded and not re-entrant (see `docs/EVENT_HANDLERS.md` §8.2); the
context is the host boundary for that rule.

- The context records the **owning thread** on first use. In **debug builds**
  (`NDEBUG` undefined) every subsequent use **asserts** it is the same
  thread; a cross-thread call is a hard failure. In **release builds** the
  check compiles out (zero cost beyond a thread-id compare).
- The **one documented exception** is `enqueueFinalizer()`: the instance
  table's `shared_ptr` deleter may fire on **any** thread (even during stack
  unwinding) and only **enqueues** `(table, closure, program)` — it never
  drives the VM. The queue is drained on the owning thread at a safe point
  (`run()`/`call()`/`callClosure()`, `clear()`, or an explicit
  `processFinalizers()`).
- If events arrive from other threads, the **engine marshals** them onto the
  owning thread before touching the context. No locking is added to the
  context itself; the contract is "one context, one thread."

## external types

See [EXTERNAL_TYPES.md](../EXTERNAL_TYPES.md)
and [docs/spec/11-external-types.md](11-external-types.md)

## serialization

See [SERIALIZATION.md](../SERIALIZATION.md)

## signals

See [docs/spec/17-signals.md](17-signals.md)



## module search paths

API C++ will provide a means to set the search paths to find modules.

```cpp
const std::vector<std::string> &moduleSearchPaths() const;
void setModuleSearchPaths(std::vector<std::string> paths);
void addModuleSearchPath(std::string dir);  // append
```

default search paths includes `the current directory` (pwd) — the working
directory at the moment the context is constructed.

provide C++ API to query the search paths and change them.

if no callback is specified, the default behavior is to look up native modules
first then try to find the module in the search path. The full order is:

1. a **native (C++) module** registered with `registerModule` / `importModule`;
2. a module **already in the registry** (`loadProgram` / `loadProgramFile`);
3. the **search paths**, in order.

Since all modules in the POC are single file then it will simply search for `[modulename].ql` in all the directories listed in the search paths. Within a directory the engine tries `[modulename].qlc` — a compiled image, taken in by `loadProgramFile` — before `[modulename].ql`, which it compiles into an image and takes in by the same path. A directory that does not exist is skipped, not an error; the first match wins.

A module is resolved **once per context**: the first successful resolution is cached under the resolved module's own name (`program.moduleName`) and a later request for that name reuses it. An import cycle is an error.

## external search callback

An external search function can be assigned to allow qlang to find modules embedded in non-standard filesystem (like Qt `:/...`).

callback prototype

`std::shared_ptr<LoadedModule> loadModuleCallback(const std::string &modulename, std::string &error)`

return the loaded module if successful. `LoadedModule` (`qlang/QLangBytecodeIO.h`) is a compiled module image plus its provenance — the same object `QLangContext::loadProgram` produces and the module registry keeps. The context holds the result **shared**: the callback may build it as a local and return it, and it stays alive as long as the context or a program built against it does. A raw pointer is not used because the registry already stores `shared_ptr<LoadedModule>`.

return `nullptr` if module not found and loading. `error` contains the cause if any:

| return | `error` | meaning |
|---|---|---|
| a module | (unused) | resolved; the registry keys it under `program.moduleName` |
| `nullptr` | empty | not found — resolution continues with the search paths |
| `nullptr` | non-empty | found but unusable — resolution stops and the message is reported |

The callback is consulted **after** the native-module and registry lookups and **before** the search paths, so it can supply or shadow a file module but not a C++ module the host registered deliberately. It must not throw — a failure is reported through `error`. Assigning `nullptr` restores the default behaviour.
