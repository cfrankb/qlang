# Plan: external modules (import/export tables, unified constant pool, builtin name table)

Spec: `docs/spec/13-bytecodes.md` § "external modules" (unmarked proposal — no status
marker yet). Precedents: MOVI (Round 30), ADDI (Round 31), SUBI + cmp/j* (Round 32) —
all followed the append-to-enum + all-dispatch-loops + codec-version pattern.

## Background / current state (verified 2026-10-31)

- Compilation is **single-source → single `CompiledProgram`**; `functions[0]` is the
  implicit `"main"` (compiler enforces, `readQlc` enforces, `QLangVM::run()` starts
  there, `QLangContext::call()` skips it).
- Constant pools are **per-function** (`Function::constants`) in memory, in `.qlc`
  (per-function `constCount × Value`), in the VM (per-frame pool), and in the
  disassembler. No cross-function dedup.
- "Modules" today = C++ native function sets only (`QLangContext::Module`; just
  `math`). `import` resolves at **compile time** into a flat negative-id native-slot
  space (spec 09: "no VM changes needed for flat import"). No `export`, no module
  identity in `CompiledProgram` or `.qlc`.
- `.qlc` = `QLC1` v2 (header → functions w/ per-fn pools → native slots → vars →
  constVars → registerCount). Reader accepts ≤ 2, requires `functions[0].name ==
  "main"`, rejects trailing bytes. **No production loader** (`QLangContext::loadProgram`
  is referenced in comments, unimplemented; HANDOFF.md already lists it as next step).
- Builtin names are hardcoded in **four places**: `Builtin` enum
  (`include/qlang/QLangBytecode.h:25`, 20 entries `ToStr=0…Set=19`),
  `QLangCompiler::builtinId()` (`src/QLangCompiler.cpp` ~3057, extra `str`/`int`
  aliases), the VM `callBuiltin` switch (`src/QLangVM.cpp` ~385), and the
  disassembler's `builtinName()` (`examples/qlang_run.cpp` ~143–190, **app-local**).
- The opcode dispatch switch is **triplicated** in `QLangVM.cpp`
  (`run()` ~1398, `runFrames()` ~1749, `callFunction()` ~2252) — every new opcode
  must land in all three.
- `SERIALIZATION.md` lags the code (documents v1; code writes v2).

The spec's five requirements map to three phases, each kept independently
shippable with `ctest` green:

| Phase | Spec requirement | Effort | Risk |
|-------|------------------|--------|------|
| 1 | builtin-function lookup table | S | Low |
| 2 | unified per-module constant table (+ `.qlc` v3) | M | Medium |
| 3 | import/export tables + standalone modules + new bytecodes | L | High |

---

## Phase 1 — builtin name table in the library (S)

Goal: one library-level id↔name table; the disassembler (and any future tool)
consumes it. No format change, no behavior change.

1. **`include/qlang/QLangBytecode.h` / `src/QLangBytecode.cpp`**
   - Add `int builtinCount();` and `const char *builtinName(int id);`
     (return `nullptr`/`""` for out-of-range) backed by a single
     `k_builtin_names[]` array sized to `Builtin::Set + 1`.
   - Add `int builtinId(const std::string &name);` (throw `QLangError` on unknown)
     with the `str`→`ToStr`, `int`→`ToInt` aliases moved from the compiler.
2. **`src/QLangCompiler.cpp`** — delete the local `builtinId()`; call the library one.
3. **`src/QLangVM.cpp`** — `callBuiltin` keeps its dispatch switch, but add a
   debug/assert cross-check against `builtinName(id)` at the entry (cheap guard
   against enum/table drift).
4. **`examples/qlang_run.cpp`** — delete the app-local `builtinName()` switch; use
   the library function for the `# builtin:<name>` notes.
5. **Tests** — `examples/codec_check.cpp`: table covers every `Builtin` id
   (0..builtinCount()-1) with non-empty names; `builtinId` round-trips every name
   incl. aliases; disassembly of a program calling several builtins prints the
   expected names.
6. **Docs** — `13-bytecodes.md` "external modules" §: note that the builtin table
   is `builtinName`/`builtinId` in `QLangBytecode.h` (single source of truth), and
   that it is a *library* table keyed by bytecode version (not serialized in
   `.qlc`). Compliance hash update **after** green run (AGENTS.md).

Out of scope here: serializing the table into `.qlc` (only needed if third-party
tools must decode `.qlc` without the library — revisit then).

## Phase 2 — unified per-module constant table + `.qlc` v3 (M)

Goal: one deduplicated constant pool per module (program), replacing per-function
pools. Prerequisite for Phase 3's link-time merge.

1. **`include/qlang/QLangBytecode.h`**
   - Move the pool from `Function` to program level: `CompiledProgram` gains
     `std::vector<QLangValue> constants;` (or keep it in the `Bytecode` carrier —
     pick one home; recommend `CompiledProgram::constants` so the VM and codec
     share it). `Function::constants` removed.
   - `Bytecode::addConstant`/`setConstants` (existing `valueKey` dedup) become the
     single pool manager for all functions.
2. **`src/QLangCompiler.cpp`**
   - `buildFunction()` stops creating a per-function pool: all `emitConst` calls
     hit the shared pool (the `Bytecode` scratch already dedups via `m_constKeys`;
     extend the same map across `buildFunction` save/restore so a constant used by
     two functions gets one slot).
   - `compile()` no longer copies main's pool into a second place;
     `CompiledProgram::code` mirror of `functions[0]` is dropped (it duplicates
     instructions *and* constants today, QLangCompiler.cpp ~526–533).
3. **`src/QLangVM.cpp`** (all three dispatch loops)
   - Frame no longer carries a per-function pool; `CONST`/`MAKEFUNC`/`MAKEGEN`
     operand `b` indexes the program pool. Closure `Function` values in the pool
     store `funcIndex` + capture regs (no pool indices) — unaffected.
4. **`src/QLangBytecodeIO.cpp`** — `.qlc` **v3**:
   - Writer: `putU32(os, 3)`; emit one module-level pool section
     (`u32 constCount + constCount × Value`) after the function records (function
     records lose `constCount × Value`).
   - Reader: version-branched — v1/v2 keep the per-function decode, v3 reads the
     module pool and assigns it to `CompiledProgram::constants`. Accept `≤ 3`.
   - Regenerate the checked-in samples `test.qlc`, `cmp.qlc` (they are v2).
5. **`examples/qlang_run.cpp` disassembler** — print one flat `constants:` list per
   program (this matches the spec's own decompiler sample, which already shows a
   flat pool — docs are currently ahead of code).
6. **Tests**
   - `spec_check2`: same outputs as today (behavior-invariant refactor); a program
     where two `def`s use the same string literal compiles to **one** pool slot.
   - `codec_check`: v3 write/read round-trip (pool equality); v2 files still load;
     v3 file with a pool index out of range is rejected.
7. **Docs** — `SERIALIZATION.md` full rewrite to v2+v3 (it currently documents v1);
   `13-bytecodes.md` unified-constant-table note + status marker; compliance hashes
   after green run.

Risks: behavior-invariant on its face, but it touches every layer (compiler, VM,
codec, disassembler). Keep it a standalone round so any regression is easy to
bisect.

## Phase 3 — import/export tables, standalone modules, new bytecodes (L)

**Blocked on spec decisions first** (see "Open spec decisions" below). The
recommended link model is **link-time merge**: loading resolves imports against
loaded modules and produces one flat `CompiledProgram` (renumbered function
indices, offset/merged pools, patched closure constants). This preserves the VM's
single-`functions`-vector invariant; a runtime module registry would be a far larger
VM redesign.

### 3a — module identity + export construct + tables (no VM change yet)

1. **Spec first**: define module name (file stem vs `module <name>` declaration),
   `export` syntax for functions/variables/constants, and the application-vs-library
   module distinction.
2. **Lexer/parser** — `export` keyword (and `module` if adopted); AST: export
   flags on existing top-level `Def`/`Var`/`Const` stmts + optional module-name
   stmt.
3. **Compiler** — `CompiledProgram` gains:
   - `std::string moduleName;`
   - `std::vector<ExportEntry> exports;` — `{kind: Func|Var|Const, name, ref}`
     where `ref` = function index / global register / pool index.
   - `std::vector<ImportEntry> imports;` — `{module, name, kind, ref}` (filled by
     the linker, empty after standalone compile).
   - Standalone compile of a library module: no `main` required; top-level code
     (if any) compiles as an `<init>` function recorded in the module header.
4. **`.qlc` v4** — module name string, export table section, import table section
   (import entries written with unresolved refs; the linker fills them). Reader
   accepts ≤ 4; the `functions[0] == "main"` check applies only to
   application-module files.

### 3b — loader + linker

1. **`QLangContext::loadProgram(...)`** (the long-missing loader, HANDOFF.md item)
   — reads a `.qlc` module, stores name + export table in a module registry on the
   context.
2. **Linker** (new, in the codec or a `QLangLinker` unit):
   - Match each import entry against loaded modules' exports (error on missing
     name/module; cycle detection for .ql→.ql imports).
   - **Merge**: concatenate function vectors (offset `CALLFN`/`MAKEFUNC`/`MAKEGEN`
     targets and closure `funcIndex` values in pool constants), concatenate pools
     (offset `CONST`/`MAKEFUNC`/`MAKEGEN` pool indices), concatenate native slots.
   - Entry point: the application module's `main` becomes `functions[0]` of the
     merged program; library modules' `<init>` functions run in dependency order
     before `main`.
   - C++ native modules (`math`) stay compile-time natives; a .ql module may
     import them (their names are known at link time from the context registry).
3. **Driver** — `qlang_run` gains `qlang_run app.ql [lib1.qlc ...]` (or
   `--import`); single-file invocation keeps today's behavior.
4. **Tests** — `codec_check`: v4 round-trip incl. tables; loader: load a library
   module, link an app against it, run; missing export / cycle / version-mismatch
   each produce a clean `QLangError`. `spec_check2`: end-to-end cross-module call
   and cross-module constant read.

### 3c — cross-module variables/constants (new bytecodes)

1. **Opcodes** (append before `kOpcodeCount`, add to `usesI32` if a target is
   32-bit):
   - `GETIMPORT` — `r[a] = <import table[b]>` (read an imported global var/const;
     reads go through the target module's global cell so writes are visible).
   - `SETIMPORT` — `<import table[b]> = r[a]` (write through; const imports
     rejected at link time).
   - Calls to imported functions need **no new opcode**: the linker renumbers
     `CALLFN` targets into the merged function vector.
2. **VM** — merged program keeps a global-cell area for module globals
   (extends the existing main-regs-seeded-from-scope mechanism, QLangVM.cpp
   ~1620–1631); `GETIMPORT`/`SETIMPORT` land in **all three** dispatch loops.
3. **Compiler** — an imported name resolves to `GETIMPORT`/`SETIMPORT` with an
   import-table index instead of a local register; imported consts may fold to
   `CONST` from the (merged) pool at link time.
4. **Tests** — cross-module variable write visible in both modules; const-import
   write is a link error; closure created in module A capturing a global and used
   from module B.
5. **Docs** — finalize `13-bytecodes.md` (operand rows for the new opcodes,
   status markers), rewrite `09-modules.md` (the "internal modules" framing is
   superseded; C++ modules become a module *kind*), `SERIALIZATION.md` v4,
   `07-api.md` (registry/link API), compliance hashes after green run.

---

## Spec cleanup (do in the round each phase lands)

Independent of implementation, `docs/spec/13-bytecodes.md` itself needs:

1. **Operand map is stale** — it omits `SUBI`, `CMP`, `CMPI`, `JE/JNE/JG/JGE/JL/JLE`
   (all already in the `Opcode` enum and `usesI32()` since Round 32). Add rows.
2. **Terminology** — "example of external modules are `ql` files" conflates source
   (`.ql`) and compiled artifact (`.qlc`); state that a compiled module is a
   `.qlc` with module name + tables. Define "compatible bytecodes" (same format
   major version; opcode set superset).
3. **Decompiler sample** shows a flat constant pool while the current implementation
   is per-function — Phase 2 makes the sample true; mark it accordingly.
4. **Status markers** — the "external modules" section has none; add
   `PROPOSAL` / `Implemented (Round N)` markers per subsection as they land.

## Open spec decisions (must resolve before Phase 3 work starts)

1. **Link model** — link-time merge (recommended) vs runtime module registry.
2. **Entry point** — application module (has `main`) vs library module (no `main`;
   optional `<init>`). Which file form is which (flag in container vs convention)?
3. **Cross-module variable semantics** — shared cell (write-through, recommended)
   vs copy-at-import.
4. **Export syntax** — `export def f()`, `export var x`, `export const k`?
   Auto-export-all-globals? Module name from file stem or explicit `module` decl?
5. **Import graph rules** — .ql modules may import C++ modules and other .ql
   modules? Cycles prohibited? Aliases (`from m import f as g`)?
6. **Compatibility rule** — formal definition of "compatible bytecodes".

## Verification (per phase, per AGENTS.md)

- `cmake -B build && cmake --build build`
- `ctest --test-dir build` (all suites incl. `codec_check`, `spec_check2`)
- Compliance SHA256 hashes + tallies in `docs/spec/README.md` /
  `docs/compliance/status.md` updated **only after** the green run.

## Explicitly out of scope

- Decompiler mnemonics (`r`/`c` prefixes) — spec says DO NOT IMPLEMENT YET.
- Separating line/col from `Instruction` (8-byte layout) — spec says DO NOT
  IMPLEMENT.
- Inheritance (already deferred to v2 per HANDOFF.md).
- Removing `JMPIFFALSE`/`JMPIFTRUE`/binary comparison ops — still needed.
- Serializing the builtin name table into `.qlc` (Phase 1 keeps it library-side).
- Qualified access syntax (`math.min(...)`) — stays a future extension unless the
  Phase 3 spec decisions say otherwise.
