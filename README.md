# QLang

A small, embeddable scripting language: a C++ toolchain that lexes, parses,
compiles to a register-machine bytecode, and runs it in a stack-of-frames VM.
Built to be embedded in an engine — a `QLangContext` owns the script scope,
injects native functions, and hands script results back to the host.

**Stage:** POC. The language core (values, control flow, functions, closures,
generators, tables, modules, native interop) is implemented and test-covered;
the design docs (serialization, external types, event handlers) are the
deliverable for the next phase, and a few features (classes, the `.qlc`
loader, external-type dispatch) are spec'd but not yet built.

## Layout

```
include/qlang/   Public API headers
src/             Library implementation (libqlang.a)
examples/        qlang_run driver, demos, and per-feature test suites
docs/            Design docs (serialization, external types, event handlers)
SPEC.md          Language specification (authoritative)
SPEC_COMPLIANCE.md  Spec ↔ implementation compliance tracker (SHA256-gated)
HANDOFF.md       Session handoff: what's done, what's next, open issues
```

## Components

| Component | Header | Role |
|-----------|--------|------|
| `QLangLexer` | `QLangLexer.h` | Tokenizes source text |
| `QLangParser` | `QLangParser.h` | Builds the AST |
| `QLangCompiler` | `QLangCompiler.h` | Lowers the AST to register-machine bytecode (`CompiledProgram`) |
| `QLangVM` | `QLangVM.h` | Executes a `CompiledProgram` (stack of frames, register file) |
| `QLangBytecode` | `QLangBytecode.h` | `Opcode` set, `Bytecode` / `Function` / `ProgramImage` |
| `QLangValue` | `QLangValue.h` | Runtime value (typed, tag-based) |
| `QLangContext` | `QLangContext.h` | Host: scope, native functions, modules, step guard, trace level |
| `QLangModules` | `QLangModules.h` | Built-in modules (e.g. `math`) |
| `QLangErrors` | `QLangErrors.h` | Structured `QLangError` |

## Build

Requires CMake 3.16+.

```bash
cmake -B build
cmake --build build
ctest --test-dir build
```

Targets: `qlang` (library → `libqlang.a`), `qlang_example` (hello),
`qlang_run` (driver), plus one executable per test suite. A single suite can
also be built and run directly against `build/libqlang.a` with g++ (C++17);
see `AGENTS.md` for the exact harness.

## Usage

```cpp
#include <qlang/QLangContext.h>
#include <qlang/QLangValue.h>

using namespace qlang;

QLangContext ctx;

// Inject a native callable from QLang:  log(x)
ctx.setFunc("log", [](QLangContext &, const std::vector<QLangValue> &args) {
  // args[0] ... ; return a QLangValue (nil if no result)
  return QLangValue::String("ok");
});

// Parse + compile + run; final variable state is written back to the scope.
ctx.run(R"(
  x = 6 * 7
  log(x)
)");

QLangValue v = ctx.get("x");   // 42
```

`ctx.parse(source)` returns the raw AST (`std::vector<StmtPtr>`); `ctx.run`
is the compile-and-execute shortcut. `ctx.call(name, args)` invokes a QLang or
native function by name. `ctx.registerModule` / `ctx.importModule` expose
named native function groups to `import`.

## Value types

| Type | C++ | Notes |
|------|-----|-------|
| `nil` | — | unset / absent |
| `bool` | `bool` | stored as Int 1/0 at runtime |
| `char` | `int8_t` | signed; distinct from `Byte` |
| `byte` | `uint8_t` | |
| `Word` | `uint16_t` | |
| `short` | `int16_t` | |
| `int` | `int32_t` | |
| `unsigned` | `uint32_t` | |
| `dword` | `uint64_t` | |
| `string` | null-terminated text | |
| `blob` | binary data | |
| `array` | list of mixed values | |
| `float` | `double` | |
| `table` | string → value map | |
| `function` | closure | `def` / arrow, with captures |
| `generator` | yield-based closure | lazy, resumable |
| `eof` | sentinel | end-of-file marker |

## Implemented features

- Control flow: `if`/`elif`/`else`, `for`, `while`, `switch`, `break`,
  `continue`, `return`, labels
- Functions: `def` and arrow closures with explicit capture lists; nested
  calls; generator functions (`yield`)
- Collections: arrays and tables (keyed maps); runtime type tracking drives
  `+=` (append vs. add) and index/member access
- Const vars (`const`) with reassignment guards
- Native interop: inject C++ functions (`setFunc`), call script functions
  from C++ (`call`), modules (`import`, built-in `math`)
- Safety: live infinite-loop step guard (`set_max_steps`, default 100 000)
- Diagnostics: gated trace tiers (`set_verbose_level`, 0–3) — never touches
  program output

**Spec'd, not yet built:** classes (compiler has scaffolding, the VM does not
run them), the `.qlc` bytecode loader, external-type object dispatch, and
per-binding ownership. See the design docs and `SPEC_COMPLIANCE.md`.

## Driver

`qlang_run <file> [args...]` runs a script. Flags:

| Flag | Effect |
|------|--------|
| `--profile` | Report compile + exec time in ms |
| `--output <file.qlc>` | Write the compiled bytecode (POC image) |

## Design docs

| Doc | Covers |
|-----|--------|
| `SPEC.md` | The language itself (authoritative) |
| `docs/SERIALIZATION.md` | `.qlc` (bytecode) and `.qls` (scope) formats, step counter |
| `docs/EXTERNAL_TYPES.md` | C++ object import: the `ObjectOps` dispatch surface, ownership model |
| `docs/EVENT_HANDLERS.md` | **Engine-specifics:** how the engine drives QLang as event handlers |

The C++ API (object dispatch, step counter, ownership model, error boundary)
is part of **QLang**; the multi-level event-handler architecture and its
budget/retention/re-entrancy policies are **engine-specifics** built on that
API (`docs/EVENT_HANDLERS.md` §0). Adding a handler level needs no QLang
change — only engine wiring.

## Next steps

- Implement classes in the VM (compiler scaffolding exists)
- `.qlc` / `.qls` loader (formats defined in `docs/SERIALIZATION.md`)
- External-type object dispatch + per-binding ownership
  (`docs/EXTERNAL_TYPES.md`)
- Wire the event-handler contract into the engine (`docs/EVENT_HANDLERS.md`)
