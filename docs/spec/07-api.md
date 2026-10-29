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

Modules are resolved at compile time — no VM changes needed for flat import.

```python
import math                   # import module into the namespace
                              # use math.min and math.max

import math as m              # m.min, m.max

from math import *            # import all public names from the math module
from math import min, max     # import `min` and `max` only
```

| Form | Behaviour |
|---|---|
| `import math` | Register the module under `math`; call as `math.min(...)`, `math.max(...)` (qualified access) |
| `import math as m` | Register the module under alias `m`; call as `m.min(...)` |
| `from math import *` | Flat import: copy all public names into the current scope |
| `from math import min, max` | Flat import: copy only the listed names into the current scope |

Qualified access (`math.min(...)`) is a future extension — v1 supports flat
import only (`from math import *` / `from math import min, max`). The module
is still resolved at compile time and each imported function is registered as
an external (native) function in the current scope.

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



# global scope 

global const and functions are visible everywhere (global, local, function). 
global variables (native and injected) are not visble in function scope unless declared via global keyword. (see example)

```python
const HELLO = "Hello World"

a = [1,2,3] # global a
b = "John"  # global b

def test() {
   println(HELLO) # HELLO is visible here

   # error, a is a global var/invisible by default
   printf("[test] len: %d\n", len(a)) 

   global a, b;
   printf("[test] len: %d\n", len(a)) 
   # output >> [test] len: 3

   global c; # error: cannot bind unknown global `c`

   a += [4,5,6] # modify global a
}

printf("[global] len: %d\n", len(a)) 
# output >> [global] len 3

test()

printf("[global] len: %d\n", len(a)) 
# output >> [global] len 6
```


# block scope

variable explicitly defined via `local` or `let` keyword while inside a block scope belong to that scope.

```
local i = 1
if true {
   local i = 3  # local declared inside block scope
}
println($i) # expected: "1"
```

example - crossing the block scope

```
local i = 1
if true {
   a = 2
   i = 3  # assign to variable outside block scope
}
println($i) # expected: "3"
println($a) # expected: "2"
```

block scope extends to if (conditions), loops (for, while, etc) and switch body.

```
local j = 0
for i in range(0, 10) {
   local j = i
}

println($j) # expected "0"

local i = 0
while (i < 10) {
   ++i;
   local j = i;
}

println($j) # expected "0"

```


