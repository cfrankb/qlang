# QLang — TODO / backlog

Status: current 2026-11-08 (`HEAD = be2ea56`; `cmake --build build` clean, `ctest --test-dir build`
**50/50** green including `doc_hashes`).
Maintained alongside `docs/plans/`; per AGENTS.md a plan that is fully implemented moves to
`docs/plans/archive/`. Sources: the 2026-11-08 plan audit,
[encoding-v6-and-indirect-plan.md](encoding-v6-and-indirect-plan.md),
[missing-features-plan.md](missing-features-plan.md),
[offset-table-elimination-plan.md](offset-table-elimination-plan.md).

## 1. Closed 2026-11-08 (this session)

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

## 2. M4 — `$$` indirect access (A3) — next milestone

Design is settled by parent plan §7 (**Q6 closed**: the declaration bit is not encoded), but the
implementation plan does not exist yet.

- [ ] **Write `docs/plans/m4-indirect-plan.md`** first (missing-features Phase 2 item 3 and the
      plan's own convention: one detailed plan per item before coding) — close **Q7–Q11**, spell
      out the six-stage check pipeline and the test plan, and follow the milestone-plan structure
      of `archive/m3-flag-flow-plan.md`
- [ ] `DECL siteId` opcode — identity **72** (appended after Phase 3's compaction; the parent
      plan's "79 → 80" predates it), `kOpcodeUsedCount` 72 → 73
- [ ] Per-function declaration table (`site → {reg, name, kind}`) as an optional `.qlc`
      section (M2 D4; `.qlc` is **v7** since Phase 3 — decide whether the table rides v7 or
      forces v8) + frame name map + per-frame bitset
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
- [ ] **C8** module search paths + `searchCallback` — **Q23–Q26**; plan; front end for
      `external-modules-plan-v3.md`
- [ ] **`external-modules-plan-v3.md` needs a re-audit**: it has no `Status:` header (unlike every
      milestone plan), no round tracking, and its version arithmetic is stale by three bumps — it
      plans `.qlc` v3 → v4 (unified pool) → v5 (module tables) while the tree is at **v7** and v5
      is retired. Nothing of Phases 1–3 has landed (no import/export tables, no
      `GETIMPORT`/`CALLIMPORT`, no builtin name table); it stays blocked on C8
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
