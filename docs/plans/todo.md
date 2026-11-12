# QLang — TODO / backlog

Status: current 2026-11-08 (`HEAD = be2ea56`; `cmake --build build` clean, `ctest --test-dir build`
**50/50** green including `doc_hashes`).
Maintained alongside `docs/plans/`; per AGENTS.md a plan that is fully implemented moves to
`docs/plans/archive/`. Sources: the 2026-11-08 plan audit,
[encoding-v6-and-indirect-plan.md](encoding-v6-and-indirect-plan.md),
[missing-features-plan.md](missing-features-plan.md),
[offset-table-elimination-plan.md](offset-table-elimination-plan.md),
[portable-module-follow-on-plan.md](archive/portable-module-follow-on-plan.md) (2026-11-11,
the two portable-module gaps left open when `portable-module-plan.md` P1–P5 closed at
Round 79; both phases closed at Rounds 80 and 81, so it is archived),
[native-slot-portability-plan.md](native-slot-portability-plan.md) (2026-11-12, the
`.qlc` native-slot table naming natives no code calls — found by the Round 81 demo;
Phase 1 + the doc corrections ✓ delivered at Round 82, Phase 2 left as optional
cleanup).

## 1. Closed 2026-11-08 / 2026-11-09 (recent sessions)

- [x] **Phase 3 S7** — the docs commit landed as `be2ea56` "docs: opcode retirement shipped
      (Round 60)"; S1–S4 were `b86460b` / `801498c` / `a6737b9`
- [x] `phase3-opcode-retirement-plan.md` moved to `docs/plans/archive/`, together with the other
      three shipped encoding plans (`m2-pipeline-switch-plan.md` Round 56,
      `m3-flag-flow-plan.md` Round 59, `instruction-format-escape-byte-plan.md` Round 37);
      their status headers were corrected to **DONE** and every live link
      (`docs/spec/13-bytecodes.md`, `docs/compliance/{status,tests}.md`,
      `encoding-v6-and-indirect-plan.md`) re-pointed at `plans/archive/`.
      `docs/compliance/history.md` was **not** rewritten (append-only record).
- [x] `docs/plans/notes.txt` deleted (`f192ad0`)
- [x] **`external-modules-plan-v4.md` moved to `docs/plans/archive/` (2026-11-09)** — complete as
      of Round 65 (Phases 1–3c, `ctest` **53/53** at `8a25945`), so AGENTS.md's housekeeping rule
      applies; its status header was corrected to **DONE 2026-11-09 — archived** and every live path
      re-pointed at `plans/archive/` (`docs/README.md`, `plans/todo.md`,
      `plans/missing-features-plan.md`, `docs/SERIALIZATION.md`,
      `docs/spec/{13-bytecodes,09-modules}.md`, `docs/compliance/{status,tests,harness}.md`,
      `CMakeLists.txt`, `include/qlang/QLangBytecode.h`, the three `tests/unit` headers).
      `docs/compliance/history.md` was **not** rewritten (append-only record).

## 2. M4 — `$$` indirect access (A3) — next milestone

Design is settled by parent plan §7 (**Q6 closed**: the declaration bit is not encoded). The
implementation plan now exists: [m4-indirect-plan.md](m4-indirect-plan.md) — written 2026-11-09,
**not started**; the rows below are its summary, S1–S8 there are authoritative.

- [x] **`docs/plans/m4-indirect-plan.md` written (2026-11-09)** — closes **Q7–Q11** (each as a
      recorded default), spells out the six-stage check pipeline and the test plan, and follows the
      milestone-plan structure of `archive/m3-flag-flow-plan.md`
- [ ] `DECL siteId` opcode — identity **74** (external-modules Phase 3c took **72/73** in Round 65,
      its **Q31**, "append last, never renumber"; the parent plan's "79 → 80" and this row's earlier
      "72" both predate that), `kOpcodeUsedCount` 74 → 75
- [ ] Per-function declaration table (`site → {reg, name, kind}`) as an optional `.qlc`
      section (M2 D4; `.qlc` is **v9** since Phase 3a — decide whether the table rides v9 or
      forces v10; **Q34**'s allocation note in `docs/SERIALIZATION.md` §4 is the place that is
      recorded) + frame name map + per-frame bitset
- [ ] Two-operand `INDGET`/`INDPUT` (reuse the placeholders, `QLangBytecode.h:123-124`;
      **Q10** decides the arity)
- [ ] Six-stage `indirect variable error:` pipeline, resolution order (locals + `global`
      reads, locals-only writes), const/function rejection, branch-dependent declaration
- [ ] `$$"literal"` fast path — whether the literal syntax ships at all is **Q11**
- [ ] Parent §11 item 8 (`$$` verification) + `05-operators.md` `## operator $$` compliance
      entry; **Q28** spec-wording edit (dup-decl "compile warning" → strict-typing level table)

## 3. Phase 3 — opcode retirement: closed (Round 60), tail still open

Plan: [archive/phase3-opcode-retirement-plan.md](archive/phase3-opcode-retirement-plan.md) —
**DONE 2026-11-08**, S1–S7. D2's prerequisite (M4 first) did not hold, so the plan's recorded
**fallback** executed: `kOpcodeUsedCount` 79 → **72** (not 80 → 73), `TST` 71 last defined,
`DECL` will append at 72.

- [ ] **D7 tail (deferred, test text only)**: rename `spec_check2`'s `movi:`/`addi:`/`subi:`
      case names, fix the `value_semantics` "MOVC clone" comments and `QLangVM.h`'s
      `CMP/CMPI` + `mov/movc` comments (Round 60 recorded this as its only open item)

## 4. Offset table (exploration → decision)

- [ ] Sign off §9 of `offset-table-elimination-plan.md` (option **A** + §7.4 `loc`
      delta-encoding; perf_stress image 6 719 B → ~3 600 B, −46 %) and record the decision
- [ ] If approved: bump `kQlcVersion` **7 → 8** — Phase 3 already spent the 6 → 7 bump
      (`a6737b9`), so the "one bump or two" question in the Phase 3 plan's follow-ups is
      closed in favour of two; update that plan's §8 bullet when the §9 decision lands

## 5. missing-features backlog (`missing-features-plan.md`)

Detailed plans owed — the plan's convention is one per item before work starts, and each opens by
closing the **Q#**s its item points at:

- [ ] **C1 strict typing** levels 0/1/2 — absorbs **B1** (warning emission path) and **B2**
      (duplicate-declaration diagnostic); finalize `16-strict_typing.md` (the `QLangContext`
      API), settle **Q12–Q18, Q27–Q28**; detailed plan needed
- [ ] **C5** inheritance (`ObjectOps::base` returns null) — spec decision first (**Q21**); plan
- [ ] **C7** enum-name string form — design decision first (**Q22**); plan
- [x] **C8** module search paths + `searchCallback` — **Q23–Q26**; ✓ **C8a** Round 67, **C8b**
      Round 69 (2026-11-10); front end for `archive/external-modules-plan-v4.md`. What
      `export`/`import` still cannot do was planned in
      [archive/module-export-import-plan.md](archive/module-export-import-plan.md) (all phases
      landed Rounds 70–73, archived)
- [x] **`external-modules-plan-v3.md` re-audited → `docs/plans/archive/external-modules-plan-v4.md`**
      (2026-11-08): v4 adds the `Status:` header, the **Q29–Q35** open-question set and an
      implementation order, and re-bases the machine on the tree — `.qlc` **v7 → v8** (unified
      pool) → **v8 → v9** (module tables); no byte-level patching of an encoded body (v6 operands
      are variable-width, so the merge happens on `RawInstr` before `finish()`); `mov` kind 2
      replaces `MOVC`; the two new opcodes append at **73/74** because M4 owns identity 72; pool
      dedup is back and spec-mandated. v3 moved to `docs/plans/archive/`. State at that audit:
      nothing implemented — Phase 1 + Phase 2 ready to start, Phase 3 gated on Q29–Q35 and (for
      name-based resolution only) C8
- [x] **`archive/external-modules-plan-v4.md` Phase 1 (builtin name table) ✓ implemented (Round 61,
      2026-11-09)**: `k_builtin_names[]` + `builtinCount`/`builtinName`/`findBuiltin`/`builtinId`
      in `QLangBytecode.h`/`.cpp` (22 ids, `static_assert`-guarded, `str`/`int` aliases),
      `QLangCompiler::builtinId()` and the disassembler's hardcoded `builtinName()` switch
      deleted, `builtin_table` suite (151 checks) added; no format change, no `.qlc` bump.
      `13-bytecodes.md` `## table for build in function (qlang base)` marked **Implemented**.
- [x] **`archive/external-modules-plan-v4.md` Phase 2 (unified constant pool) ✓ implemented (Round 62,
      2026-11-09)**: `ConstantPool` (values + `valueKey` dedup index) lives in
      `CompiledProgram::constants` and is owned by `QLangCompiler`, so dedup spans `def`s;
      `Function::constants`, `FnEmitter::consts`/`constKeys`/`addConst()` and the dead
      `CompiledProgram::code` mirror deleted; the VM's frame loop and the disassembler read the one
      program pool. **`.qlc` v7 → v8** — pool section after the function records (single-version
      reader, v1–v7 rejected), new load-time check: every const-pool operand (`mov` kind 2,
      `makefunc`, `makegen`, `emit`) must be `< poolCount`. `codec_check` 330 → 337, corpus
      byte-identical, ctest 51/51. `13-bytecodes.md` `## unified constant table` marked
      **Implemented**; **Q34 settled** (v8 = pool, v9 = Phase 3, recorded in `SERIALIZATION.md` §4).
      Phase 3 (`export` + import/export tables, `GETIMPORT`/`CALLIMPORT`, `.qlc` v9) is next —
      gated on **Q29–Q33** + **Q35**, and on C8 for name-based resolution
- [x] **`archive/external-modules-plan-v4.md` Phase 3a (`export` + module identity + tables) ✓ implemented
      (Round 63, 2026-11-09)**: `export` is a keyword with both spec forms — the prefix on a `def` /
      `const` / assignment / arrow closure and the standalone `export NAME;` (forward references
      allowed) — global scope only, enforced twice (the parser's body-depth guard, then the
      compiler's export pre-pass for a global-scope `if`/`for`/`while`/`switch` block).
      `CompiledProgram` gains `moduleName`, `exports` (`ExportEntry {kind, name, ref}`) and
      `imports` (`ImportEntry {module, name, kind, ref, alias}`); `setModuleName` / `setLibraryMode`
      are the new compiler options (library mode compiles the top level as `<init>` and requires
      `export const MODULE_NAME`). **`.qlc` v8 → v9** — the three sections append after
      `registerCount` (`docs/SERIALIZATION.md` §4.6), the reader stays single-version (v1–v8
      rejected) and validates the tail at load. **Q29 / Q30 ratified as defaulted** (name = source
      `MODULE_NAME` > driver stem > `main`, mandatory only in library mode; `readonly` binds the
      *importer* — the defining module keeps writing to an exported variable, but a name exported
      as a `def` cannot be rebound). **Q31 untouched**: no opcode was appended, `kOpcodeUsedCount`
      stays **72** and the import table is always empty. `13-bytecodes.md` `## keyword export`
      marked **Implemented**. New `export` suite (88 checks), `codec_check` 337 → 352, corpus
      byte-identical, ctest **52/52**. Phase 3b (loader + linker) is next — gated on **Q32**,
      **Q33**, **Q35** and, for name-based resolution, on **C8**; Phase 3c (`GETIMPORT` /
      `CALLIMPORT`) on **Q31**
- [x] **`archive/external-modules-plan-v4.md` Phase 3b (loader + linker) ✓ implemented (Round 64,
      2026-11-09)**: `QLangContext::loadProgram` / `loadProgramFile` + the module registry
      (`loadedModule` / `loadedModules`, native-slot **names** re-resolved, all missing names in one
      `Io` error), compile-time resolution of all five import forms against a loaded export table,
      and the new `QLangLinker` unit — one function table, one `valueKey`-folded pool, one global
      register area sliced per module, `CompiledProgram::initSequence` run by `QLangVM::run()`.
      **No `.qlc` bump** (still v9): what 3b adds is link-time state, so `writeQlc` refuses a merged
      program. **Q32 ratified *not as defaulted*** (the linker's input is an encoded image, so
      decode → rewrite → `encodeRawStream()` is the only path), **Q33** / **Q35** as defaulted,
      **Q31** untouched. New `linker` suite (75 checks), `codec_check` 352 → 354, corpus
      byte-identical, ctest **53/53**
- [x] **`archive/external-modules-plan-v4.md` Phase 3c (`GETIMPORT` + `CALLIMPORT`) ✓ implemented (Round 65,
      2026-11-09) — the plan is complete**: `GETIMPORT` / `CALLIMPORT` emitted for every imported
      name and executed in `runFrames()`; **Q31 ratified *not as defaulted*** — identities **72/73**
      (not 73/74: M4 has not landed, "append last, never renumber"), `kOpcodeUsedCount` **72 → 74**,
      so **`DECL` takes 74 when M4 lands** (`encoding-v6-and-indirect-plan.md` §4/§9 corrected);
      `OperandRole::ImportIndex` + the reader's `import index out of range` check; the 3b provisional
      lowering and `CompiledProgram::pendingLinks` **deleted**; one merged, deduplicated import table
      and `callimport` resolved away by the linker (`callfn`, or `makegen` with a synthesized closure
      for an exported generator); a **library** may import another `.ql` (transitive chain);
      `import` inside a `def` body is a compile error (was silently dropped). Also fixed:
      `buildFunction` read `fn.captures` **after** `std::move(fn)`, so every generator's pool spec
      had zero captures. `linker` 75 → 126, `codec_check` 354 → 358, `gen` 5 → 7, corpus
      byte-identical, ctest **53/53**. Docs: `13-bytecodes.md` new opcode section + status flips,
      `09-modules.md` rewritten as two module kinds, `07-api.md` quote scoped to C++ modules,
      `SERIALIZATION.md` refreshed. *Remaining for `# external modules`:* a runtime module registry
      and name-based discovery — **C8**
- [ ] `missing-features-plan.md` itself drifted after Rounds 56/59/60: the A5 row still says
      "pipeline still emits v5", Phase 2 still says "`kOpcodeUsedCount` is 73 … A5 raises it to
      80", Conventions still say "`.qlc` v5 stays the only supported format", and Dependencies
      still lists M2/M3 as pending

## 6. Defects and drift with no plan of their own

- [ ] **C3 guard defect (small, unblocks "verified")**: `hasMethod` accepts `BoundMethod`
      (`src/QLangContext.cpp:635`) but the table `ObjectOps::callMethod` requires `Function`
      (`src/QLangObjects.cpp:187`) — `hasMethod` says yes, `callMethod` then throws
      `member '…' is not a function`; fix the guard and add a case
- [ ] **C3 tally drift**: `status.md`, `tests.md` and `harness.md` still record `external` at
      **49** checks where HEAD has **66** (since `c4ee131`) — recorded as pending C3, so it needs
      the fix above before it can be re-recorded
- [ ] **codegen defect (pre-existing, found 2026-11-08)**: `compileForIn`'s runtime-bound path
      emits a skip `JMP` after the backward guard that is never patched, so its target stays
      instruction 0 — a descending loop with non-constant bounds spins there and trips the step
      guard. Repro: `local n = 3` / `for i in range(5, n) step -1 { }` → "infinite loop (step
      limit)". Constant bounds take the other path and work. Marked with a `Defect` comment in
      `src/QLangCompiler.cpp:2178`
- [ ] **compiler defect (pre-existing, found 2026-11-09 while verifying Round 63)**: a top-level
      `const` interpolated inside an **f-string in a function body** evaluates to nil —
      `const G = "hello"` / `def a(n) { return f"{G}" }` / `println(a(1))` prints `nil`, while the
      plain (non-f-string) read of the same const works. Reproduced identically on a `HEAD`
      worktree build, so it is not a Phase 3a regression; recorded in `history.md` Round 63
- [ ] **perf**: `perf_stress` execute **10.974 ms** vs the committed baseline **10.382 ms**
      (+5.7 %, deliberately not re-baselined in Round 60). Either land the loop-direction hoist
      that recovers it (`archive/phase3-opcode-retirement-plan.md` §8: 8 → 7 instructions in the
      inner loop) or re-baseline deliberately in its own commit
- [ ] **Round 58 has no `history.md` entry** (Round 59 admits it: recorded in the `status.md`
      tally only) — the append-only record has a hole; backfill it
- [ ] Spec-wording edits owed (each is a spec-module edit → rebuild, re-test, re-hash):
      **Q28** `05-operators.md` dup-decl "compile warning" → the strict-typing level table;
      **Q9** `13-bytecodes.md` `OP_INDIRECT_READ`/`OP_INDIRECT_WRITE` → the implemented
      `indget`/`indput` mnemonics; `## opCode divi` / `## OpCode: swap` still show `movi`-era
      listings (Phase 3 §8)
- [ ] `Opcode::Nop` casing (odd one out) — cosmetic, separate change

- [x] **module defect (pre-existing, found 2026-11-10 auditing C8b): FIXED Round 70 (plan P0)** — a
      module-level `const` that a `def` of the same module read was unreachable at run time (the def
      and every import saw nil; the module's own `<init>` saw the value). The def's `MAKEFUNC` spec
      had captured the frame register while the linker's capture ABI names the slot, and the init
      frame's release then moved the value out of the slot into that capture `Env`; the spec now
      reuses the sentinel cell, and an unreachable def capture in a library module is a compile
      error. Guards: `testModuleConstOneIdentity` (`linker`) + the `ql_module_const` script.
      Audit closed: [../bugs/module-export-import-audit.md](../bugs/module-export-import-audit.md) F1.
      The same family (f-string const, script mode) remains open one row above.
- [x] **module defect (found 2026-11-10): FIXED Round 71 (plan P1)** — `from a import *` +
      `from b import *` always collided on `MODULE_NAME`; the star-expansion now skips it
      (identity, not an entity — **Q40** resolved), explicit and qualified reads unchanged,
      spec `09-modules.md` names the exception. Guards: `testStarImportIdentity` (`linker`) +
      the `ql_star_import` script — audit **F2** closed
- [x] `docs/plans/module-export-import-plan.md` (2026-11-10) — **all phases landed, archived**:
      **P0** Round 70 (F1 + the silent-nil half of F5), **P1** Round 71 (F2), **P2** Round 72
      (F3, per-name alias, `.qlc` stays v9 — **Q41** resolved), **P3** Round 73 (F5 closed: the
      not-imported-module message); owned **Q39–Q43** (all resolved except Q42); F4 (class
      export), the runtime module registry and the rest moved with the plan's deferred table to
      [missing-features-plan.md](missing-features-plan.md) §"Deferred from the module
      export/import plan" — audit **F1–F3, F5** closed

## 7. Codegen follow-ups recorded by M3 / Phase 3 (no plan yet)

- [ ] Hoist the `for` loop's direction re-test out of the back-edge (see §6 perf; recovers the
      Round 60 +8.7 % A/B cost)
- [ ] `switch` case checks (`EQ` + `JMPIFTRUE`, `src/QLangCompiler.cpp:2941`) and `compileGotoIf`
      (`:2208`) → `eqf` + `je` / `tst` + `je`; each frees a temp but changes corpus shapes
- [ ] `&&` / `||` short-circuits in control position via flags (`tst` covers them; no emission)
- [ ] Genuinely unsigned relations, and the compiler emitting immediate forms 0–3
- [ ] Widening the remaining ALU families to kinds 0/2 beyond `mov`/`add`/`sub`/`cmp`
- [ ] An assembler (`qlang_run --assemble`?) is the prerequisite for `ja`/`jae`/`jb`/`jbe` to be
      more than spec/disassembly aliases (M3 D6)

## 8. Housekeeping

- [ ] `docs/plans/` has no `README.md` entry point, although AGENTS.md says to consult directory
      `README.md` files as entry points (`todo.md` currently plays that role)
- [ ] Regenerate or drop the untracked `examples/qla/*.qla` dumps (gitignored since `51f9d33`,
      still v5-era) and the root `hello_*.qlc` / `test-hello_*.qla` / `perf_result.json`
      artifacts (also gitignored)
