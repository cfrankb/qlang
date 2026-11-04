# Plan: external modules (import/export tables, unified constant pool, builtin name table)

Spec: `docs/spec/13-bytecodes.md` § "external modules" (lines 338–416, unmarked
proposal — no status marker yet). Its "technical specifications" block
(397–417) already resolves the link model, entry point, export syntax, import
graph, and compatibility rule — see "Spec decisions" below. Precedents: MOVI
(Round 30), ADDI (Round 31), SUBI + cmp/j* (Round 32) — all followed the
append-to-enum + all-dispatch-loops + codec-version pattern.

Updated 2026-10-31 (post-`58a9659` MOVI-Ints): all code claims re-verified;
opcode dispatch is now 4-fold; `SETIMPORT` dropped (spec makes imports readonly);
"Open spec decisions" replaced by the spec's own resolutions.

## Background / current state (re-verified 2026-10-31, post-`58a9659`)

- Compilation is **single-source → single `CompiledProgram`**; `functions[0]` is the
  implicit `"main"` (compiler enforces, `readQlc` enforces, `QLangVM::run()` starts
  there, `QLangContext::call()` skips it).
- Constant pools are **per-function** (`Function::constants`, QLangBytecode.h:260)
  in memory, in `.qlc` (per-function `constCount × Value`), in the VM (fetched
  per-iteration via `functions[funcIndex].constants`, QLangVM.cpp:1365/:2490 —
  the `Frame` itself carries no pool, QLangVM.h:50–67), and in the disassembler.
  No cross-function dedup. Since `58a9659`, 32-bit ints are **MOVI immediates,
  not pool slots**; pools now hold only String/Float/Bool/nil/closures/Array/
  Table plus 64-bit literals.
- "Modules" today = C++ native function sets only (`QLangContext::Module`,
  QLangContext.h:109–113; just `math`, registered at QLangModules.cpp:161–169).
  `import` (five forms — `import mod`, `import mod.f`, `import mod as a`,
  `from mod import *`, `from mod import f,g`; QLangCompiler.cpp:1697–1763)
  resolves at **compile time** into a flat negative-id native-slot space
  (spec 07-api.md:62: "no VM changes needed for flat import"; 09-modules.md:
  "v1 uses flat import only"). No `export`, no module identity in
  `CompiledProgram` or `.qlc`.
- `.qlc` = `QLC1` v2 (writer QLangBytecodeIO.cpp:311–365: header → functions w/
  per-fn pools → native slots → vars → constVars → registerCount). Reader
  (367–494) accepts ≤ 2 (386–387), requires `functions[0].name == "main"`
  (437–438), rejects trailing bytes (481–482). Checked-in samples: `test.qlc`
  is **v1**, `cmp.qlc` is **v2**. **No production loader**
  (`QLangContext::loadProgram` appears only in comments/docs;
  `docs/plans/archive/HANDOFF.md` lists it as a next step).
- Builtin names are hardcoded in **four places**: `Builtin` enum
  (`include/qlang/QLangBytecode.h:25`, 20 entries `ToStr=0…Set=19`),
  `QLangCompiler::builtinId()` (`src/QLangCompiler.cpp:3056–3078`, extra
  `str`/`int` aliases), the VM `callBuiltin` switch (`src/QLangVM.cpp:385`), and
  the disassembler's `builtinName()` (`examples/qlang_run.cpp:143–190`,
  **app-local**).
- The opcode dispatch switch is **4-fold** in `QLangVM.cpp`: `run()` 1398,
  `runFrames()` 1749, and `callFunction()` (2048) contains **two** copies —
  the generator path (2145) and the normal path (2506). Every new opcode must
  land in all **four**.
- `SERIALIZATION.md` lags the code (documents v1; code writes v2 — it even lists
  "version 2" as a malformed-input rejection case).
- The spec's "technical specifications" (13-bytecodes.md:397–417) has **already
  decided** the questions the earlier draft of this plan left open (link model,
  entry point, export syntax, import graph, compatibility) — see "Spec
  decisions" below; Phase 3 is written against those decisions.

The spec's five requirements map to three phases, each kept independently
shippable with `ctest` green:

| Phase | Spec requirement | Effort | Risk |
|-------|------------------|--------|------|
| 1 | builtin-function lookup table | S | Low |
| 2 | unified per-module constant table (+ `.qlc` v3) | M | Medium |
| 3 | import/export tables + standalone modules + `GETIMPORT` | L | High |

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
2. **`src/QLangCompiler.cpp`** — delete the local `builtinId()` (3056–3078); call
   the library one. Note: the local one returns `-1` on unknown names (3077) —
   adapt the call sites to the throwing library API.
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
   - `Bytecode::addConstant`/`setConstants` (existing `m_constKeys`
     serialized-value-key dedup, QLangBytecode.cpp:154–170) become the single
     pool manager for all functions.
2. **`src/QLangCompiler.cpp`**
   - `buildFunction()` (634) stops resetting the pool per function (today
     `m_code = Bytecode()` at 689 discards the enclosing pool saved at 651):
     all `emitConst` calls hit the shared pool (the `Bytecode` scratch already
     dedups via `m_constKeys`; keep the same map across `buildFunction`
     save/restore so a constant used by two functions gets one slot). Note:
     ints never reach the pool since `58a9659` — they are MOVI immediates
     (`emitImm`, QLangCompiler.cpp:432–441), so only non-Int values (String,
     Float, Bool, nil, closures, Array, Table, 64-bit literals) need dedup.
   - `compile()` no longer copies main's pool into a second place;
     `CompiledProgram::code` mirror of `functions[0]` is dropped (it duplicates
     instructions *and* constants today, QLangCompiler.cpp ~526–533).
3. **`src/QLangVM.cpp`** (all **four** dispatch loops)
   - Frames already carry no pool — the loops fetch
     `m_prog.functions[frame.funcIndex].constants` into a local `consts`
     (e.g. 1365, 2490); point that fetch at `m_prog.constants` instead.
     `CONST`/`MAKEFUNC`/`MAKEGEN` operand `b` then indexes the program pool.
     Closure values in the pool store `funcIndex` + capture regs (no pool
     indices, QLangVariant.h:28–31) — unaffected.
4. **`src/QLangBytecodeIO.cpp`** — `.qlc` **v3**:
   - Writer: `putU32(os, 3)`; emit one module-level pool section
     (`u32 constCount + constCount × Value`) after the function records (function
     records lose `constCount × Value`).
   - Reader: version-branched — v1/v2 keep the per-function decode, v3 reads the
     module pool and assigns it to `CompiledProgram::constants`. Accept `≤ 3`.
   - Regenerate the checked-in samples (`test.qlc` is v1, `cmp.qlc` is v2).
   - `codec_check.cpp` already asserts the written version byte == 2 (268–328)
     and has a malformed-input suite that rejects "version 3" (607–734) — both
     must be updated in this round.
5. **`examples/qlang_run.cpp` disassembler** — print one flat `constants:` list per
   program (this matches the spec's own decompiler sample, which already shows a
   flat pool — docs are currently ahead of code).
6. **Tests**
   - `spec_check2`: same outputs as today (behavior-invariant refactor); a program
     where two `def`s use the same string literal compiles to **one** pool slot
     (int sharing is already slot-free via MOVI — no pool assertion needed).
   - `codec_check`: v3 write/read round-trip (pool equality); v1/v2 files still
     load; v3 file with a pool index out of range is rejected.
7. **Docs** — `SERIALIZATION.md` full rewrite to v2+v3 (it currently documents v1);
   `13-bytecodes.md` unified-constant-table note + status marker; compliance hashes
   after green run.

Risks: behavior-invariant on its face, but it touches every layer (compiler, VM,
codec, disassembler). Keep it a standalone round so any regression is easy to
bisect.

## Phase 3 — import/export tables, standalone modules, `GETIMPORT` (L)

The spec's decisions (see "Spec decisions" below) fix the design: **this version
ships link-time merge** — loading resolves imports against loaded modules and
produces one flat `CompiledProgram` (renumbered function indices, offset/merged
pools, patched closure constants). This preserves the VM's single-`functions`-
vector invariant. The spec requires a **runtime module registry too**, with both
models "sharing a reusable solution" — so the export/import table types, module
registry data, and name-resolution logic are built as standalone units that a
runtime-registry implementation can reuse; the registry itself is deferred.

Key consequences of the spec's decisions for this phase:

- **No `main` function** in modules (spec: "global scope script only ... no main
  function"). Every module is a global-scope script; top-level code compiles to
  the module's entry/`<init>` function. The `functions[0] == "main"` check applies
  only to legacy v1–v3 files; the entry module is chosen by the driver, not the
  file format.
- **Imports are readonly** (spec: "exported entities are `readonly` ... variables
  can be read but not modified"). Only **`GETIMPORT`** is needed — no
  `SETIMPORT`. A write through an import is a compile/link error.
- **Shared cell semantics**: `GETIMPORT` reads the defining module's global cell
  (its `<init>`/top-level writes are visible to importers); storage is shared,
  mutation is allowed only in the defining module.
- **Module name**: no `module` declaration in v1 — the spec's own example
  exposes it as `export const MODULE_NAME = "my_module"`. Single-file modules
  only in this version.

### 3a — module identity + export construct + tables (no VM change yet)

1. **Spec**: export syntax is already decided (13-bytecodes.md:370–395). What
   remains to pin down: the module-name convention — the spec's example exposes
   it as `export const MODULE_NAME = "my_module"`; decide whether that is
   mandatory, with file-stem fallback. No `module` declaration in v1.
2. **Lexer/parser** — `export` keyword, both spec forms: prefixed
   (`export def f()`, `export const k = …`) and standalone (`export NAME;`).
   AST: export flags on existing top-level `Def`/`Var`/`Const` stmts.
   Compile errors per spec: `export` outside global scope; duplicate export of
   the same name ("already exported").
3. **Compiler** — `CompiledProgram` gains:
   - `std::string moduleName;`
   - `std::vector<ExportEntry> exports;` — `{kind: Func|Var|Const, name, ref}`
     where `ref` = function index / global register / pool index.
   - `std::vector<ImportEntry> imports;` — `{module, name, kind, ref, alias?}`
     (filled by the linker, empty after standalone compile; alias support per
     the spec's `from m import f as g`).
   - Every module (application or library) is a global-scope script: top-level
     code compiles as the module's entry/`<init>` function recorded in the
     module header; no `main` requirement. The compiler API today is a single
     option-less `compile()` (QLangCompiler.h:66) — library mode is a new
     compile flag/entry.
4. **`.qlc` v4** — module name string, export table section, import table section
   (import entries written with unresolved refs; the linker fills them). Reader
   accepts ≤ 4; the `functions[0] == "main"` check applies only to legacy
   v1–v3 files.

### 3b — loader + linker

1. **`QLangContext::loadProgram(...)`** (the long-missing loader,
   `docs/plans/archive/HANDOFF.md` item) — reads a `.qlc` module, stores name +
   export table in a module registry on the context (the reusable unit the
   future runtime-registry model builds on).
2. **Linker** (new, in the codec or a `QLangLinker` unit):
   - Match each import entry against loaded modules' exports (error on missing
     name/module; recursion/cycle check for .ql→.ql imports per spec).
   - **Merge**: concatenate function vectors (offset `CALLFN`/`MAKEFUNC`/`MAKEGEN`
     targets and closure `funcIndex` values in pool constants), concatenate pools
     (offset `CONST`/`MAKEFUNC`/`MAKEGEN` pool indices), concatenate native slots
     while preserving the native-slot table invariants (`slot = -1 - id`,
     non-negative/ascending/unique — QLangBytecodeIO.cpp:440–449).
   - Entry point: the **entry module's** top-level (`<init>`) function becomes
     `functions[0]` of the merged program; all other modules' `<init>` functions
     run in dependency order before it. (No `main` anywhere — spec.)
   - Import graph rules per spec (13-bytecodes.md:406–415): the C++ API can
     import .ql modules into a context; .ql may import C++ modules and other .ql
     modules; C++ modules cannot load .ql modules at this stage. C++ natives
     (`math`) stay compile-time natives; a .ql module may import them (names
     known at link time from the context registry).
3. **Driver** — `qlang_run` gains `qlang_run app.ql [lib1.qlc ...]` (or
   `--import`); single-file invocation keeps today's behavior.
4. **Tests** — `codec_check`: v4 round-trip incl. tables; loader: load a library
   module, link an app against it, run; missing export / cycle / version-mismatch
   each produce a clean `QLangError`. `spec_check2`: end-to-end cross-module call
   and cross-module constant read.

### 3c — cross-module variables/constants (`GETIMPORT`)

1. **Opcodes** (append before `kOpcodeCount`, currently 68):
   - `GETIMPORT` — `r[a] = <import table[b]>` (read an imported global var/const;
     reads go through the target module's global cell so writes made by the
     defining module are visible). Operands fit the existing `a`/`b` shorts —
     no `usesI32()` entry needed (precedent: `CMP` is a register-register op and
     is likewise not in `usesI32`).
   - **No `SETIMPORT`** — the spec makes imported entities readonly; a write
     through an import is a compile error.
   - Calls to imported functions need **no new opcode**: the linker renumbers
     `CALLFN` targets into the merged function vector.
2. **VM** — merged program keeps a global-cell area for module globals
   (extends the existing scope→main-regs seeding in `run()`, QLangVM.cpp
   1333–1336; the reverse sync on RETURN from main is 1620–1631); `GETIMPORT`
   lands in **all four** dispatch loops.
3. **Compiler** — an imported name resolves to `GETIMPORT` with an
   import-table index instead of a local register; a write to an imported name
   is a compile error; imported consts may fold to `CONST` from the (merged)
   pool at link time (int consts → `MOVI`).
4. **Tests** — write in the defining module visible to the importer via
   `GETIMPORT`; write through an import is a compile/link error; closure created
   in module A capturing a global and used from module B.
5. **Docs** — finalize `13-bytecodes.md` (operand row for `GETIMPORT`, status
   markers), rewrite `09-modules.md` (the "internal modules" framing is
   superseded; C++ modules become a module *kind*), update the 07-api.md:62
   "no VM changes needed for flat import" quote, `SERIALIZATION.md` v4,
   `07-api.md` (registry/link API), compliance hashes after green run.

---

## Spec cleanup (do in the round each phase lands)

Independent of implementation, `docs/spec/13-bytecodes.md` itself needs:

1. **Operand map is stale** — the table (lines 41–82) omits all nine of
   `SUBI`, `CMP`, `CMPI`, `JE/JNE/JG/JGE/JL/JLE` (all in the `Opcode` enum,
   QLangBytecode.h:179–197, since Round 32; all in `usesI32()` **except `CMP`**,
   a register-register compare). Add rows.
2. **Terminology** — line 344 "example of external modules are `ql` files"
   conflates source (`.ql`) and compiled artifact (`.qlc`); state that a compiled
   module is a `.qlc` with module name + tables. "Compatible bytecodes" is
   already defined at line 417 (exact same compiler version) — keep and cross-
   reference, don't redefine.
3. **Decompiler sample** (lines 105–119) shows a flat constant pool while the
   current implementation is per-function — Phase 2 makes the sample true; mark
   it accordingly.
4. **Status markers** — the "external modules" section (lines 338–416) has none;
   add `PROPOSAL` / `Implemented (Round N)` markers per subsection as they land.
5. **`docs/compliance/README.md` header** still says "last audit Round 29" while
   `status.md` reflects the SUBI/cmp/j* round — refresh it in each phase's round.

## Spec decisions (resolved in 13-bytecodes.md:397–417)

The earlier draft of this plan left these open; the spec's "technical
specifications" block now decides them:

1. **Link model** — the spec requires **both** link-time merge and a runtime
   module registry, and the two "should share a reusable solution". This plan
   ships link-time merge (`.qlc` v4); the export/import table types, module
   registry, and name resolution are built as standalone reusable units; the
   runtime registry itself is deferred (see out of scope).
2. **Entry point** — "global scope script only at this point. no main function.
   single file qlang modules in this version only." → no `main` in any module;
   the entry module is chosen by the driver, not the file format.
3. **Cross-module variable semantics** — shared cell (write-through). Combined
   with the `export` rules ("variables can be read but not modified"), imported
   entities are **readonly**: the defining module writes, importers read.
4. **Export syntax** — `export` keyword, global scope only; prefixed
   (`export def`/`export const`) and standalone (`export NAME;`) forms;
   exported entities readonly; duplicate export is an error; entities shared by
   pointer/reference.
5. **Import graph rules** — the C++ API can import .ql modules into a context;
   .ql may import C++ modules and other .ql modules; C++ modules cannot load .ql
   modules at this stage; a recursion check prevents import cycles; aliases
   (`from m import f as g`) are required.
6. **Compatibility rule** — "compatible bytecode is `qlang` bytecodes generated
   with the exact same compiler version."

Remaining plan-level decision (not a spec gap): module-name convention —
`export const MODULE_NAME` (the spec's example) vs file-stem fallback; decide in
the 3a round.

## Verification (per phase, per AGENTS.md)

- `cmake -B build && cmake --build build`
- `ctest --test-dir build` (all suites incl. `codec_check`, `spec_check2`)
- Compliance SHA256 hashes + tallies in `docs/spec/README.md` /
  `docs/compliance/status.md` updated **only after** the green run.

## Explicitly out of scope

- Runtime module registry — the spec requires it eventually (both link models
  "should share a reusable solution"); Phase 3 builds the reusable units (tables,
  registry data, resolution) but the registry itself is deferred.
- Decompiler mnemonics (`r`/`c` prefixes) — spec says DO NOT IMPLEMENT YET.
- Separating line/col from `Instruction` (8-byte layout) — spec says DO NOT
  IMPLEMENT.
- Inheritance (already deferred to v2 per `docs/plans/archive/HANDOFF.md`).
- Removing `JMPIFFALSE`/`JMPIFTRUE`/binary comparison ops — still needed.
- Serializing the builtin name table into `.qlc` (Phase 1 keeps it library-side).
- Qualified access syntax (`math.min(...)`) — stays a future extension (not in
  the spec's decisions; existing import forms already cover qualified use).
