## Round 64 (2026-11-09)

**External-modules plan Phase 3b — a module becomes *importable*: the loader, the module registry,
compile-time resolution against an export table, and `QLangLinker`.**
Plan `docs/plans/external-modules-plan-v4.md` Phase 3b (steps 1–5); spec
`docs/spec/13-bytecodes.md` `## import table (lookup)` is now **Implemented** (the table is filled
and consumed) and `## technical specifications` carries a per-item status. **No `.qlc` version
change** — still **v9**: everything Phase 3b adds is either link-time state that an image must not
carry, or a *refusal* (`writeQlc` rejects a merged program). Single-file behavior is unchanged: the
23-script corpus is stdout byte-identical.

- **Why it was worth doing.** Round 63 gave an image a name and two tables, but nothing could read
  them: `import` still bound C++ modules only, and a `.qlc` that exported names was a curiosity.
  This round is the consumer — load an image, resolve an `import` against its export table, and
  merge the images into one program that actually runs the other module's code and cells.
- **What landed.**
  - **One source for "what does this operand name?"** — `OperandRole` +
    `operandRole(op, slot, kind)` (`include/qlang/QLangBytecode.h`, `src/QLangBytecode.cpp`):
    `None` / `Register` / `PoolIndex` / `FunctionIndex` / `NativeSlot` / `CaptureIndex` /
    `JumpTarget`. Two consumers must not disagree about the answer — the `.qlc` reader, which
    range-checks every table reference an image carries, and the linker, which rewrites them. The
    reader gained the check that came from it: a `callfn` operand outside `[0, funcCount)` is now
    `function index out of range` at load (`docs/SERIALIZATION.md` §5.1), where before only
    const-pool indices were checked.
  - **The codec can now go backwards** — `encodeRawStream()` was split out of
    `FnEmitter::finish()` and `decodeRawStream()` added (`QLangCompiler.h`), so a stream that came
    out of an image rather than an emitter is encoded by the *same* code that chose the widths in
    the first place. This is **Q32** made mechanical: the linker decodes, rewrites the
    `RawInstr`, re-encodes; no encoded byte is ever patched in place, so a merged index that needs
    a wider operand costs nothing.
  - **The loader** — `QLangContext::loadProgram(istream, path)` / `loadProgramFile(path)` /
    `loadedModule(name)` / `loadedModules()`, and `LoadedModule` (image + `path` + the on-disk
    native-slot table) in `QLangBytecodeIO.h`. The registry lives in the pimpl (a `ModuleRegistry`
    beside `ProgramCache`/`TypeRegistry`), keyed by `program.moduleName` — the name `import` binds
    it under — and rejects a duplicate name. Every native-slot **name** is re-resolved against
    this context's natives *and* its registered C++ modules, all missing names reported in one
    `QLangError(Io)` (`missing native function(s): 'a', 'b'`), so a library's `import math` works
    without the host repeating the mapping. `clear()` drops the registry.
  - **Compile-time resolution** — `import m` / `import m.a` / `from m import *` /
    `from m import a, b` / `import m as x` now check the loaded images first
    (`compileQlImport`): each bound name becomes an `ImportEntry` (so `CompiledProgram::imports`
    is **no longer always empty**), a name claimed by both a C++ module and a `.ql` module is a
    compile error, as is an unknown module, an unknown export, a write through an import
    (`guardNotImported` on assignment, `++`/`--` and `delete` — importer-side `readonly`, **Q30**,
    enforced at last), an import inside a function body where the lowering cannot reach, and an
    import cycle (`checkImportCycles`, with a second half in the linker). C++-registered modules
    are untouched: still compile-time natives in the negative-slot space of `CALL`, never table
    entries.
  - **Provisional lowering (option B).** `GETIMPORT`/`CALLIMPORT` are Phase 3c, so an imported
    name lowers to the instruction the merged program can execute — `callfn` for a function, `mov`
    for a variable/const/closure — carrying the **defining** module's handle. An operand cannot
    say "this is not mine" (every index operand is unsigned), so the reference is recorded as a
    `PendingLink {funcIndex, instrIndex, importIndex, kind, poolIndex}` in
    `CompiledProgram::pendingLinks`, with `kind` = `OperandB` | `ClosureConst` (an imported
    generator's `MAKEGEN` spec names the defining module's function through a pool constant).
    **Q31 untouched:** nothing was appended to the enum, `kOpcodeUsedCount` stays **72**, and 73/74
    stay reserved.
  - **Global captures are named absolutely.** A capture whose home is a module-level cell is
    stored as `-1 - <absolute global-table slot>` instead of a register (compiler +
    `QLangVM::bindCaptures`). A merged program's `vars` map holds only the entry module's names
    and a `.qlc` carries no capture names at all, so name-based re-derivation could not work — and
    an imported `def` is called with `callfn`, so its capture spec cannot depend on who calls it.
  - **The linker** — new unit `QLangLinker` (`include/qlang/QLangLinker.h`,
    `src/QLangLinker.cpp`, 4th library source file). Depth-first, dependencies first, it
    concatenates the function tables, gives each module a **slice** of the shared global register
    area, folds the constant pools through `valueKey` (**Q33** — a literal two modules share
    occupies one slot, and closure constants are rebased *before* the fold so it compares merged
    function indices), concatenates the native slots deduped by name, rewrites the pending links,
    and records `CompiledProgram::initSequence` (**Q35**). It never loads anything and never
    decides anything about meaning; it can only fail on an image that does not fit (module not
    loaded, an entry that is already linked, a library carrying pending links or its own `.ql`
    imports, a merged reference out of range).
  - **The VM** — `RegFile` gained `base` (register 0 of this frame's address space; always 0 for
    an owned frame) and `run()` executes `initSequence` before the entry's top level: each module
    top level is a main-like frame aliasing the same `GlobalTable::slots` at its own offset, so
    every module's `<init>` runs exactly once, dependencies first, and its globals are live cells
    by the time the entry reads them.
  - **The driver** — `qlang_run app.ql [lib.qlc …]` and `--import <lib.qlc>` (repeatable, same
    thing): libraries are loaded into the context **before** the entry compiles and linked after.
    Level 5 gained the merged import table (`module.name -> merged handle`) and one block per
    linked module (its `regBase`, its function range, its exports as both module-local and merged
    refs). `--output` is refused for a merged program — see the format note.
- **Format.** No version bump. Two refusals instead: `writeQlc` rejects a program with a non-empty
  `initSequence` or `pendingLinks` (`docs/SERIALIZATION.md` §4, "A linked program is not an
  image" — those fields have no section, and an image that carried them would run against the
  wrong functions and cells), and the reader's new `callfn` range check closes the last
  table-reference hole. `LoadedModule` is deliberately not a `.qlc` concept: it is an image plus
  provenance the *host* holds.
- **Decisions (plan §Open questions).** **Q32** ratified — rewrite the decoded `RawInstr` and
  re-encode with `encodeRawStream()`. The plan's "preferred" variant (rewrite before
  `FnEmitter::finish()`) is not applicable: the linker's input is an encoded image, so the
  plan's *fallback* became the only path, and it is safe for the reason the plan gives (jump
  offsets are relative and the merge preserves instruction count). **Q33** ratified as defaulted —
  the merged pool folds cross-module duplicates through `valueKey`. **Q35** ratified as defaulted
  — each module's `<init>` runs exactly once, in dependency order, before the entry's.
- **Deviations from the plan as written.** (1) **No register operand is rewritten.** The plan
  offsets each non-entry module's register operands; the implementation instead gives that
  module's top-level frame a `base` into the shared table, so a module keeps the numbers its
  compiler assigned and only the operands the compiler flagged (pending links) move. (2) The
  entry's `functions[0]` stays the entry's top level; the libraries' `<init>` records are appended
  and run through `initSequence` rather than the entry's being promoted to index 0. (3) The plan's
  "`CALLIMPORT` → `CALLFN`" step could not happen — there is no `CALLIMPORT` yet — hence the
  provisional lowering above. (4) A **library** image cannot import another `.ql` module in 3b:
  the unresolved references it would need are a link artifact the format does not carry, so the
  compiler rejects it with a message naming Phase 3c (an entry importing two libraries works).
  (5) The end-to-end cases live in the new `linker` suite rather than `spec_check2` (that harness
  is SPEC.md's; the loader/linker is plan-level). (6) `QLangContext::execute()`
  (`SERIALIZATION.md` §5.2/§6) was **not** added — running a loaded image is
  `QLangVM(prog, ctx).run()`, which is what the `qlc-exec` gate and `qlang_run` already do; a
  second entry point over the registry would be a second way to do the same thing.
- **Verification.** `cmake -B build && cmake --build build` clean; `ctest --test-dir build`
  **53/53 green** (`doc_hashes` skipped by default). New suite **`linker`**
  (`tests/unit/linker_test.cpp`, **75** checks): the registry (load by stream/file, lookup, load
  order, duplicate name, unresolvable native, `clear()`), the merge (one function table, one
  folded pool, deduped native slots, disjoint register slices), the rewrites (a 300-`def` entry
  pushes a merged function index past the 1-byte operand range — the re-widening is real, not
  vacuous), the run (each `<init>` once, dependencies first; an exported variable is one cell the
  defining module writes and the importer reads), and the rejections (already linked, not loaded,
  not linkable, `writeQlc` of a merge). `codec_check` 352 → **354** (+2: a `callfn` naming a
  function of the image loads, one naming index 5 of 1 is refused with `function index out of
  range` — the pair proves the new check is neither vacuous nor over-eager). Corpus
  `tests/scripts/*.ql` stdout **byte-identical** to the pre-change build (23/23, exit 0 on both,
  measured against a `HEAD` build of `qlang_run`). Driver smoke: a `setLibraryMode` image with
  four exports, `qlang_run app.ql lib.qlc` and `--import lib.qlc app.ql` both run the library's
  `<init>`, call its `def` across the boundary and read its exported cell.
- **Found, not fixed.** An exported `def` that calls a **sibling `def` which itself has captures**
  cannot be imported: the callee is materialized as a closure value in a top-level register and the
  caller captures that register frame-relative, which only the defining module's frame can seed.
  The compiler rejects it with `imported function 'b' captures a local of its own module and
  cannot be called across a module boundary yet (external-modules plan Phase 3c)` — a clean
  compile error, not a wrong answer — and the guard's comment, which claimed this "only fires on
  something the compiler could not prove", was corrected to name the real case. The general fix is
  Phase 3c's (a capture of a module cell named absolutely, or `GETIMPORT` reading the cell).
- **Docs.** `docs/SERIALIZATION.md` §4 (a linked program is not an image), §5.1 (the `callfn`
  check, both checks from `operandRole()`), §6 (the registry API replaces the planned
  `loadProgram`/`execute` sketch), §9 item 3, §10; `docs/spec/13-bytecodes.md` (`## import table
  (lookup)` → **Implemented**, `## export table (lookup)` gains what reads it, `## keyword export`
  gains importer-side `readonly`, `# external modules` gains its status and the source-vs-artifact
  wording, `## technical specifications` gains a per-item status); `docs/compliance/*` (this
  round); `docs/plans/external-modules-plan-v4.md` (status header, Q32/Q33/Q35 ratified, 3b
  **DONE**).
- **Tallies re-recorded.** counted unit cases 37 821 → **37 898** (+75 `linker`, +2 `codec_check`);
  ctest entries 52 → **53**; unit harnesses 26 → **27**; `encoding_v6`, `flag_flow` and the perf
  baseline untouched.

## Round 63 (2026-11-09)

**External-modules plan Phase 3a — an image becomes a *module*: `export` syntax, the export and
import tables, a module name, and `.qlc` v9.**
Plan `docs/plans/external-modules-plan-v4.md` Phase 3a (steps 1–5); spec
`docs/spec/13-bytecodes.md` `## keyword export` is now **Implemented**, alongside the
`## export table (lookup)` / `## import table (lookup)` notes. **On-disk breaking change — `.qlc`
v8 → v9** (single-version reader: v1–v8 rejected at the version field). Runtime behavior is
unchanged — nothing executes the new sections yet; Phase 3a is the write side.

- **Why it was worth doing.** Phase 3's linker needs a module to *say* what it exposes and what it
  needs; until an image carried an export table there was nothing to resolve an `import` against,
  and `qlang_run`'s level-5 listing could only print "what main defines and could export".
- **What landed.** `export` is a keyword (`QLangLexer`) with both spec forms
  (`docs/spec/13-bytecodes.md` `## keyword export`): the **prefix** form marks the `def` / `const` /
  assignment / arrow-closure it prefixes (`Stmt::isExported`), the **standalone** form is a new
  `StmtKind::ExportRef` naming a global declared earlier *or later*. Global scope only: the parser
  tracks a `m_bodyDepth` (`BodyDepthGuard` in `def`/arrow/`class` bodies) and rejects the keyword
  there, and the compiler's pre-pass rejects it inside a global-scope `if`/`for`/`while`/`switch`
  block, where it would otherwise compile and be silently dropped from the table.
  `QLangCompiler::collectExports()` runs **before** `compile()` — so a standalone export resolves
  forward, a duplicate is `'<name>' is already exported`, and an exported `def`'s name is already
  un-rebindable when the body compiles (`guardNotExportedFunc` on assignment and `++`/`--`);
  `finishExports()` resolves each entry to its function index or register afterwards.
  `CompiledProgram` gains `moduleName`, `exports` (`ExportEntry {kind, name, ref}`,
  `ExportKind` = `Var` 0 / `Const` 1 / `Func` 2) and `imports`
  (`ImportEntry {module, name, kind, ref, alias}`), declared in `include/qlang/QLangBytecode.h`.
  `setModuleName` / `setLibraryMode` are the new compiler options: library mode compiles the top
  level as `<init>` instead of `main` and requires `export const MODULE_NAME`.
- **Decisions (plan §Open questions).** **Q29** ratified as defaulted — the name is the module's own
  `export const MODULE_NAME` (a `const` holding a constant string, so a loader reads it without
  running the module), else the driver's name (`qlang_run` passes the source file stem), else
  `"main"`; mandatory only in library mode. **Q30** ratified as defaulted — `readonly` is a property
  of the *importer*: the defining module keeps writing to an exported variable (the shared cell of
  `## technical specifications` 3 is write-through), while rebinding a name exported as a `def` is a
  compile error. **Q31** untouched: `GETIMPORT`/`CALLIMPORT` stay reserved at 73/74 for Phase 3c, so
  `kOpcodeUsedCount` stays **72** and the import table is always **empty** here (`import` still binds
  C++ modules through the negative-slot space of `CALL`).
- **Format.** v9 appends three sections **after `registerCount`** — `moduleName`, the export table,
  the import table (`docs/SERIALIZATION.md` new §4.6) — so every v8 section keeps its byte position:
  `hello.ql`'s image is 3 792 B → 3 809 B, byte-identical through `registerCount` plus a 17-byte
  tail. The reader validates the tail as it reads it (non-empty module name, `kind <= Func`,
  non-empty entry name, non-negative `ref`, a `Func` ref inside `[0, funcCount)` and a `Var`/`Const`
  ref inside `[0, registerCount)`, no duplicate export name, both counts under the §8 cap) and the
  `functions[0].name == "main"` check becomes `"main"` **or** `"<init>"`.
- **Verification.** `cmake -B build && cmake --build build` clean; `ctest --test-dir build`
  **52/52 green**. `codec_check` 337 → **352** (+15: the version matrix moves to v9 — **3/4/5/6/7/8**
  and **10** rejected — a tail-less v8 image is refused, every §4.6 violation rejected, and a valid
  tail with two exports and one aliased import survives the round trip, as does a `<init>` top
  level); new suite **`export`** (`tests/unit/export_test.cpp`, **88** checks). Corpus
  `tests/scripts/*.ql` stdout **byte-identical** to the pre-change build (23/23, exit 0 on both;
  measured against a `HEAD` build of `qlang_run` in a scratch tree), and the emitted image is the
  v8 bytes plus the tail.
- **Docs.** `docs/SERIALIZATION.md` §4 → version 9 (new §4.6 module identity + tables, §4.1
  `main`/`<init>`, §5.1 v1–v8 rejected + the tail invariants, §9 v8/v10 probes + the tail violation
  list, §10); `docs/spec/13-bytecodes.md` (`## keyword export` → Implemented with the Q29/Q30
  reading, the two table sections, `## encoding extended` gains the v9 bullet and "readers accept
  v9"); `examples/qlang_run.cpp` names the entry module after its source file, level 5 prints
  `module:` + the export table (`kind` → function index or register, `readonly` / `shared cell`) and
  renames the old "exports (main)" list to `unexported defs`, help text v9.
- **Tallies re-recorded.** counted unit cases 37 718 → **37 821** (+88 `export`, +15 `codec_check`);
  ctest entries **52**; `encoding_v6`, `flag_flow` and the perf baseline untouched.
- **Found, not fixed (pre-existing).** A top-level `const` interpolated inside an **f-string in a
  function body** evaluates to nil (`const G = "hello"` / `def a(n) { return f"{G}" }` /
  `println(a(1))` → `nil`). Reproduced identically on the pre-change build, so it is not a Phase 3a
  regression; filed in `docs/plans/todo.md`.

## Round 62 (2026-11-09)

**External-modules plan Phase 2 — the constant pool becomes program-level: one
deduplicated table per module, `.qlc` v8, and a load-time pool-index check the
per-function pools never needed.**
Plan `docs/plans/external-modules-plan-v4.md` Phase 2 (steps 1–7); spec
`docs/spec/13-bytecodes.md` `## unified constant table` is now **Implemented**.
**On-disk breaking change — `.qlc` v7 → v8** (single-version reader: v1–v7 rejected
at the version field). Runtime behavior is unchanged; disassembly *text* changes by
design (one flat `constants:` list per program, indices shift).

- **Why it was worth doing.** The spec requires "all the functions within a module
  must share a common const table or at the very least must not duplicate constants"
  — v7's per-function pools duplicated every literal used in two `def`s, and Phase
  3's module merge needs one table per module to merge.
- **What landed.** `ConstantPool` (`include/qlang/QLangBytecode.h` /
  `src/QLangBytecode.cpp`): the pool plus its `valueKey` dedup index behind
  `add()` / `take()`. `QLangCompiler` owns one (`m_pool`), so dedup spans function
  boundaries — a string used in two `def`s folds to one slot — and `compile()`
  moves it into the new `CompiledProgram::constants`. Deleted: `Function::constants`,
  `FnEmitter::consts` / `constKeys` / `addConst()`, and the dead `CompiledProgram::code`
  mirror — `Bytecode` shrinks to the `name()` mnemonic alone. The VM's frame loop
  reads `m_prog.constants`.
- **Format.** v8 moves the pool out of the function record into a module-level
  section after the function records, before the native-slot table
  (`docs/SERIALIZATION.md` new §4.2; NativeSlot/VarEntry/instruction-stream renumber
  to §4.3/§4.4/§4.5). For a single-function file the bytes are identical to v7 —
  the pool sits at the same offset — so the hand-built fixtures needed no byte
  changes. New load-time invariant: every const-pool operand (`mov` kind 2,
  `makefunc`, `makegen`, and `emit`, whose `b` is a pool index though its kind
  stays register) is collected during decode and range-checked once the pool is
  read → `constant pool index out of range`; the closure-`funcIndex` scan walks the
  program pool.
- **Verification.** `cmake -B build && cmake --build build` clean; `ctest
  --test-dir build` **51/51 green**. `codec_check` 330 → **337** (+7: the version
  matrix moves to v8 — v3–v7 and v9 rejected, v8 round-trip with **program-pool**
  equality — plus the pool-index accept/reject pair); `flag_flow` **1324**
  unchanged (the harness now merges per-function pools into the program pool with
  operand renumbering through `decodeInstruction`/`encodeInstruction`);
  `phase0_api` F1 corruption check reads the program pool. Corpus
  `tests/scripts/*.ql` stdout byte-identical to the pre-change build (behavior
  invariant).
- **Docs.** `docs/SERIALIZATION.md` §4 → version 8 (new §4.2 module pool + Q34
  version allocation, §5.1 v1–v7 rejected + pool-index invariant, §9 pool-index
  case + v7/v9 probes, §10); `docs/spec/13-bytecodes.md` (`## unified constant
  table` → Implemented, the `## opcode: MOVI` decompiler sample refreshed to the
  real v8 output — flat pool, `mov e=0x08 … pool[00]`, no `movc` —, the three
  stale operand-map rows `CONST`/`DELETE`/`IN` → `MOV (kind 2)`/`DELETEx`/`INx`,
  `SERIALIZATION.md` §4.4 refs → §4.5); `examples/qlang_run.cpp` + `hello.cpp`
  print the flat program pool, help text v8. **Q34 recorded:** v8 = unified pool
  (this round), v9 = Phase 3 module tables; `offset-table-elimination-plan.md`
  (unsigned) takes the next free number.
- **Tallies re-recorded.** counted unit cases 37 711 → **37 718** (+7, all in
  `codec_check`); ctest entries **51**; `encoding_v6`, `flag_flow` and the perf
  baseline untouched.

## Round 61 (2026-11-09)

**External-modules plan Phase 1 — the builtin-function names move into the library: one
id ↔ name table for the compiler, the disassembler and any future tool.**
Plan `docs/plans/external-modules-plan-v4.md` Phase 1 (steps 1–6); spec
`docs/spec/13-bytecodes.md` `## table for build in function (qlang base)` is now
**Implemented**. No format change, no `.qlc` bump, no behavior change.

- **Why it was worth doing.** The 22 builtin names lived in four places: the `Builtin` enum,
  `QLangCompiler::builtinId()` (a hardcoded `if` chain), the VM's `callBuiltin` switch, and a
  second hardcoded `switch` inside the disassembler. The spec's requirement for this section is
  precisely the last one — "hardcoding function names in the disassembly source (which is highly
  discouraged) will not longer be required".
- **What landed.** `include/qlang/QLangBytecode.h` / `src/QLangBytecode.cpp` gained the table
  `k_builtin_names[]` (22 entries, `Builtin` order) plus four free functions:
  `builtinCount()`, `builtinName(id)` (`nullptr` out of range), `findBuiltin(name)` (**−1** on a
  miss) and `builtinId(name)` (**throws** on a miss). A `static_assert` on the array length vs
  `Builtin::Deepcopy + 1` makes enum/table drift a build error instead of a silent `nullptr`.
  The `str` → `toStr` and `int` → `toInt` aliases moved here from the compiler's chain, so both
  lookups accept them.
- **The two-API split is the design, not decoration.** `findBuiltin` is the **classification**
  answer and `builtinId` the **resolution** answer. All four compiler sites classify
  (`QLangCompiler.cpp` free-variable scan, `deepcopy` lowering guard, ternary-block capture scan,
  and the `CALL` operand-`b` emit path): on the emit path a miss is *normal* — a native C++
  function takes the negative-slot branch and an unknown name raises the compiler's own
  `unknown function '<name>'`. Routing the emit path through the throwing API would have broken
  every module call, so `builtinId` currently has no in-tree caller; it exists for the caller that
  already knows the answer, and the suite pins both behaviours.
- **Consumers rewired.** `QLangCompiler::builtinId()` (the member function) is deleted, together
  with its declaration in `QLangCompiler.h`. `examples/qlang_run.cpp`'s app-local `builtinName()`
  switch (51 lines) is deleted; both the level-4 `# builtin:<name>` note and the level-1..4
  pseudo-mnemonic call `qlang::builtinName`. The VM's `callBuiltin` switch stays as-is — it is
  keyed by the enum and never needs a name.
- **Only observable difference: disassembly text.** The deleted switch spelled ids 0/1 `tostr` /
  `toint`; the table returns the canonical `toStr` / `toInt`, so `--asm` listings now print
  `# builtin:toStr` and the pseudo-mnemonic `toStr(r1)`. That is the *fix* — the old spellings
  were not names the language accepts. No test asserted on them (grep over `tests/` finds none)
  and no program output changed: corpus stdout is byte-identical, 23/23 scripts.
- **Verification.** `cmake -B build && cmake --build build` clean; `ctest --test-dir build`
  **51/51 green** (49 with `-E perf`, `doc_hashes` skipped). New `builtin_table` suite
  (`tests/unit/builtin_table_test.cpp`, **151 checks**): id → name against an independent oracle
  list, name uniqueness, `nullptr` out of range, the round trip through both APIs incl. the two
  aliases, case-sensitivity/no-trim/no-prefix misses, the `-1`-vs-throw split and the throw text,
  the ids the compiler actually emits for `len`/`toStr`/`str`/`int`/`toInt`/`type`/`println`, the
  negative-slot native path (`import math` + `min(3,1,4)` still compiles to `b < 0` and runs),
  `unknown function 'nope'` kept as a *compile* error, builtins inside closures not captured, and
  `deepcopy` lowering + its arity guard.
- **Tallies re-recorded.** counted unit cases 37 560 → **37 711** (+151); ctest entries 50 →
  **51**; unit harnesses 24 → **25**. `codec`, `encoding_v6`, `flag_flow` and the perf baseline
  are untouched — the change emits identical bytecode (it is a name lookup, not a codegen path).
- **Still open (the plan's later phases).** Phase 2 (unified per-module constant pool, `.qlc`
  v8) and Phase 3 (`export`, import/export tables, `GETIMPORT`/`CALLIMPORT`, `.qlc` v9) are
  untouched; Phase 3a–3c stay gated on **Q29–Q35** and, for automatic name-based module
  resolution, on **C8** (`missing-features-plan.md`, **Q23–Q26**). Serializing the table into
  `.qlc` remains out of scope — the compatibility rule (`13-bytecodes.md` `## technical
  specifications` 6) licenses a library-side table.

## Round 60 (2026-11-08)

**Phase 3 opcode retirement — the seven spellings the v6 encoding made redundant are deleted,
their identities compacted, and `.qlc` becomes v7: same byte layout, smaller identity space,
`tst` + `je`/`jne` the only way to branch on truthiness.**
Plan `docs/plans/phase3-opcode-retirement-plan.md`; the code landed in `b86460b` (S1, compiler
lowers bare truthiness to `tst` + `je`/`jne`), `801498c` (S3, enumerators + codec rows +
mnemonics + VM handlers removed, identities compacted) and `a6737b9` (S4, `kQlcVersion` 7,
reader accepts v7 only); **this round is the documentation step (S6) plus the tally re-record
(S5's measurement half)**. Touched: `docs/spec/13-bytecodes.md`, `docs/SERIALIZATION.md`,
`docs/compliance/{status,tests,harness,history,README}.md`,
`docs/plans/{encoding-v6-and-indirect,phase3-opcode-retirement,todo}.md`.

- **Why it was worth doing.** Since Round 56 the modifier byte carries a right-operand **kind**,
  so `movi`/`movc` are `mov`, `addi` is `add`, `subi` is `sub` and `cmpi` is `cmp` — the separate
  identities had no producer left (Phase 2, Round 56) and only cost decode-table entries.
  `jmpfalse`/`jmptrue` duplicated what `tst` + `je`/`jne` express, and keeping both spellings kept
  the spec honest only by saying "deprecated".
- **What was removed.** `MOVC` (0), `JMPIFFALSE` (45), `JMPIFTRUE` (46), `MOVI` (57), `ADDI` (58),
  `SUBI` (59), `CMPI` (61) — enumerator, codec row, mnemonic, VM handler and the condition-register
  jump form in `emit()`. `kOpcodeUsedCount` **79 → 72**.
- **The compaction (D3).** One-time, not incremental: `new = old − |{removed < old}|`, so every
  identity above a removed one shifts down. Spot-checked after the change: `mov` 0, `jmp` 43,
  `continue` 44, `yield` 53, `cmp` 54, `je` 55, `jle` 60, `nop` 61, `emit` 62, `deepcopy` 63,
  `indexget` 64, `indexput` 65, `eqf` 66, `ltf` 67, `gtf` 68, `ltef` 69, `gtef` 70, `tst` **71**
  (last defined, hence the count 72). The plan's "73" assumed **M4** (`DECL`) landed first; it has
  not, so the D2 fallback applies and `DECL` will be identity **72** / count **73** when M4 lands.
- **`.qlc` v7 is a version bump with no layout change.** The escape-byte stream, the 2/2/4
  modifier, the offset table and the loc side table are byte-for-byte the v6 ones; only the
  version field (4) and the set of decodable identities differ. Readers accept **7 only** —
  v1–v6 fail at the header with `qlc: unsupported version (byte offset 8)`, exit 3, never a parse
  attempt — which is what makes the removal safe rather than a compatibility break in disguise.
- **`tst` is now emitted (M3 D4 closed).** 15 `JMPIFFALSE`/`JMPIFTRUE` references converted;
  `emitJumpIfTrue`/`emitJumpIfFalse` emit `tst r` + `je`/`jne` and return the **jump** index, with
  the `tst` at index − 1 (where a loop back-edge must point to re-evaluate the condition), so every
  hardcoded `+2`/`+3` label offset became an explicitly captured index. Cost: one more instruction
  per truthiness test — `hello.ql` 211 → **215** (+4 sites), `perf_stress.ql` 521 → **565** (+44);
  `money.ql` 151 and `cast.ql` 163 unchanged (no truthiness test). Corpus stdout **23/23 scripts
  byte-identical**, all exit 0.
- **Measured outcome (perf).** `perf_bench` **PASS 12/12 stages** within ±20 % / 0.50 ms;
  `perf_stress` execute **10.974 ms** vs the 10.382 ms baseline (**+5.7 %**), consistent with the
  +8.7 % S1 A/B (11.349 ms) recorded in the plan — the inner `for j = 1 to 200` back-edge now
  executes one extra dispatch. **No re-baseline**; the loop-invariant direction re-test that causes
  it is filed as a follow-up (plan §8), not a `tst` cost.
- **Verification.** `cmake -B build && cmake --build build` clean; `ctest --test-dir build`
  **50/50 green** (48 with `-E perf`, `doc_hashes` skipped). Producer gate: zero references to the
  seven opcodes in `src/` + `examples/`, and the `--asm-level 4` scan of all 23 corpus listings has
  **0 deprecated mnemonics**. v6 and v8 images rejected at the version field; a v7 write → read →
  `loadProgram` → `execute` round-trip is stdout byte-identical to `ctx.run(source)`.
- **Tallies re-recorded.** `codec_check` 281 → **330** (v7 version matrix: 3/4/5/6/8 rejected,
  v7 round-trip per script); `encoding_v6` 37 859 → **35 083** (the sweep now walks 72 identities —
  parity sweep 30 392 rejected modifiers / 2 376 accepted / 15 144 truncations, 14 274 rejected
  `(opcode, modifier)` pairs cross-checked); `flag_flow` **1 324** unchanged; counted unit cases
  40 287 → **37 560**.
- **Still open (S5's other half, labels only — no code surface).** `tests/unit/spec_check2.cpp`
  case names still read `movi:` / `addi:` / `subi:`, `tests/unit/value_semantics.cpp` comments
  still say "the MOVC clone", and `include/qlang/QLangVM.h` still documents "CMP/CMPI" and
  "mov/movc". Renaming them is a test-comment change, deliberately not mixed into this round.

## Round 59 (2026-11-07)

**M3 flag flow — milestone 3 of `docs/plans/encoding-v6-and-indirect-plan.md` (plan
`docs/plans/m3-flag-flow-plan.md`): the five flag-setting comparisons `eqf`/`ltf`/`gtf`/`ltef`/`gtef`
and `tst` are appended, executed by the VM and emitted by the compiler — comparison conditions in
branch position now lower to a flag compare + flag jump instead of `cmp` + flag jump — and the spec's
"specified, not yet emitted" markers flip to shipped.**
The code landed across `4ead571` (identities + tables + codec/encoding suites), `1e47009` (VM
`flagCompare` + disassembler), `0086112` (the `flag_flow` flag × jump matrix), `a1984fa` (compiler
lowering + budgets) and `56998cf` (`flags.ql` script regression); **this round is the documentation
round (plan S7) plus the tally drift those commits left behind** — `docs/spec/13-bytecodes.md`,
`docs/SERIALIZATION.md`, `docs/compliance/{status,tests,harness,history,README}.md`,
`docs/plans/{encoding-v6-and-indirect,m3-flag-flow}-plan.md`. **No on-disk format change — `.qlc`
stays v6**: six identities appended last (`EQF` 73, `LTF` 74, `GTF` 75, `LTEF` 76, `GTEF` 77,
`TST` 78), `kOpcodeUsedCount` 73 → **79** of 128; `DECL` is M4's and will make it 80.

- **Why it was worth doing.** The spec (`## opCode scope`, `### extended OpCode eq, lt, gt, lte,
  gte`, finalized 2026-11-06) defines comparisons over **general types** that set the flags
  directly; until now every `if (a < b)` paid for `cmp` — which orders numbers only — and a
  two-instruction sequence, and comparing strings or mixed types in condition position went
  through the generic truthiness path.
- **The family.** Register left operand, right operand of kind immediate/register/const per the
  permitted-kind table (`TST` register-only). `QLangVM::flagCompare` compares through the existing
  `cmpValues`/`isEqual` (numbers, strings, bool, nil, containers by content) and returns **one-hot**
  flags (`CMP_GR` 1, `CMP_EQ` 2, `CMP_LT` 4) or zero, so `flagsSatisfied` and every flag jump are
  unchanged — the spec's `lte`/`gte` rows are capability masks, not multi-bit results. `TST` sets
  `EQ` iff `truth(r[a])`.
- **The compiler lowering and the D1 correction.** `emitCmpJumpFalseExpr` now has two lowerings of
  the same condition (D2): both operands statically `Int` keep `cmp` + flag jump (bytes unchanged
  from pre-M3, and `cmp`'s total order keeps the flags reusable for a second jump), everything else
  — exactly the guards that previously paid for a boolean temp + `jmpfalse` — takes a flag compare +
  flag jump. The negative branch **cannot reuse the same comparator**: a partial comparator (`ltf`,
  `gtf`) leaves no bits set when the relation is false, and no flag-jump predicate tests the
  none-set state — so the false branch uses the comparator of the **dual** relation with the jump
  the condition needs (`==`→`jne`, `!=`→`je`, `<`→`gtef`+`jge`, `>`→`ltef`+`jle`, `<=`→`gtf`+`jg`,
  `>=`→`ltf`+`jl`), implemented as `flagCompareIfFalse`. The plan's original D1 said "same
  comparator, inverted jump" — corrected during implementation; the rule is now normative in the
  spec (`### shared registers`) and recorded in both plans.
- **`tst` is implemented but not emitted (D4).** It decodes, executes and disassembles and is
  covered by the matrix, but bare truthiness still lowers to `jmpfalse`/`jmptrue`; emitting `tst`
  belongs with the **removal** of that deprecated pair (Phase 3, `.qlc` v7). `ja`/`jae`/`jb`/`jbe`
  stay spec/disassembly aliases — the repo has no assembler and they have no opcode identity (D6).
  An immediate right operand on a flag compare is **always escaped** (D5): every flag compare's
  `defaultRightKind` is register, so a base form can only mean register and `eqf r12, 5` is
  `[C9][40][0C][05]` (4 B) — the spec's 3-byte base-form example was wrong and is fixed here.
- **Measured outcome.** A comparison condition in branch position is one byte shorter per site:
  `hello.ql` 215 → **211** instructions, `perf_stress.ql` 522 → **521** (`codegen_size` budgets
  lowered in `a1984fa`; `money`/`cast` unchanged). `perf` gate green with **no re-baseline**
  (re-verified in `56998cf`).
- **Verification.** `cmake -B build && cmake --build build` clean; `ctest --test-dir build`
  **50/50 green** (48 with `-E perf`). New `flag_flow` suite (**1324 checks**, 879 lines): the full
  matrix (6 comparators × 6 flag jumps × 3 relations × 3 right-operand kinds), the "no bit set"
  state spelled out case by case (the D1 rationale), the `tst` truth table, the general value
  domains the Int gate rejects with error-text parity (D3), `__str__` dispatch, `cmp`'s Int-only
  contract (D2), the alias name table (D6) and reserved-slot rejection for the new identities. New
  `tests/scripts/flags.ql` (23rd script regression, 182 lines). Drift corrected: `codec_check`
  276 → **281** (+5 S5 lowering cases landed unrecorded) and the `encoding_v6` §18 parity sweep
  re-tallied for the new identities (30298 rejected / 2470 accepted / 15731 truncations, was
  30517/2251/14401). Round 58 (call args into slots) was recorded in the `status.md` tally but
  never got its own entry here. **50 ctest entries, 50/50 green.**

## Round 57 (2026-11-07)

**v6 decode fast path: the Round 56 performance regression is investigated instead of
re-baselined — `decodeInstruction` becomes a single-pass, table-driven decoder that is
behaviour-identical to the one it replaces, and the equivalence is guarded in-repo.**
**No language, encoding or `.qlc` change** — the byte stream, the opcode table, the
decoded fields and every error message are unchanged; only *how* the bytes are read.
`src/QLangBytecode.cpp`, `tests/unit/encoding_v6_test.cpp`, `tests/perf/baseline.json`,
`docs/compliance/{status,tests,harness,history,README}.md`,
`docs/plans/m2-pipeline-switch-plan.md`.

- **Why it was worth doing.** `decodeInstruction` runs once per VM step, so it *is* the
  hot loop. Measured decode-only over the `perf_stress.ql` instruction stream: v5 (before
  the switch) **6.03 ns**/instruction, the Round 56 v6 decoder **16.4 ns**, i.e. the
  +32.5 % `perf_stress` regression was decode cost, not the extra byte per instruction.
  Two causes: (a) every opcode-class question (`baseOperandCount`, `defaultRightKind`,
  `permittedKinds`, `isJumpOpcode`, `isConditionalJump`, `isAluOrFlagCompare`) was a
  separate `switch` call on the same opcode, and (b) the escaped form called
  `escapedInstructionSize` — which walks the whole modifier table — **and then re-read the
  operands**, so the size computation ran twice per instruction.
- **The fast path.** A cold-built 128-entry `OpMeta` table (base operand count, default
  right kind, permitted-kind mask, jump / conditional-jump / ALU-or-flag-compare / `CALL`
  flags) replaces the six per-opcode predicate calls; `constexpr kWidthBytes[4]` and
  `kImmBytes[16]` replace the width/form tables; the jump class gets its own branch (it is
  the only class whose modifier does not select operand widths); modifier validation is
  inline bit tests evaluated **in the same order, with the same messages**, as
  `escapedInstructionSize`; each operand is read once through `loadLE`. Result
  **6.68 ns**/instruction — back to v5 parity (6.03).
- **Equivalence proven before landing, not assumed.** An exhaustive differential harness
  (a verbatim copy of the shipped decoder as the reference) swept 128 identities ×
  escaped/base × 256 modifiers × 4 operand byte patterns × 2 extra-byte variants × stream
  lengths 0–12 and compared *throw vs value, the exact message text, and all 13 decoded
  fields*: **3 421 184 cases, 3 325 824 throwing, 0 mismatches**. A second harness walked
  the compiled byte stream of every `tests/scripts/*.ql` and `examples/*.ql` **at every
  byte offset**, not just at instruction starts (that is what would desynchronise `pc`):
  **42 448 cases, 0 mismatches**.
- **Permanent in-repo guard (`encoding_v6` §18, +4 checks, 34466 → 34470).** The
  differential harnesses were scratch tools; the repo needs something that fails when the
  decoder and the size table drift. `escapedInstructionSize` is used as the **oracle** over
  all **128** identities (not just the 73 used ones — the decoder must be total) × 256
  modifiers: every modifier the table rejects the decoder must reject with the *same
  message*, every accepted one must decode to exactly the predicted size and kind, and
  every truncation of an accepted instruction must throw a `truncated` error. Base forms
  are checked against `baseInstructionSize` / `defaultRightKind` (`CALL` → "must be
  escaped"). Swept: **30517 rejected modifiers, 2251 accepted, 14401 truncations**. The
  guard was verified to work by perturbing one decoder message — §18 failed with the
  table/decoder pair side by side — then reverted.
- **Measured outcome.** `perf_stress` execute **12.995 → 10.382 ms** (−20.1 %), 77 % of the
  Round 56 regression recovered (min-of-N, interleaved against both builds: v5 `0b4a4fb`
  9.751 ms, Round 56 HEAD 12.852 ms, this change 10.465 ms). `tests/perf/baseline.json`
  re-recorded with `--update-baseline` (deliberate act, own commit); the other eleven
  medians moved by ≤ 6.4 µs and stay under the 0.5 ms absolute floor. `hello`/`money`/`cast`
  execute improved ~8–14 % in the same micro-runs.
- **Residual, deliberately not chased.** +5.8 % against the pre-switch 9.809 ms is the
  wider instruction stream itself (`perf_stress` code 1837 B vs 1714 B in v5), not the
  decoder. Follow-ups recorded in the M2 plan: shrink the 56-byte `DecodedInstruction` and
  avoid the full default-initialisation on every decode.
- **Verification.** `cmake -B build && cmake --build build` clean; `ctest --test-dir build`
  **47/47 green** (`encoding_v6` 34470, `codec_check` 199 unchanged, 22 script regressions);
  the `perf` gate green on three consecutive standalone runs. Docs re-hashed only after the
  green run. **47 ctest entries, 47/47 green.**

## Round 56 (2026-11-07)

**`.qlc` v6 pipeline switch — milestone 2 of `docs/plans/encoding-v6-and-indirect-plan.md`:
the v6 encoding of Round 55 becomes the *only* instruction codec, the v5 one is deleted,
and the compiler emits the unified `mov`/`add`/`sub`/`cmp` with per-literal immediate
selection, so `bool`, `nil` and `float` leave the constant pool.**
**On-disk breaking change — `.qlc` v5 → v6, clean break per the Round 37 precedent**
(single-version reader, no migration shim; no external v5 images exist).
`include/qlang/QLangBytecode.h`, `src/QLangBytecode.cpp`,
`include/qlang/QLangBytecodeIO.h`, `src/QLangBytecodeIO.cpp`, `src/QLangCompiler.cpp`,
`include/qlang/QLangVM.h`, `src/QLangVM.cpp`, `examples/qlang_run.cpp`,
`examples/hello.cpp`, `CMakeLists.txt`, `tests/`, `docs/SERIALIZATION.md`,
`docs/spec/13-bytecodes.md`, `docs/compliance/{status,tests,harness,history,README}.md`,
`docs/plans/{m2-pipeline-switch,encoding-v6-and-indirect}.md`.
Executed as steps S1–S8 of `docs/plans/m2-pipeline-switch-plan.md`; S1–S5 + S6 landed as
separate commits (`3bc55c7`, `35edb7a`, `e0a8323`, `337584c`, `d577612`, `2c1e480`), S7
(tests) and S8 (docs) complete the round in the working tree.

- **Six decisions settled before coding** (recorded in §2 of the M2 plan). **D1** the `V6`
  suffix is dropped — with exactly one codec left, `encodeInstruction` / `decodeInstruction`
  / `instructionSize(op, escaped, e)` / `makeModifier` / `baseModifier` are the plain names
  (zero churn at the 14 external call sites: the v5 signatures were identical). **D2**
  `DecodedInstruction` gains `immForm` — `ImmPayload::Signed` alone cannot distinguish
  `Int` (`i8`/`i16`/`i32`) from `Long` (`i64`). **D3** immediate *emission* is restricted to
  literals that round-trip exactly — `Int` within ±2³¹, `Float`, `Bool`, `nil`;
  `Long`/`Qword`/`Unsigned` stay in the pool (the codec supports `i64`/`u64`, but the value
  type of a 64-bit literal is ambiguous). **D4** `.qlc` v6 ships **without** the declaration
  table — M4 adds it inside v6, Phase 3 bumps v7 (§8 annotated). **D5** the v5 harness
  `tests/unit/encoding_test.cpp` (41 checks) is retired; its two unique cases (jump-width
  tiling, truncated-stream decode error) are ported to `encoding_v6_test.cpp`. **D6** pool
  reads clone containers only where the value is *stored* (`mov`/`movc` →
  `freshContainerValue`); ALU/`cmp` right operands are read-only and never copied.
- **Codec + IO.** The v5 `escapedInstructionSize` / `instructionSize` / `encodeInstruction`
  / `decodeInstruction` are deleted; `kQlcVersion = 6` and `readQlc` accepts **6 only**
  (v1–v5 → "unsupported format version"). `readQlc` now validates every modifier byte at
  **load** through the v6 decoder — reserved right kind `3`, reserved left form `3`,
  reserved immediate forms `12`–`15`, a kind the opcode does not permit, unescaped `CALL` —
  and the `codeSize` guard moved from `kMaxInstructions × 7` to `× 15` (measured widest
  instruction: `CONV` with a dword left + `u64` immediate = 15 B).
- **VM.** A new `QLangVM::operandValue` helper materializes the decoded right operand by
  kind (register / pool / immediate, `ImmPayload` + `immForm` → `QLangValue`); `mov`/`movc`,
  `add`/`sub`/ALU and `cmp` route through it, the ALU family gained the §2.6 **in-place
  arity** branch (`isInPlaceForm`), and the deprecated `movi`/`movc`/`addi`/`subi`/`cmpi`
  spellings keep decoding and executing (§3 coexistence — the *v5 encoding* was removed,
  not the *deprecated opcodes*). Flag model untouched.
- **Compiler.** `RawInstr` carries `kind`/`form`/`payload`; `computeModifier` returns
  `(modifier, escaped)` — under 2/2/4 `e = 0x00` is a legal modifier, so the flag is the
  only base-form signal — and is driven by the opcode's right kind, not by an opcode list;
  `baseOperandCount` became public because `INC`/`RETURN`/`TABCREATE`-class opcodes have no
  right operand to widen. New emission helpers `emitImmI`/`emitImmF`/`emitImmB`/`emitImmNil`
  /`emitPool` behind one `emitLiteral` decision point; `literalImmOf` picks the narrowest
  permitted form per literal (D3); `foldNegLiteral` folds `-1.5` to a single float
  immediate. **Zero** `MOVI`/`MOVC`/`ADDI`/`SUBI`/`CMPI` emissions remain; the jump path is
  byte-for-byte unchanged. Encoding consequence: `defaultRightKind(MOV) == Register`, so a
  *pool* `mov` must announce its kind — 4 B where the old `movc` base form was 3.
- **Disassembler + demos** (same change, AGENTS.md mandate). `operandText()` renders the
  right operand by kind (`imm` signed/unsigned/float/`true`/`false`/`nil`, `pool[NN]`,
  `rN`) with the in-place arity rule so a listing can never read past an instruction;
  `CALL` prints `dest, id, argc`; `pseudoText()` merged the suffixed cases and gained
  `SUBI`/`JNE`/`nop` pseudo rows it never had; `hello.cpp` prints operands by kind too.
- **Tests.** `encoding` ctest target deleted; `encoding_v6` 34249 → **34466** (+ §2.9 golden
  wire bytes, + a stream tiled by every (kind, form) pair walked pc-by-pc asserting
  `instructionSize` == the encoder's own length, §11 item 3); `codec_check` 142 → **199**
  (+20 v6 load-time validation cases, +15 deprecated-spelling cases that write, re-read and
  **execute** a hand-encoded legacy stream, §11 item 6); new `tests/scripts/imm_kinds.ql`
  (22nd script regression) drives every immediate form end to end.
- **Measured effects.** Corpus: all 21 pre-existing `tests/scripts/*.ql` produce
  **byte-identical stdout** and identical exit codes against a `git worktree` of `d577612`
  (§11 item 1). Pool: **515 → 477** entries — `bool` 5→0, `nil` 27→0, `float` 6→0, strings
  and containers unchanged (§11 item 10, exactly the D3 set); code bytes **+1.9 %** (the
  11-byte float immediate and the pool-`mov` modifier byte cost more than the pool slot
  they free). `.qlc` write → load → run round-trips for all 22 scripts. `perf_stress`
  execute 9.809 → 12.995 ms (+32.5 %, one extra byte per escaped instruction in the hot
  loop) → `tests/perf/baseline.json` re-recorded deliberately (own concern in the commit).
- **Docs.** `docs/SERIALIZATION.md` §4/§4.4/§5.1/§9/§10 and `docs/spec/13-bytecodes.md`
  (`## encoding extended`, `### bytecode encoding`, `## constant pool`, `## future plans`)
  moved from "v6 specified" to **v6 shipped**; opcode identity bound corrected to
  `kOpcodeUsedCount` **73**; the parent plan's §6.1 M2 row flipped to DONE with D1–D6
  recorded and §8 annotated per D4.
- **Verification.** `cmake -B build && cmake --build build` clean; `ctest --test-dir build`
  **47/47** green (46 + `doc_hashes`; `perf` runs and passes against the re-recorded
  baseline). `./update-doc-hashes.sh` re-recorded the drifted rows after the green run
  (AGENTS.md: testing first, hash second).
- **Not in this round (milestones 3–4):** `eqf`/`ltf`/`gtf`/`ltef`/`gtef`/`tst` and the
  `ja`/`jae`/`jb`/`jbe` aliases (M3); `DECL`, the declaration table, frame name map +
  bitset, two-operand `INDGET`/`INDPUT` (M4); deleting the deprecated identities from the
  enum (Phase 3, `.qlc` v7).

## Round 55 (2026-11-06)

**v6 encoding layer — milestone 1 of `docs/plans/encoding-v6-and-indirect-plan.md`
(A5): the 2/2/4 modifier byte, the immediate set and a `*V6` codec land
*additively*, beside the shipped v5 ones. Nothing emits or decodes v6 yet.**
`include/qlang/QLangBytecode.h`, `src/QLangBytecode.cpp`, new
`tests/unit/encoding_v6_test.cpp`, `CMakeLists.txt`,
`docs/plans/{encoding-v6-and-indirect,missing-features}.md`,
`docs/compliance/{status,tests,harness,history,README}.md`.
**No language change; `.qlc` stays v5.**

- **Scope.** Round 54 finalized the spec and wrote the plan; this round implements
  the plan's first milestone — the **encoding layer only**: the vocabulary
  (`RightKind`, `OperandWidth`, `ImmForm`, `ImmPayload`), the modifier-byte
  accessors (`makeModifierV6`, `modifierLeftForm` / `modifierRightKind` /
  `modifierRightForm`), the width and form tables (`operandWidthBytes`,
  `immediateFormBytes`, `immediateFormType`), the per-opcode **permitted-kind**
  table (`permittedKinds`, `kindPermitted`), the **in-place arity** predicate
  (`isInPlaceForm`), the base-form equivalences (`baseModifierV6`: `0x04` /
  `0x40` / `0x08`, `CALL` → `0x50`, the jump class and `nop` → `kNoBaseModifier`)
  and `encodeInstructionV6` / `decodeInstructionV6` / `instructionSizeV6`. The v5
  codec, `kQlcVersion = 5` and every call site are **untouched** — the diff in
  `QLangBytecode.cpp` is two pure insertions.
- **Two deviations from the plan text, both forced by the encoding itself.**
  (1) `instructionSizeV6(op, **escaped**, e)` takes an explicit escape flag: under
  2/2/4 **`e = 0x00` is a legal modifier** (kind immediate, form `u8` — the only
  way to fit `mov r3, 200` in one operand byte), so v5's
  "`modifier == 0` ⇒ base form" convention cannot be reused; the caller knows
  whether the opcode byte had bit 7 set and must say so. (2) An **unescaped
  `CALL`** is rejected in both codec directions — `CALL` is always escaped with
  `e = 0x50` (2-byte signed function id + 1-byte argument count), so accepting the
  bare identity would silently decode garbage. Both are recorded in §6.1 of the
  plan and in the `status.md` open item.
- **Sizes verified** (op + `e` + `a` + operand bytes): zero-byte immediate **3 B**,
  `i8` 4 B, `i16` 5 B, `i32` 7 B, `i64` and `float` **11 B**, `CALL` 6 B (7 with a
  word-width `a`); escaped `mov r3, r7` 4 B vs 3 B in the base form. The plan's
  §2.9 example table was wrong in seven rows (it still carried v5 nibble
  modifier bytes) and is corrected, with `mov r3, 200` (`e = 0x00`) and
  `mov r3, 4000000000` (`e = 0x20`) added.
- **New `encoding_v6` suite — 34249 checks** (`encoding_v6` ctest entry): typed
  round-trips for every immediate form, size/encode/decode agreement, reserved
  kind `3` and forms `12`–`15` rejected, permitted-kind enforcement per opcode
  class, base forms **byte-identical to v5**, truncated-stream and
  out-of-range-function-id rejection, and an **exhaustive sweep** of all 73
  opcodes × 256 modifier bytes — **1756 accepted / 14138 rejected**, every
  accepted pair cross-checked between `instructionSizeV6`, the encoder and the
  decoder.
- **Pre-existing defect fixed:** four `Bytecode::name(...) == "literal"`
  **pointer** comparisons in `tests/unit/codec_check.cpp` (comparing a
  `const char*` against a string literal — never true). Reproduced failing on a
  clean tree (`git stash -u`) before any of this round's changes, so it is not a
  regression introduced here; now `std::strcmp`. The 142-check tally is unchanged.
- **Verification.** `cmake -B build && cmake --build build` clean;
  `ctest --test-dir build` **47/47** green (`encoding_v6` added; the v5 pipeline
  is untouched, so every pre-existing suite had to stay green by construction).
  Formatting checked per-file with `clang-format --lines=…` on the diff only — a
  blanket `clang-format -i` on the new test file was tried and reverted (it
  explodes the braced-init tables and the repo has hundreds of pre-existing
  violations). Tally **1002 → 35251** counted unit cases; `tests.md` 22 → 23
  harnesses, 46 → 47 entries; `docs/compliance/README.md` "last audit" → Round 55.
- **Not in this round (milestones 2–4):** the compiler's fusion sites, the VM
  dispatch and the `qlang_run` disassembler still emit/decode v5; `.qlc` is still
  v5; `eqf`/`ltf`/`gtf`/`ltef`/`gtef`/`tst`/`decl` are not appended; no behavior
  harness cases.

## Round 54 (2026-11-06)

**A5 extended-encoding spec finalized: the modifier byte settled as 2/2/4, the
flag model corrected to one-hot, `$$` (Q6) closed without encoding space, and a
detailed v6 plan written.**
`docs/spec/13-bytecodes.md`, new `docs/plans/encoding-v6-and-indirect-plan.md`,
`docs/plans/instruction-format-escape-byte-plan.md`,
`docs/plans/missing-features-plan.md`, `docs/compliance/{status,history,README}.md`.
**Docs only — no source, test-source, or `.qlc` change; the engine still ships
v5.**

- **Trigger.** The AGENTS.md session-start reload, then a request to *explain*
  the encoding rules behind inventory item A5. Explaining them required reading
  the implementation, and the two disagreed in places the spec could not settle:
  `## encoding extended` was an **empty heading**, `## opCode scope` was a
  malformed table, "shared registers" was referenced but never defined, and the
  immediate/flag tables contradicted each other and the code. Round 53 had
  tracked A5 as *draft, finalize before planning*; this round does that
  finalization and writes the plan.
- **Modifier byte settled (2/2/4).** `bit 0-1` left operand form (always a
  register: 1/2 B, 4 B reserved for a future register file, `3` reserved),
  `bit 2-3` right-operand **kind** (`0` immediate, `1` register, `2` const pool,
  `3` **reserved**), `bit 4-7` right-operand **form**. Three rules the draft left
  implicit are now normative: the **base form** of each class is pinned
  (`e = 0x04` register-right, `0x40` immediate-right, `0x08` const-right, so
  `mov r3, r5` stays 2 bytes); a per-opcode **permitted-kind** table makes any
  other combination a **decode error** (copy/ALU/flag-compare accept kinds 0/1/2,
  `makefunc`/`makegen` kind 2 only, `call` kind 0 with the form fixed to 5, the
  rest kind 1 only); and the **arity rule** for 3-operand ops — kind 1 is
  `r[a] = r[b] op r[c]`, kinds 0/2 are in-place `r[a] = r[a] op right`, which is
  exactly why `add a, imm` *is* `addi`. The jump class keeps the v5 width
  selector (`jmp8`/`jmp16`/`jmp32`) rather than the 2/2/4 split.
- **The flag model was wrong, and the correction is the substantive one.** The
  draft's extended-comparison table listed `lte` → `eq|1|1|0` and `gte` →
  `eq|1|0|1`, i.e. **two bits set at once**. That is incoherent with the
  implemented predicates (`QLangVM::flagsSatisfied`, `src/QLangVM.cpp:1063-1074`):
  `jle` is `(f & (LT|EQ)) != 0`, so after an `lte` that set `eq|lt` for `a > b`
  it would jump, and `je` (`f == EQ`) would fire after `lte` when `a < b`.
  Adopting the two-bit states would also make `cmp` + `jle` stop firing on
  equality. **Resolved:** `CMP_GR`/`CMP_EQ`/`CMP_LT` stay independent relation
  predicates and a comparator sets **exactly the relations that hold** — at most
  one bit, and *none* is legal — so the `lte`/`gte` rows are documented as
  **capability masks** (which relations that comparator can distinguish), not
  encodable states. `QLangVM::flagsSatisfied` therefore needs **no change**, and
  the spec's jump table now carries the implemented predicates verbatim.
- **Opcode reconciliation.** `movi`/`movc` → `mov` with the kind field;
  `addi`/`subi`/`cmpi` → the in-place kind-0 form of `add`/`sub`/`cmp`;
  `addc`/`subc` **rejected** (`mov` kind 1 already covers them). The suffix
  identities stay **decodable** in v6 (coexistence) and are retired by a later
  bump. `ja`/`jae`/`jb`/`jbe` are **assembler aliases** of
  `jg`/`jge`/`jl`/`jle` — the flags carry no signedness, so distinct identities
  would be dead weight (a genuinely unsigned compare stays deferred). The
  flag-setting comparisons ship as `eqf`/`ltf`/`gtf`/`ltef`/`gtef` (general
  types; the value-producing `EQ`/`NE`/`LT`/`GT`/`LE`/`GE` are untouched), plus
  `TST` (`flags = truth(r[a]) ? eq : 0`), which retires `jmpfalse`/`jmptrue`,
  plus `DECL` for `$$`: **7 opcodes appended last, 73 → 80 of 128**.
- **Immediates.** Signed and unsigned 1/2/4/8 B, `float` (8 B), `bool` (forms
  9/10, zero operand bytes) and `nil` (form 11) — the constant pool keeps only
  what cannot be an immediate (strings, blobs, closure/function constants).
  `12`–`15` and kind `3` stay **reserved**.
- **Q6 closed — `$$` needs no encoding space.** The declaration bit is
  per-*event* runtime state: one register can carry several declaration sites and
  the same store opcode executes on both the declaring and the re-assigning path,
  so no instruction bit can express it (this was true of v5's nibbles and stays
  true of v6's fields). Adopted: compile-time declaration-site ids + a
  per-function **declaration table** (`site → {reg, name, kind}`) in the `.qlc`
  v6 function record + a per-frame **bitset** cleared at frame creation, set by
  the `DECL siteId` marker; `INDGET`/`INDPUT` are ordinary register-class ops;
  the `$$"literal"` fast path ships only under same-block dominance. Consequence
  for the roadmap: the Round-53 dependency edge `A5 layout → A3 Q6` is
  **removed** — A3 and A5 are independent and share only the `.qlc` v6 bump.
- **Stale documentation corrected** (all verified against the tree first):
  the "Instruction operand map (8-byte layout)" header is marked **historical** —
  the struct and `usesI32()` were deleted in Round 37, only the *operand roles*
  survive; the operand table gained the missing `NOP` (68) and `EMIT` (69) rows;
  `MOVI`/`ADDI`/`SUBI` gained deprecation notes; the `shared registers` block (a
  mangled table) became a real subsection; the jump examples were rewritten from
  pre-v5 **absolute** `i32` targets to the shipped **relative** offsets with byte
  sizes; the example `eq e=0x00 0c, 5` contradicted its own form table (`0x00` =
  *unsigned*) and is now `eqf … e=0x40` (signed 1 B). In
  `instruction-format-escape-byte-plan.md` §4/§5.3/§13.4 are marked superseded
  and §14's "readers v1..v5" is corrected: the reader accepts **v5 only**
  (`version != kQlcVersion` rejected, `src/QLangBytecodeIO.cpp:469`) — v1–v4 do
  not load, and v6 follows the same clean-break precedent.
- **Verification.** `cmake -B build && cmake --build build` clean; `ctest`
  **46/46** green — expected, nothing in `src/` or `tests/` changed.
  `./update-doc-hashes.sh --check` first reported the two drifted rows
  (`13-bytecodes.md`, `status.md`); with the suites green the rows were
  re-recorded (3: those two plus `history.md`) and `ctest` re-run — **46/46**
  with `doc_hashes` now **Passed** rather than Skipped, and `--check` reporting
  **24 modules verified across 2 indexes**, no drift. `docs/compliance/README.md`
  "last audit" pointer → Round 54; the `docs/spec/README.md` row description for
  `13-bytecodes.md` now names the extended-encoding sections. Tally unchanged
  (1002 counted unit cases + 21 script regressions + the perf gate).

## Round 53 (2026-11-06)

**`missing-features-plan.md` re-audit: C3 landed without a round, an untracked
extended-encoding spec becomes inventory item A5, four stale spec hashes.**
`docs/plans/missing-features-plan.md`, `docs/compliance/{status,history,README}.md`,
`docs/EXTERNAL_TYPES.md`, `docs/EVENT_HANDLERS.md`, `docs/spec/README.md` +
`docs/compliance/README.md` (hash rows only).
**Docs only — no source, test-source, or spec-module content change.**

- **Trigger.** The AGENTS.md session-start reload, then a line-by-line audit of
  `docs/plans/missing-features-plan.md` (last audited 2026-11-05, Round 52)
  against the working tree and `git log`. Plans are **not** hash-tracked, so
  nothing in the tooling detects their drift — they have to be re-audited by
  hand against `history.md` (newest-first) and the `status.md` Open Items.
- **C3 `hasMethod`/`callMethod` is implemented but untracked.** The pair
  (`include/qlang/QLangContext.h`, `src/QLangContext.cpp`,
  `tests/unit/test_external.cpp` §19–§20, 49 → 66 checks, `./build/test_external`
  green) landed as **`c4ee131`** during this audit — a code commit with no round,
  no Open-Item flip and no plan update (it also touched
  `docs/spec/13-bytecodes.md`, so its hash row had to be re-recorded). Moved in
  the plan from §C (open) to §D (implemented) and dropped from Phase 3 (items
  renumbered). **Deliberately not marked verified:** `hasMethod` accepts a table
  field holding a `BoundMethod`, while `table_ops::callMethod` requires
  `Type::Function` and throws `member 'x' is not a function` — the guard does not
  protect the call it guards, breaking the `docs/EVENT_HANDLERS.md` §6
  no-op-on-missing-handler contract. Fix (per-kind callable test: table/object →
  `Function` only, external → `findMember` `kind == 2`) is its own code commit
  and its own round; `status.md` records C3 as *implemented, pending the fix*.
  Plan Q19/Q20 adopted as defaulted.
- **`1db387d` added an unimplemented encoding spec nobody was tracking.**
  `docs/spec/13-bytecodes.md` gained `## encoding extended`, `## opCode scope`,
  `### bytecode encoding` and `### extended OpCode eq, lt, gt, lte, gte`: a
  modifier byte (left register 2 bits, right-operand kind 2 bits with value 3
  reserved, right form 4 bits), `addc`/`subc`, the `ja/jb/jae/jbe` family and
  extended `eq/lt/gt/lte/gte`. None of it is implemented and it is **breaking**
  (`.qlc` v5 → v6). Added as inventory item **A5** + Phase 2 item 4, with
  dependency edges `spec finalization → A5` and `A5 layout → A3 Q6` — the plan
  previously told the next session to settle Q6 (where the `$$` declaration bit
  lives) *before* touching encoding, which is now backwards, so **Q6 was
  re-opened** (default unchanged: a VM-side per-frame side table, because the
  declaration bit is per-event state, not durable).
- **Anchor drift.** The plan cited `## redeclared variable within a scope`
  (singular) five times; the spec heading is
  `## redeclared variables within a scope` (plural, `docs/spec/16-strict_typing.md`).
  Corrected everywhere.
- **A deferral reason that no longer exists.** The duplicate-declaration Open
  Item deferred the diagnostic because the `# warning` section requires warnings
  to be toggleable — that section was deleted in the `docs/spec/`
  modularization (`grep` over `docs/spec/` finds no `# warning` section). The
  live requirement is `16-strict_typing.md` `## redeclared variables within a
  scope`, which makes the diagnostic **level-dependent** (0 silent / 1 warning /
  2 error) and therefore dependent on the unimplemented strict-typing level.
  Reason restated; the work stays deferred, folded into the plan as B1/B2.
- **`docs/EVENT_HANDLERS.md` documented a method that does not exist.** The
  host-side call surface used `ctx.getVar(...)`; `QLangContext` exposes
  `get(name)` (`include/qlang/QLangContext.h`) — corrected, plus a caveat that
  the documented `hasMethod` guard is currently unreliable for table fields
  holding a `BoundMethod`. `docs/EXTERNAL_TYPES.md` §9/§10.1 updated to match
  (both design docs are outside the hash gate).
- **`status.md` "SPEC.md Changes Since Last Audit" had no 2026-11-06 entry.**
  The section still described the Round-40 edits as uncommitted and listed
  nothing after them — the reason `1db387d`'s spec edits were invisible to the
  next session. A paragraph now records the committed `1db387d` / `c4ee131`
  spec edits (extended encoding → A5, the `# warning` deletion, the strict-typing
  dup-decl section, the `03-declarations.md` wording drift) and states that the
  older "uncommitted" notes describe the state at their own round.
- **Four stale spec hashes.** `./update-doc-hashes.sh --check` reported
  `03-declarations.md`, `11-external-types.md`, `13-bytecodes.md` and
  `16-strict_typing.md` drifted — `1db387d` edited spec content without
  re-recording (the gate is warn-only since Round 49, so CI stayed green).
  Suites re-run first, then the rows re-recorded (AGENTS.md: testing proves
  correctness, the hash merely detects change).
- **Verification.** `cmake -B build && cmake --build build` clean; `ctest`
  **46/46** green (`doc_hashes` Skipped by design); `./build/test_external` all
  checks passed; `./update-doc-hashes.sh` re-recorded **6 rows** (the four spec
  files plus `status.md` and `history.md`) and `--check` then reported **24
  modules verified across 2 indexes**, no drift. `docs/compliance/README.md`
  "last audit" pointer → Round 53. Tally unchanged (the 66-check `test_external`
  is working-tree only).

## Round 52 (2026-11-05)

**Spec↔compliance reload audit: an untracked `.gitattributes`, tallies left one
round behind, pre-migration `examples/` paths, mangled markdown.**
`.gitattributes`, `docs/compliance/{status,tests,harness,classes,history,README}.md`.
**Docs + git hygiene only — no source, test-source, or spec-module change.**

- **Trigger.** The AGENTS.md session-start reload: `docs/spec/` re-read against
  `docs/compliance/`. `./update-doc-hashes.sh --check` reported **24 modules
  verified** (no hash drift) and the root stubs were correct (`SPEC.md` 17
  modules, `SPEC_COMPLIANCE.md` all 7 compliance modules), so this round is
  about the facts *inside* the modules rather than the rows tracking them.
- **`.gitattributes` was never committed.** Round 48's CRLF fix — the reason the
  MSVC job stopped reporting 24 phantom hash drifts — existed only on disk
  (`git ls-files` → not in HEAD), so a fresh clone still checked out CRLF on
  `windows-latest`. Now staged with `git add`.
- **`harness.md` tally lagged a round.** "Round 50 … 966 counted unit cases + 20
  script regressions … 45 ctest entries" → re-verified as **1002 + 21 + 1 perf =
  46 entries** (the numbers themselves were already correct in `tests.md` /
  `status.md`; only this copy had not been re-pointed after Round 51).
- **`tests.md` counts and layout.** Requirement table said 21 C++ harnesses
  (22), 20 `.ql` scripts (21), "20 script regressions" (21) and "all 21 unit
  harnesses" (22); the layout tree omitted `table_subscript.ql`.
- **`status.md` contradicted itself on `step`.** The `## Working` range bullet
  still said the `step` variants are "**NOT implemented** (parser parses no
  `step`)" while the resolved Open Item in the same file records `e955a8d`.
  Re-verified empirically — `for i in range(0,10) step 2` → `30` — and the
  bullet corrected. Two `examples/range_check.cpp` references (and one
  `examples/class_static_test.cpp` in `classes.md`) predate the Round 32 move to
  `tests/unit/` → corrected.
- **Markdown mangled by an old table→list conversion.** Six list items carried
  stray leading pipes (`|-`, `||-`, `|||-`, `||||-`) and five table rows a
  doubled leading pipe (an empty first column); two of the list items also ended
  in a trailing `|`. All normalized — no wording changed.
- **`update-doc-hashes.sh` mode.** Recorded as `100755` in the index
  (`git update-index --chmod=+x`) so the documented `./update-doc-hashes.sh`
  invocation works on a fresh clone; the working-tree bit still needs a local
  `chmod +x` (the sandbox denies `chmod`), which shows as a mode-only diff until
  then.
- **Still pending (sandbox denies file deletion).** `docs/plans/missing` — the
  scratch note Round 46 recorded as superseded by `missing-features-plan.md` and
  "pending removal" — plus root-level scratch `debug_{nested,s1,s1b,s2,s2b,s3}.cpp`,
  `repro_forin.cpp` and `perf_result.json` (AGENTS.md: generated files must not
  touch the source root). All untracked; `rm` them by hand.
- **Verification.** `cmake -B build && cmake --build build` clean; `ctest`
  **46/46** green (`doc_hashes` Skipped by design, Round 49); hashes
  re-recorded afterwards for the five edited compliance modules.

## Round 51 (2026-11-05)

**Table subscript `t[key]`: new opcodes `INDEXGET` / `INDEXPUT`.**
`docs/spec/13-bytecodes.md`, `include/qlang/QLangBytecode.h`,
`src/QLangBytecode.cpp`, `src/QLangVM.cpp`, `src/QLangCompiler.cpp`,
`examples/qlang_run.cpp`, `tests/unit/spec_check2.cpp`,
`tests/unit/codec_check.cpp`, new `tests/scripts/table_subscript.ql`,
`CMakeLists.txt`. Closes the gap found in Round 50.

- **Gap.** `docs/spec/03-declarations.md` states `t["age"] = 33  # valid;
  equivalent to t.age` and `docs/spec/05-operators.md` refers to "ordinary
  dynamic table access (`t[key]`)", but every postfix `[...]` lowered to
  `ARRGET`/`ARRPUT`, which accept `Type::Array` only. `TABGET`/`TABPUT` — which
  already resolve keys through `ObjectOps::getField`/`setField` — were reachable
  only from the `.` member path.
- **Design choice.** Two new opcodes rather than making `arrget`/`arrput`
  polymorphic: the existing names describe an array, and keeping them intact
  leaves the single-kind forms available (and `.qlc` opcode numbers unchanged).
  Appended **last** — identities 71 and 72, `kOpcodeUsedCount` 71 → 73; `.qlc`
  stays **v5** (no encoding change: three register operands, no `i32` slot, the
  `baseOperandCount()` default). Distinct from the reserved `INDGET`/`INDPUT`
  (`$$` indirect access) placeholders, which remain unimplemented.
- **VM.** `runFrames()` dispatches on the runtime base type: `Type::Array`
  reproduces the `ARRGET`/`ARRPUT` semantics exactly (same out-of-range error);
  anything with `ObjectOps` stringifies the key (`valueToString`) and calls
  `getField`/`setField`; anything else throws `cannot subscript <type> value` /
  `cannot assign into <type> value` (the old messages named only "non-array",
  which is no longer true). No test asserted the old wording.
- **Why `ObjectOps` and not a direct map lookup.** It makes the subscript form
  inherit the member rules instead of re-implementing them: on a class instance
  `a["zz"] = 1` → `D has no field 'zz'` and a `const` field → `cannot assign to
  const field 'x'` — byte for byte what `a.zz` reports.
- **Compiler.** `ExprKind::Index` emits `INDEXGET`; `compileAssignTarget` emits
  `INDEXPUT` for both the plain and the compound (`t["n"] += 5`) paths.
  `ARRGET`/`ARRPUT` are now emitted nowhere (verified by a codec check asserting
  zero emissions) but stay valid opcodes. `TABGET`/`TABPUT` remain in heavy use
  (member targets, class/statics machinery). The empty subscript `ab[] = v`
  still lowers to `ARRAPP`, so `t[] = 44` still errors — with the pre-existing
  `cannot append to non-array value`, not a new message.
- **Disassembler.** `examples/qlang_run.cpp` folds both into the existing
  subscript mnemonics (`r5 = r3[r4]`, `r3[r4] = r5`); the register-use switch
  already defaults to three operands.
- **Tests.** `spec_check2` 512 → **529** (+17: table read/write/add-field/
  variable key/compound, array read/write/compound/append unchanged, class
  instance read/write, closed-field-set, const guard, scalar read/write,
  `t[] = 44`, out-of-range). `codec_check` 123 → **142** (+19: identities,
  names, validity, 4-byte base size, encode/decode round-trips, `DEEPCOPY`
  identity unchanged, `ARRGET`/`ARRPUT` still valid, compiler lowering, empty
  subscript → `ARRAPP`). One pre-existing malformed-input case had used opcode
  byte 71 as "undefined" — moved to 73. New `tests/scripts/table_subscript.ql`
  added to the explicit `QL_SCRIPTS` list (21st script regression).
- **Verification.** `cmake -B build && cmake --build build` clean; `ctest`
  **46/46** green (1002 counted unit cases + 21 script regressions + the perf
  gate), `doc_hashes` re-recorded afterwards for `docs/spec/13-bytecodes.md`,
  `docs/spec/README.md` (untouched), `docs/compliance/{status,tests,history,
  harness,README}.md` as edited.
- **Note for future rounds.** `.ql` sources use `#` comments — `//` is not a
  comment token and the lexer reports `unexpected character` on it (and on
  non-ASCII such as `§` inside what looks like a comment).

## Round 50 (2026-11-05)

**Spec↔compliance reload audit: untracked spec text, drifted harness tallies,
stale root stubs.** `docs/compliance/status.md`, `docs/compliance/harness.md`,
`docs/spec/README.md`, root `SPEC.md` + `SPEC_COMPLIANCE.md`. **Docs only — no
source, test-source, or spec-module change.**

- **Trigger.** A full reload of `docs/spec/` against `docs/compliance/` with the
  `doc_hashes` gate plus an empirical probe of every "Working" bullet that could
  be checked from `qlang_run`.
- **Untracked spec text (now an Open Item).** `docs/spec/07-api.md` carries two
  uncommitted sections — `## module search paths` (set search paths; a POC module
  is one file, so resolution is `[modulename].ql` per directory) and
  `## external search callback` (`string searchCallback(string modulename)` for
  non-standard filesystems such as Qt `:/...`). Neither had compliance tracking and
  neither is implemented (`searchPath`/`searchCallback` appear nowhere in
  `include/` or `src/`); `status.md` gained the Open Item, `docs/spec/README.md`
  the two extra domain terms in the 07-api row.
- **`harness.md` had drifted three times.** It still described the sources under
  `examples/` (they moved to `tests/unit/`), `spec_check2` at 421 cases (512 since
  Round 44), and `codec_check` as "format version 3, readers accept v1..v3" (v5
  escape-byte stream, v5-only reader since Round 37). Its `## Current Round
  Summary` still carried the Round 8 snapshot ("295 passed … 412 total"). Paths and
  counts corrected; the summary now states the current tally once and defers to
  `tests.md` / `status.md` instead of keeping a third copy of the numbers.
- **Root stubs.** `SPEC.md` said "16 domain modules" (17 exist); `SPEC_COMPLIANCE.md`
  omitted `migration-value-semantics.md` (7 modules exist, 6 listed).
- **Empirical finding (NOT fixed here — Round 51).** `docs/spec/03-declarations.md`
  states `t["age"] = 33  # valid; equivalent to t.age`, but subscript on a table
  throws: read → `type error: cannot subscript non-array value`, write → `cannot
  assign into non-array value`. `QLangCompiler` emits `ARRGET`/`ARRPUT`
  unconditionally for a subscript (`src/QLangCompiler.cpp:3543-3548`,
  `3976-3995`) and the VM handlers accept `Type::Array` only
  (`src/QLangVM.cpp:2018-2039`). No test anywhere exercises table subscript —
  `spec_check2.cpp` and `tests/scripts/*.ql` contain no `t["…"]` — so
  `status.md`'s `[x] table member read/write t.name, t["age"]` bullet was
  unverified. `t.name` and the spec's `t[] = 44 → error` both behave correctly.
  Tracked as an Open Item and implemented in Round 51.
- **Verification.** `cmake --build build` clean; `ctest --test-dir build` **44
  passed + `doc_hashes` Skipped** before the re-record, **45/45** after
  (`07-api.md`, `status.md`, `harness.md` rows re-hashed once green, per AGENTS.md
  order). `spec_check2` 512/0 and `signal_test` 18/0 re-run directly.
- **Deferred (sandbox denies `chmod`/`rm`).** `update-doc-hashes.sh` is committed
  `100644`, so `./update-doc-hashes.sh` needs `git update-index --chmod=+x
  update-doc-hashes.sh`; the stale scratch note `docs/plans/missing` (Round 46
  deleted it; it reappeared untracked with pre-Round-39 claims) needs `rm`. Both
  left for the user to run.

## Round 49 (2026-11-05)

**`doc_hashes` made optional (drift reported, not failed) + manual re-record
script.** `cmake/check_doc_hashes.cmake`, `CMakeLists.txt`,
`update-doc-hashes.sh` (new). **Tooling only — no source, test-source, or
spec-module change.**

- **Why.** Round 48 removed the *false* failure, but the gate still stopped a CI
  job for what is a documentation-hygiene defect: a stale SHA256 row says
  nothing about the build. The check keeps running everywhere and keeps printing
  the full report; it just no longer decides the job.
- **Warn-only by default.** The entry now runs the gate with
  `-DDOCS_HASH_WARN_ONLY=ON` and declares
  `set_tests_properties(doc_hashes PROPERTIES SKIP_REGULAR_EXPRESSION
  "QLANG_DOC_HASH_DRIFT")`, so ctest prints **Skipped** (and the summary counts
  it as skipped) instead of Failed. `-DQLANG_DOC_HASHES_STRICT=ON` — the
  repository's first `option()` — registers the entry without the flag,
  restoring the hard failure.
- **Why a marker instead of an exit code.** CMake script mode cannot exit with a
  chosen status: `message(FATAL_ERROR)` always exits 1 and there is no `quit()`
  command (verified with CMake 4.4). The report therefore carries the literal
  `QLANG_DOC_HASH_DRIFT` token and ctest maps *that* to Skipped — so a genuine
  gate error (missing `docs/`, bad `QLANG_SOURCE_DIR`) keeps failing, unlike
  `SKIP_RETURN_CODE 1`, which would have swallowed it. Precedent for skipping
  rather than failing: `perf` and its `SKIP_RETURN_CODE 77`.
- **`update-doc-hashes.sh` (repository root).** The manual step, so the re-record
  is one command instead of a remembered `cmake -D… -P …` invocation: no argument
  re-records only the stale rows and prints the resulting README diff; `--check`
  verifies and exits 1 on drift without writing. Style follows `build.sh`
  (`set -euo pipefail`, `cd "$(dirname "$0")"`, `case` dispatch, exit 2 on bad
  usage). It needs the executable bit (`chmod +x update-doc-hashes.sh`, or run
  it as `bash update-doc-hashes.sh`) — the sandbox denies `chmod`.
- **Verification.** Drift probe (append a line to `docs/compliance/coverage.md`):
  default `ctest -R doc_hashes` → *"did not run: 45 - doc_hashes (Skipped)"*,
  job green; `-DQLANG_DOC_HASHES_STRICT=ON` in a separate build dir → **Failed**;
  `./update-doc-hashes.sh` → exactly one row rewritten; `--check` → exit 1 with
  the report, exit 0 once current. Clean tree: `cmake --build build` clean and
  `ctest --test-dir build` **45/45** (the entry *passes* when the rows are
  current — Skipped only appears with drift). Probe files restored, hashes
  re-recorded after the suites were green.

## Round 48 (2026-11-05)

**`doc_hashes` failed only on the MSVC job: a CRLF checkout invalidates every
recorded hash.** `.gitattributes` (new) + `cmake/check_doc_hashes.cmake`.
**Tooling only — no source, test-source, or spec-module change.**

- **Symptom (second MSVC CI run).** Build clean, `ctest -E perf` → **42/43
  passed**, the single failure `doc_hashes`; the same commit passes on Linux
  (`git worktree add … HEAD` → `24 modules verified`). The 43 vs 45 entry count
  is not a missing Windows test: `-E perf` is a *regex*, so it drops `perf` and
  `script_perf_stress` (45 − 2 = 43, reproduced locally).
- **Cause.** The gate hashed the bytes on disk (`file(SHA256)`). GitHub's
  `windows-latest` image sets `core.autocrlf=true` in git's system config, so
  `actions/checkout@v4` materialises every `.md` with CRLF while the committed
  blobs are LF — all 24 recorded SHA256 drift at once, and no other test reads
  document bytes, which is why it was the only failure. Reproduced without
  Windows: `git -c core.autocrlf=true clone` of this repo → `file
  docs/spec/01-types.md` says "with CRLF line terminators" and the gate reports
  `24 stale SHA256`.
- **Fix 1 — `.gitattributes`.** `* text=auto eol=lf`, plus explicit `binary`
  entries for images and compiled artifacts (`*.png`, `*.qlc`, `*.qla`,
  object/library/executable types) because `eol=` implies "text" and would
  corrupt binaries on commit. Checkouts are byte-identical on every platform, so
  a recorded hash still equals `sha256sum` of the file on disk. No tracked file
  contains CR, so no renormalization commit is required. Verified: cloning the
  pinned tree with `core.autocrlf=true` yields LF `.md` files and a
  byte-identical `docs/images/*.png`.
- **Fix 2 — hash the committed content, not the working tree.** The gate now
  reads the module, `string(REPLACE "\r\n" "\n")`, and hashes with
  `string(SHA256)`, so a clone that predates `.gitattributes` (or overrides it
  locally) still gets a true answer instead of a false drift report. Recorded
  hashes are unchanged — they were always LF hashes.
- **Docs drift found while verifying.** `docs/compliance/tests.md` claimed "All
  workflows trigger on push to `main` only", but `.github/workflows/ci-msvc.yml`
  triggers on `master` (the branch rename is still pending, per the workflow
  comments). The CI matrix row now records the difference; the workflow files
  are unchanged.
- **Verification.** `cmake -B build && cmake --build build` clean; `ctest
  --test-dir build` **45/45** green (tally unchanged: 966 counted unit cases +
  20 script regressions + `perf` + `doc_hashes`). The gate was exercised three
  ways: LF tree → 24 verified; CRLF copy of `docs/` → 24 verified (the
  normalisation); CRLF copy with the normalisation removed → 24 stale, i.e. the
  original CI failure. Hashes re-recorded only after the suites were green.

## Round 47 (2026-11-05)

**`QL_SIGNAL` obtains its signature without odr-using the signal function.**
MSVC CI link failure (`signal_test`: `LNK2019`). **Header-only
(`include/qlang/QLangMeta.h`) + docs — no language, opcode, `.qlc`, or test
change.**

- **Symptom (first MSVC CI run).** Every target **compiled**; only
  `signal_test` failed to *link*: `unresolved external symbol
  "public: void __cdecl Sensor::levelChanged(int)"` (and the same for `DupSig`),
  each "referenced in function … `QLangContext::import<Sensor>(void)`".
- **Cause.** `QL_SIGNAL(name)` expanded to
  `meta_detail::signal(#name, &SelfType::name)`. Forming the member pointer
  **odr-uses** the function, and a signal's member function is by design
  *declared, never defined* (`docs/spec/17-signals.md`) — only its signature is
  used, to type-check `emit` arguments. GCC and Clang discard the unused address
  before the link step, so the missing definition is invisible on Linux; MSVC
  keeps the reference. `Clash::level` (declared *and* defined, for the
  property/method-clash test) linked, which is why exactly two symbols were
  reported.
- **Fix.** Name the **type**, not the value:
  `QL_SIGNAL(name)` → `meta_detail::signal<decltype(&SelfType::name)>(#name)`.
  `decltype`'s operand is unevaluated, so there is no odr-use.
  `meta_detail::SignalArgs<Sig>` strips the member-pointer type back to its
  parameter pack (non-const + `const` specializations; the primary template
  carries a `static_assert`, so a data member or a non-member name now fails
  with "QL_SIGNAL requires the name of a member function" instead of a template
  wall); `signal_args` builds the identical one-`arg_check<A>`-per-parameter
  list through `index_sequence` / `tuple_element_t`. The two
  `signal(const char*, R (T::*)(A...))` overloads are gone, so the hazardous
  form cannot be reached again. `<tuple>` added to the includes.
- **Reproduced without Windows.** The reference survives in the object file when
  the optimiser does not discard it: `g++ -std=c++20 -Iinclude -O0 -c
  tests/unit/signal_test.cpp` then `nm -C … | grep levelChanged` → `U
  Sensor::levelChanged(int)` + `U DupSig::levelChanged(int)` with the old header,
  **no output** with the new one (same check with `clang++` and
  `x86_64-w64-mingw32-g++`). A compile-only smoke test cannot see this class of
  defect — the MSVC job *links*, which is what caught it.
- **Docs.** `docs/spec/17-signals.md`: the "`QL_SIGNAL` takes the member function
  pointer `&SelfType::levelChanged`" bullet corrected to the type-only form (the
  spec described the mechanism, not just the behaviour).
  `docs/COMPILER_PORTABILITY.md`: construct **3** added (odr-use → `LNK2019`),
  matrix row + Open item updated to "compiled clean, link blocked, fixed", plus a
  note that `QL_SIGNAL` is covered by `signal_test`, not the macro smoke test.
- **Verification.** `cmake --build build` clean; `ctest` **45/45** green
  (`signal` 18 checks; tally unchanged at 966 counted unit cases). Hashes
  re-recorded after the suites were green.

## Round 46 (2026-11-05)

**Documentation hash gate; tracking hygiene.**
New `cmake/check_doc_hashes.cmake` + the `doc_hashes` ctest entry.
**Tooling + docs only — no source, test-source, or spec-module change.**

- **Why.** AGENTS.md requires a SHA256 per spec/compliance module, re-recorded
  after every verified change, and "never bump a hash before testing". Nothing
  checked the rule mechanically, and drift had already been found twice (Round
  45 corrected a stale case count and a stale "last audit" row).
- **The gate.** `cmake -DQLANG_SOURCE_DIR=<src> -P cmake/check_doc_hashes.cmake`
  verifies (1) every `docs/spec/*.md` and `docs/compliance/*.md` module has a
  row in its README index, (2) the recorded SHA256 still matches the file, (3)
  no row names a deleted file, and reports malformed (non-64-hex) or duplicated
  rows. `-DDOCS_HASH_UPDATE=ON` rewrites only the affected rows in place. CMake
  script mode — no binary, no shell — so it runs on every CI job including
  MSVC. Registered as the `doc_hashes` ctest entry (44 → 45 entries).
  `file(GLOB)` is deliberate here: an untracked new module must fail the gate,
  which is the opposite of the explicit-list rule used for test sources.
- **Implementation notes.** CMake regex gotchas: `.` matches a newline and
  `{n}` repetition is unreliable, so rows are matched one line at a time
  (`file(STRINGS … ENCODING UTF-8)` — the default ASCII reading truncates the
  rows containing en dashes/arrows) with `[0-9a-f]+` plus an explicit
  64-character length check.
- **Verification (before touching any hash, per AGENTS.md).** `cmake -B build &&
  cmake --build build` clean; `ctest --test-dir build` **45/45 green** — the
  966 counted unit cases unchanged + 20 script regressions + the `perf` gate +
  the new `doc_hashes` (24 modules: 17 spec + 7 compliance). The gate was
  exercised on a scratch copy of `docs/`: appended text → stale-hash failure;
  `-DDOCS_HASH_UPDATE=ON` → exactly one row rewritten; an untracked module, a
  ghost row, a 40-character hash and a duplicated row each fail with their own
  message.
- **Hygiene.** The `status.md` Open Item "Reserved word `set`" still said **NOT
  implemented** although Round 26 landed it — re-verified (`local set = 1` →
  `expected variable name but found 'set'`; `set(1,5,7)` → `set(1, 5, 7)`) and
  flipped. The untracked scratch note `docs/plans/missing` is superseded by
  `docs/plans/missing-features-plan.md` and pending removal (the sandbox blocks
  file deletion).
- **Not done (needs explicit approval).** `AGENTS.md` is untouched — its
  "Build & test" section should mention `doc_hashes`, but editing that file
  requires user permission. Untracked root artifacts (`debug_*.cpp`,
  `repro_forin.cpp`, `perf_result.json`, `hello_*.qlc`, `test-hello-*.qla`) are
  left in place.

## Round 45 (2026-11-05)

**deepcopy Phase 4 — closure audit; plan complete.**
Plan `docs/plans/deepcopy-plan.md` Phase 4 (migration note + tracking).
**Documentation only — no source, test, or spec-module change.**

- **What was left.** Phase 4 items 1–3 were audited against the tree rather
  than assumed: the migration note (`docs/compliance/migration-value-semantics.md`)
  had already landed with Phase 2 (Round 43), and every spec marker the plan
  asked for was already flipped — `01-types.md` `## value semantics` carries the
  "fully implemented (Phase 1 + Phase 2)" bullet, `13-bytecodes.md` `## OpCode
  deepcopy` opens with "Implemented 2026-11-05", `10-classes.md` `## deepcopy`
  and `06-functions.md` `### deepcopy` state the behavior with no "planned"
  framing. Remaining work was therefore the Open Item flip, this audit round,
  and the hash re-verification.
- **Verification (before touching any hash, per AGENTS.md).** `cmake -B build &&
  cmake --build build` clean; `ctest --test-dir build` **44/44 green** — 966
  counted unit cases (512 `spec_check2` incl. the Round 42 deepcopy + Round 44
  hook cases, 123 `codec_check`, 40 `value_semantics`, 18 `signal`, the phase*/
  scope/api suites) + 20 script regressions + the `perf_bench` baseline gate.
- **Hashes.** All 17 `docs/spec/*.md` hashes and all compliance self-hashes
  re-computed with `sha256sum` and compared to the index tables: **every row
  already matched** (no spec module was edited this round, so
  `docs/spec/README.md` is untouched). Only `status.md` and `history.md` — the
  files this round edited — were re-hashed in `docs/compliance/README.md`.
- **Drift corrected.** `status.md` intro still described the harness as
  `spec_check2` "(499 cases)" — it has been 512 since Round 44.
  `docs/compliance/README.md` still said "last audit Round 43" while
  `history.md` carried Round 44; the `history.md` index row said "rounds 7–44".
- **Open decisions — disposition.** #1 arity: **compile-time** (the compiler
  rejects `deepcopy()` / `deepcopy(a, b)` with `deepcopy expects exactly one
  argument, got N`; the spec's wording stands, no runtime `need()` fallback).
  #2 `Type::Object`: **share the handle** (no language construct builds an
  `ObjectDescriptor` today, so there is no user-visible behavior to define).
  #3 hook mechanism: **QLang `__deepcopy__` resolved through the instance
  table** (no `ObjectOps` entry added). #4 index-assignment defect: **fixed**
  in Round 42 (`compileAssignTarget` branches on `isCompound`). #7 rollout:
  **clean break** (resolved in Round 43). #5 `shallowcopy` and #6 the `copy`
  param specifier stay **deferred** — explicitly out of scope for this plan.
- **Deliberately not touched.** The MOVI/ADDI sections of
  `docs/spec/13-bytecodes.md` and the Round 30/31 rows of `status.md` still
  say `usesI32()` / "all four exec loops" (stale since the escape-byte format
  migration and the single-`runFrames()` executor). They are historical
  records; correcting them means re-hashing a spec module, which the plan
  assigns to its own docs round.

## Round 44 (2026-11-05)

**deepcopy Phase 3 — `__deepcopy__` override.**
Plan `docs/plans/deepcopy-plan.md` Phase 3; spec `10-classes.md` `## deepcopy`
("a class may override the default by defining `__deepcopy__()`").

- **Hook dispatch.** `deepCopyValue` moved from the anonymous namespace to
  `QLangVM::deepCopyValue` — the override is a QLang method, so dispatching it
  needs the VM call path (open decision #3: instance-table resolution, not an
  `ObjectOps` entry). In the `Table` case, **after** the cycle check: resolve
  `__deepcopy__`, require `Type::Function`, invoke it on the **original**
  (`callClosure(hook, {v})` — the same self-as-sole-arg shape as `__str__` in
  `displayString` and `__destroy__` in `runFinalizer`); its return value IS
  the copy; a throwing hook propagates (refuse the copy). The result is
  registered in `seen`, so two references to one hooked original share ONE
  copy and the hook runs once. Nested instances inside a default copy
  dispatch their own hooks.
- **Guards.** A hook must return a distinct instance or throw: returning nil
  or a non-table is a runtime error (`__deepcopy__ must return an instance,
  got T`); returning the original handle is a runtime error (`__deepcopy__
  must return a new instance, not self`).
- **Re-entrancy.** `m_deepCopyHooked` (per-payload set, RAII-cleared on a
  throwing hook): a hook that deep-copies `self` re-enters on the same
  payload, the nested call skips the hook and takes the default field copy —
  `def __deepcopy__() { return deepcopy(self) }` terminates. Per-payload (not
  a bool like `displayString`), so other objects copied inside a hook still
  dispatch their own hooks.
- **`attachFinalizer` idempotent.** The wrapping deleter became the named
  `FinalizerDeleter` struct; a table already carrying it is left alone
  (`std::get_deleter`). Without the guard, a hook returning an instance the
  constructor already registered would be wrapped twice — two control blocks,
  `__destroy__` enqueued twice. The copy of a plain `__destroy__` instance
  still gets its own registration (DEEPCOPY handler unchanged).
- **Known limitation (pre-existing, out of scope).** Class names do not
  resolve inside method bodies (`unknown function 'Point'`), so a hook cannot
  construct a fresh instance of its own class; it returns a hand-built table
  or a pre-built instance (a global capture — captures alias the global slot,
  so no closure→instance cycle forms).
- **Tests.** `spec_check2` 499 → 512 (+13: hook return used, original
  untouched, returns self / nil error, throws to refuse, `deepcopy(self)`
  terminates, nested hook, hook once per shared original + result shared,
  copy finalizes independently, hook result finalized exactly once). Full
  `ctest` 44/44 green.

## Round 43 (2026-11-05)

**deepcopy Phase 2 — `array` / `set` / `blob` share by default** (**breaking**).
Plan `docs/plans/deepcopy-plan.md` Phase 2; spec `01-types.md`
`## value semantics` (Phase 1 landed the same date, recorded in `status.md` as
Round 42).

- **Storage.** `QLangVariant.h`: `ArrayValue` is now
  `std::shared_ptr<std::vector<QLangValue>>` (`set` reuses the alternative) and
  the blob alternative became `BlobValue = std::shared_ptr<std::vector<uint8_t>>`.
  Copying a `QLangValue` copies the handle, so assignment and call arguments
  **share** — the spec's rule for every non-primitive. `string` deliberately
  stays `std::string` (immutable exception, design decision #3).
- **API.** `QLangValue::asArray()/asSet()/asBlob()` (const + mutable) return the
  payload handle; call sites deref (`v.asArray()->size()`, `(*v.asArray())[i]`).
  Each container factory gained three forms: `Array(shared_ptr)` **adopts**
  (shares) an existing payload, `Array(std::vector<…>)` builds a fresh one,
  `Array()` / `Blob()` / `Set()` build a fresh empty one (the zero-arg form also
  resolves the old `Blob({})` ambiguity). Updated: `QLangVM.cpp` (every
  array/set/blob site, `cmp`, `isEqual`, `lenOf`, `seqGet`, `isTrue`,
  `displayString`, `convert`/`strict_convert`, `doBinary`, `callBuiltin`, the
  `runFrames` `ARRGET/ARRSET/ARRAPP/ARRAPPV` handlers), `QLangCompiler.cpp`,
  `QLangBytecode.cpp` (`valueKey`), `QLangBytecodeIO.cpp`, `QLangModules.cpp`,
  `QValue.h`, `examples/qlang_run.cpp`.
- **Literal freshness (`MOVC`).** A container constant lives in the pool for the
  whole run, so handing its payload to a register would alias the literal. `MOVC`
  now runs containers through `freshContainerValue` (recursive: nested arrays,
  sets, blobs and tables inside a literal are fresh too). This also fixes the
  interaction with constant-pool dedup — two identical literals folded to one
  entry used to hand out one shared table, which is why `perf_stress`'s
  `{n=0, v=0}` loop and the `L3` check matter. Scalars, the immutable string,
  closures, generators and external views are copied as-is (sharing those
  handles is the semantics).
- **Array literals with non-constant elements** (`local outer = [inner]`) are now
  built at runtime: `MOVC []` into the destination + `ARRAPP` per element
  (precedent: the runtime-built table literal). Previously every element went
  through `evalConstExpr`, so a variable element silently became a compile error.
- **`arr += <array-typed expr>`** now emits `ARRAPPV` (extend) instead of
  `ARRAPP` (append one element), decided by `inferType()` of the RHS — the old
  test was `ExprKind::Array` only, so `x += x` appended the array as a single
  element, contradicting `01-types.md` / the `+=` spec.
- **`deepCopyValue`.** Array/set/blob payloads have a stable address now, so all
  three join the cycle identity map (registered **before** recursing); a blob
  copy is a new buffer. `a[] = a` is constructible and round-trips as a cycle
  instead of recursing forever — this removes the Phase 1 "cycles are only
  observable through tables" limitation.
- **Self-aliasing guards.** `ARRAPPV` snapshots the source when the operand
  shares the destination's payload (`a += a`), which previously invalidated the
  iterators mid-`insert`. `a + b`, `slice`, `range`, `cast<array>(set)` and
  `cast<set>(array)` build **fresh** payloads; `isEqual`/`cmp` compare containers
  by content, never by handle.
- **`.qlc` unchanged (v5).** The durable form stores payloads, the pointer is an
  in-memory detail — `SERIALIZATION.md` gained rule **R5** saying so; the codec
  only derefs around the existing encode/decode. **Open decision #7 resolved:
  clean break** — no version bump, no compat flag, no copy-semantics shim for old
  images (the spec's rule is "the exact same compiler version"). Migration note:
  `docs/compliance/migration-value-semantics.md`.
- **Tests.** New `value_semantics` suite (`tests/unit/value_semantics.cpp`,
  40 checks: S1–S4 share on assign/call/nested alias, L1–L5 literal freshness
  incl. deduped twins, nested literals, the table-literal loop and enum tables
  after the `MOVC` clone, D1–D3 `deepcopy` independence + array cycle + blob,
  X1–X4 `+`/`cast` freshness, `a += a`, `string` still a value type) and
  `tests/scripts/value_share.ql` (`script_value_share`). `codec_check` (123) and
  `spec_check2` (499) unchanged apart from the `Blob()`/`asBlob()` call sites.
- **Perf.** `perf_stress` execute 13.780 ms → 9.809 ms (−29 %): the array churn no
  longer deep-copies on every move. The ±20 % gate flags deviations in both
  directions, so `tests/perf/baseline.json` was re-recorded with
  `--update-baseline` (deliberate act, own commit). It is the only stage above
  the 0.5 ms absolute floor; every other stage moved by ≤ 74 µs and is host
  noise (see `tests.md`).
- **Suite:** 44/44 ctest entries green (was 42; +`value_semantics`,
  +`script_value_share`); 953 counted unit cases.

## Round 42 (2026-11-05)

**`deepcopy` specified** (docs only — no code change). The feature was adopted
in `docs/plans/deepcopy-plan.md` (2026-11-01) but the spec only carried the two
encoding-guideline bullets (`13-bytecodes.md`); it now defines the language
feature.
- `01-types.md`: new `## value semantics` — primitives and `string` are copied;
  `array`/`set`/`table`/`blob`/class instances are **shared** on assignment and
  on call; a deep copy is always explicit.
- `06-functions.md`: `deepcopy(var)` row in the built-in table + `### deepcopy`
  — per-type result table (containers recursed; closures, generators, external
  objects, bound methods and type refs return the same handle), recursion,
  cycle preservation, never mutates the argument, fixed arity, examples.
- `10-classes.md`: new `## deepcopy` — class/method table shared, data fields
  recursed, function/closure/generator/external fields shared, const rules and
  finalization unchanged, `__deepcopy__()` override.
- `13-bytecodes.md`: `DEEPCOPY` operand-map row + `## OpCode deepcopy`
  (`r[a] = deepcopy(r[b])`, two register operands, appended last, lowered
  directly — no `call`, no `r[a+1..a+c]` staging), marked **Proposed — not yet
  implemented**.
- **New gap** tracked in `status.md` Open Items. The spec also turns current
  behavior into a documented deviation: `array`/`set`/`blob` are value-backed
  (`QLangVariant`), so they deep-copy on every move/call instead of sharing.
  `table`/object/closure/external (pointer-backed) and `string` (copied) already
  conform.
- **Examples validated against the engine.** The first draft used syntax the
  engine rejects (`a[1][0] = 99`, `v.tags[] = "b"`, `println("{s[0]}")`); the
  block now uses only forms `qlang_run` accepts (`a[] = v` append, `t.field = v`,
  `str(...)`), and was executed to confirm it parses.
- **Plan doc** `missing-features-plan.md`: inventory row **A4**, new
  `### Phase 5 — deepcopy + value semantics` (structural phase renumbered to
  Phase 6), dependency edges (Phase 1 additive → Phase 2 breaking → Phase 3
  hook). `deepcopy-plan.md` Phase 1 corrected to current facts: single executor
  (`runFrames`, one switch — the "four dispatch loops" note is stale),
  `kOpcodeUsedCount` 70 → 71, `Builtin` enum is `ToStr=0…Release=20` so
  `Deepcopy` = 21, plus the `baseOperandCount()` two-operand registration.
- **Found while validating (not fixed here):** array **index assignment**
  `ab[0] = 55` — spec'd in `03-declarations.md`, marked implemented in
  `status.md` — is a compile error (`unsupported operator ''`).
  `QLangCompiler::compileAssignTarget()` does an unconditional
  read-modify-write for a non-empty subscript, so `op.text == "="` reaches
  `binOp("")`. `ab[0] += 5` (compound) works. `status.md` line 107 is wrong.
- **Plan re-audited and rewritten** (`deepcopy-plan.md`, docs only — no code).
  Five blockers found against the tree and corrected: (1) class instances are
  **tables** carrying `__class__`/`__fields__`/`__const_fields__`/
  `__field_types__`/`__statics__` — **not** `ObjectDescriptor` (`Type::Object`
  is the raw/external ops substrate, created only by embedders) — so the
  instance copy rules and the `__deepcopy__` override are now specified against
  the table model (meta keys shared, hook resolved as a QLang method like
  `__str__`, `__destroy__` re-registered through `attachFinalizer`); (2) the
  cycle identity map only keys `shared_ptr` payloads, so array/set cycles are
  unobservable until Phase 2; (3) builtin arity is a runtime `need()` concern —
  the spec's "compile error" has no mechanism, recorded as an open decision;
  (4) `tests/unit/codec_check.cpp:835–841` uses opcode byte **70** as the first
  undefined identity (comment still says `kOpcodeUsedCount == 69`) — it must
  move to 71 when `DEEPCOPY = 70` lands, or the negative test silently stops
  rejecting; (5) `deepCopyValue` must cover `Eof`/`Function`/`BoundMethod`/
  `TypeRef`. Stale references removed (`usesI32()`, "all four dispatch loops",
  `callBuiltin :385`, the `13-bytecodes.md` "lines 428–434" anchor); Phase 4
  items 1–3 dropped as already landed this date. The remaining `usesI32()` /
  four-loops wording in `13-bytecodes.md:104–109,158–166` and in historical
  `status.md` rows is left as-is (accurate for rounds 30/31; fixing it means
  re-hashing a spec module).
- No implementation, no opcode, no test changes. Suite re-run unchanged
  (`ctest`: 42/42); spec hashes re-verified for `01-types`, `06-functions`,
  `10-classes`, `13-bytecodes`.

## Round 41 (2026-11-05)

**Enum C++ injection API implemented** (`missing-features-plan.md` C2; spec
`docs/spec/02-lexical.md` `## enum` — "C++ api needed to inject enums to the
global context" resolved). Host code can now seed a global enum without
QLang source.
- **API:** `QLangContext::registerEnum(name, members, baseType = Int)` —
  `members` is a declaration-order `vector<pair<string, int64_t>>`; values
  are converted to `baseType` exactly as `compileEnum` types a declared
  enum's (the draft plan's "base type is metadata only" was wrong —
  `m_enums` stores typed `QLangValue`s and member folding emits them).
  Validation → `QLangError`: name/member not an identifier lexeme or a
  reserved word (`keywordType`), empty members, duplicate member,
  non-numeric base type, name already in use (live global, previous
  injection, or hidden-var clash).
- **Mechanics:** the members are seeded as the hidden const table
  `__enum_<Name>` (`set(..., isConst)`; the hidden name is also registered
  in `m_injectedConsts` so the compiler loads it on the injected-const
  path); `run()` passes the member values to the new
  `QLangCompiler::addExternalEnums`, which populates `m_enums` +
  `m_globalEnums` — so `Name::M`/`Name.M` fold, `Name` resolves to the
  table, and a script `enum Name` colliding with an injected one hits the
  existing "cannot redeclare enum" check. `compile()` pre-binds each hidden
  var to its global slot so bare-enum reads and function captures resolve;
  member folding inside a function body accepts injected enums via
  `m_injectedEnumNames` (block-scoped `local enum` invisibility unchanged).
  `remove(name)` drops the enum + hidden var; `clear()` drops all
  injections.
- **Tests:** `test_external` 23 → 49 checks (injection both notations,
  `len`, function-body visibility, hidden table const, byte base type,
  redeclaration/unknown-member rejection, all validation paths,
  remove/clear lifecycle). Full suite: **42/42 ctest**, **878 counted unit
  cases**.
- **Docs:** `02-lexical.md` records the API (hash re-verified);
  `status.md` enum Open Item flipped to implemented (C2); `tests.md`
  tally; `missing-features-plan.md` C2 marked implemented.

## Round 40 (2026-11-05)

**Signals implemented** (spec module `docs/spec/17-signals.md`, finalized
this round; plan `docs/plans/signals-implementation-plan.md`; supersedes
`missing-features-plan.md` C6). External (C++) types can now notify script
code of state changes. v1 scope: **external types only** — no signal
declaration on QLang classes.
- **Declaration:** `QL_SIGNAL(name)` in the `QL_OBJECT` meta; the argument
  signature is deduced from a **declared, unimplemented** member function
  (Qt-style; no `Q_SIGNALS` marker — every `QL_` line is self-describing).
  `SignalMeta{name, per-arg convertibility checkers}` via
  `meta_detail::signal()`; `TypeMeta`/`TypeDescriptor` gain
  `std::vector<SignalMeta> signals`; `registerType` copies + validates
  (property/method name clash, duplicate signal → `QLangError` at import).
  A signal is **not a method** — `obj.sig(1)` is a type error.
- **Emit:** `emit` is a reserved word (`02-lexical.md`; `TokenType::Emit` in
  `keywordType()`), parsed as a statement (`StmtKind::Emit`, like `assert`);
  the target is a postfix chain and the **last** dot separates target from
  signal name (`emit a.b.c(1)` emits `c` on `a.b`). New `EMIT` opcode
  appended **last** (`kOpcodeUsedCount` 69 → 70; `.qlc` stays v5):
  `EMIT a,b,c` = sender reg `a`, signal-name const `b`, arg count `c`, args
  in `r[a+1..a+c]` (the `CALL` convention). `compileEmit()` lowers the
  statement.
- **Delivery:** `QLangVM::deliverSignal()` — resolve the signal against the
  object's type descriptor (unknown name → `type error: unknown signal 'x'
  on type 'T'`), check arity/types (same conversion rules as method calls),
  iterate connections, `callHandler` for `Function` (callClosure) +
  `BoundMethod` (inline, like `CALLCLOS`). Synchronous, connection order,
  zero connections = silent no-op; re-entrant emits recurse, contained by
  the step budget (no re-entrancy guard). C++ `ctx.emitSignal()` creates a
  VM and delegates to the same method; `ctx.hasSignal()` mirrors
  `hasMethod`.
- **Connect / disconnect:** arity-dispatched native builtins.
  `connect(sender, "signal", handler)` (function/`BoundMethod`) and
  `connect(sender, "signal", target, "method")` (string-slot; method
  resolved at emit time via `ObjectOps::callMethod`, missing method = silent
  no-op per the event-handler miss rule) return an int handle;
  `disconnect(handle)` / `disconnect(sender, "signal")` (stale handle =
  silent no-op). The context-level connection registry
  (`unordered_map<int,SignalConnection>`) holds a **weak** sender
  (`weak_ptr<void>`, does not keep alive) + **strong** handler (the
  closure's refcount is raised while connected);
  `dropExpiredConnections()` (called from `processFinalizers()`)
  auto-disconnects destroyed senders.
- **Tally:** `signal_test` 18 (new suite, precedent `class_static_test`);
  **852 counted unit cases**, 42/42 ctest green.
- **Docs:** `17-signals.md` finalized (draft → final; `Q_SIGNALS`
  section-marker wording dropped); `02-lexical.md` reserved-word list gains
  `emit`; `spec/README.md` hash row updated; `missing-features-plan.md` C6
  → implemented (§D + Phase 4 item 10 + C6 section); `status.md` — signals
  Open Item added, external-types "Still open" list updated, Working bullet,
  Current Status tally + Round 40 note.

## Round 39 (2026-11-04)

**Phase 1 of `docs/plans/missing-features-plan.md` — A1 `nop` opcode + A2 `?`
ternary operator.** No spec changes; no `.qlc` format change (v5). One new
opcode (`nop`, appended last); the ternary reuses existing branch opcodes.
- **A1 `nop`:** `Opcode::Nop` appended **last** in the enum (identity 68) so
  existing `.qlc` opcode numbers stay valid; `Bytecode::name` → `"nop"`;
  zero-operand (base size 1 byte; the escaped form is rejected — a 0-operand
  opcode has nothing to widen); no-op in the VM dispatch; the compiler never
  emits it (spec: "this OpCode takes no action"). 12 new `codec_check` cases
  (identity, name, validity, size, 1-byte encode/decode round-trip,
  escaped-form rejection, `.qlc` program round-trip).
- **A2 `?` ternary:** `TokenType::Question` (`?`) in the lexer. Parser
  `parseTernary` sits **below every binary operator** (lowest precedence) and
  is right-associative (`a ? b : c ? d : e` → `a ? b : (c ? d : e)`); each
  branch is an expression or a brace block — a `{` in branch position is
  always a block (a table-literal branch must be parenthesized). Codegen:
  condition to a temp, `JMPIFFALSE` to the false side, `JMP` past it; block
  branches lower to a zero-arg closure call (`compileTernaryBlock`) whose
  `return` yields the ternary's value. **Capture fix:** F3 removed implicit
  capture of enclosing locals, so the block's free variables are computed
  (`collectStmtIds` − `collectLocalVars`, minus names `buildFunction`
  auto-resolves) and passed as an **explicit capture list** (the
  arrow-closure pattern); at script level top-level locals have live
  registers and are captured like locals, inside a function scope
  unimported globals stay invisible (clean `use of undeclared variable`
  error). `collectIds` (free-var detection) gained a `Ternary` arm so a
  ternary inside a function body captures its sub-expression free vars.
  16 new `spec_check2` cases (both branches, nesting, block form, mixed
  branches, free-var capture, precedence vs `&&`/`||`/arithmetic, closure +
  script-level scope).
- **Tally:** `spec_check2` 463 → 479, `codec_check` 96 → 108; **834 counted
  unit cases**, 41/41 ctest green.
- **Docs:** `status.md` — `?` gap row → implemented, `nop` Open Item →
  implemented, Current Status tally + Round 39 note, `?` added to the
  Working operators list.

## Round 38 (2026-11-04)

**Constant-pool dedup restored** (SPEC §bytecodes `## constant pool`: "constant
in the pool must have unique values. duplicated entries should be folded into a
single one."). The pre-Phase-4 dedup was dropped during the per-function
`FnEmitter` refactor; this round recovers it from git history and adapts it to
the current architecture. No language changes, no new opcodes, no `.qlc`
format change (v5).
- **`valueKey(QLangValue)`** (free function, `QLangBytecode.cpp`): canonical
  byte-string key — a type byte + payload. Value types key structurally
  (recursively: scalars by bits, String/Blob by len+bytes, Array/Set by
  len+subkeys, Table by size+key-sorted entries+subkeys); `Function` keys on
  its compile-time spec (funcIndex + capture regs — cells bind at MAKEFUNC
  time); the remaining reference types (Generator, Object, External,
  BoundMethod, TypeRef) key by **identity** (pointer) so distinct objects never
  collide. (The old version remapped those to Nil — a latent collision — fixed
  here.)
- **`Bytecode::addConstant`/`setConstants`** (`QLangBytecode.cpp`): keep a
  `std::unordered_map<std::string,int> m_constKeys` (key → pool index) in sync
  with `m_consts`; `addConstant` returns the existing index on a key hit,
  `setConstants` rebuilds the map.
- **`FnEmitter::addConst`** (`QLangCompiler.h`): the per-function emitter dedups
  the same way via its own `constKeys` map before `finish()` moves `consts` →
  `fn.constants`, so each function's pool holds unique values.
- **Tests:** 11 new `codec_check` pool-dedup cases — repeated strings/nil/
  floats/bools/arrays fold to the right unique count, no-duplicate scans over
  the pools, folded scalars + arrays load correctly at runtime, and a real
  program (def + string concat) is duplicate-free across all function pools.
  Full suite green (41/41 ctest; 806 counted unit cases).
- **Docs:** `status.md` §bytecodes gains the constant-pool row; the
  "Current Status" tally is corrected to include `encoding_test` (41) and the
  19 script regressions (was 754/16 — both under-counted vs `tests.md`).

## Round 37 (2026-11-04)

**Instruction format — escape-byte encoding (flat byte stream)** (plan:
`docs/plans/instruction-format-escape-byte-implementation-plan.md`; design:
`docs/plans/instruction-format-escape-byte-plan.md`). No language changes, no
new opcodes (68 of 128 identities used). The `.qlc` format bumps 4 → 5 and
**drops support for v1–v4** (v5-only reader, per user direction).
- **Flat byte stream:** the fixed 8-byte `Instruction` struct is deleted.
  `Function` now stores `std::vector<uint8_t> code` + `std::vector<size_t>
  instrOffsets` (byte offset of each instruction); `loc` stays parallel to
  `instrOffsets`. `Bytecode` wraps the byte buffer. `usesI32()` removed.
- **Escape-byte opcode:** opcode byte bit7 = escape flag, bits0–6 = identity
  (128 base ops). The base (no-escape) form is uniformly 1-byte operands:
  register/pool index 1 byte, immediate 1-byte signed Int (−128..127), jump
  `jmp8` 1-byte relative. The escaped form adds a modifier byte `e` that
  widens operands: 2-byte reg/pool, 2/4-byte signed immediate, `jmp16`
  (e=0) / `jmp32` (e=1). Jumps are three-tier relative (`jmp8`/`jmp16`/
  `jmp32`); the compiler picks the narrowest that fits via iterative
  backpatch. Single register pool + single constant pool (index 0–255 → 1
  byte, 256–65535 → 2 bytes; no cap, no spill path).
- **Compiler:** per-function `FnEmitter` raw-instruction buffer; `finish()`
  computes sizes, `instrOffsets`, the jump-width fixpoint, and encodes into
  `fn.code`. `buildFunction`/`compileClass` save/restore the emitter.
- **VM:** byte-offset PC; `decodeInstruction` per step; `instructionIndexAt`
  (binary search over `instrOffsets`) resolves line/col on the error path.
- **Codec v5 (v5-only):** `kQlcVersion = 5`; writer emits the per-function
  code blob + `instrOffsets`; reader reads the blob + offsets and validates
  (loc size == offset count, strictly increasing offsets, every instruction
  decodes, last instruction ends exactly at `code.size()`). **v1–v4 files are
  rejected** (the legacy fixed-width read path is removed).
- **Disassembler + dump:** `examples/qlang_run.cpp` `disassemble()` iterates
  `instrOffsets` and decodes; `examples/hello.cpp` `dumpBytecode` updated.
- **Tests:** `codec_check` rewritten for v5 (version assertion 5, v4 now
  rejected, blob/offset round-trip). Step 7 stress tests landed:
  `tests/unit/encoding_test.cpp` (41 checks: base/escaped round-trips for
  MOV/MOVI/JMP/ADD, truncated-buffer and reserved-modifier rejection, size
  helpers) + `tests/scripts/ext_regs.ql` (258 locals, 2-byte register
  encoding), `tests/scripts/ext_consts.ql` (258 string constants, 2-byte
  constant encoding), `tests/scripts/jump_widths.ql` (jmp8/jmp16/jmp32 all
  exercised). Full suite green (41/41 ctest; 795 counted unit cases).
- **Docs:** `docs/SERIALIZATION.md` rewritten to v5 (v5-only reader);
  `docs/spec/13-bytecodes.md` gains a "constant pool" (unique values, dedup)
  + "bytecode serialization" (→ SERIALIZATION.md) section.
- Open (unchanged): `$$` indirect operator (fully specified, unimplemented),
  duplicate-declaration warning (deferred pending warning-toggle infra).

## Round 36 (2026-11-03)

**Instruction format Phase 1 — line/col side-table (12 → 8 B)** (plan:
`docs/plans/instruction-format-optimization-plan.md` §Phase 1; uncommitted).
No language changes, no new opcodes. The `.qlc` format bumps 3 → 4.
- **`Instruction` shrinks 12 → 8 B:** `line`/`col` drop out of the hot-path
  struct (`static_assert(sizeof(Instruction) == 8)`). The VM decode loop now
  reads 8 B/instruction (32 per 256 B L1 line, was 21).
- **`Function::loc` side-table:** `std::vector<std::pair<uint16_t,uint16_t>>`
  parallel to `instructions`; the compiler stamps it in `emit()`/`emitImm()`
  (and the CALL-site override), and `buildFunction()`/`compileClass()`
  save/restore it with the code buffer. The VM's `vmThrowAt()` and the
  builtin/call throw sites resolve `loc[pc]` only on the error path.
- **`.qlc` v4:** per-function location section (`u32 locCount` + `locCount ×
  (u16 line, u16 col)`, `locCount <= instrCount`) after the instruction
  stream; instructions are 8 B on the wire (`u16 op, u16 a, i32`).
  `kQlcVersion = 4`; the reader accepts v1..v4.
- **`examples/qlang_run.cpp`:** `disassemble()` reads `f.loc[j]` (guarded)
  for the level-2 source-line interleave.
- **Tests:** `codec_check` raw-file builders gain the loc section; the
  unsupported-version case now uses v5; the version assertion is 4; the
  truncated-instruction case chops the i32 field. 85 checks, all green.
  Full suite green (37/37 ctest; 754 counted unit cases unchanged).
- **Docs:** `docs/SERIALIZATION.md` (header v4, §4 version 4 + loc side
  table, §5.1 `version <= 4`, §9 malformed suite v5, §10 status).
- Open (unchanged): `$$` indirect operator (fully specified, unimplemented),
  duplicate-declaration warning (deferred pending warning-toggle infra).
  Phase 2 (8-bit opcode + 8-bit registers, 8 → 4 B) and Phase 3 (universal
  scratch registers 240–255) remain per the plan.

## Round 35 (2026-11-03)

**Phase 5 — paper cuts** (plan:
`docs/plans/architecture-remediation-plan.md` §5; uncommitted). No language
changes, no new opcodes, no `.qlc` format change. Closes F6, H5, F7 from the
plan's defect list — the final phase of the remediation plan.
- **5.1 Step budget on the context (F6):** the instruction counter moves from
  the per-VM `m_steps` (reset on every dispatch) to the context —
  `QLangContext::stepsUsed()` (cumulative), `budget()` (the live `maxSteps`
  cap), `step()` (increment, checked once in the single dispatch loop),
  `resetSteps()`. The VM no longer owns a counter, so the host can read
  `stepsUsed()` before/after a call to compute per-dispatch cost.
  `EVENT_HANDLERS.md` §8.1 rewritten to the context-owned story.
- **5.2 Threading contract (H5):** "one context, one thread" — the context
  binds the creating thread's `std::thread::id` and a debug-build `assert`
  fires on cross-thread use (compiles out under `NDEBUG`). Every public
  entry point calls `bindThread()`; `enqueueFinalizer()` is exempt (the
  deleter may fire on any thread). Documented in `docs/spec/07-api.md`
  (new "threading contract" section). No threading model built.
- **5.3 CMake install/export (F7):** `install(TARGETS qlang EXPORT
  qlangTargets)` + headers + `qlangTargets.cmake` (namespace `qlang::`) +
  `qlangConfig.cmake` / `qlangConfigVersion.cmake` (hand-written version
  file — CMake 4.4.3 lacks `write_basic_package_config_file`);
  `target_include_directories` uses `$<BUILD_INTERFACE:…>` /
  `$<INSTALL_INTERFACE:include>`. Embedders can now `find_package(qlang)`
  against an install tree (verified with a consumer project). `file(GLOB)`
  for the `.ql` regressions replaced with an explicit 16-entry list.
- **Tests:** new `tests/unit/phase5_api.cpp` (17 checks, S1–S7: fresh
  `stepsUsed()==0`, accumulation across dispatches (no reset), per-dispatch
  cost via `call()` > 0 and stable, `budget()` == `maxSteps()` (default
  100000, tracks `setMaxSteps`), `resetSteps()` zeroes, runaway loop →
  `Runtime` "step limit", `set_max_steps()` from QLang sets `budget()`).
  ctest 36 → 37 entries; counted unit cases 737 → 754. Full suite green
  (37/37).
- **Docs:** `docs/spec/07-api.md` gains "step budget" + "threading contract"
  sections (new hash `f7c9932f…`); `docs/EVENT_HANDLERS.md` §8.1.
- Open (unchanged): `$$` indirect operator (fully specified, unimplemented),
  duplicate-declaration warning (deferred pending warning-toggle infra).

## Round 34 (2026-11-03)

**Phase 4 — `.qlc` format + compiler hygiene** (plan:
`docs/plans/architecture-remediation-plan.md` §4; uncommitted). No language
changes, no new opcodes, no value-tag changes. The one and only `.qlc` format
version bump in the plan lands here (2 → 3). Closes F1–F5 from the plan's
defect list.
- **4.1 Explicit on-wire encoding (F1):** writer and reader now emit/read
  every multi-byte field in explicit little-endian order, byte by byte — no
  struct `memcpy`, no host-endian `putU32`. Instructions are written field by
  field (`u16 op, u16 a, i32, u16 line, u16 col`); the raw 12-byte struct is
  no longer the on-disk encoding. The version is written and validated from a
  single shared `constexpr kQlcVersion = 3` (`QLangBytecodeIO.h`); the reader
  accepts v1..v3 (legacy files are byte-identical on little-endian hosts) and
  rejects anything newer. Non-durable types (Generator/Object/External/
  BoundMethod/TypeRef): `writeQlc` rejects a program whose constant pool holds
  one (returns `false`), and the reader rejects a file carrying tags
  15/16/20/21/22 with a clear `Io` error (never a silent Nil).
- **4.2 Compiler statement split (F2):** the 1,456-line `compileStmt` switch
  is split into one private method per `StmtKind` arm, with a dispatch switch
  at the top. Mechanical; no behavior change.
- **4.3 Remove auto-capture (F3):** the name-scanning free-variable
  augmentation is deleted — the parser's explicit capture list is the only
  capture mechanism. `global` resolves by name against the global table
  (Phase 2), killing the register-0 fallback. Compile-time tightening: scripts
  that relied on implicit capture now fail to compile (none did — the parser
  already required the explicit list).
- **4.4 Parser depth limit (F4):** a depth counter in the recursive-descent
  entry points (`parseStatement`/`parseExpression`); past `kMaxParseDepth =
  256` the parser throws a clean `QLangError(Parse, "nesting too deep")`
  instead of overflowing the C stack. RAII guard decrements on all paths.
- **4.5 Constant pool (F5):** the string-key dedup (which serialized every
  value to a key and roughly doubled pool memory) is dropped — `addConstant`
  now appends and returns the pool index per literal.
- **Tests:** `codec_check` updated for v3 (version assertion 2 → 3, the
  unsupported-version case now uses v4, the `Bin` test-file builder emits
  explicit little-endian). 85 checks, all green. Full suite green (36/36
  ctest; 737 counted unit cases unchanged).
- **Docs:** `docs/SERIALIZATION.md` (header v3, §3 R2 program-vs-value
  rejection, §3 R4 explicit load-time capture-rebinding rule, §4 version 3 +
  field-by-field instruction encoding, §5.1 `version <= 3`, §9 malformed
  suite v4, §10 status). `harness.md` codec line updated to v3.

## Round 33 (2026-11-03)

**Phase 3 — host API hardening** (plan:
`docs/plans/architecture-remediation-plan.md` §3; uncommitted). No language
changes, no new opcodes, no `.qlc` format change. Closes H1–H4 from the plan's
defect list.
- **3.1 Stable type handles + `type_info` validation (H2, H4):**
  `registerType`/`import<T>()` now return `std::shared_ptr<TypeDescriptor>`
  (was a raw `const TypeDescriptor*` invalidated by `clear()`). The Pimpl
  `TypeRegistry` drops only its own reference, so a handle the host legitimately
  holds — including a copied `NativeFunc` ctor lambda — stays valid across
  `clear()`. `ExternalRef` gains `const std::type_info* expected` + a `check`
  predicate; `self()`/`selfMut()` validate the resolved pointer and throw a
  clean `QLangError(Type, "external type mismatch: …")` on a mismatch
  (EXTERNAL_TYPES §10.2's "a wrong `weak_ptr` is UB" is now a runtime error).
  An expired borrow throws `QLangError(Runtime, "object destroyed: …")`.
- **3.2 Finalizer queue (H3):** the instance table's `shared_ptr` custom
  deleter no longer dispatches script — it only enqueues `(table, closure,
  program)` on the context's finalizer queue (`enqueueFinalizer`, thread-safe,
  never throws, so it is safe from a foreign thread or during unwinding).
  `processFinalizers()` drains at safe points — the start of `run()`/`call()`/
  `callClosure()`, inside `clear()`, or explicitly from the host — to a
  fixpoint (a finalizer that releases more instances runs them in the same
  call), swallows finalizer errors, and is guarded against re-entrancy
  (`m_drainingFinalizers`) and against a drain during context destruction
  (`m_destroying`).
- **3.3 `release()` builtin (H2 retention leak):** `Builtin::Release`
  (`releaseExternal`) drops the `owned` shared_ptr of an owned `External`,
  letting scripts free script-constructed objects; the value becomes `Nil`
  (use-after-release is a clean nil error). A borrowed external (host-owned) or
  a non-external is a type error. The argument is mutated in place (special-
  cased in `CALL`; the compiler writes the slot back to the source variable so
  the original reference is dropped).
- **3.4 Context split (H1):** `QLangContext`'s Pimpl now holds two small
  classes — `ProgramCache` (the compiled-program cache, cleared by `clear()`
  only after the global table) and `TypeRegistry` (the type handles). The
  public header surface stays source-compatible for existing embedders.
- **Tests:** new `tests/unit/phase3_api.cpp` (23 checks: T1 handle survives
  `clear()`, T2 type-mismatch clean error, T3 expired-borrow clean error,
  F1–F7 finalizer queue — not-from-deleter / drain-at-run / fixpoint /
  error-swallow / clear-drain / foreign-thread-enqueue / re-entrancy-guard,
  R1–R4 `release()` — owned freed / borrowed rejected / non-external rejected /
  use-after-release, C1 copied `NativeFunc` survives `clear()`). ctest 32 → 36
  entries; counted unit cases 696 → 737. Full suite green (36/36; `perf`
  skipped on this non-Release build).
- **Docs:** `docs/EXTERNAL_TYPES.md` §3/§5/§6/§10.2/§11 (shared handles,
  `type_info` validation, `release()`, UB → clean error) and
  `docs/EVENT_HANDLERS.md` §0/§8.2 (finalizer drain point) updated.
- Open (unchanged): `$$` indirect operator (fully specified, unimplemented),
  duplicate-declaration warning (deferred pending warning-toggle infra).

## Round 32 (2026-11-02)

Test migration + compliance docs (plan: `docs/plans/test-migration-plan.md`;
Phases 0–5 landed in `cb853be` / `5b202ae` / `8f20139` / `0036dec` / `3e6e8b8` /
`1577bf7`). No language changes, no new opcodes.
- **Test suite relocated to `tests/`:** 15 unit harnesses in `tests/unit/`
  (incl. `test_native` / `test_call` from the orphan-file triage), 16 `.ql`
  regressions in `tests/scripts/`, perf harness + immutable baseline in
  `tests/perf/`; `examples/` holds demos only (`hello.cpp`, `qlang_run.cpp`).
- **ctest: 6 → 32 entries** — 15 unit + 16 script (`script_*`, driven by
  `qlang_run`) + 1 `perf`; all green (re-verified 2026-11-02). **696 counted
  unit cases**: 463 `spec_check2` (+42 scope/boolean/trace coverage, Phase 5),
  33 `spec_check`, 20 `spec_cast_test`, 7 `test_module`, 5 `gen_test`, 22
  `range`, 13 `class_str`, 17 `class_static`, 23 `external`, 8
  `test_dollar_operator`, 85 `codec`; plus `ALL PASS` (`test_native`,
  `test_call`, `test_inject`).
- **Perf baseline (spec `15-tests.md` "performance tests"):**
  `tests/perf/perf_bench.cpp` — median-of-5 per parse/compile/execute stage over
  `hello.ql`, `money.ql`, `cast.ql`, `perf_stress.ql` (one unmeasured warm-up
  pass per script) — compared against the committed
  `tests/perf/baseline.json` (±20% `QL_PERF_TOLERANCE` + 0.5 ms `QL_PERF_EPS_MS`
  floor; `--update-baseline` for deliberate regeneration). The gate runs in the
  gxx CI job (g++/Linux Release calibration); non-Release builds → exit 77 →
  ctest Skipped; clang/MSVC jobs exclude it (`ctest -E perf`).
- **CI matrix (spec `15-tests.md` "automatic testing" — "three separate
  ci/cd yaml"):** `ci.yml` (g++: full ctest incl. `perf` gate + `perf-result`
  artifact; clang++: ctest `-E perf`; mingw-w64 cross: build-only),
  `ci-msvc.yml` (MSVC x64 / `windows-latest`: ctest `-E perf` + compiler-version
  step feeding `docs/COMPILER_PORTABILITY.md`), `ci-wasm.yml` (emscripten:
  `qlang` + `qlang_example` with `-sALLOW_MEMORY_GROWTH -fexceptions`, smoke via
  `node build/qlang_example.js`). Push-to-`main` triggers.
- **Compliance:** new `docs/compliance/tests.md` module (validates
  `15-tests.md`); spec hashes re-verified — `13-bytecodes.md` → `1eac6a97…`,
  `14-scope.md` → `ec1eadb2…`, `15-tests.md` → `dd9c8fe3…` (stale since the
  Round 31 spec corrections and the 15-tests CI-yaml amendment); status/harness
  paths refreshed to `tests/unit/`.
- Open (unchanged): `$$` indirect operator (fully specified, unimplemented),
  duplicate-declaration warning (deferred pending warning-toggle infra).

## Round 30 (2026-10-29)

**MOVI** — 32-bit `Int` literal immediate; inlines the `CONST poolIdx + mov`
pair for integer literals (plan step 4.4, `docs/plans/qlc-codec-plan.md`).
- `Opcode::MOVI` appended **last** in the `Opcode` enum (existing `.qlc`
  opcode numbers unchanged); `usesI32()` covers it. Layout: `a` = destination
  register, `i32` = 32-bit `Int` literal — `r[a] = Int(i32)`, `Type::Int`
  only.
- Compiler: `local`/plain-assignment inlined into the target register (saves
  the temp + `mov`); expression context keeps the temp + `MOV`.
- `.qlc` writer emits format version 2 (readers accept ≤ 2).
- VM: dispatched in all four exec loops. `codec_check` passes.
- **Step 4.4 (2026-10-30, `58a9659`):** compiler-generated `Type::Int`
  constants (for-loop unit/zero/direction counters, for-in index init, class
  `__static_const__`/`__const_fields__` markers, per-field `__fields__`
  indices, `ab[]` append index) also compile to `MOVI` — the constant pool
  holds no `Type::Int` entries at all; instruction count unchanged (1:1).

## Round 31 (2026-10-29)

**ADDI + SUBI (Part 1) and alternate conditional jump — CMP/CMPI +
je/jne/jg/jge/jl/jle (Part 2).**
- Part 1: `Opcode::ADDI` (`r[a] = r[a] + Int(i32)`, in-place, `Type::Int`
  only) and `Opcode::SUBI` (`r[a] = r[a] - Int(i32)`) appended after `MOVI`;
  `usesI32()` covers both; VM dispatch in all four exec loops; compiler emits
  them for integer-literal `+=`/`-=`.
- Part 2: flag-based conditional jump. `Opcode::CMP` (flags =
  `cmp(r[a], r[b])`) and `Opcode::CMPI` (flags = `cmp(r[a], Int(i32))`) set
  independent GT/EQ/LT bits (mutually exclusive per comparison); `JE`/`JNE`/
  `JG`/`JGE`/`JL`/`JLE` jump on the flags (`JE` = EQ, `JNE` = EQ not set,
  `JG` = GT, `JGE` = GT|EQ, `JL` = LT, `JLE` = LT|EQ), pc target in `i32`.
  The spec flag/jump tables in `docs/spec/13-bytecodes.md` were corrected to
  the coherent independent-bits model, and the operand map gained rows for
  SUBI/CMP/CMPI/J*.
- `codec_check` covers the new MOVI + ADDI + SUBI + CMP/j* suites; all
  suites green (421 `spec_check2`, ctest 5/5).

## Round 16 (2026-10-27)

Spec drift audit: SPEC.md moved `1edc77ac` (41751 B / 1485 lines) → `c7f30b2a` (42276 B / 1502 lines) → `658c5631` (42315 B / 1503 lines, staged refinement mid-audit).
- **Round 15 work committed** in `72e2711` (implicit `__str__` collection rendering + tests + compliance), **plus two SPEC.md changes**: the `# implicit __str__ rules` heading rename and the **new `## operators < and >` section** (11-row type-pair table), later refined (staged, uncommitted): `array|array` per-index added, `table|table` = "comparison of each key value", `table|array` invalid, `class|any` ("bytecode cannot be compared").
- **`## operators < and >` implemented:** `cmp()` gains an optional `QLangVM*` parameter — (1) **string vs table/class/array** compares the string against `displayString(object)` (i.e. `__str__()` / class name / implicit collection rendering), either order; (2) **array vs array** compares each value at the relative index — first differing value decides, shorter array is less; (3) **table vs table** compares each key value in key order — same first-difference/shorter-is-less rule. Class instances (tables carrying `__class__`) and table|array fall through to the throw (spec: table|class, table|array, class|any → invalid). `doBinaryVm` routes LT/GT/LE/GE through the vm-aware `cmp` (same pattern as Round 13's EQ/NE). All other pairs keep the existing behavior: number|string, number|object, nil|any → `type error: operator '<' cannot compare X and Y`. The `in` opcode's `cmp` call is unchanged (no vm → legacy behavior).
- 20 new `spec_check2` cases (262 → 282): 13 valid (number/float/string/string-object/string-class/string-array/table-per-index/table-prefix/table-le/array-per-index ×2/array-prefix/array-ge) + 7 invalid (number|string, number|object, nil|any, table|class, class|class, table|array, class|array).
- Harness re-run: **282/282** (`spec_check2`), **33** (`spec_check`), **20** (`spec_cast_test`), **7** (`test_module`), **5** (`gen_test`), **22** (`range`), **13** (`class_str`), **17** (`class_static`), all green = **399 total**.
- `# error reporting` remains **not implemented**.

## Round 17 (2026-10-27)

Impl fixes from the `examples/scripts/cmp.ql` bug report; SPEC.md unchanged (hash `658c5631` still current).
- **Boolean-producing operators now return `Bool`, not `Int(1/0)`:** `EQ` `NE` `LT` `GT` `LE` `GE` `IN` `LAND` `LOR` in `doBinary` + `doBinaryVm`, and `NOT` in `doUnary`. So `type(1 < 2)` → `"bool"`, `f"{1 < 2}"` → `"true"`; truthiness, `isEqual`, and arithmetic on the results are unchanged (`isTrue`/`isNumeric` already handle `Bool`).
- **Unknown function calls are now a compile error:** `printl("x")` previously emitted `CALL b=-1`, which the VM mis-dispatched as native func `idx = -1 - (-1) = 0` — silently calling the first auto-registered builtin-module function and returning nil (no error, no output). The compiler now throws `parse error at line N,C: unknown function 'name'` when the callee is not a user `def`, closure var, generator var, builtin, or native/module function. The parser's general `name(args)` path now also records `callLine`/`callCol` (previously only the builtin special-cases did), so the error carries a real position — a small step toward `# error reporting`.
- 9 new `spec_check2` cases (282 → 291): `type()` of `<` `>` `==` `&&` `!` `in` → `"bool"`, f-string bool rendering, bool arithmetic (`1 + (1 < 2)` → 2), unknown-function throw.
- Harness re-run: **291/291** (`spec_check2`), **33** (`spec_check`), **20** (`spec_cast_test`), **7** (`test_module`), **5** (`gen_test`), **22** (`range`), **13** (`class_str`), **17** (`class_static`), all green = **401 total**; `helpers.ql` + `cmp.ql` (minus its deliberate error line) clean.
- `# error reporting` remains **not implemented**.

## Round 18 (2026-10-27)

Spec drift audit: SPEC.md moved `658c5631` (42315 B / 1503 lines) → `1b777ec6` (42557 B / 1511 lines). No code changes.
- **Committed since last audit:** `b1e6743` (Round 17 Bool-operators/unknown-function fix + the staged `## operators < and >` table refinement + `|||` whitespace fix — the "staged, uncommitted" refinement from Round 16 is now in HEAD).
- **New uncommitted SPEC.md edits (2):** (1) reserved-word list cleanup `range => keyword` → `range` (staged; `range` is already reserved in the lexer — documentation-only, no behavior change); (2) two new numeric-literal examples in `## declarations and assignments`: `local my_float = 1.0f` ("floats use a special notation") and `local my_uint = 44u` ("unsigned int").
- **Probed:** both suffix forms are **NOT implemented** — `QLangLexer::number()` has no suffix handling; `1.0f` → `parse error at line 0,0: use of undeclared variable 'f'`; `44u` → `use of undeclared variable 'u'`. New Open Item + Not-Yet-Implemented row added.
- Harness re-run (no code changes): **291/291** (`spec_check2`), **33** (`spec_check`), **20** (`spec_cast_test`), **7** (`test_module`), **5** (`gen_test`), **22** (`range`), **13** (`class_str`), **17** (`class_static`), all green = **408 total** (the prior "401 total" was an arithmetic slip — the components sum to 408).
- `# error reporting` remains **not implemented**.

## Round 19 (2026-10-27)

Numeric literal suffixes `1.0f` / `44u` implemented (SPEC §declarations and assignments examples from the Round 18 drift; SPEC.md unchanged, hash `1b777ec6` still current).
- **Lexer** (`QLangLexer::number()`): after the digit loop, a single directly-attached suffix is consumed — `f` forces `TokenType::Float` (`1.0f`, and `123f` → `Float 123.0`), `u` forces the new `TokenType::UInt` (`44u`); the suffix is stripped from the token text so `123 f` / `123 u` (whitespace) remain two tokens. Hex path untouched (no hex suffixes in the spec).
- **Parser** (`QLangParser::parsePrimary()`): new `case TokenType::UInt` → `QLangValue::Unsigned(stoull(value))`; the bare-literal statement check now includes `UInt`.
- `TokenType::UInt` added to the enum after `Float` (outside the `TypeByte..TypeTable` 1:1 range — no mapping impact).
- 4 new `spec_check2` cases (291 → 295): `type(1.0f)` → `"float"`, `type(44u)` → `"unsigned"`, `44u` value 44, float-suffix arithmetic.
- Harness re-run: **295/295** (`spec_check2`), **33** (`spec_check`), **20** (`spec_cast_test`), **7** (`test_module`), **5** (`gen_test`), **22** (`range`), **13** (`class_str`), **17** (`class_static`), all green = **412 total**.
- `# error reporting` remains **not implemented**.

## Round 29 (2026-10-28)

Phase 4 of `docs/action_plan.md`: **`# error reporting`** (SPEC section
committed in `649be35`). QLang-side only — `$$` and the C++ API / external
data stay out of scope (as does the warning infrastructure). SPEC.md gained a
`## format` subsection documenting the report shape (new hash `7b2e6604…`,
1712 lines / 48172 bytes — the diff also includes the earlier uncommitted
enum-spec lines).
- **pc → line/col table (compiler):** `Stmt` gains `line`/`col` (1-based,
  first token of the statement); `QLangParser::parseStatement()` wraps
  `parseStatementImpl()` to stamp them. `QLangCompiler` tracks `m_curLine`/
  `m_curCol` (set at the top of `compileStmt`) and `emit()` writes them into
  every `Instruction.line/col` (the fields already existed on `Instruction`).
- **`QLangError` (header rewrite):** gains `module` (string) + `frames`
  (`std::vector<QLangFrameRef{name, line}>`, innermost first) + `message()`
  (raw message accessor); `formatReport()` renders
  `module 'main' line L, col C in function 'fn': <kind>: <message>` + one
  `at line N in <fn>` per frame (missing pieces omitted); `report()` rethrows
  with `what()` = the full report (kind/line/col/module/frames preserved).
  Plain errors keep the old `<kind> at line L,C: <msg>` `what()`.
- **VM throw sites:** new `QLangVM::vmThrow(kind, msg)` /
  `vmThrowAt(kind, line, col, msg)` — walk `m_frames` innermost-first,
  resolve each frame's pc → instruction line, take the current pc's line as
  the error line, attach `m_scope.moduleName()` + the frame chain. All
  ~97 member-function throw sites now go through them. Free helpers
  (`num`, `cmp`, `callBuiltin`, `strict_convert`, `doBinary`, `doUnary`,
  `toSetElements`, `appendValue`, `lenOf`, `seqGet`) gained an optional
  `QLangVM *vm` parameter threaded from their call sites and route through a
  `vThrow(vm, …)` shim (null vm → plain 0,0 error, as before). `callBuiltin`
  passes its existing caller line/col through.
- **Top-level catch:** `QLangContext::run()` wraps the pipeline in try/catch
  and rethrows `QLangError::report(e)` — callers (and the harness) see the
  full formatted message. `QLangContext::moduleName()` = `"main"`.
- 6 new `spec_check2` cases via an `expectMsg(name, src, …substrs)` helper:
  lex-error line, parse-error line (line 3 of 3), runtime type error
  (module + line 3 + kind), builtin arity error (line + `main` frame),
  named-function error (`in function 'foo'` + trace), deep call chain
  (`a→b→c` division by zero: `at line 2 in c`, `at line 5 in b`,
  `at line 8 in a`, `at line 10 in main` — innermost first).
- Harness re-run: **369/369** (`spec_check2`), **33** (`spec_check`), **20**
  (`spec_cast_test`), **7** (`test_module`), **22** (`range`), **13**
  (`class_str`), **17** (`class_static`), all green = **481 total**. ctest 4/4.
- Known limits: `QLangValue` accessor throws (e.g. `asInt()` on a string,
  `QLangValue.cpp`) and `QLangObjects.cpp` object-dispatch throws still pass
  0,0 (no VM context at those call sites — external-type paths are out of
  scope this phase); they still get the module name via the `run()` catch
  when thrown through it. Trace lines use the function name only (the spec's
  "module/class/function/table" per node is satisfied for the common case —
  QLang frames are functions; class/table context is not separately
  attributed).

## Round 28 (2026-10-28)

Phase 3 of `docs/action_plan.md`: the `enum` type (SPEC §enum). SPEC.md at
`a350a25c` (unchanged this round — the `## enum` section was staged earlier).
- **Lexer**: `TokenType::Enum` + `enum` in `keywordType()` (reserved word).
- **Parser**: `parseEnum()` — `enum Name[:numeric-type] { A, B, C = 100, D }`;
  optional `:type` restricted to the 8 numeric types; member values must be
  integer literals; empty body → parse error; `local`/`let` prefix supported.
- **Compiler**: enum members are compile-time constants. Values computed at
  codegen (implied sequential from 0; `= N` resumes at N+1) and typed per the
  enum's base type. Storage: an immutable table (`name → value`) in a hidden
  variable `__enum_<Name>` (class-prototype pattern) — so `len(Enum)` works
  via the table path and `type(Enum::M)` reports the member's type. Member
  access `E::M` and `E.M` folds to `CONST <value>`; unknown enum/member →
  compile error. Redeclaration (visible same-name enum, or a global enum
  inside a function) → compile error. `local`/`let` → block scope (hidden var
  in the block register map); global enums captured into function bodies via
  the free-variable mechanism (hidden var captured, like global consts).
- 16 new `spec_check2` cases (347 → 363): implied values, `= N` resume,
  `::` + `.` notation, typed `:byte` (value + `type()`), `len`, arithmetic,
  redeclaration / unknown enum / unknown member / valueless / non-integer
  value errors, block scope (visible inside, not outside), global-in-function,
  member-not-assignable. Harness: **363/363**; full tally **475**.
  ctest 4/4. Remaining spec gaps (not in this phase): the C++ enum-injection
  API is stated as "needed" but undefined; `$$` and external-data injection
  are Phase 4.

## Round 27 (2026-10-28)

SPEC.md advanced to `a350a25c` (1659 lines, 47151 bytes) — it **closed the
remaining set gaps**, so this round implements the rest of SPEC §sets:
- **`x in set(1,2,3)`** — membership, "similar to `x in [1,2,3]`". IN opcode
  gains a Set branch using `setContains` (the same exact-match element rule
  set dedup uses, so `in` is consistent with dedup and `==`). Works in `if`,
  `while`, and any boolean context.
- **`for x in set(1,2,3)`** — iteration, "similar to `for x in [1,2,3]`".
  `seqGet` gains a Set branch (yields elements in storage/insertion order;
  order is implementation-defined per spec).
- **`set == set`** — "only true if set is an exact match." `isEqual` gains a
  Set case: equal size + every element of A present in B. **Order-independent**
  (consistent with "value order is dependant on the implementation"); so
  `set(1,2,3) == set(3,1,2)` → true.
- **`cast<set>(array)`** / **`cast<array>(set)`** — new cast-table rows.
  Parser cast-type check + tmap gain `TypeSet`; `strict_convert` converts
  Set→Array (`v.asSet()`) and Array→Set (`toSetElements`, dedup). `cast<int>(set)`
  now rejected (Set added to the numeric-target reject list).
- 10 new `spec_check2` cases (337 → 347). Harness re-run: **347/347**
  (`spec_check2`), **33** (`spec_check`), **20** (`spec_cast_test`), **7**
  (`test_module`), **22** (`range`), **13** (`class_str`), **17**
  (`class_static`), all green = **459 total** (gen_test 5/5 separately).
  ctest 4/4. SPEC §sets is now **fully covered** — no set gaps remain.

## Round 26 (2026-10-28)

Phase 2 of `docs/action_plan.md`: the `set` type (SPEC §sets). SPEC.md at the
time was `fe8c72fe` (superseded by `a350a25c` in Round 27) — code + tests only.
- **Value type**: `QLangValue::Type::Set` appended **last** after `Long`
  (keeps the fixed `.qlc` serialization tags stable); stores an
  `ArrayValue` (the variant already had it — no variant change). Factory
  `Set(ArrayValue)`, accessors `asSet()` const/mut, `typeName()` → `"set"`.
- **Lexer/parser**: `TypeSet` token (`set` is now reserved — `local set = 1`
  is a parse error); `parsePrimary` special case treats `set(...)` as a
  `Call` with callee `"set"` (same pattern as `str()`/`int()`); `x:set`
  accepted in `local`/`const` typed decls (tmap entries).
- **VM**: `Builtin::Set` — `set(1,5,7)` variadic, `set([1,6,46,5])` array
  coerced; duplicates dropped (first-seen order kept); elements restricted
  to numbers and strings (else type error). `convert()` (CONV) coerces an
  array to a set for typed decls. `len(s)` = count; empty → false, non-empty
  → true; `__str__`/display → `set(1, 2, 3)` (strings quoted, per the SPEC
  example); `valueToString` → `"<set>"`. ARRAPP (the 4 duplicated dispatch
  sites) now routes arrays+sets through one `appendValue` helper, so
  `s[] = v` appends with dedup.
- **Not implemented (SPEC is silent — flagged, not invented):** `in`
  membership on sets, `==` between sets, `isset` on set elements, `for x in
  set` iteration, `cast<set>`.
- 12 new `spec_check2` cases (325 → 337). Harness re-run: **337/337**
  (`spec_check2`), **33** (`spec_check`), **20** (`spec_cast_test`), **7**
  (`test_module`), **22** (`range`), **13** (`class_str`), **17**
  (`class_static`), all green = **449 total** (gen_test 5/5 separately).
  ctest 4/4.

## Round 25 (2026-10-28)

Phase 1 of `docs/action_plan.md` (plan excludes `$$` and C++ API / external
data). SPEC.md unchanged (hash `56e3b092…` still current) — code + tests only.
- **Valueless `local` defaults to 0** (SPEC §declarations): the Declaration
  case emits `CONST Int(0)` instead of nil; a typed valueless decl
  (`local b:byte`) gets zero of that type via the existing CONV. The 4
  nil-equality cases (`eq/ne nil nil/x`) updated to declare `nil`
  explicitly — they had encoded the old nil default.
- **`fetch` reserved word**: `TokenType::Fetch` in the lexer enum +
  `keywordType()` — `local fetch = 1` → `parse error: expected variable name
  but found 'fetch'`. Behavior unspecified in SPEC; reserving is all it asks.
- **math `rand()`**: `mathRand` in `QLangModules.cpp` (`std::mt19937` +
  `uniform_int_distribution<int32_t>`, returns `Int`), registered in the math
  module.
- 6 new `spec_check2` cases (319 → 325). Harness re-run: **325/325**
  (`spec_check2`), **33** (`spec_check`), **20** (`spec_cast_test`), **7**
  (`test_module`), **22** (`range`), **13** (`class_str`), **17**
  (`class_static`), all green = **437 total** (gen_test 5/5 separately).
  ctest 4/4. Probe: `local z` → `type(z)` = "int", `z` = 0.
- **Duplicate-declaration warning REVERTED** (post-round, same day): the
  Round 25 warning (stderr at the Declaration site) was removed per user
  direction — the new `# warning` SPEC section requires warnings to be
  toggleable, so the warning is deferred to a later plan with the warning
  infrastructure (2 `spec_check2` cases removed; 327 → 325).

## Round 24 (2026-10-28)

SPEC.md one-line tweak (hash `3f1afe5b…` → `46cd4668…`, 1544 lines). Fixed the
`examples/scripts/cast.ql` bug report — four root causes in `src/QLangVM.cpp`:

1. `format()` (f-string interpolation) passed a `double` to a `%s` placeholder
   (`snprintf` UB) → Float args rendered empty/garbled. Now renders `%.15g`
   (same as `valueToString`) when the placeholder is `%s`.
2. `format()` numeric branch passed a `long long` to a `%d` placeholder —
   `%d` reads only 32 bits, so `cast<dword>(-1)` (4294967295) printed `-1`.
   Conversion is now `lld` (and explicit `:d`/`:i` specs are widened too).
3. `format()` had no Qword path — qword args went through `toInt64()`, which
   throws for values > INT64_MAX. New Qword branch uses `%llu`;
   `cast<qword>(-1)` now prints `18446744073709551615`.
4. `strict_convert` Float target went through `toInt64` → `cast<float>(1.24)`
   truncated to 1.0. Now `toDouble(v)` (float→float is identity).

Latent crash fixed: four VM debug-trace sites (`[DBG-RET]`, `[DBG-RES]`,
`[DBG-MAIN]` map dump, `[DBG-YIELD]` ×2) evaluated `toInt64()` in their
arguments — evaluated **before** `trace()` short-circuits — so any qword >
INT64_MAX in a traced register threw even with tracing **off**. The implicit
main-frame `RETURN` (retVal = regs[0]) hit this at end of every program whose
first variable held a big qword. Trace args now use non-throwing
`valueToString`.

`examples/scripts/cast.ql`: all 13 lines print the expected values; the
previously-commented qword block is uncommented and the stale `# bug:` lines
removed. 4 new `spec_check2` cases (f-string float, f-string unsigned > int32
max, f-string qword max, `cast<float>` identity).

- Harness re-run: **319/319** (`spec_check2`), **33** (`spec_check`), **20**
  (`spec_cast_test`), **7** (`test_module`), **22** (`range`), **13**
  (`class_str`), **17** (`class_static`), all green = **431 total**. ctest 4/4.
- Pre-existing, out of scope: a 2⁶⁴−1 **literal** (`18446744073709551615`)
  aborts in the lexer (`std::stoll` out_of_range) — only reachable via a
  literal, since `cast<qword>` now covers the value range.

## Round 23 (2026-10-28)

SPEC.md unchanged (hash `3f1afe5b…` from Round 22). Implemented the `long` type
(spec `## data types` row `|long|8bytes|int64_t|`):

- `QLangVariant` gains `int64_t`; `QLangValue::Type::Long` appended **last**
  (after `Eof`) so the fixed `.qlc` value tags (docs/SERIALIZATION.md R1) are
  unchanged — `Long` is tag 17, `i64` payload.
- Lexer: `long` → `TokenType::TypeLong` (outside the `TypeByte..TypeTable`
  1:1 range, like `TypeQword`). Parser: decl + cast `tmap` entries,
  type-token checks in `local`/`const`/`cast<…>`.
- VM: `isNumeric`, `valueToString`, `lenOf` (8), declaration `convert`,
  `strict_convert` (numeric tower). Modules: min/max/sum treat `Long` as
  integer. Context: `levelFromValue`, `setMaxStepsFunc`. Objects:
  `isNumericType` includes `"long"`. `.qlc` writer: tag + `u64` payload.
- 5 new `spec_check2` cases (typed decl with 64-bit literal, `type()` →
  `"long"`, `len` → 8, `const`, `cast<long>`). `examples/scripts/len.ql`
  now runs clean through `long is 8 byte`.
- Docs: README type table (`dword` alias + `long` rows), SERIALIZATION tag
  17, EXTERNAL_TYPES boundary table (`int64` → Long, `uint64` → Qword).
- Suites: `spec_check2` 315, `spec_check` 33, `spec_cast_test` 20,
  `test_module` 7, `range_check` 22, `class_str` 13, `class_static` 17 —
  all green; ctest 4/4.

## Round 22 (2026-10-28)

Spec wave: `dword` redefined in SPEC.md `## data types` as a **4-byte `uint32_t` alias of `unsigned`** — the original spec's 8-byte `dword` was an error; 8 bytes is `qword`. `qword` remains the `uint64_t` type.

- **Implementation (1 line):** `QLangLexer::keywordType` — `dword` now lexes as `TokenType::TypeUnsigned`, reusing the entire `unsigned` machinery: `local d:dword = 5` is an `Unsigned` declaration, `type(d)` → `"unsigned"`, `len(d)` → 4, `cast<dword>` → `Unsigned`. No parser/VM/value-type changes needed.
- **Tests:** 4 new `spec_check2` cases (dword alias typed to u32 max, `type()` → `"unsigned"`, `len()` → 4, const). `examples/scripts/len.ql` restored with a `dword` line → prints `unsigned is 4 byte`.
- **Tally:** spec_check2 310; total **427 passed, 0 failed** (all 8 suites green; `ctest` 4/4).
- **SPEC.md:** `3f1afe5b0556230e6349f8eb7ed37eb2920a2802ac4f2713bd3c839a004855d5` (1543 lines, 43928 bytes), uncommitted.
- **Still open:** `long` (int64_t) and `set` types; `set`/`long` reserved words (spec `## data types` lists them; not lexer keywords — `local a:long` → `unknown type after ':'`).

## Round 21 (2026-10-28)

Spec wave implemented: `dword` → `qword` rename, `len()` storage byte sizes, `delete` nil/external guards.

### Changes
- **`dword` → `qword` (spec errata applied):**
  - `QLangLexer.cpp` — keyword `qword` (still maps to internal `TokenType::TypeQword`).
  - `QLangValue.cpp` — `Type::Dword` type name is now `"qword"`.
  - `QValue.h` — public helper `qval_dword` → `qval_qword`.
  - `QLangObjects.cpp` — `isNumericType` accepts `"qword"`.
  - `SPEC.md` — errata note removed; data types table, reserved words, `$` operator rows, C++ API list, implementation notes all use `qword`.
  - `README.md` — type table row `dword` → `qword`.
  - Tests: `spec_check2.cpp` + `spec_cast_test.cpp` cases renamed to `qword`.
  - `dword` is now a free identifier (probe-verified). Lexer token renamed `TokenType::TypeDword` → `TokenType::TypeQword` (8 sites: header + `QLangLexer` + `QLangParser`); all internal value identifiers renamed to `Qword` (`QLangValue::Type::Qword`, `QLangValue::Qword()`, `asQword()`, `isQword()`).
- **`len()` storage byte sizes (`QLangVM::lenOf`):** char/byte 1, word/short 2, int/unsigned 4, qword/float 8, bool 1, class instance (table with `__class__`) 1, nil → throws `len: undefined/nil has no length`; string/array/blob element counts and plain-table size unchanged.
- **`delete` guards:**
  - `QLangParser::parseDelete` — `delete nil` → parse error `cannot delete nil`.
  - `QLangCompiler` — new `addExternalVars(std::set<std::string>)` + `m_externalVars`; `StmtKind::Delete` throws `cannot delete external variable '<name>'` for writable injected host vars, `delete of undeclared variable '<name>'` for everything else.
  - `QLangContext::run()` — passes `m_vars` minus `m_injectedConsts` (writable injected vars) to the compiler.
- **Tests:** 11 new `spec_check2` cases (9 `len` + 2 `delete`); `spec_check2` 295 → 306.

### Verification
- `cmake --build build` clean; `ctest` 4/4.
- Full tally: spec_check2 **306**, spec_check 33, spec_cast_test 20, test_module 7, gen_test 5, range_check 22, class_str 13, class_static 17 = **423 passed, 0 failed**.
- Probes: `dword` free identifier; `delete nil` → `cannot delete nil`; `delete` on `ctx.set()` var → `cannot delete external variable 'host_var'`; `delete` on `ctx.set(..., true)` const → `delete of undeclared variable 'host_c'`.

### SPEC.md
- SHA256: `6a0856c581ad415e0e1df8139f55ddbd8accd67f95260e05b6f71f45fd5b1fc7` (1539 lines, 43636 bytes).
- Still unimplemented from the 2026-10-28 wave: `set`/`long` reserved words; `long`/`set` types.

## Round 20 (2026-10-28)

Spec drift audit: SPEC.md moved `1b777ec6` (42557 B / 1511 lines) → `c5d69bc2` (43372 B / 1528 lines), staged uncommitted. HEAD `b564302` committed the Round 19 work (incl. the `1.0f`/`44u` SPEC examples + `range` reserved-word cleanup + `hello.ql` `printl`→`println` typo fixes).
- **New uncommitted SPEC.md wave:** `## data types` table (new rows `long`/`qword`/`set` + dword→qword errata note); `set`/`long` added to reserved words; `## strings` section moved up; declarations type-inference intro; **`len()` numbers → storage byte size**; **`delete` external/nil → error**; `sum` "coerce non-float to float" note.
- **Probed:** `len(123)` → 1 (spec: int = 4 bytes), `len(1.13)` → 1 (spec: float = 8) — **not implemented**; `local set = 1` / `local long = 1` compile — reserved words **not implemented**; `local x:long` / `x:qword` / `x:set` → `parse error: unknown type after ':'` — types **not implemented** (`dword` remains the working 64-bit type); `delete nil` → parse error (errors, wording "expected variable name"); `delete` on a `ctx.set()`-injected var → `parse error: delete of undeclared variable` (errors, wording differs).
- Harness re-run (no code changes): **295/295** (`spec_check2`), **33** (`spec_check`), **20** (`spec_cast_test`), **7** (`test_module`), **5** (`gen_test`), **22** (`range`), **13** (`class_str`), **17** (`class_static`), all green = **412 total**.
- `# error reporting` remains **not implemented**.

## Round 15 (2026-10-27)

Spec drift audit: SPEC.md moved `967b17e5` (41449 B / 1471 lines) → `1edc77ac` (41751 B / 1485 lines).
- **Round 14 work committed** in `649be35` (bool literals/`isset` → `Bool`, f-string/bool snprintf segfault fix, triple-quoted string leak fix, `helpers.ql`, Round 14 compliance) — **plus the two new SPEC.md sections** `# implicit __str__` and `# error reporting`.
- **Uncommitted:** SPEC.md heading rename `# implicit __str__` → `# implicit __str__ rules` (content unchanged).
- **`# implicit __str__ rules` implemented:** `QLangVM::displayString` now renders arrays as `[e1, e2, ...]` and plain tables as `{k=v, ...}` via a new recursive `renderElement` (strings double-quoted, nested collections recurse, scalars in plain display form). All display contexts conform: print/println/printf %s/sprintf %s/f-string/join/`str()`. `str([0,1,2,3,4])` → `"[0, 1, 2, 3, 4]"`; `str({name="Bob", age=9})` → `"{age=9, name=\"Bob\"}"`; `f"{b}"` on array `b` → `[1, 2, 3]`. `valueToString` (the `+=` path) and `cast<string>` intentionally unchanged (the `+=` spec table says "any other types → string<\type>"). **Caveat:** table keys render lexicographic (`std::map`), not the spec example's insertion order — no ordering rule in the spec; flag if insertion order is required.
- 4 new `spec_check2` cases (258 → 262); 3 prior `<table>` expectations updated to `{a=1}` (`class_str_test` ×2, `spec_check2` ×1).
- Harness re-run: **262/262** (`spec_check2`), **33** (`spec_check`), **20** (`spec_cast_test`), **7** (`test_module`), **5** (`gen_test`), **22** (`range`), **13** (`class_str`), **17** (`class_static`), all green = **379 total**.
- `# error reporting` remains **not implemented** (now committed in the spec).

## Round 14 (2026-10-27)

Spec drift audit: SPEC.md moved `c46efc2f` (41038 B / 1457 lines) → `967b17e5` (41449 B / 1471 lines).
- **Round 13 work committed** in `c62b8e4` ("string == object via `__str__` fix"): the `==`/`!=` example table + typo fix (SPEC.md), `valueEqual`/`doBinaryVm` (code), 6 `spec_check2` cases, CMake test targets, `moneyv2.ql`, Round 13 compliance text.
- **New uncommitted SPEC.md section `# error reporting`** (requirements + execution trace) — **not implemented**: no module name in errors, no class/function/table context, no execution trace, and 152/162 throw sites carry no line/col. See Open Items.
- Harness re-run (no code changes): **258/258** (`spec_check2`), **33** (`spec_check`), **20** (`spec_cast_test`), **7** (`test_module`), **5** (`gen_test`), **22** (`range`), **13** (`class_str`), **17** (`class_static`), all green = **375 total**.
- Working tree: `M SPEC.md` (error reporting section); untracked `docs/class_action_plan.md`.
- **Impl fixes** (from `examples/scripts/helpers.ql` bug report; no spec change, hash unchanged): (1) triple-quoted strings (`"""..."""` and `f"""..."""`) leaked the first two opening quotes into the content — `QLangLexer::stringLiteral` **and** `fstringLiteral` consumed only one opening quote in the triple case, now consume three. (2) `isset` is now a **reserved keyword** (`TokenType::Isset` in `keywordType()`); `parsePrimary()` compiles a bare-identifier arg to its name string, so `isset(xx)` on an undeclared variable probes the existing `Builtin::IsSet` runtime check instead of `parse error: use of undeclared variable`. (3) **booleans are now proper `Bool` values**: `true`/`false` literals compile to `QLangValue::Bool(1/0)` (was `Int(1/0)`) and `isset` returns `Bool` — so `type(true)` → `"bool"`, `f"{true}"` → `"true"`, `s += true` → `"...true"` (spec: `$` matrix + `+=` table); equality/arithmetic/truthiness unchanged (`isEqual`/`isTrue` already handle `Bool`). (4) **segfault fix**: `format` builtin's `Bool` branch passed a `long long` to `snprintf` with a `%s` placeholder (`f"...{b}..."` on a `:bool`-typed var crashed in libc); now `%s`-spec bools render as `"true"`/`"false"` text, numeric specs stay 1/0. Self-check: `helpers.ql` (`qlang_run`), full suite re-run green (375).

## Round 13 (2026-10-27)

`string == object` `__str__`-equality implemented (SPEC §operator == and != new example table); harness re-run: **258/258** (`spec_check2`), **33** (`spec_check`), **20** (`spec_cast_test`), **7** (`test_module`), **5** (`gen_test`), **22** (`range`), **13** (`class_str`), **17** (`class_static`), all green = **375 total**.
- SPEC.md moved `204f0702` (40335 B / 1443 lines) → `c46efc2f` (41038 B / 1457 lines): 12-row `==`/`!=` example table + "equivalency worths both ways" note + `(byte)0x255`→`(byte)0xFF` typo fix (uncommitted). Every row except `string==object` already conformed (probed against the lib: `1==1.0` true, `"3"==3` false, `' '==32` true, `-1==(byte)0xFF` false, `1==(unsigned)1` true).
- **`string == object`:** `QLangVM::valueEqual` (member) — String vs object (table with `__str__` or `__class__`) compares against `displayString`; everything else → free `isEqual`. New member `doBinaryVm` routes EQ/NE through `valueEqual` in all 4 exec loops; other opcodes still go to the static `doBinary`. No new opcodes. `"toto" == C()` → true when `__str__` returns `"toto"`, false otherwise; `1 == C()` stays false; plain-table equality unchanged.
- 6 new `spec_check2` cases (252 → 258).
- Committed since Round 12: `a6c339a` (static spec additions + plan docs), `6a40633` (static/`__str__` implementation + Round 12 compliance). Uncommitted: `==` table + typo fix (SPEC.md), `valueEqual`/`doBinaryVm` + 6 cases (code), CMake test targets, `docs/` renames, untracked `.draft/`/`.hermes/`/`docs/class_action_plan.md`.

## Round 12 (2026-10-27)

Class `static` + `__str__` display dispatch implemented (plan: `class static implementation.md`); **no new opcodes** (reuses TABGET/TABPUT/TABCREATE/CALLCLOS). Harness re-run: **252/252** (`spec_check2`), **13** (`class_str`), **17** (`class_static`), **22** (`range`), **7** (`test_module`), all green = **369 total**.
- **`__str__` display dispatch fixed** (Part 1): `print`/`println`/`printf %s`/`sprintf %s`/f-string/`join` now route through `QLangVM::displayString` (same `__str__` call + class-name fallback as `str()`), with a `m_inDisplayString` re-entrancy guard so an `__str__` that displays `self` terminates. `table_ops::ToStr` dispatch kept honest. 13 cases in `class_str_test` (ctest `class_str`).
- **`static` keyword + `::` operator** (Part 2): `TokenType::Static` in `keywordType()` (`local static = 1` → parse error); `TokenType::ScopeRes` for `::` (lexed before single `:`, `parsePostfix` handles it → `Expr.scopeStatic`). `parseClass` accepts `static` field / `static const` / `static method`; `static`+`local`/`let` and duplicate/static-instance name collisions are parse errors. f-string format-spec splitter skips `::` (so `{Money::banner}` is not split at the scope colon).
- **Static storage + codegen** (Part 3): statics live in a class-level `__statics__` table (shared `shared_ptr` map) embedded in the prototype and copied **by reference** onto every instance. The prototype is created once at class definition into a hidden `__proto_<Name>` variable (shared by all instances + `::` access). Static methods are compiled with a hidden first parameter (the `__statics__` table) so they need no self; bare static names in method bodies resolve via `self.__statics__` (instance) or the anchor (static). `C::name` → two-hop TABGET; `C::name(args)` → TABGET closure + pass the statics table as anchor. `ClassRecord` gained `staticFields`/`staticConstFields`/`staticMethodFuncIndices`/`staticMethodArities`.
- **Instance-level static dispatch** (Part 4): `table_ops::getField`/`setField`/`callMethod` fall back to `__statics__` for names not in the instance field schema (so `money.banner` / `money.banner = "Turtle"` work and mutate shared state); `setField` rejects `static const` writes. No `ObjectOps` signature change (`callMethod` re-derives from-statics from the table).
- **Deviations from the plan:** (a) the plan's "hidden capture of `__statics__`" was replaced by a **hidden anchor parameter** for static methods — a capture by frame-register reference dangles once the prototype frame is popped (`deque::pop_back` destroys it), so the anchor (a shared `shared_ptr` passed as an arg) is lifetime-safe; (b) instance-call-of-a-static-method (`money.getBanner()`) is **not** routed to pass the anchor (the compiler's `obj.method()` path emits TABGET+CALLCLOS, not the VM `callMethod`); spec is silent and it is not in the required test set — left unsupported. See plan "Risks & Ceilings".
- 17 cases in `class_static_test` (ctest `class_static`): static field via class/instance, instance-write→class, cross-instance sharing, static method via class, arity error, f-string static (class+instance), `__str__` referencing a static, `static const` read (class+instance) + write error, `static local`/`static let`/`local static` parse errors, `C::missing` error, closed-field error intact.

## Round 11 (2026-10-27)

Spec drift audit (three waves same day): SPEC.md moved `efd53057` (38991 B / 1384 lines) → `30580ccf` (39692 B / 1426 lines) → `65df6095` (40105 B / 1437 lines) → `204f0702` (40335 B / 1443 lines); working tree dirty (uncommitted spec edits).
- Wave 1: `static` reserved word; Money example gains `static banner` + `static getBanner()`/`static setBanner()` + `Money::setBanner(...)` usage + `__str__` interpolating `{banner}`; new `## static` section.
- Wave 2: `::` row added to the operator table ("scope resolution operator"); Money example extended — static **field** read via class name (`f"{Money::banner}"`), static field read via **instance** (`f"{money.banner}"`), and instance write `money.banner = "Turtle"` mutates class-level storage (`Money::banner` then reads "Turtle").
- Wave 3: `## static` gains `static`+`const` allowed / `static`+`local`/`let` forbidden, plus an explicit note that `static`/`::` is a **new extension beyond v1** — added per user direction (v1 is implemented and stands; the `## v1 restrictions` contradiction is resolved by that note, no restriction line changed).
- Probed: `static` is **not** in `keywordType()` — tokenizes as `Ident`; no `::` token/parser path; no parser/VM support for static members or `ClassName::` calls. **Not implemented.**
- Harness re-run after each wave (no code changes): **252/252** (`spec_check2`), **33** (`spec_check`), **20** (`spec_cast_test`), **7** (`test_module`), **5** (`gen_test`), **22** (`range_check`) = **339 total**, all green.
- Open: `static` + `::` (new), duplicate-declaration warning (unchanged), `$$` (unchanged), `fetch` (unchanged).

## Round 10 (2026-10-26)

Block scope implemented (SPEC `# block scope`, committed `dde51e4`); harness re-run: **252/252** (`spec_check2`), all suites green (339 total).
- **Block scope: full conformance.** `QLangCompiler` gained `m_blockScopes` (stack of `map<name,reg>`) + `m_blockDepth`; `declareVar` pushes into the innermost block scope when depth > 0, else into `m_regs` (functions unchanged — closures/generators/mutation semantics intact). `regOf` consults the scope stack before `m_regs`; `undeclaredReg` consults it before treating a name as undeclared (plain-assignment crossing preserved). Scope pushed/popped at: `if`/`else` bodies, `for` body (loop var declared in enclosing scope per spec), `for-in` body (both range and array sub-paths — the range sub-path has an early case-exit that also pops), `while` body, and each `switch` case body + default body. 6 tests at spec_check2 L633–L640 (if isolation, nested shadow, else branch, while body, for body + loop-var visibility after loop, for-in shadow).
- No new opcodes; zero VM changes.

## Round 9 (2026-10-26)

Spec drift audit: SPEC.md moved `44e9fd2d` (38593 B / 1357 lines) → `600f11a8` (38921 B / 1382 lines), working tree dirty.
- `052b45e` (committed): `$` nil row `"false"` → `"nil"` (previously uncommitted) + compliance/handoff doc cleanup, stale docs deleted. No code changes; harness unaffected (333 total, last green run at HEAD `0c63f4b`).
- Uncommitted SPEC edits (3): duplicate-declaration error → **warning** (one example line self-contradictory); `## operator $$` "no block scope" clause removed; new `# block scope` section.
- Probed: dup-decl forms compile with no error and no warning; `local i = 3` in a block overwrites the outer `i` (`$i` → "3", spec expects "1") — **block scope not implemented**. Plain-assignment crossing works.
- Open: block scope (new section), duplicate-declaration warning (spec now asks for a warning, not an error), `$$` (unchanged), `fetch` (unchanged).

## Round 8 (2026-10-26)

Class conformance completed and the `$` operator implemented; harness re-run at HEAD `0c63f4b`: **246/246** (`spec_check2`), all suites green (333 total).
- **User-defined classes: full conformance.** Phases 3–7 landed this round: `6a4c559` (unique method/dunder names — compile error), `8c013a1` (fixed arity — constructor + method call checks), `b242645` (`__str__`), `0d7115b` (reference counting + `__destroy__` finalization). 42 class tests at spec_check2 L449–L615. Deviations from the plan documented in `class_action_plan.md` (Phase 0.2 no separate `Object` value type; Phase 9.2 `$` rejection on instances is runtime, not compile).
- **`$` dynamic-key operator implemented** (`0c63f4b`): `Dollar` token + `OP_EVALKEY` opcode; the commit fixed two bugs — EVALKEY was emitted but never dispatched (added to all 4 exec loops) and table-literal `$` keys degenerated to bare identifiers (parser now wraps the operand in a Unary node). 6 tests at spec_check2 L617–L626.
- **SPEC.md:** `$` section nil row corrected `"false"` → `"nil"` (matches implementation; **uncommitted** in working tree).
- **Re-verified gap:** duplicate-declaration compile check is still **not implemented** (`t = 5; local t = 9` compiles silently, prints `9`).
- `$$` indirect access: still **not implemented** (unchanged).

## Round 7 (2026-10-26)

Spec diff audit vs `21b1ce65` (1357 lines; `c83ebc8` + `e08005b` over `9cc0a3f`); harness re-run at HEAD `e08005b`: **204/204** (`spec_check2`), all suites green.
- `## operator $$` committed in full → tracked as **not implemented** (no `$$` token, no `OP_INDIRECT_READ`/`OP_INDIRECT_WRITE`, no declaration-state tracking, no harness cases).
- `## user defined classes` expanded → the existing **prototype is non-conforming** (closed field set, `const` fields, unique-name/arity compile checks, `__str__`, reference counting, `__destroy__`, `type(x)` all missing). Conformance baseline: `docs/EXTERNAL_TYPES.md` §9 — the `ObjectOps` dispatch layer must land first (classes are its first consumer).
- **New gap:** duplicate-declaration compile check is **not implemented** (spec examples committed in `9cc0a3f`; verified 2026-10-26 — duplicates silently compile, initializer becomes a plain assignment).
- `global c` unknown-binding example: ✓ implemented (compile error; message wording differs).
- `import`/`from` reserved words: ✓ implemented in the lexer (round 2).
- Class example `spend` fix (`+=` → `-=`): no harness case covered the example; no test change needed.

