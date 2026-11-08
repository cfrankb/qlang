# QLang — TODO / backlog

Status: current 2026-11-07 (`HEAD = 56998cf` + the staged M3 S7 documentation edits).
Maintained alongside `docs/plans/`; per AGENTS.md a plan that is fully implemented moves to
`docs/plans/archive/`. Sources: the 2026-11-07 plan audit,
[encoding-v6-and-indirect-plan.md](encoding-v6-and-indirect-plan.md),
[missing-features-plan.md](missing-features-plan.md),
[offset-table-elimination-plan.md](offset-table-elimination-plan.md).

## 1. In flight (uncommitted working tree)

- [ ] M3 S7 tail — plan-file statuses: parent §6.1 M3 row → **DONE** (`4ead571`…`56998cf`,
      Round 59); parent §2.9 base-form rows → escaped (m3 D5); parent §10 "four exec loops" →
      one dispatch switch; `m3-flag-flow-plan.md` status header → DONE (it still says
      "S6 landed (uncommitted)" — S6 is committed at `56998cf`)
- [ ] Rebuild + `ctest --test-dir build` green → `bash update-doc-hashes.sh` → re-verify
      `doc_hashes` (the script is not executable)
- [ ] Commit the staged M3 S7 docs + M2 S9 housekeeping (`.gitignore`, `.gitattributes` are
      already staged; suggested: `docs: flag flow shipped (M3, Round 59)`)

## 2. M4 — `$$` indirect access (A3) — next milestone

Design is settled by parent plan §7 (**Q6 closed**: the declaration bit is not encoded);
a detailed plan is still owed for the check pipeline + test plan (missing-features Phase 2
item 3; open **Q7–Q11**).

- [ ] `DECL siteId` opcode — identity 79, `kOpcodeUsedCount` 79 → 80
- [ ] Per-function declaration table (`site → {reg, name, kind}`) as an optional `.qlc` v6
      section (M2 D4) + frame name map + per-frame bitset
- [ ] Two-operand `INDGET`/`INDPUT` (reuse the placeholders, `QLangBytecode.h:123-124`;
      **Q10** decides the arity)
- [ ] Six-stage `indirect variable error:` pipeline, resolution order (locals + `global`
      reads, locals-only writes), const/function rejection, branch-dependent declaration
- [ ] `$$"literal"` fast path — whether the literal syntax ships at all is **Q11**
- [ ] Parent §11 item 8 (`$$` verification) + `05-operators.md` `## operator $$` compliance
      entry; **Q28** spec-wording edit (dup-decl "compile warning" → strict-typing level table)

## 3. Phase 3 — opcode retirement (`.qlc` v6 → v7)

Plan: [phase3-opcode-retirement-plan.md](phase3-opcode-retirement-plan.md) (written
2026-11-07; prerequisite per its D2: M4 lands first).

- [ ] Execute S1–S7 there: `tst` + `je`/`jne` for bare truthiness → retire
      `MOVC`/`MOVI`/`ADDI`/`SUBI`/`CMPI`/`JMPIFFALSE`/`JMPIFTRUE` → identity compaction
      (80 → 73) → `.qlc` v7 → tests → docs

## 4. Offset table (exploration → decision)

- [ ] Sign off §9 of `offset-table-elimination-plan.md` (option **A** + §7.4 `loc`
      delta-encoding; perf_stress image 6 719 B → ~3 600 B, −46 %) and record the decision
- [ ] If approved: it bumps `kQlcVersion` 6 → 7 as well — decide with Phase 3 whether one
      version bump carries both changes or two (see Phase 3 plan, follow-ups)

## 5. missing-features backlog (`missing-features-plan.md`)

- [ ] **C3 defect (small, unblocks "verified")**: `hasMethod` accepts `BoundMethod`
      (`QLangContext.cpp:635`) but the table `ObjectOps::callMethod` requires `Function`
      (`QLangObjects.cpp:188`) — fix the guard, add a case
- [ ] **C1 strict typing** levels 0/1/2 — absorbs **B1** (warning emission path) and **B2**
      (duplicate-declaration diagnostic); finalize `16-strict_typing.md` (the `QLangContext`
      API), settle **Q12–Q18, Q27–Q28**; detailed plan needed
- [ ] **C5** inheritance (`ObjectOps::base` returns null) — spec decision first (**Q21**)
- [ ] **C7** enum-name string form — design decision first (**Q22**)
- [ ] **C8** module search paths + `searchCallback` — **Q23–Q26**; front end for
      `external-modules-plan-v3.md`
- [ ] Detailed per-item plans for A3, C1, C5, C7, C8 (the plan's own convention)

## 6. Housekeeping

- [ ] Delete `docs/plans/notes.txt` (stale scratch status — superseded by this file)
- [ ] Regenerate the untracked `examples/qla/*.qla` dumps (gitignored, still v5-era)
