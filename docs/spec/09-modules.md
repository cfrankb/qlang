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

## import statement

Every form keeps its shape; what it *binds* is the whole rule.

| Form | Binds | Reached as |
|---|---|---|
| `import m` | `m` — **the module** | `m.name` |
| `import m as a` | `a` — the module under an alias (`m` itself is not bound) | `a.name` |
| `from m import *` | every public name `m` exposes, flat — except `MODULE_NAME` (see below) | `name` |
| `from m import a, b` | the listed names, flat (`m` is not bound) | `a`, `b` |
| `import m.f` | the one name `f`, flat — the legacy single-entity form (`m` is not bound) | `f` |

`import math` puts **math** in the namespace, so `math.min(...)` resolves and `min(...)` does
not: implicit flat loading is gone. A bare name is visible only because a form above bound it
flat — `from math import min`, `from math import *`, or `import math.min`. `MODULE_NAME` is the
one exception: it is the module's **identity, not an entity it offers**, so `from m import *`
never binds it and two star-imports coexist. It stays reachable through the qualifier
(`m.MODULE_NAME`), through an explicit `from m import MODULE_NAME` / `import m.MODULE_NAME`, and
in the export table.

```python
import math
c = math.min(4, 1)            # c = 1 — the qualifier
from math import min
d = min(4, 1)                 # d = 1 — this line bound `min` itself
e = max(1, 2)                 # compile error: unknown function 'max'
```

The same holds for both kinds of module: `math.max(...)` (native) and `vec.scale(...)` (a
`.qlc`) are read the same way, and `from vec import scale` is what makes a bare `scale(...)`
legal.

- **A qualifier resolves wherever a name does** — inside a `def` body, at any nesting depth, in
  any expression. It is resolved at compile time like any other call target.
- **A module's entities are not values.** Reading one as a value (`println(vec.scale)`) is a
  compile error that names the call spelling to use instead.
- **Nothing writes through a qualifier.** `m.x = 1`, `m.x += 1` and `m.x++` are compile errors
  (`cannot assign to module member 'm.x'`): the module owns its entities. A *flat* import is
  read-only for the same reason (`cannot assign to imported name 'x'`).
- **A local wins.** A variable, parameter, class or function named `m` shadows the qualifier in
  its scope, exactly as it shadows any other name.

- **Only a module import takes an alias.** `import m as a` renames the qualifier; a per-name
  alias (`from m import a as b`) is not parsed.

### Where an import may appear

`import` is a **module-level statement** and is resolved before the rest of the module
compiles, so it may sit below its first use. Inside a `def` body or a nested block it is a
compile error (`import of module '…' must appear at module top level`): a function body has
private registers and no module scope to bind into.

`m` is the same name for both kinds: a C++ module registered under it, or a loaded `.qlc` whose
`MODULE_NAME` is it. A name claimed by **both** is a compile error, never a precedence.


## C++ (native) modules

Modules are registered from C++ via `ctx.registerModule(name, module)`. The `import` statement
is resolved at compile time: the entity it binds becomes an external (native) function of the
importing module, keyed by the **spelling the importer uses** — `import math` makes `math.min`
a slot, `from math import min` makes `min` one. An image carries that spelling in its native-slot
table and the context re-resolves it at load ([13-bytecodes.md](13-bytecodes.md)
`## native function table`). See [07-api.md](07-api.md) for `registerModule` /
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
c = math.min(4, 1)  # c = 1
c = math.min(2, 0, 4, 3)  # c = 0
c = math.min([2.5,4,1]) # c = 1
c = math.min([3,5,6,1], [2,5,1,2]) # errror: only 1 array allowed
#import min and max accept: numbers (int, char, float) or an array ONLY
#min and max refuse data if types are mixed (int and float)
#min and max refuse data if types have mixed signed and unsigned types (char and byte)
#max used the same rules as min

d = math.sum(1,10,20) # d = 31
d = math.sum([1,4,20]) # d = 25
d = math.sum([1,4,20], [50,2]) # d = 77
# sum accepts: numbers (int, char, float etc) or arrays ONLY
# sum return float if a type float is present. coerce non-float to float
# sum return int   if data is mixed signed and unsigned types (char and byte) and no float are present

from math import rand  # the flat spelling is a choice the importer makes, per name
e = rand()
```

## QLang (`.ql`) modules

A `.ql` compiles **standalone** into a `.qlc` module image (**v9**) that carries its identity
and both lookup tables — the export table of what it offers and the import table of what it
needs ([13-bytecodes.md](13-bytecodes.md) `## keyword export`,
`## export table (lookup)`, `## import table (lookup)`). The module an `import` binds is that
**artifact**; the `.ql` is how it was made.

- **Identity** — `export const MODULE_NAME = "<name>"`, the name other modules import by and the
  registry's key. A library compile (`QLangCompiler::setLibraryMode`) needs the module to be
  **namable**: its own `MODULE_NAME`, or the name the host is compiling the file under (the
  driver passes the source file's stem), since a module no name reaches cannot be imported. Its
  top level then compiles as `<init>` instead of `main`.
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
  (`QLangContext::loadProgram` / `loadProgramFile`). Finding one by name — the search paths, the
  external search callback, and a `[name].ql` hit compiled on the spot — is
  [07-api.md](07-api.md) `## module search paths`, so `qlang_run app.ql` resolves `import lib`
  from a `lib.qlc` or `lib.ql` on the path; `--import lib.ql` / `qlang_run app.ql lib.qlc` name
  libraries explicitly and keep the image beside the source.

**Implemented (Round 65, 2026-11-09)** — plan
[../plans/archive/external-modules-plan-v4.md](../plans/archive/external-modules-plan-v4.md) Phases 1–3c:
everything above except a runtime module registry (executing an image without merging it — the
two opcodes run, the feature around them does not exist) and name-based discovery. A `.ql`
module that imports a name whose captures belong to the defining module's frame is refused at
compile time: the import table says *which* function, not which frame seeds its captures.

