# Plan v4: external modules (builtin name table, unified constant pool, import/export tables)

Status: **Phase 1 ✓ implemented (Round 61, 2026-11-09); Phase 2 ✓ implemented (Round 62,
2026-11-09); Phase 3a ✓ implemented (Round 63, 2026-11-09); Phase 3b gated on Q32 + Q33 + Q35,
Phase 3c gated on Q31.**
Re-audited 2026-11-08 against `HEAD = ac97651` (Round 60); Phase 1 landed on top of that
baseline (library builtin name table — `builtinCount`/`builtinName`/`findBuiltin`/`builtinId`
in `QLangBytecode.h`, `builtin_table` suite, 151 checks; spec
`docs/spec/13-bytecodes.md` `## table for build in function (qlang base)` marked
**Implemented**; compliance Round 61). Phase 2 landed on top of Phase 1 (`.qlc` **v8**,
`CompiledProgram::constants`, `Function::constants` deleted; spec
`docs/spec/13-bytecodes.md` `## unified constant table` marked **Implemented**; compliance
Round 62) — **Q34 is settled by that round: v8 = the unified pool, v9 = Phase 3's module
tables.** Phase 3a landed on top of Phase 2 (`.qlc` **v9**: `export`, the export and import
tables, the module name; spec `docs/spec/13-bytecodes.md` `## keyword export` marked
**Implemented**; compliance Round 63) — **Q29** and **Q30** were ratified in that round, both as
defaulted, and **Q31** was left open on purpose: 3a emits no new opcode, so 73/74 stay reserved
for 3c.
What remains of Phase 3 is gated on **Q32** + **Q33** + **Q35** (3b) and **Q31** (3c) and, for
automatic name-based import resolution only, on **C8** (`missing-features-plan.md`).
Supersedes: `docs/plans/archive/external-modules-plan-v3.md` (2026-11-03) and,
through it, v2 (2026-11-02) and v1 (all three under `docs/plans/archive/`). The language requirements are unchanged
since v1; what keeps changing is the **machine** — `.qlc` is at **v9** with a
single-version reader, the instruction stream is the v6 variable-width
modifier-byte format, the `i`/`c`/`jmp*` opcode spellings are retired, and the
constant pool is one deduplicated table per module.
Builds on / blocked by: `missing-features-plan.md` **C8** (module search paths +
external search callback, **Q23–Q26**) — the front end that lets `import mod`
*find* a module; this plan owns what happens once it is found.
Reference spec: `docs/spec/13-bytecodes.md` § `external modules`
(**lines 599–797** as of Round 63 — `## table for build in function (qlang base)`,
`## unified constant table` and `## keyword export` now carry **Implemented** markers and the two
table sections carry what-is/what-is-not notes; the link model, `GETIMPORT`/`CALLIMPORT` and
importer-side `readonly` are still open), `docs/spec/09-modules.md`,
`docs/spec/07-api.md` (`## module search paths`, `## external search callback`).

**v4 (2026-11-08) changes vs v3** — every one of them is a consequence of
Rounds 37–60 (escape-byte codec, `.qlc` v5 → v6 → v7, flag flow, opcode
retirement). The v3 text was correct on 2026-11-03; it is stale by three
version bumps and one encoding rewrite.

1. **Version arithmetic re-based.** v3 planned `.qlc` v3 → v4 (unified pool) →
   v5 (module tables). The tree is at **v7** (`kQlcVersion`,
   `include/qlang/QLangBytecodeIO.h:35`) and v5/v6 are *retired formats*.
   This plan now bumps **v7 → v8** (Phase 2, unified pool) and **v8 → v9**
   (Phase 3, module tables).
2. **No legacy decode branches.** The reader is **single-version** —
   `version != kQlcVersion` → `"unsupported version"` (`src/QLangBytecodeIO.cpp:470`,
   `docs/SERIALIZATION.md` §5.1 item 1). v3's "reader accepts 1..4 / 1..5" and
   its "expand + remap legacy pools" step are **deleted**: a v8 reader reads v8
   only. The checked-in legacy fixtures v3 protected (`examples/qla/test.qlc`
   v1, `cmp.qlc` v2) **no longer exist** — no `.qlc`/`.qla` artifact is checked
   in any more (verified in Round 60), so there is nothing to regenerate and
   nothing to keep.
3. **The linker cannot byte-patch operands.** Since `.qlc` v5/v6 the stream is
   a flat byte array whose operand widths are chosen per instruction by the
   escape bit + 2/2/4 modifier byte (`QLangBytecode.h:249–320`,
   `docs/SERIALIZATION.md` §4.4). v3's "offset every register operand
   (`a`/`b`/`c`)" assumed the old fixed 8-byte record. Phase 3's merge now runs
   on the compiler's **raw instruction level** (`RawInstr`/`FnEmitter`,
   `QLangCompiler.h:81–100`) or on decode → rewrite → `encodeInstruction`
   (**Q32**); jump offsets survive either way because they are *relative* and
   the merge never changes the instruction count.
4. **`MOVC` is gone; the pool-reading sites changed.** `movi`/`movc`/`addi`/
   `subi`/`cmpi`/`jmpfalse`/`jmptrue` were retired in Round 60 and the opcode
   identities compacted (`kOpcodeUsedCount = 72`, `QLangBytecode.h:247`). A
   pool read is `mov` with right-operand **kind 2** (`RightKind::ConstPool`);
   the only pool-index opcodes left are `MAKEFUNC` and `MAKEGEN`
   (`permittedKinds`: kind 2 only). Every v3 sentence naming `MOVC` is now
   "`mov` kind 2".
5. **New opcodes land at 73/74, not 68/69.** `GETIMPORT`/`CALLIMPORT` append
   after `TST` (identity 71). `DECL` — M4 of
   `encoding-v6-and-indirect-plan.md`, still pending — is *owed* identity 72
   (that plan §3/§6.1). Identity allocation is **Q31**.
6. **Constant dedup is back, and the spec now mandates it.** Remediation
   Phase 4 (F5) had dropped the dedup; commit `3d9a5ed` (2026-11-04,
   "restore constant-pool dedup") reinstated it: `Bytecode::addConstant` folds
   duplicates through `valueKey` (`src/QLangBytecode.cpp:1099–1107`) and
   `FnEmitter` carries a per-function `constKeys` map
   (`QLangCompiler.h:100–105`). `docs/spec/13-bytecodes.md:960` states pool
   entries "must have unique values. duplicated entries should be folded into a
   single one." v3's "there is no dedup machinery to preserve" is **inverted**:
   Phase 2 must *move* the dedup map to program level, and Phase 3's merged
   pool must fold cross-module duplicates (**Q33**).
7. **Builtin table grew and moved.** 22 entries now (`ToStr = 0 … Deepcopy`,
   `QLangBytecode.h:25–48`). `QLangCompiler::builtinId()` is at
   `src/QLangCompiler.cpp:4194`; call sites are **1442** and **4148** (name
   *classification*), **3320** (`Deepcopy` lowering test), **3732** (emit path).
   The app-local `builtinName()` is at `examples/qlang_run.cpp:213`, used at
   **490** and **896**.
8. **The loader half-exists.** v3 said "no production loader". `qlang_run`
   already loads a `.qlc` image and resolves its native-slot table
   (`examples/qlang_run.cpp:1305–1335`, `readQlc` at 1326). What is still
   missing is the **library** entry point (`QLangContext::loadProgram` appears
   only in a comment, `include/qlang/QLangBytecodeIO.h:28`) and a module
   registry to hold loaded modules.
9. **The disassembler is a 6-level tool now.** `--asm-level 0–5`
   (`examples/qlang_run.cpp:74–77`), and level 5 already prints a module
   import/export breakdown (`78d717c`) — Phase 3 gets its listing surface for
   free. The pool is printed per function at 855–857.
10. **Spec anchors and cleanup list refreshed.** The `external modules` section
    moved from 378–457 to **594–673**; its technical-specifications block still
    decides the same six questions. The operand map is now explicitly marked
    "**Historical** — the 8-byte struct was deleted in Round 37" and
    `usesI32()` is documented as deleted, so v3's cleanup items 1–2 shrink to
    "rename the three stale rows" and "refresh the decompiler sample".
11. **Dependencies are now tracked, not folklore.** C8 (module search paths +
    `searchCallback`) is an inventory item with open questions **Q23–Q26**
    (`missing-features-plan.md` §C8) and is listed as this plan's front end in
    `docs/plans/todo.md`, `missing-features-plan.md` and
    `docs/compliance/status.md`. `offset-table-elimination-plan.md` §9 wanted the
    same bump — **Q34 settled it in Round 62**: v8 went to this plan's Phase 2, so
    that plan takes the number after Phase 3's v9.
12. **Plan hygiene** (the gap `todo.md` §5 records): this document carries a
    `Status:` header, round tracking, an `## Open questions` section with
    defaults (so an item can start without a design round-trip), and an
    `## Implementation order` section.

---

## Background / current state (re-verified 2026-11-08, `HEAD = ac97651`)

- Compilation is still **single-source → single `CompiledProgram`**
  (`include/qlang/QLangCompiler.h:28–37`); `functions[0]` is the implicit
  `"main"` — enforced by the compiler (`compile()` names it at
  `src/QLangCompiler.cpp:1191`) and by the reader
  (`src/QLangBytecodeIO.cpp:555`). `CompiledProgram::code` is still a mirror of
  `functions[0]` (`outCode.setCode(...)`/`setConstants(...)` at
  `QLangCompiler.cpp:1202–1209`) — duplicated instructions *and* constants.
- Constant pools are still **per-function** (`Function::constants`,
  `include/qlang/QLangBytecode.h:441`), in `.qlc` (per-function
  `u32 constCount × Value`, writer `QLangBytecodeIO.cpp:417–419`, reader
  549–552), and in the VM (fetched per frame iteration into a local
  `consts`, `src/QLangVM.cpp:1994` — the `Frame` carries no pool). Dedup is
  per-function in the compiler (`FnEmitter::constKeys`) and per-`Bytecode` in
  the library (`m_constKeys`).
- Since Round 56/60 the pool holds **strings, blobs and closure constants**
  only: `Int`/`Float`/`Bool`/`nil` are immediates (modifier forms 4–11,
  `QLangBytecode.h:285–293`); 64-bit literals stay in the pool (M2 decision
  D3). Pool reads are `mov` kind 2; `MAKEFUNC`/`MAKEGEN` are kind-2-only
  (`permittedKinds`, `QLangBytecode.h:348`).
- `.qlc` = `QLC1` **v7** (`QLangBytecodeIO.h:35`; writer
  `QLangBytecodeIO.cpp:372–446`, reader 448–612). v7 = the v6 2/2/4
  modifier-byte encoding with the seven retired identities removed and the
  remaining ones compacted. The reader accepts **v7 only**, decodes every
  instruction at load (528–548), validates the native-slot table
  (unique/ascending/non-empty, 558–572), validates closure `funcIndex`
  against the function table (590–597), and rejects trailing bytes (599).
- Builtin names **were** hardcoded in **four places** (the Round-60 audit): the `Builtin`
  enum (`QLangBytecode.h:25–48`, 22 entries), `QLangCompiler::builtinId()`
  (`QLangCompiler.cpp:4194`, returns `-1` on unknown, extra `str`/`int`
  aliases; call sites 1442, 3320, 3732, 4148), the VM `callBuiltin` switch
  (`src/QLangVM.cpp:501`, dispatched from `runFrames()` at 2250, native slots
  via `-1 - i.b` at 2234), and the disassembler's app-local `builtinName()`
  (`examples/qlang_run.cpp:213`, used at 490 and 896). **Phase 1 (Round 61) collapsed the
  three name-holding places into one:** `k_builtin_names[]` + `builtinCount`/`builtinName`/
  `findBuiltin`/`builtinId` in `QLangBytecode.h`/`.cpp` (aliases included, `static_assert`-
  guarded); the compiler member and the disassembler switch are deleted and their sites call the
  library table. The VM switch remains — it dispatches on the enum, not on names.
- The dispatch switch is still **single**: `QLangVM::runFrames()`
  (`src/QLangVM.cpp:1988`), driven by `run()`, `callFunction()` and generator
  resumption. Every new opcode lands in exactly one switch.
- Top-level variables live in the context's **`GlobalTable`**
  (`include/qlang/QLangContext.h:49–54`, `globalSlots()` at 107): the main
  frame's register file aliases the table's slots directly; `compile()` starts
  the main program's non-global registers above `globalCount()`
  (`src/QLangCompiler.cpp:1166–1176`).
- "Modules" today = C++ native function sets only (`QLangContext::Module`,
  `QLangContext.h:201–209`; `math`, `src/QLangModules.cpp`). `import` has five
  forms (`src/QLangCompiler.cpp:2516–2585`) and resolves at **compile time**
  into the flat negative-id native-slot space (`slot = -1 - id`,
  `QLangCompiler.cpp:2570`, VM `2234`). `import` is a lexer keyword
  (`src/QLangLexer.cpp:58`); **`export` does not exist anywhere** (lexer,
  parser, AST, compiler). Provenance of imported names is already tracked for
  diagnostics/disassembly (`QLangCompiler::ImportedModuleFunc`,
  `QLangCompiler.h:245–252`).
- Spec: `docs/spec/09-modules.md:25` still says "v1 uses flat import only";
  `docs/spec/07-api.md:67` still says "no VM changes needed for flat import".
  Both need rewriting when Phase 3 lands.
- `docs/SERIALIZATION.md` documents v7 (single-version reader, §4.4 instruction
  stream, §5.1 invariants). `docs/compliance/README.md`: last audit Round 60,
  2026-11-08. Test suite: 29 named `add_test` entries plus the
  `tests/scripts/*.ql` script suite (`CMakeLists.txt:101–335`).

The spec's requirements still map to three phases, each independently shippable
with `ctest --test-dir build` green:

| Phase | Spec requirement (`13-bytecodes.md`) | Effort | Risk |
|-------|--------------------------------------|--------|------|
| 1 ✓ | builtin-function lookup table (616–620) — **landed Round 61** | S | Low |
| 2 | unified per-module constant table + `.qlc` v8 (622–624, 960) | M | Medium |
| 3 | import/export tables + standalone modules + `GETIMPORT`/`CALLIMPORT` + `.qlc` v9 (594–673) | L | High |

---

## Open questions

Numbering continues the global **Q#** series of `missing-features-plan.md`
(Q1–Q28); this plan owns **Q29–Q35**.

### 3a — module identity and export semantics

- **Q29 Is `export const MODULE_NAME = "…"` mandatory?** — the spec's only
  module-name mechanism is that export (`13-bytecodes.md:641`); there is no
  `module` declaration and a `.qlc` carries no name today.
  **Default:** mandatory for a *library* compile; an application/entry module
  may omit it and takes the file stem (driver-supplied) as its name.
  **Ratified as defaulted (Round 63, Phase 3a):** precedence is source > driver >
  `"main"` — `export const MODULE_NAME` (a `const` holding a constant string, so a loader reads
  the identity without running the module) wins over `QLangCompiler::setModuleName`, which
  `qlang_run` supplies as the source file stem; mandatory only in library mode
  (`setLibraryMode`).
- **Q30 May the defining module mutate its exported variables?** — the spec
  says exported entities are `readonly` and "variables can be read but not
  modified" (`:631–633`) while also requiring shared-cell write-through
  (`:658`). Read literally, a shared cell could never change.
  **Default:** the defining module may mutate; importers are readonly (a write
  through an import is a compile error). Pin the wording in `13-bytecodes.md`
  in the same round.
  **Ratified as defaulted (Round 63, Phase 3a):** `readonly` is a property of the *importer*.
  The defining module keeps writing to an exported variable (the shared cell of
  `## technical specifications` 3 is write-through — a cell nobody may write is a constant, not a
  variable), while rebinding a name exported as a `def` is a compile error on assignment and on
  `++`/`--`. The wording is pinned in `13-bytecodes.md` `## keyword export`; importer-side
  enforcement is 3b/3c work (nothing can import a `.ql` name yet).

### 3b/3c — encoding and merge mechanics

- **Q31 Which opcode identities do `GETIMPORT`/`CALLIMPORT` take?** —
  `kOpcodeUsedCount = 72` (`QLangBytecode.h:247`); `DECL` is owed identity 72
  by M4 (`encoding-v6-and-indirect-plan.md` §6.1) but has not landed.
  **Default:** M4 keeps 72; this plan appends `GETIMPORT = 73`,
  `CALLIMPORT = 74`. Whoever lands second takes the next slot — the rule is
  "append last, never renumber".
  **Unchanged by Round 63:** Phase 3a emits no opcode, so `kOpcodeUsedCount` stayed **72** and
  73/74 remain reserved (the import table is therefore always empty in a 3a image).
- **Q32 Where does the linker rewrite operands?** — the v6 stream is
  variable-width, so a merged register/pool/function index may need a *wider*
  operand and byte-patching is impossible.
  **Default:** merge at the `RawInstr` level before `FnEmitter::finish()`
  (widths are then chosen once, by the existing `computeModifier`); the
  fallback is decode → rewrite → `encodeInstruction`, which is only safe
  because jump offsets are relative and the merge preserves instruction count.
- **Q33 Does the merged pool fold cross-module duplicates?** — the spec
  requires a shared table *or* no duplicates (`:624`) and unique values
  (`:960`); per-module pools already dedup internally.
  **Default:** yes — concatenate with `valueKey` folding, since the linker
  rewrites every pool operand anyway (**Q32**). Without it a merged program can
  carry the same string twice, which `:960` forbids.
- **Q34 Which `.qlc` version does each phase take?** — **decided 2026-11-09
  (Round 62):** **v8 = the unified pool** (Phase 2 shipped on it), **v9 = module
  tables** (Phase 3). `offset-table-elimination-plan.md` §9 (option A, not yet
  signed off, `todo.md` §4) takes the next free number after v9. Recorded in
  `docs/SERIALIZATION.md` §4 ("Version allocation").
- **Q35 When does an imported module's top-level code run?** — every module is
  a global-scope script with no `main` (spec `:656`), so each has an `<init>`.
  **Default:** each module's `<init>` runs exactly once, in dependency order
  (topological over the import graph), before the entry module's `<init>`.

---

## Phase 1 — builtin name table in the library (S) — ✓ implemented (Round 61, 2026-11-09)

Goal: one library-level id↔name table; the disassembler (and any future tool)
consumes it. No format change, no behavior change, no `.qlc` bump.

1. **`include/qlang/QLangBytecode.h` / `src/QLangBytecode.cpp`** — add, backed
   by a single `k_builtin_names[]` array sized to `Builtin::Deepcopy + 1`
   (22 entries):
   - `int builtinCount();`
   - `const char *builtinName(int id);` — `nullptr` out of range.
   - `int builtinId(const std::string &name);` — **throws `QLangError`** on
     unknown: for the emit path, where an unknown name is a bug.
   - `int findBuiltin(const std::string &name);` — returns **`-1`**: for name
     *classification*, where "not a builtin" is a normal answer.
   The `str`→`ToStr`, `int`→`ToInt` aliases move here from
   `QLangCompiler::builtinId` (`src/QLangCompiler.cpp:4194`).
2. **`src/QLangCompiler.cpp`** — delete the local `builtinId()`; route
   **1442** and **4148** (classification) and **3320** (the `Deepcopy` test)
   through `findBuiltin`, and **3732** (emit) through the throwing `builtinId`.
   Do not route classification through the throwing API.
3. **`src/QLangVM.cpp`** — `callBuiltin` (501) keeps its switch: it is keyed by
   the `Builtin` enum, so the only drift risk is enum↔array order, which the
   Phase 1 tests cover exhaustively.
4. **`examples/qlang_run.cpp`** — delete the app-local `builtinName()` (213);
   use the library function at **490** and **896** (the `# builtin:<name>`
   notes).
5. **Tests** — `tests/unit/codec_check.cpp` (or a small new `builtin_table`
   suite if it keeps `codec_check` focused): every id `0..builtinCount()-1` has
   a non-empty name and the array length equals `builtinCount()`; `builtinId`
   round-trips every name incl. the two aliases and throws on unknown;
   `findBuiltin` returns `-1` on unknown; disassembly of a program calling
   several builtins prints the expected names.
6. **Docs** — `13-bytecodes.md` § `table for build in function (qlang base)`
   (616–620): record that the table is `builtinName`/`builtinId`/`findBuiltin`
   in `QLangBytecode.h` (single source of truth) and that it is a *library*
   table keyed by compiler version — not serialized in `.qlc` (the
   compatibility rule at `:673` makes that legitimate). Status marker +
   compliance hash **after** the green run (AGENTS.md).

Out of scope: serializing the table into `.qlc` (only needed if third-party
tools must decode `.qlc` without the library).

## Phase 2 — unified per-module constant table + `.qlc` v8 (M)

Goal: one constant pool per module (program), replacing per-function pools.
Prerequisite for Phase 3's link-time merge. Behavior-invariant refactor.

1. **`include/qlang/QLangBytecode.h`** — move the pool from `Function`
   (`:441`) to `CompiledProgram` (`QLangCompiler.h:28`): `std::vector<QLangValue>
   constants;`, `Function::constants` removed. `Bytecode::addConstant` /
   `setConstants` (`src/QLangBytecode.cpp:1099–1116`) become the single pool
   manager — **and they already carry the dedup map** (`m_constKeys`,
   `valueKey`), which is what the spec's uniqueness rule (`:960`) needs.
   `FnEmitter::constKeys` (`QLangCompiler.h:100–105`) moves with it: the dedup
   index must span functions, so a string used in two `def`s folds to one slot.
2. **`src/QLangCompiler.cpp`** — `buildFunction()` stops swapping the pool in
   and out (today `m_emitter = FnEmitter()` at **1390**/**2625**, restored at
   **1539**/**2905**): all `addConst`/`emitConst` calls hit the shared pool
   across the save/restore. `compile()` drops the `CompiledProgram::code`
   mirror of `functions[0]` (`1202–1209`) — with one pool the mirror has no
   reason to exist (check consumers first: `Bytecode` is also the writer's
   view of main).
3. **`src/QLangVM.cpp`** — the single `runFrames()` loop fetches
   `fn.constants` into a local `consts` (**1994**); point that fetch at the
   program pool. The pool-reading sites are `mov` **kind 2** and
   `MAKEFUNC`/`MAKEGEN` operand `b`. Closure values store `funcIndex` + capture
   regs, not pool indices — unaffected.
4. **`src/QLangBytecodeIO.cpp`** — `.qlc` **v8** (`kQlcVersion` 7 → 8):
   - Writer: function records lose `u32 constCount × Value` (417–419); one
     module-level pool section is emitted after the function records (before
     the native-slot table). The non-durable pre-check (374–380) walks the
     single pool.
   - Reader: **v8 only** — no legacy branch, no remap (the single-version rule,
     §5.1 item 1). Pool indices are validated against the pool length at load,
     next to the existing closure-`funcIndex` validation (590–597).
   - `tests/unit/codec_check.cpp`: the version assertions move 7 → 8
     (**397–406**, **1581–1582**) and the single-version rejection cases
     (**1591–1602**) gain v7 (one below) and v9 (one above).
5. **`examples/qlang_run.cpp`** — print one flat `constants:` list per program
   instead of per function (855–857); the pool note at **885–886** reads the
   program pool. This makes the spec's own decompiler sample (`:141–155`) true
   — and that sample must also be refreshed to v7/v8 spelling (`movc` is gone).
6. **Tests** — `spec_check2`: identical **runtime** output (behavior-invariant);
   disassembly *text* changes (flat pool, indices shift) — update any
   disassembly-text assertions. A program where two `def`s use the same string
   literal now compiles to **one** pool entry (assert it — this is the dedup
   requirement `:960` + the shared-table requirement `:624`). `codec`: v8
   round-trip incl. pool equality; a pool index out of range is rejected;
   reserved/legacy versions rejected.
7. **Docs** — `docs/SERIALIZATION.md` §4 rewritten for v8 (it documents v7);
   `13-bytecodes.md` § `unified constant table` (622–624) gets the
   "Implemented (Round N)" marker; compliance hashes after the green run.

> **DONE (Round 62, 2026-11-09).** All seven steps landed in one round.
> 1. `ConstantPool` (`include/qlang/QLangBytecode.h` / `src/QLangBytecode.cpp`) — the values plus
>    their `valueKey` dedup index behind `add()` / `take()`; `Function::constants` deleted,
>    `CompiledProgram::constants` is the home. Deviation from the step as written: the pool manager
>    is a new `ConstantPool` type rather than `Bytecode::addConstant`/`setConstants` — `Bytecode`
>    is the writer's view of *one* function and has no business owning a program-level table, so
>    it shrank to the `name()` mnemonic instead.
> 2. `QLangCompiler` owns one pool (`m_pool`); the emitter's `consts`/`constKeys`/`addConst()` and
>    the save/restore swap are gone, so dedup spans function boundaries; `compile()` moves the pool
>    into the program. `CompiledProgram::code` deleted (no consumers left once the writer reads
>    `functions[0]` through `Bytecode`).
> 3. `runFrames()` reads `m_prog.constants` (one line).
> 4. `kQlcVersion` 7 → 8; the pool section moved after the function records, before the native-slot
>    table — for a single-function file the bytes are identical to v7, so the hand-built fixtures
>    needed no byte edits. Reader stays single-version (v1–v7 rejected, no remap). New invariant:
>    const-pool operands (`mov` kind 2, `makefunc`, `makegen`, **and `emit`**, whose `b` is a pool
>    index although its kind stays register) are collected during decode and range-checked once the
>    pool is read → `constant pool index out of range`; the closure-`funcIndex` scan walks the
>    program pool.
> 5. `qlang_run` prints one flat `constants:` list above `function #0`; `hello.cpp` likewise.
> 6. `codec_check` 330 → **337** (version matrix 3/4/5/6/7/9 rejected, v8 round-trip with
>    program-pool equality, cross-`def` dedup, the pool-index accept/reject pair); `flag_flow`
>    **1324** unchanged (its disassembly harness merges the pools it used to emit, renumbering
>    operands through `decodeInstruction`/`encodeInstruction`); `phase0_api` F1 reads the program
>    pool. Corpus: 23/23 `tests/scripts/*.ql` stdout byte-identical against a `HEAD` worktree build.
> 7. `docs/SERIALIZATION.md` §4 → v8 (new §4.2, NativeSlot/VarEntry/instruction stream renumber to
>    §4.3/§4.4/§4.5, §5.1 + §9 + §10), `13-bytecodes.md` `## unified constant table` → Implemented
>    + the decompiler sample refreshed to real v8 output, compliance Round 62 (counted unit cases
>    37 711 → **37 718**, ctest **51**).
> `cmake --build build` clean, `ctest --test-dir build` **51/51 green**.

Risks: behavior-invariant on its face, but it touches compiler, VM, codec and
disassembler. Keep it a standalone round so a regression is easy to bisect.
**Landed as a standalone round as planned** (Round 62, no other feature on top of it).

## Phase 3 — import/export tables, standalone modules, `GETIMPORT` + `CALLIMPORT` (L)

The spec's decisions (§ Spec decisions) fix the design: **this version ships
link-time merge** — loading resolves imports against loaded modules and
produces one flat `CompiledProgram` (renumbered functions, disjoint register
ranges, merged pool, patched closure constants), preserving the VM's
single-`functions`-vector invariant. The spec also requires a **runtime module
registry** with both models "sharing a reusable solution" (`:655`), so the
table types, registry data and name resolution are built as standalone units;
the registry itself is deferred. With `GETIMPORT`/`CALLIMPORT` in *module*
bytecode, the shared solution covers the encoding too — a registry model can
execute module bytecode unchanged.

Consequences of the spec's decisions:

- **No `main` in modules** (`:656`): every module is a global-scope script; its
  top level compiles to the module's `<init>`. The `functions[0] == "main"`
  check (`QLangBytecodeIO.cpp:555`) applies only to v7-and-earlier files; in
  v9 the entry module is chosen by the driver.
- **Imports are readonly** (`:631–633`): only `GETIMPORT` exists — no
  `SETIMPORT`; a write through an import is a compile error.
- **Shared cells** (`:658`): `GETIMPORT` reads the defining module's live
  global cell; mutation happens only in the defining module (**Q30**).
- **Module name** via `export const MODULE_NAME` (`:641`), **Q29**.
- **Cross-module references go through the import table in module bytecode**
  (`:608`, `:614` "bytecode to reference the look up table"): the compiler
  emits `GETIMPORT`/`CALLIMPORT` with an import-table index; the merge linker
  rewrites `CALLIMPORT` → `CALLFN`; a registry model executes both directly.

### 3a — module identity + `export` construct + tables (no VM change) — ✓ implemented (Round 63, 2026-11-09)

1. **Module name** — per **Q29**; no `module` declaration in this version.
2. **Lexer/parser** — `export` keyword (today only `import` exists,
   `src/QLangLexer.cpp:58`), both spec forms (`:636–651`): prefixed
   (`export def f()`, `export const k = …`) and standalone (`export NAME;`).
   AST: export flags on the existing top-level `Def`/`Var`/`Const` statements.
3. **Export semantics** —
   - Defining-side protection through the existing `constVars`/`guardNotConst`
     path (`QLangCompiler.h:401`): an exported **function** name is
     un-rebindable everywhere; an exported **variable** is mutable in the
     defining module and immutable in importers (**Q30**).
   - `export NAME;` resolves against global-scope declarations in a post-parse
     pass; undefined name = error; forward references allowed; a second export
     of the same name (any form) = "already exported" (the spec's own example,
     `:648–649`).
   - `export` in the entry module is allowed and a no-op.
4. **Compiler** — `CompiledProgram` gains `std::string moduleName;`,
   `std::vector<ExportEntry> exports;` (`{kind: Func|Var|Const, name, ref}`)
   and `std::vector<ImportEntry> imports;` (`{module, name, kind, ref, alias?}`)
   where `ref` is resolved at **compile time** against the defining module's
   layout and remapped by the linker. Every module's top level compiles as its
   `<init>`, recorded in the module header. The compiler API is still a single
   option-less `compile()` (`QLangCompiler.h:254`) — library mode is a new flag
   or entry point.
5. **`.qlc` v8 → v9** — module name string, export-table section, import-table
   section (entries carry compile-time-resolved refs). The reader stays
   single-version; the `main` check becomes version/role-conditional.

> **DONE (Round 63, 2026-11-09).** All five steps landed in one round — the **write side only**:
> no instruction reads either table yet, and no module is resolved against another.
> 1. **Module name** — `CompiledProgram::moduleName`, precedence source
>    (`export const MODULE_NAME`) → driver (`QLangCompiler::setModuleName`; `qlang_run` passes the
>    source file stem) → `"main"`, mandatory only in library mode (**Q29**, ratified as defaulted).
> 2. **Lexer/parser** — the `export` keyword; `Stmt::isExported` carries the prefix form (`def`,
>    `const`, variable assignment, arrow closure) and a new `StmtKind::ExportRef` the standalone
>    form. Global scope is enforced **twice**: the parser's body-depth guard rejects the keyword
>    inside a `def`, an arrow closure or a `class` body, and the compiler's pre-pass rejects it
>    inside a global-scope `if`/`for`/`while`/`switch` block, where it would otherwise compile and
>    be silently dropped from the table.
> 3. **Export semantics** — `QLangCompiler::collectExports()` runs **before** `compile()` and
>    `finishExports()` resolves each entry to its function index or register afterwards.
>    Deviation from the step as written, twice: the pass is *pre-compile* rather than *post-parse*
>    (the table must exist before any body compiles, so `already exported` and the rebind guard are
>    live while the top level is emitted, and a standalone export resolves forward), and the
>    defining-side protection is **not** the `constVars`/`guardNotConst` path — an exported
>    *variable* must stay writable in its own module (**Q30**), so a name exported as a `def` rides
>    its own guard (`guardNotExportedFunc`, on assignment and on `++`/`--`). Duplicate export →
>    `'<name>' is already exported`.
> 4. **Compiler** — `CompiledProgram` gains `moduleName`, `exports` (`ExportEntry {kind, name,
>    ref}`; `ExportKind` = `Var` 0 / `Const` 1 / `Func` 2) and `imports` (`ImportEntry {module,
>    name, kind, ref, alias}`); `setModuleName` / `setLibraryMode` are the new options, and library
>    mode compiles the top level as `<init>` instead of `main`. The import table is **always empty**
>    this round (**Q31** untouched) — `import` still binds C++ modules through `CALL`'s
>    negative-slot space.
> 5. **`.qlc` v8 → v9** — the three sections append **after `registerCount`**
>    (`docs/SERIALIZATION.md` §4.6), so every v8 byte keeps its position: `hello.ql`'s image is
>    3 792 B → 3 809 B, byte-identical through `registerCount` plus a 17-byte tail. The reader
>    stays single-version (v1–v8 rejected) and validates the tail as it reads it (non-empty module
>    name, `kind <= Func`, non-empty entry name, non-negative `ref`, a `Func` ref inside
>    `[0, funcCount)` and a `Var`/`Const` ref inside `[0, registerCount)`, no duplicate export name,
>    both counts under the §8 cap); the `functions[0].name == "main"` check becomes `"main"` **or**
>    `"<init>"`.
> `qlang_run` names the entry module after its source file, and level 5 prints `module:` + the
> export table (`kind` → function index or register, `readonly` / `shared cell`), with the old
> "exports (main)" list renamed `unexported defs`.
> `cmake --build build` clean, `ctest --test-dir build` **52/52 green**: new suite `export`
> (`tests/unit/export_test.cpp`, **88** checks), `codec_check` 337 → **352**, corpus 23/23 stdout
> byte-identical against a `HEAD` build. Spec `## keyword export` → **Implemented** with the
> Q29/Q30 reading; compliance Round 63.

### 3b — loader + linker

1. **`QLangContext::loadProgram(...)`** — the library entry point the app
   already half-implements (`examples/qlang_run.cpp:1305–1335`): read a `.qlc`,
   resolve its native slots against the context, store name + export table in a
   **module registry** on the context (the reusable unit the future registry
   model builds on).
2. **Import resolution at compile time** — the driver loads library `.qlc`s
   before compiling; the existing five-form resolver
   (`src/QLangCompiler.cpp:2516–2585`, today C++-module-only) gains a second
   lookup source, loaded `.ql` export tables:
   - `from mod import *` = all entries; `import mod.f` / `from mod import f` =
     one; aliases per spec (`:671`).
   - A name in **both** a C++ module and a `.ql` module = compile error.
   - C++ natives (`math`) stay compile-time natives in the negative-slot space;
     the import table carries only `.ql`→`.ql` entities.
   - All errors raised here, not at link time: unknown module, unknown export,
     duplicate export, `export` outside global scope, write through an import,
     rebind of an exported function name, import-cycle detection (`:669`).
   - **Finding** a module by name (`import mod` with no explicit load) is
     **C8**'s job (search paths + callback, **Q23–Q26**); until C8 lands the
     driver must load libraries explicitly — which is exactly what
     `qlang_run` already does for its single input.
3. **Linker** (new `QLangLinker` unit) — purely mechanical; if compilation
   succeeded, linking cannot fail:
   - **Functions**: concatenate; each non-entry module gets a register offset =
     cumulative `registerCount` of earlier modules (disjoint ranges).
   - **Operand rewriting per Q32** — on `RawInstr` before `finish()` (or decode
     → rewrite → `encodeInstruction`). Never on encoded bytes: operand widths
     are per-instruction (v6 modifier byte).
   - **Pool**: one merged pool, folded with `valueKey` (**Q33**); rewrite `mov`
     kind-2 operands and `MAKEFUNC`/`MAKEGEN` indices, plus closure
     `funcIndex` values in pool constants.
   - **Calls**: offset `CALLFN`/`MAKEFUNC`/`MAKEGEN` function indices;
     `CALLIMPORT` → `CALLFN` with the merged index.
   - **Native slots**: concatenate, deduping identical names, preserving the
     table invariants the reader validates (`QLangBytecodeIO.cpp:558–572`).
   - **Entry point**: the entry module's `<init>` becomes `functions[0]`; the
     others run in dependency order before it (**Q35**).
   - **`vars`/`constVars`**: the entry module's maps are kept; other modules'
     name→register maps are dropped (per-module debug maps optional).
     Cross-module name collisions are legal — disjoint register ranges.
   - **Global cells**: the main frame aliases `GlobalTable::slots`
     (`QLangContext.h:49–54`, `globalSlots()`); for a merged program the table
     must span the **full merged register range** so `GETIMPORT` reaches the
     defining module's live cell. Preferred: one flat table over the merged
     span, which leaves the aliasing mechanism unchanged.
4. **Driver** — `qlang_run app.ql [lib1.qlc …]` (or `--import`); the
   single-file invocation keeps today's behavior. Level-5 listing
   (`78d717c`) gains the module tables.
5. **Tests** — `codec`: v9 round-trip incl. tables; loader: load a library,
   compile + link an app against it, run; unknown export / cycle / version
   mismatch each produce a clean `QLangError`. `spec_check2`: end-to-end
   cross-module call and cross-module constant read.

### 3c — `GETIMPORT` + `CALLIMPORT`

1. **Opcodes** — appended after `TST` (identity 71) per **Q31**:
   - `GETIMPORT` — `r[a] = <import table[b]>` (imported global var/const; reads
     the defining module's live cell).
   - `CALLIMPORT` — `r[a] = call import[b](r[a+1 .. a+c])` (same operand shape
     as `CALLFN`: dest `a`, import index `b`, arg count `c`).
   Both are register-indexed; under the v6 layout they are ordinary
   register-class opcodes (`permittedKinds` = kind 1, `CALLIMPORT`'s `b` being
   an import-table index like `CALLFN`'s function index). In the **merged**
   program `CALLIMPORT` does not survive (the linker rewrites it); `GETIMPORT`
   does. In **module** bytecode both are the cross-module mechanism a future
   registry executes directly.
2. **VM** — the merged global-cell area spans the full merged register range
   (3b); both opcodes land in the single `runFrames()` loop.
3. **Compiler** — an imported name lowers to `GETIMPORT` (var/const) or
   `CALLIMPORT` (function) instead of a local register; a write to an imported
   name is a compile error; imported consts may fold to an immediate or a pool
   `mov` at link time.
4. **Tests** — a write in the defining module is visible to the importer via
   `GETIMPORT`; write through an import = compile error; rebind of an exported
   function name = compile error; a closure created in module A capturing a
   global works from module B; a merged program contains no `CALLIMPORT`.
5. **Docs** — finalize `13-bytecodes.md` (operand rows for both opcodes, status
   markers), rewrite `09-modules.md` (the "internal modules" framing is
   superseded; C++ modules become a module *kind*), update the `07-api.md:67`
   "no VM changes needed for flat import" quote, `SERIALIZATION.md` v9,
   compliance hashes after the green run.

---

## Spec cleanup (do in the round each phase lands)

`docs/spec/13-bytecodes.md` § external modules (594–673) and its tables:

1. **Operand map rows** — the map is now explicitly "Historical" (lines 9–17),
   but still spells `CONST` (line 60), `DELETE` (line 69) and `IN` (inside the
   "ADD … IN (17 binary ops)" row, line 63) → `MOV` (kind 2) / `DELETEx` /
   `INx`. The rows appended since (NOP 61, EMIT 62, DEEPCOPY, INDEXGET/PUT,
   EQF…GTEF, TST) are current, so this is a three-name fix, not a rewrite.
2. **Decompiler sample** (141–155) — shows a flat pool (Phase 2 makes that
   true) *and* `movc` (retired in v7). Refresh both.
3. **Terminology** — line 600 "example of external modules are `ql` files"
   conflates source and artifact: a compiled module is a `.qlc` with a module
   name + tables. Cross-reference the compatibility rule at `:673`, don't
   redefine it.
4. **Status markers** — the section has none; add `PROPOSAL` /
   `Implemented (Round N)` per subsection as phases land.
5. **Superseded quotes elsewhere** — `09-modules.md:25` ("v1 uses flat import
   only") and `07-api.md:67` ("no VM changes needed for flat import").

(v3's items "operand map uses pre-rename spellings" and "`usesI32` list is
stale" were largely absorbed by the v6/v7 spec rewrites: the map is marked
historical and `usesI32()` is documented as deleted — only the three row names
survive as work.)

## Spec decisions (resolved in `13-bytecodes.md`:653–673)

1. **Link model** — both link-time merge and a runtime module registry,
   sharing a reusable solution (`:655`). This plan ships merge (`.qlc` v9); the
   table types, registry data and resolution are standalone reusable units; the
   registry is deferred.
2. **Entry point** — global-scope script only, no `main`, single-file modules
   in this version (`:656`).
3. **Cross-module variables** — shared cell, write-through (`:658`); combined
   with the readonly export rules this means: defining module writes, importers
   read (**Q30**).
4. **Export syntax** — `export` keyword, global scope only, prefixed and
   standalone forms, readonly entities, duplicate export is an error, entities
   shared by pointer/reference (`:626–651`).
5. **Import graph rules** — C++ API imports `.ql` modules into a context;
   `.ql` may import C++ modules and other `.ql` modules; C++ modules cannot
   load `.ql`; cycle check; aliases required (`:662–671`).
6. **Compatibility rule** — compatible bytecode is generated by the exact same
   compiler version (`:673`). This is what licenses the single-version reader
   and the library-side builtin table.

Plan-level decisions made in this draft (ratify in the 3a round):

7. **`CALLIMPORT` encoding** — module bytecode references the import table for
   cross-module calls; the merge linker rewrites it to `CALLFN`; a registry
   model executes it directly. A *merged* program is merge-model-specific; the
   *module* `.qlc` is model-agnostic.
8. **Register-range renumbering** — disjoint ranges per module; merged
   `vars`/`constVars` reflect the entry module; cross-module name collisions
   are legal.
9. **Import resolution timing** — compile time, against loaded export
   metadata; the linker is mechanical and cannot fail on a compiled program.
10. **Merge happens pre-encoding** — on `RawInstr`, never on encoded bytes
     (**Q32**).

---

## Implementation order

Each step is one reviewable round; every round ends with
`cmake -B build && cmake --build build && ctest --test-dir build` green, the
corpus (`tests/scripts/*.ql`) byte-identical where the change is supposed to be
behavior-invariant, and compliance/spec hashes re-recorded **after** the green
run (AGENTS.md).

**Ready now — no open questions block these:**

1. **Phase 1** (builtin table) — steps 1–6 above. Smallest, zero format risk;
   land it first so Phase 2's disassembler work builds on it. **✓ done (Round 61,
   2026-11-09):** the table and the four APIs are in `QLangBytecode.h`/`.cpp`, the compiler's
   local `builtinId` is gone (all four sites classify through `findBuiltin`), the disassembler's
   hardcoded switch is gone, and `builtin_table` (151 checks) is the 51st ctest entry. Deviation
   from step 1 as written: the emit path (`compileExpr`, `CALL` operand `b`) also classifies with
   `findBuiltin` rather than resolving with the throwing `builtinId` — a non-builtin callee is a
   normal answer there (native slot / `unknown function`), so `builtinId` has no in-tree caller
   yet and is pinned by the suite instead.
2. **Phase 2** (unified pool, `.qlc` v8) — steps 1–7. **✓ done (Round 62,
   2026-11-09):** `ConstantPool` lives in `CompiledProgram::constants` and is
   owned by `QLangCompiler`, so dedup spans `def`s; `Function::constants`,
   `FnEmitter::consts`/`constKeys`/`addConst()` and the `CompiledProgram::code`
   mirror are deleted; `.qlc` is v8 with the pool as a module-level section and a
   load-time const-pool index check; `codec_check` 330 → 337, corpus byte-identical,
   ctest 51/51. **Q34** was decided first, as required (v8 here, v9 for Phase 3).
   Deviation: the pool manager is the new `ConstantPool` type, not
   `Bytecode::addConstant`/`setConstants` — see the Phase 2 **DONE** note.

**Gated:** (3a was the last item blocked on **Q29**/**Q30** — it landed in Round 63 and is listed
here only to keep the numbering of the phases stable)

3. **Phase 3a** (export syntax + tables, `.qlc` v9 write side) — **✓ done (Round 63,
   2026-11-09):** `export` in both spec forms (global scope only, enforced in the parser and again
   in the compiler's pre-pass), `CompiledProgram::moduleName` / `exports` / `imports`,
   `setModuleName` / `setLibraryMode` (`<init>` top level), and `.qlc` **v9** with the §4.6 tail
   validated at load. **Q29** and **Q30** ratified as defaulted in the round; **Q31** deliberately
   left open — nothing was appended to the enum, so `kOpcodeUsedCount` is still 72 and the import
   table is always empty. New suite `export` (88 checks), `codec_check` 337 → 352, corpus
   byte-identical, ctest 52/52. Deviations recorded in the 3a **DONE** note.
4. **Phase 3b** (loader + linker) — needs **Q32**, **Q33**, **Q35**; needs the
   driver to load libraries explicitly (automatic name resolution waits for
   **C8**/**Q23–Q26**).
5. **Phase 3c** (`GETIMPORT`/`CALLIMPORT` + docs rewrite) — needs **Q31**;
   lands with the `09-modules.md` / `07-api.md` rewrites.

Cross-plan coordination: `DECL` (M4) and this plan's two opcodes both append to
the enum (**Q31** — still open). The version counter (**Q34**) is settled and
recorded in `docs/SERIALIZATION.md` §4 "Version allocation": v8 taken by Phase 2,
v9 reserved for Phase 3, the offset-table plan takes the next free number.

## Verification (per phase, per AGENTS.md)

- `cmake -B build && cmake --build build`
- `ctest --test-dir build` (all suites; `codec`, `encoding_v6`, `flag_flow`,
  `spec_check2`, `test_module`, the `script_*` suite)
- `build/qlang_run --asm-level 5 --profile tests/scripts/hello.ql` and a
  compile → `.qlc` → load → run round-trip (the `codec_check` `qlc-exec` gate)
- Compliance SHA256 hashes + tallies in `docs/spec/README.md` /
  `docs/compliance/*` updated **only after** the green run
  (`./update-doc-hashes.sh`).

## Explicitly out of scope

- Runtime module registry — the spec requires it eventually (`:655`); Phase 3
  builds the reusable units (tables, registry data, resolution) **and** the
  table-dispatching opcodes a registry model would execute directly; the
  registry itself is deferred.
- Module search paths / `searchCallback` — **C8** (`missing-features-plan.md`,
  Q23–Q26). This plan consumes its resolver; it does not build it.
- `$$` indirect access (M4 of `encoding-v6-and-indirect-plan.md`) — shares the
  opcode tail and the version counter with this plan (**Q31**, **Q34**) but is
  an independent feature.
- Decompiler mnemonics (`r`/`c` prefixes) — spec says DO NOT IMPLEMENT YET.
- Separating line/col from `Instruction` — already done (the `loc` side-table);
  further compression is the offset-table plan's decision, not this one.
- Inheritance — deferred to v2 (`missing-features-plan.md` C5).
- Serializing the builtin name table into `.qlc` — Phase 1 keeps it
  library-side (legitimate under the compatibility rule, `:673`).
- Qualified access syntax (`math.min(...)`) — stays a future extension.
- Multi-file modules — the spec limits this version to single-file modules
  (`:656`).
