# QLang — TODO / backlog

Status: current 2026-11-08 (`HEAD = a6737b9` + the staged Phase 3 S6 documentation edits).
Maintained alongside `docs/plans/`; per AGENTS.md a plan that is fully implemented moves to
`docs/plans/archive/`. Sources: the 2026-11-07 plan audit,
[encoding-v6-and-indirect-plan.md](encoding-v6-and-indirect-plan.md),
[missing-features-plan.md](missing-features-plan.md),
[offset-table-elimination-plan.md](offset-table-elimination-plan.md).

## 1. In flight (uncommitted working tree)

- [ ] Phase 3 S6 docs (Round 60) + this file — commit as
      `docs: opcode retirement shipped (Round 60)` (plan §S7 item 5; S1–S4 are already
      committed at `b86460b`/`801498c`/`a6737b9`)
- [ ] After the commit: move `phase3-opcode-retirement-plan.md` to `docs/plans/archive/`
      (AGENTS.md — the plan is DONE except that commit)
- [ ] Rebuild + `ctest --test-dir build` green → `bash update-doc-hashes.sh` → re-verify
      `doc_hashes` (the script is not executable)

## 2. M4 — `$$` indirect access (A3) — next milestone

Design is settled by parent plan §7 (**Q6 closed**: the declaration bit is not encoded);
a detailed plan is still owed for the check pipeline + test plan (missing-features Phase 2
item 3; open **Q7–Q11**).

- [ ] `DECL siteId` opcode — identity **72** (appended after Phase 3's compaction; the plan
      text's "79 → 80" predates it), `kOpcodeUsedCount` 72 → 73
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

## 3. Phase 3 — opcode retirement (`.qlc` v6 → v7)

Plan: [phase3-opcode-retirement-plan.md](phase3-opcode-retirement-plan.md) — **DONE
2026-11-08** except its S7 docs commit. D2's prerequisite (M4 first) did not hold, so the
plan's recorded **fallback** executed: `kOpcodeUsedCount` 79 → **72** (not 80 → 73), `TST` 71
last defined, `DECL` will append at 72.

- [x] **S1 + S2** (`b86460b`): bare truthiness lowers to `tst` + `je`/`jne` at all 15
      references; producer gate clean. Cost: `perf_stress` execute 10.442 → 11.349 ms
      (+8.7 % A/B; +5.7 % vs the committed baseline at close-out — inside the ±20 % gate,
      above the plan's ~5 % caution; root cause and mitigation in its S1 record and §8)
- [x] **S3** (`801498c`): the seven spellings retired, identities compacted
      (`new = old − |{removed < old}|`), `kOpcodeUsedCount` 79 → 72
- [x] **S4** (`a6737b9`): `.qlc` **v7**, reader accepts v7 only (v1–v6 rejected at the
      version field); v7 write → read → execute round-trip covered for all 23 scripts
- [x] **S5** (tallies + perf re-measured) + **S6** (Round 60 docs, staged)
- [ ] **S7**: the docs commit (`docs: opcode retirement shipped (Round 60)`), then archive
      the plan
- [ ] **D7 tail (deferred, test text only)**: rename `spec_check2`'s `movi:`/`addi:`/`subi:`
      case names, fix the `value_semantics` "MOVC clone" comments and `QLangVM.h`'s
      `CMP/CMPI` + `mov/movc` comments

## 4. Offset table (exploration → decision)

- [ ] Sign off §9 of `offset-table-elimination-plan.md` (option **A** + §7.4 `loc`
      delta-encoding; perf_stress image 6 719 B → ~3 600 B, −46 %) and record the decision
- [ ] If approved: bump `kQlcVersion` **7 → 8** — Phase 3 already spent the 6 → 7 bump
      (`a6737b9`), so the "one bump or two" question in the Phase 3 plan's follow-ups is
      closed in favour of two; update that plan's §8 bullet when the §9 decision lands

## 5. missing-features backlog (`missing-features-plan.md`)

- [ ] **codegen defect (found 2026-11-08 while converting Phase 3 S1; pre-existing,
      left as is)**: `compileForIn`'s runtime-bound path emits a skip `JMP` after the
      backward guard that is never patched, so its target stays instruction 0 — a
      descending loop with non-constant bounds spins there and trips the step guard.
      Repro: `local n = 3` / `for i in range(5, n) step -1 { }` → "infinite loop (step
      limit)". Constant bounds take the other path and work. Marked with a `Defect`
      comment in `src/QLangCompiler.cpp`
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
