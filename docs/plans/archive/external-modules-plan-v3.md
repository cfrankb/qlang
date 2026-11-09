# Plan v3: external modules (import/export tables, unified constant pool, builtin name table)

Supersedes: `external-modules-plan-v2.md` (2026-11-02). v3 re-verifies every
line reference and structural claim against the post-remediation tree
(2026-11-03, Round 35; `architecture-remediation-plan.md` Phases 0–5 complete,
now archived at `docs/plans/archive/`). The language spec is unchanged; what
changed is the machine: the four dispatch loops are one executor, the scope
sync is a global table, and `.qlc` is already at version 3.

Spec: `docs/spec/13-bytecodes.md` § "external modules" (lines 378–457, unmarked
proposal — no status marker yet). Its "technical specifications" block
(437–457) already resolves the link model, entry point, export syntax, import
graph, and compatibility rule — see "Spec decisions" below. Precedents: MOVI
(Round 30), ADDI (Round 31), SUBI + cmp/j* (Round 32) — all followed the
append-to-enum + dispatch + codec-version pattern.

**v3 (2026-11-03, post-Round 35) changes vs v2:**

- **Version numbering fixed.** v2 assumed the unified pool would bump `.qlc`
  2 → 3. The remediation plan's Phase 4 already consumed v3 (explicit
  little-endian encoding, non-durable rejection). This plan now bumps
  **v3 → v4** (Phase 2, unified pool) and **v4 → v5** (Phase 3, module
  tables). Readers accept 1..4 after Phase 2, 1..5 after Phase 3.
- **One dispatch loop, not four.** Remediation Phase 1 collapsed the four
  opcode switches into a single `QLangVM::runFrames()` (QLangVM.cpp:1564).
  Every "lands in all four loops" instruction becomes "lands in the single
  loop".
- **No scope sync to extend.** Remediation Phase 2 deleted the start/end
  scope sync: the main frame aliases the context's `GlobalTable` slots
  (QLangContext.h:49–54). Phase 3's merged-program global-cell design is
  written against the table, not the deleted seed/sync.
- **No pool dedup to preserve.** Remediation Phase 4 (F5) dropped the
  string-key constant dedup; `Bytecode::addConstant` appends per literal
  (QLangBytecode.cpp:39–43). Phase 2's shared pool is a plain append pool;
  cross-function dedup is an optional later optimization.
- **Builtin enum grew.** 21 entries now (`ToStr=0 … Release=20`; `Release`
  added by remediation Phase 3).
- **Spec cleanup list trimmed.** The operand map already gained the
  SUBI/CMP/CMPI/J* rows (Round 32); the `AUTHOR` example typo is fixed.
  Remaining: pre-rename spellings (CONST/DELETE/IN) and the stale `usesI32`
  list.
- **All line references re-verified** against the 2026-11-03 tree.

## Background / current state (re-verified 2026-11-03, post-Round 35)

- Compilation is **single-source → single `CompiledProgram`**
  (`QLangCompiler.h:28–37`); `functions[0]` is the implicit `"main"`
  (compiler enforces at `QLangCompiler.cpp:728`, `readQlc` enforces at
  `QLangBytecodeIO.cpp:515`, `QLangVM::run()` starts there,
  `QLangContext::call()` skips it).
- Constant pools are **per-function** (`Function::constants`,
  QLangBytecode.h:256) in memory, in `.qlc` (per-function
  `constCount × Value`), in the VM (fetched per-iteration at
  QLangVM.cpp:1570 — the `Frame` itself carries no pool), and in the
  disassembler. No cross-function dedup. Since `58a9659`, 32-bit ints are
  **MOVI immediates, not pool slots**; pools now hold only String/Float/Bool/
  nil/closures/Array/Table plus 64-bit literals (Qword/Long,
  QLangCompiler.cpp:577).
- "Modules" today = C++ native function sets only (`QLangContext::Module`,
  QLangContext.h:186–190; just `math`, registered at QLangModules.cpp:162–168).
  `import` (five forms — `import mod`, `import mod.f`, `import mod as a`,
  `from mod import *`, `from mod import f,g`; QLangCompiler.cpp:2023–2080)
  resolves at **compile time** into a flat negative-id native-slot space
  (spec 07-api.md:67: "no VM changes needed for flat import"; 09-modules.md:
  "v1 uses flat import only"). No `export`, no module identity in
  `CompiledProgram` or `.qlc`.
- `.qlc` = `QLC1` **v3** (`kQlcVersion`, QLangBytecodeIO.h:32; writer
  QLangBytecodeIO.cpp:378–441, reader 443–575). v3 (remediation Phase 4) made
  the on-wire encoding explicit little-endian, field by field, and made
  non-durable value tags (Generator/Object/External/BoundMethod/TypeRef) a
  hard write/read rejection. Reader accepts `1..3`, requires
  `functions[0].name == "main"` (515), rejects trailing bytes. Checked-in
  legacy fixtures: `examples/qla/test.qlc` is **v1**, `examples/qla/cmp.qlc`
  is **v2**. **No production loader** (`QLangContext::loadProgram` appears
  only in comments/docs; `docs/plans/archive/HANDOFF.md` lists it as a next
  step).
- Builtin names are hardcoded in **four places**: `Builtin` enum
  (`include/qlang/QLangBytecode.h:15–37`, 21 entries `ToStr=0…Release=20`),
  `QLangCompiler::builtinId()` (`src/QLangCompiler.cpp:3536`, returns `-1` on
  unknown, extra `str`/`int` aliases; call sites: 975 = name
  *classification*, 3199 = emit path), the VM `callBuiltin` switch
  (`src/QLangVM.cpp:432`), and the disassembler's `builtinName()`
  (`examples/qlang_run.cpp:186`, **app-local**; used at 363, 760).
- The opcode dispatch switch is **single** in `QLangVM.cpp`: `runFrames()`
  (1564). `run()`, `callFunction()`, and generator resumption all drive it
  (remediation Phase 1). Every new opcode lands in exactly one switch.
  (The decode/operand-check `switch(op)` sites are separate and unchanged.)
- Top-level variables live in the context's **`GlobalTable`**
  (QLangContext.h:49–54, remediation Phase 2): the main frame's register file
  aliases `globalSlots()` directly; there is no seed-in at run start and no
  sync-out at run end (or on throw). `delete` erases per slot.
- `SERIALIZATION.md` is current (documents v3, explicit LE encoding,
  non-durable rejection, R4 capture-rebinding load rule).
- `docs/compliance/README.md` is current (last audit Round 35, 2026-11-03).
- The spec's "technical specifications" (13-bytecodes.md:437–457) has
  **already decided** the questions the earlier drafts left open (link model,
  entry point, export syntax, import graph, compatibility) — see "Spec
  decisions" below; Phase 3 is written against those decisions.

The spec's five requirements map to three phases, each kept independently
shippable with `ctest` green:

| Phase | Spec requirement | Effort | Risk |
|-------|------------------|--------|------|
| 1 | builtin-function lookup table | S | Low |
| 2 | unified per-module constant table (+ `.qlc` v4) | M | Medium |
| 3 | import/export tables + standalone modules + `GETIMPORT`/`CALLIMPORT` (+ `.qlc` v5) | L | High |

---

## Phase 1 — builtin name table in the library (S)

Goal: one library-level id↔name table; the disassembler (and any future tool)
consumes it. No format change, no behavior change.

1. **`include/qlang/QLangBytecode.h` / `src/QLangBytecode.cpp`**
   - Add, backed by a single `k_builtin_names[]` array sized to
     `Builtin::Release + 1` (21 entries):
     - `int builtinCount();`
     - `const char *builtinName(int id);` (return `nullptr` for out-of-range)
     - `int builtinId(const std::string &name);` — **throws `QLangError`** on
       unknown names. For the emit path, where an unknown name is a bug.
     - `int findBuiltin(const std::string &name);` — returns **`-1`** on
       unknown names. For name *classification* queries, where "not a
       builtin" is a normal answer.
     The `str`→`ToStr`, `int`→`ToInt` aliases move here from the compiler.
2. **`src/QLangCompiler.cpp`** — delete the local `builtinId()` (3536);
   call-site 975 (`if (builtinId(id) >= 0) continue; // builtin`) switches to
   `findBuiltin`; call-site 3199 (emit path) switches to the throwing
   `builtinId`. Do **not** route 975 through the throwing API — that would
   turn a normal lookup into an exception.
3. **`src/QLangVM.cpp`** — `callBuiltin` (432) keeps its dispatch switch
   unchanged. The switch is keyed by the `Builtin` enum, so the only drift
   risk is enum↔array order, which the Phase 1 tests cover exhaustively.
4. **`examples/qlang_run.cpp`** — delete the app-local `builtinName()` switch
   (186); use the library function for the `# builtin:<name>` notes (363, 760).
5. **Tests** — `tests/unit/codec_check.cpp`: table covers every `Builtin` id
   (0..builtinCount()-1) with non-empty names; `builtinId` round-trips every
   name incl. aliases and throws on unknown; `findBuiltin` returns -1 on
   unknown; disassembly of a program calling several builtins prints the
   expected names.
6. **Docs** — `13-bytecodes.md` "external modules" § (builtin-table
   subsection, 400–404): note that the builtin table is `builtinName`/
   `builtinId`/`findBuiltin` in `QLangBytecode.h` (single source of truth),
   and that it is a *library* table keyed by compiler version (not serialized
   in `.qlc`). Compliance hash update **after** green run (AGENTS.md).

Out of scope here: serializing the table into `.qlc` (only needed if third-party
tools must decode `.qlc` without the library — revisit then).

## Phase 2 — unified per-module constant table + `.qlc` v4 (M)

Goal: one constant pool per module (program), replacing per-function pools.
Prerequisite for Phase 3's link-time merge.

1. **`include/qlang/QLangBytecode.h`**
   - Move the pool from `Function` to program level: `CompiledProgram` gains
     `std::vector<QLangValue> constants;`. `Function::constants` removed.
   - `Bytecode::addConstant`/`setConstants` (QLangBytecode.cpp:39–47) become
     the single pool manager for all functions. Note: there is **no dedup
     machinery to preserve** — remediation Phase 4 (F5) dropped the string-key
     dedup, so `addConstant` appends per literal. The spec's requirement is
     "share a common const table **or** at the very least must not duplicate
     constants" (13-bytecodes.md:408): a shared pool satisfies the first
     clause. Cross-function dedup (e.g. a 64-bit digest map) is an optional
     later optimization, not a Phase 2 requirement.
2. **`src/QLangCompiler.cpp`**
   - `buildFunction()` stops resetting the pool per function (today
     `m_code = Bytecode()` at 923 discards the enclosing pool saved earlier,
     restored at 1074): all `emitConst` calls hit the shared pool across
     `buildFunction` save/restore. Ints never reach the pool since `58a9659`
     — they are MOVI immediates (`emitImm`), so only non-Int values (String,
     Float, Bool, nil, closures, Array, Table, 64-bit literals) enter it.
   - `compile()` no longer copies main's pool into a second place; the
     `CompiledProgram::code` mirror of `functions[0]` is dropped (it
     duplicates instructions *and* constants today, QLangCompiler.cpp:743–747,
     QLangCompiler.h:29).
3. **`src/QLangVM.cpp`** (the **single** dispatch loop)
   - Frames carry no pool — `runFrames()` fetches `fn.constants` into a local
     `consts` (1570); point that fetch at `m_prog.constants` instead.
     `MOVC`/`MAKEFUNC`/`MAKEGEN` operand `b` then indexes the program pool.
     Closure values in the pool store `funcIndex` + capture regs (no pool
     indices, QLangVariant.h) — unaffected.
4. **`src/QLangBytecodeIO.cpp`** — `.qlc` **v4**:
   - `kQlcVersion` 3 → 4.
   - Writer: emit one module-level pool section (`u32 constCount +
     constCount × Value`) after the function records (function records lose
     `constCount × Value`).
   - Reader: version-branched —
     - **v4**: read the module pool into `CompiledProgram::constants`.
     - **v1/v2/v3**: keep the per-function decode, then **expand** each
       function's pool into `CompiledProgram::constants` in function order
       and **remap** `MOVC`/`MAKEFUNC`/`MAKEGEN` operand `b` by the
       per-function base offset. (Once `Function::constants` is gone from
       the struct, legacy files have no other home for their pools.)
     - Accept `≤ 4`.
   - **Fixtures: do NOT regenerate** the checked-in samples —
     `examples/qla/test.qlc` (v1) and `examples/qla/cmp.qlc` (v2) are the
     legacy-decode fixtures; v4 coverage is generated in-test.
   - `tests/unit/codec_check.cpp` asserts the written version byte == 3
     (~300–309) and has a malformed-input suite rejecting "version 4"
     (~628–630) — both updated in this round.
5. **`examples/qlang_run.cpp` disassembler** — print one flat `constants:`
   list per program (this matches the spec's own decompiler sample, 13-
   bytecodes.md:114–128, which already shows a flat pool — docs are currently
   ahead of code).
6. **Tests**
   - `spec_check2`: same **runtime** outputs as today (behavior-invariant
     refactor); disassembly *text* changes (flat pool; indices may shift) —
     update any disassembly-text assertions. A program where two `def`s use
     the same string literal compiles to **one** pool (int sharing is already
     slot-free via MOVI — no pool assertion needed).
   - `codec_check`: v4 write/read round-trip (pool equality); v1/v2 fixtures
     load with correctly remapped pool indices; v4 file with a pool index out
     of range is rejected.
7. **Docs** — `SERIALIZATION.md` extended to v1+v2+v3+v4 (it currently
   documents v3); `13-bytecodes.md` unified-constant-table note (406–408) +
   status marker; compliance hashes after green run.

Risks: behavior-invariant on its face, but it touches every layer (compiler,
VM, codec, disassembler). Keep it a standalone round so any regression is
easy to bisect.

## Phase 3 — import/export tables, standalone modules, `GETIMPORT` + `CALLIMPORT` (L)

The spec's decisions (see "Spec decisions" below) fix the design: **this
version ships link-time merge** — loading resolves imports against loaded
modules and produces one flat `CompiledProgram` (renumbered function indices,
offset registers, offset/merged pools, patched closure constants). This
preserves the VM's single-`functions`-vector invariant. The spec requires a
**runtime module registry too**, with both models "sharing a reusable
solution" — so the export/import table types, module registry data, and
name-resolution logic are built as standalone units that a runtime-registry
implementation can reuse; the registry itself is deferred.

Key consequences of the spec's decisions for this phase:

- **No `main` function** in modules (spec 440: "global scope script only ...
  no main function"). Every module is a global-scope script; top-level code
  compiles to the module's entry/`<init>` function. The
  `functions[0] == "main"` check applies only to legacy v1–v4 files; the
  entry module is chosen by the driver, not the file format.
- **Imports are readonly** (spec 415–417: "exported entities are `readonly`
  ... variables can be read but not modified"). Only **`GETIMPORT`** is
  needed for data — no `SETIMPORT`. A write through an import is a compile
  error.
- **Shared cell semantics** (spec 442): `GETIMPORT` reads the defining
  module's global cell (its `<init>`/top-level writes are visible to
  importers); storage is shared, mutation is allowed only in the defining
  module. *(Plan-level interpretation — see "Export semantics" in 3a.)*
- **Module name**: no `module` declaration in v1 — the spec's own example
  exposes it as `export const MODULE_NAME = "my_module"` (425). Single-file
  modules only in this version.
- **Cross-module calls go through the import table in module bytecode**
  (per spec 392/398 "bytecode to reference the look up table"): the compiler
  emits **`CALLIMPORT`** with an import-table index; the merge-model linker
  rewrites it to a plain `CALLFN` in the merged program; a future registry
  model would execute `CALLIMPORT` directly. The current function-index logic
  is preserved as the *merged* form, not as the module form.

### 3a — module identity + export construct + tables (no VM change yet)

1. **Module name** — the spec's example exposes it as
   `export const MODULE_NAME = "my_module"`; decide whether that is
   mandatory, with file-stem fallback. No `module` declaration in v1.
2. **Lexer/parser** — `export` keyword, both spec forms (410–435): prefixed
   (`export def f()`, `export const k = …`) and standalone (`export NAME;`).
   AST: export flags on existing top-level `Def`/`Var`/`Const` stmts.
3. **Export semantics** (the spec's rules need enforcement points):
   - **Defining-side write protection** via the existing `constVars` /
     `guardNotConst` path: an exported **function** name is un-rebindable
     everywhere (spec: "functions can be executed but they not modified"); an
     exported **variable** is mutable in the defining module, immutable in
     importers (shared-cell semantics). *The spec is ambiguous — "variables
     can be read but not modified" (417) can be read as readonly even in the
     defining module. This plan takes the defining-module-may-mutate reading
     (required for shared cells to be useful); pin it in the spec in the 3a
     round.*
   - **Standalone-form resolution**: `export NAME;` resolves `NAME` against
     global-scope declarations in a post-parse pass; undefined name = compile
     error; forward references (export before declaration) allowed; duplicate
     export of the same name via any combination of forms = "already exported"
     (matches the spec's own example, 432–433).
   - **`export` in the entry module**: allowed, no-op (nothing imports the
     entry module).
4. **Compiler** — `CompiledProgram` gains:
   - `std::string moduleName;`
   - `std::vector<ExportEntry> exports;` — `{kind: Func|Var|Const, name, ref}`
     where `ref` = function index / global register / pool index.
   - `std::vector<ImportEntry> imports;` — `{module, name, kind, ref, alias?}`
     where `ref` is **resolved at compile time against the defining
     module's layout** (function index / global register / pool index within
     the defining module); the linker remaps `ref` into the merged layout.
     Alias support per the spec's `from m import f as g` (455).
   - Every module (application or library) is a global-scope script:
     top-level code compiles as the module's entry/`<init>` function recorded
     in the module header; no `main` requirement. The compiler API today is
     a single option-less `compile()` (QLangCompiler.h:86) — library mode is
     a new compile flag/entry.
5. **`.qlc` v5** — module name string, export table section, import table
   section (entries carry compile-time-resolved refs; the linker remaps
   them, it does not fill them). Reader accepts ≤ 5; the
   `functions[0] == "main"` check applies only to legacy v1–v4 files.

### 3b — loader + linker

1. **`QLangContext::loadProgram(...)`** (the long-missing loader,
   `docs/plans/archive/HANDOFF.md` item) — reads a `.qlc` module, stores
   name + export table in a module registry on the context (the reusable
   unit the future runtime-registry model builds on).
2. **Import resolution at compile time**: the driver loads library `.qlc`s
   **before** compiling; the compiler's import resolver (extending the
   existing five forms at QLangCompiler.cpp:2023–2080, which today resolves
   only against `QLangContext::Module`) gains a second lookup source — loaded
   `.ql` export tables:
   - `from mod import *` = all export entries; `import mod.f` /
     `from mod import f` = one entry; aliases per spec.
   - A name found in **both** a C++ module and a `.ql` module = compile
     error.
   - C++ natives (`math`) stay compile-time natives in the negative `CALL`
     slot space; the import table carries only `.ql`→`.ql` entities.
   - Compile errors (all raised here, not at link time): unknown module,
     unknown export name, duplicate export, `export` outside global scope,
     write through an import, rebind of an exported function name,
     import-cycle detection (graph check across the loaded module set,
     per spec 453).
3. **Linker** (new `QLangLinker` unit) — purely mechanical merge; if
   compilation succeeded, linking cannot fail (any failure is an internal
   error):
   - **Functions**: concatenate; each non-entry module gets a register
     offset = cumulative `registerCount` of earlier modules (disjoint
     register ranges); offset every register operand (`a`/`b`/`c`) in the
     module's instructions by its offset.
   - **Pools**: concatenate (Phase 2's per-module pool); offset
     `MOVC`/`MAKEFUNC`/`MAKEGEN` pool indices.
   - **Calls/closures**: offset `CALLFN`/`MAKEFUNC`/`MAKEGEN` function
     indices and closure `funcIndex` values in pool constants.
   - **`CALLIMPORT`**: resolve import entry → export entry → function;
     rewrite to `CALLFN` with the merged function index.
   - **Native slots**: concatenate while deduping identical names and
     preserving the native-slot table invariants (`slot = -1 - id`,
     non-negative/ascending/unique — QLangBytecodeIO.cpp:518–532).
   - **Entry point**: the **entry module's** top-level (`<init>`) function
     becomes `functions[0]` of the merged program; all other modules'
     `<init>` functions run in dependency order before it. (No `main`
     anywhere — spec.)
   - **Merged `vars`/`constVars`**: the entry module's maps are kept;
     non-entry modules' name→register maps are dropped (per-module debug
     maps optional). Cross-module name collisions are legal — the modules
     live in disjoint register ranges.
   - **Global cells**: the main frame's register file aliases the context's
     `GlobalTable` slots (remediation Phase 2 — there is no seed/sync to
     extend). For a merged program the table must span the **full merged
     register range** (per-module sub-ranges), so `GETIMPORT` reads the
     defining module's live cell. Design detail to resolve in 3b: one flat
     table over the merged span vs. per-module sub-tables; the former keeps
     the existing aliasing mechanism unchanged.
4. **Driver** — `qlang_run` gains `qlang_run app.ql [lib1.qlc ...]` (or
   `--import`); single-file invocation keeps today's behavior.
5. **Tests** — `codec_check`: v5 round-trip incl. tables; loader: load a
   library module, compile+link an app against it, run; unknown export /
   cycle / version-mismatch each produce a clean `QLangError`. `spec_check2`:
   end-to-end cross-module call and cross-module constant read.

### 3c — cross-module variables/constants (`GETIMPORT`) + calls (`CALLIMPORT`)

1. **Opcodes** (append before `kOpcodeCount`, currently 68 → becomes 70):
   - `GETIMPORT` (68) — `r[a] = <import table[b]>` (read an imported global
     var/const; reads go through the target module's global cell so writes
     made by the defining module are visible).
   - `CALLIMPORT` (69) — `r[a] = call import[b](r[a+1 .. a+c])` (call an
     imported function; `b` = import-table index, `c` = arg count — same
     operand shape as `CALLFN`).
   Both are register-indexed — **no `usesI32()` entries** (precedent: `CMP`
   is a register-register op and is likewise not in `usesI32`).
   - **No `SETIMPORT`** — the spec makes imported entities readonly; a write
     through an import is a compile error.
   - In the **merged** program, `CALLIMPORT` does not survive (linker
     rewrites it to `CALLFN`); `GETIMPORT` does. In the **module** bytecode
     both are the cross-module reference mechanism — this is what lets a
     future runtime-registry model execute module bytecode directly with no
     further opcode work.
2. **VM** — the merged program's global-cell area spans the full merged
   register range (see 3b "Global cells"); `GETIMPORT` (and `CALLIMPORT`,
   for the registry path) lands in the **single** `runFrames()` dispatch
   loop.
3. **Compiler** — an imported name resolves to `GETIMPORT` with an
   import-table index (var/const) or `CALLIMPORT` (function) instead of a
   local register; a write to an imported name is a compile error; imported
   consts may fold to `MOVI` (int) / `MOVC` from the (merged) pool at link
   time.
4. **Tests** — write in the defining module visible to the importer via
   `GETIMPORT`; write through an import is a compile error; rebind of an
   exported function name is a compile error; closure created in module A
   capturing a global and used from module B; merged program contains no
   `CALLIMPORT`.
5. **Docs** — finalize `13-bytecodes.md` (operand rows for `GETIMPORT`/
   `CALLIMPORT`, status markers), rewrite `09-modules.md` (the "internal
   modules" framing is superseded; C++ modules become a module *kind*),
   update the 07-api.md:67 "no VM changes needed for flat import" quote,
   `SERIALIZATION.md` v5, `07-api.md` (registry/link API), compliance hashes
   after green run.

---

## Spec cleanup (do in the round each phase lands)

Independent of implementation, `docs/spec/13-bytecodes.md` itself needs:

1. **Operand map uses pre-rename spellings** — the table (lines 41–91)
   already includes the SUBI/CMP/CMPI/J* rows (Round 32), but still spells
   `CONST` (line 43), `DELETE` (line 52), and `IN` (line 49) — now
   `MOVC`/`DELETEx`/`INx`. Rename.
2. **`usesI32` list is stale** — lines 29–31 list only the jump family plus
   `MOVI`/`ADDI`; the code (QLangBytecode.h:202–224) also covers `SUBI`,
   `CMPI`, and `JE/JNE/JG/JGE/JL/JLE`. Update.
3. **Terminology** — line 384 "example of external modules are `ql` files"
   conflates source (`.ql`) and compiled artifact (`.qlc`); state that a
   compiled module is a `.qlc` with module name + tables. "Compatible
   bytecodes" is already defined at line 457 (exact same compiler version) —
   keep and cross-reference, don't redefine.
4. **Decompiler sample** (lines 114–128) shows a flat constant pool while the
   current implementation is per-function — Phase 2 makes the sample true;
   mark it accordingly.
5. **Status markers** — the "external modules" section (lines 378–457) has
   none; add `PROPOSAL` / `Implemented (Round N)` markers per subsection as
   they land.

(The v2 draft's "example typo" item is done: line 431
`AUTHOR = "cfrankb";` now carries its closing quote.)

## Spec decisions (resolved in 13-bytecodes.md:437–457)

The earlier drafts of this plan left these open; the spec's "technical
specifications" block now decides them:

1. **Link model** — the spec requires **both** link-time merge and a runtime
   module registry, and the two "should share a reusable solution" (439).
   This plan ships link-time merge (`.qlc` v5); the export/import table
   types, module registry, and name resolution are built as standalone
   reusable units; the runtime registry itself is deferred (see out of
   scope). With `GETIMPORT`/`CALLIMPORT` in the module bytecode, the shared
   solution now covers the *encoding* as well as the tables — the registry
   model executes module bytecode directly.
2. **Entry point** — "global scope script only at this point. no main
   function. single file qlang modules in this version only." (440) → no
   `main` in any module; the entry module is chosen by the driver, not the
   file format.
3. **Cross-module variable semantics** — shared cell (write-through, 442).
   Combined with the `export` rules ("variables can be read but not
   modified"), imported entities are **readonly to importers**: the defining
   module writes, importers read.
4. **Export syntax** — `export` keyword, global scope only; prefixed
   (`export def`/`export const`) and standalone (`export NAME;`) forms
   (410–435); exported entities readonly; duplicate export is an error;
   entities shared by pointer/reference.
5. **Import graph rules** (446–453) — the C++ API can import .ql modules
   into a context; .ql may import C++ modules and other .ql modules; C++
   modules cannot load .ql modules at this stage; a recursion check
   prevents import cycles; aliases (`from m import f as g`) are required.
6. **Compatibility rule** — "compatible bytecode is `qlang` bytecodes
   generated with the exact same compiler version." (457)

Plan-level decisions made in this draft (not spec gaps — to be ratified
in the 3a round):

7. **`CALLIMPORT` encoding** — module bytecode references the import table
   for cross-module function calls; the merge-model linker rewrites
   `CALLIMPORT` → `CALLFN`; a future registry model executes `CALLIMPORT`
   directly. (Consequence: a *merged* program is merge-model-specific; the
   *module* `.qlc` is model-agnostic.)
8. **Register-range renumbering** — each non-entry module's registers are
   offset into a disjoint range (cumulative `registerCount`); the merged
   `vars`/`constVars` reflect the entry module only; cross-module name
   collisions are legal.
9. **Import resolution timing** — imports resolve at **compile time**
   against loaded module export metadata (import entries carry refs valid in
   the defining module's layout); the linker is purely mechanical and
   cannot fail on a successfully compiled program.
10. **Defining-side export semantics** — the defining module may mutate its
    exported variables; exported function names are un-rebindable anywhere.
    *(Pending spec pin-down — the spec's wording is ambiguous.)*

## Verification (per phase, per AGENTS.md)

- `cmake -B build && cmake --build build`
- `ctest --test-dir build` (all suites incl. `codec` (codec_check) and
  `spec_check2`)
- Compliance SHA256 hashes + tallies in `docs/spec/README.md` /
  `docs/compliance/status.md` updated **only after** the green run.

## Explicitly out of scope

- Runtime module registry — the spec requires it eventually (both link
  models "should share a reusable solution"); Phase 3 builds the reusable
  units (tables, registry data, resolution) **and** the table-dispatching
  opcodes (`GETIMPORT`/`CALLIMPORT`) the registry model will execute
  directly, but the registry itself is deferred.
- Decompiler mnemonics (`r`/`c` prefixes) — spec says DO NOT IMPLEMENT YET.
- Separating line/col from `Instruction` (8-byte layout) — spec says DO NOT
  IMPLEMENT.
- Inheritance (already deferred to v2 per `docs/plans/archive/HANDOFF.md`).
- Removing `JMPIFFALSE`/`JMPIFTRUE`/binary comparison ops — still needed.
  (Note: the spec's "bytecode encoding guidelines" section, 459+, pushes
  toward flag-based jumps; cross-reference it so this doesn't get "fixed"
  ad hoc.)
- Serializing the builtin name table into `.qlc` (Phase 1 keeps it
  library-side).
- Qualified access syntax (`math.min(...)`) — stays a future extension (not
  in the spec's decisions; existing import forms already cover qualified
  use).
- Cross-function constant dedup — the shared pool (Phase 2) satisfies the
  spec's "share a common const table" clause; dedup is a performance
  optimization deferred per remediation Phase 4 (F5) rationale.
