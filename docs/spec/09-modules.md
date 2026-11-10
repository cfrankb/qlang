# modules

A **module** is a named bundle of functions and values an `import` can bind. There are two
kinds, and they differ in who wrote them and how the importer reaches them — not in what
`import` means:

| kind | written in | lives in | how an import binds it |
|-|-|-|---|
| **C++ (native)** | C++ (`ctx.registerModule`) | the library / the host program | compile time: each function becomes a native slot (the negative-slot space of `CALL`) |
| **QLang (`.ql`)** | `.ql`, compiled to a `.qlc` module image | a file the host loads into the context | compile time: each name becomes an entry of the importing module's **import table**, read or called through `getimport` / `callimport` |

Both are resolved at **compile time** — nothing is looked up while a program runs. What the
two kinds need from the machine differs: a native call is one instruction with a slot number,
while a `.ql` import is a table entry, because the entity lives in another module's function
table and register area.

`FLAT IMPORT ONLY UNTIL THE MODULE SUPPORT IS COMPLETED`
```
# future extension
import math # math.min, math.max, math.sum
```

Qualified access (`math.min(...)`) is a future extension for **both** kinds; v1 supports flat
import only.

## import statement

```python
import math                   # flat import: min, max, sum, rand usable by name

import math as m              # m.min, m.max

from math import *            # import all public names from the math
from math import min, max     # import `min` and `max`

```

| Form | Behaviour |
|---|---|
| `import m` | flat import of everything `m` exposes |
| `import m as a` | the same, and `a` names the module for a later `import a.f` |
| `from m import *` | flat import of everything `m` exposes |
| `from m import a, b` | flat import of the listed names only |
| `import m.f` | legacy single-function form (flat import of that one name) |

`m` is the same name for both kinds: a C++ module registered under it, or a loaded `.qlc` whose
`MODULE_NAME` is it. A name claimed by **both** is a compile error, never a precedence.

`import` is a **module-level statement**. Inside a `def` body it is a compile error
(`import of module '…' must appear at module top level`): a function body has private
registers and no module scope to bind into.

## C++ (native) modules

Modules are registered from C++ via `ctx.registerModule(name, module)`. The `import` statement
is resolved at compile time: each function in the module is registered as an external (native)
function in the current scope. See [07-api.md](07-api.md) for `registerModule` /
`importModule` / `clear`.

Module can be written in C++ or Qlang. System modules (C++) are part of the qlang library.

qlang context (ctx) parses the source. it creates the `main` module.
when a module is registered, it will provide it's own name.
all errors messages will reference the module name.

### math module

```cpp
ctx.registerModule("math", {
  {"min", [](QLangContext&, const std::vector<QLangValue>& args) -> QLangValue { ... }},
  {"max", ...},
  {"sum", ...}
});
```

|functions|-|-|
|--------|-|-|
|min(number...\|array)| returns the smallest value in a group number (int, float, byte etc). |
|max(number...\|array)| returns the largest value in the group of numbers (int, float, byte etc). |
|sum(number...\|array)| returns a sum of the group of numbers (int, float, byte etc). |
|rand()|return a random int value|

```
import math
c = min(4, 1)  # c = 1
c = min(2, 0, 4, 3)  # c = 0
c = min([2.5,4,1]) # c = 1
c = min([3,5,6,1], [2,5,1,2]) # errror: only 1 array allowed
#import min and max accept: numbers (int, char, float) or an array ONLY
#min and max refuse data if types are mixed (int and float)
#min and max refuse data if types have mixed signed and unsigned types (char and byte)
#max used the same rules as min

d = sum(1,10,20) # d = 31
d = sum([1,4,20]) # d = 25
d = sum([1,4,20], [50,2]) # d = 77
# sum accepts: numbers (int, char, float etc) or arrays ONLY
# sum return float if a type float is present. coerce non-float to float
# sum return int   if data is mixed signed and unsigned types (char and byte) and no float are present
```

## QLang (`.ql`) modules

A `.ql` compiles **standalone** into a `.qlc` module image (**v9**) that carries its identity
and both lookup tables — the export table of what it offers and the import table of what it
needs ([13-bytecodes.md](13-bytecodes.md) `## keyword export`,
`## export table (lookup)`, `## import table (lookup)`). The module an `import` binds is that
**artifact**; the `.ql` is how it was made.

- **Identity** — `export const MODULE_NAME = "<name>"`, mandatory in a library compile
  (`QLangCompiler::setLibraryMode`), whose top level then compiles as `<init>` instead of
  `main`.
- **Exports** — `export` on a `def` / `const` / assignment / arrow closure, or standalone
  `export NAME`. Global scope only. An exported variable is a **shared cell**: the defining
  module writes it, importers read it; assigning through an import, `++`/`--` on it and
  `delete` of it are compile errors.
- **Imports** — resolved against a loaded image's export table at compile time. The importer's
  instruction names its **own** import table, never the defining module's handle
  (`getimport` / `callimport`, [13-bytecodes.md](13-bytecodes.md)
  `### OpCode getimport / callimport`).
- **Linking** — `QLangLinker` merges the images an entry imports into one runnable program:
  one function table, one folded constant pool, one global register area sliced per module, one
  import table, and every module's `<init>` run exactly once, dependencies first. A library may
  import another library; the chain is followed transitively.
- **Loading** — the host loads an image into the context
  (`QLangContext::loadProgram` / `loadProgramFile`); finding one by name (search paths, an
  external search callback) is [07-api.md](07-api.md) and not implemented yet, so
  `qlang_run app.ql lib.qlc` / `--import lib.qlc` names libraries explicitly.

**Implemented (Round 65, 2026-11-09)** — plan
[../plans/external-modules-plan-v4.md](../plans/external-modules-plan-v4.md) Phases 1–3c:
everything above except a runtime module registry (executing an image without merging it — the
two opcodes run, the feature around them does not exist) and name-based discovery. A `.ql`
module that imports a name whose captures belong to the defining module's frame is refused at
compile time: the import table says *which* function, not which frame seeds its captures.

