# Phase 3 — Opcode Retirement: Remove the Deprecated Spellings, Compact Identities, `.qlc` v7

Status: **S1 landed (`b86460b`); S2 gate re-verified there; S3 landed (2026-11-08,
uncommitted) — `kOpcodeUsedCount` is now 72 — S4–S7 open.** Baseline
`HEAD = f192ad0` ("planning"); the M3 S7 documentation edits and the M2 S9 housekeeping
landed in `51f9d33`. **M4 has not landed**, so `kOpcodeUsedCount` is still **79** (not
the 80 D2 assumes): §4's table is correct as written except the `DECL` row, which is
prospective. Suite at this baseline: ctest **50** entries (48 with `-E perf`),
`encoding_v6` **34 470** checks, `codec_check` **199**, `flag_flow` **1324**,
`perf_stress` execute **10.382 ms** (committed baseline; **10.44 ms** in the S1 session —
see the S1 record for the pair).
Parent plan: [encoding-v6-and-indirect-plan.md](encoding-v6-and-indirect-plan.md) — this file
executes its **§6 Phase 3 (removal)** and cites §3 (coexistence rule + opcode budget),
§5.4 (`TST` subsumes the `jmpfalse`/`jmptrue` pair), §11 item 6 (the guarantee this plan
inverts) and §12 item 6 (the emission policy this plan flips).
Reference spec: `docs/spec/13-bytecodes.md` (`## opCode scope`, `### bytecode encoding`,
`### OpCode tst`, `## future plans` Phase 3) · `docs/SERIALIZATION.md` §4 (version field,
identity rule, validation).
Precedent for this document's structure: [m3-flag-flow-plan.md](m3-flag-flow-plan.md).
Prerequisite: **M4 (`$$`) lands first** (D2); `todo.md` §2–3 track the order.

Three deviations from the parent plan, stated up front:

1. **The §12 item-6 default is flipped.** The parent kept `jmpfalse`/`jmptrue` for bare
   truthiness "until they are removed"; removal is this plan, so bare truthiness lowers to
   `tst` + `je`/`jne` **here** (S1). The spec already states this end-state
   (`13-bytecodes.md:397`: "Emitting `tst` belongs with the removal of the deprecated pair
   (Phase 3, `.qlc` v7)").
2. **§11 item 6 is inverted, not re-verified.** "A hand-built v6 stream containing the
   deprecated spellings still executes" becomes "a v6 image is rejected by the version field
   and the removed identities are never produced or decoded". The legacy-execution suite is
   retired (D5), not kept as a must-fail test.
3. **Ordering vs M4 is made explicit** (D2). The parent implies M4 → Phase 3 (`DECL` = 79,
   declaration table inside v6); this plan is written against that baseline
   (`kOpcodeUsedCount` 80 → 73, matching the §3 budget "80 … 73 after
   `JMPIFFALSE`/`JMPIFTRUE` are removed"). The fallback (Phase 3 first: 79 → 72, `DECL`
   appended later at 72) is recorded there.

---

## 1. Scope

**In scope (Phase 3):**

1. The compiler emits `tst` + `je`/`jne` for every bare truthiness test — all 15
   `JMPIFFALSE`/`JMPIFTRUE` sites (S1).
2. `MOVC`, `MOVI`, `ADDI`, `SUBI`, `CMPI`, `JMPIFFALSE`, `JMPIFTRUE` are deleted from the
   `Opcode` enum (S3).
3. A **one-time identity compaction** closes the seven holes (D3); `kOpcodeUsedCount`
   **80 → 73** (with M4 landed; 79 → 72 in the D2 fallback).
4. The codec tables, `Bytecode::name()` mnemonics, VM dispatch cases and the disassembler
   special cases for the seven identities are deleted (S3).
5. `.qlc` format version **6 → 7**; the reader accepts v7 only (v6 rejected like v1–v5,
   clean-break precedent per Round 37/56) (S4).
6. Tests: legacy-spelling cases retired, hand-built streams retargeted, version-boundary
   cases added, tallies re-recorded (S3–S5).
7. Documentation: spec `## opCode scope` / `## future plans`, `SERIALIZATION.md` §4,
   compliance round entry, parent-plan statuses (S6).

**Out of scope:**

| Item | Where it belongs |
|------|------------------|
| `$$` machinery (`DECL`, declaration table, name map/bitset, two-operand `INDGET`/`INDPUT`) | **M4** (parent §7; `todo.md` §2) |
| Offset-table compression / elimination (also a v6 → 7 bump — see follow-ups) | `offset-table-elimination-plan.md` §9 decision |
| `ja`/`jae`/`jb`/`jbe` as code | nowhere — no assembler exists (M3 D6) |
| The jump-class width selector (jmp8/16/32) | unchanged — Phase 3 touches identities only |
| `Long`/`Qword`/`Unsigned` literal widening | M2 D3 follow-up (parent §9) |

**Invariants:** the encoding **layout does not change** — 2/2/4 modifier byte, escape bit,
permitted-kind tables, immediate forms and the jump class are untouched; only the identity
table changes (parent §6: "it changes no layout, only the identity table"). Corpus stdout
stays byte-identical. The reader stays single-version. No opcode identity is appended here
(`DECL` is M4's); the count only shrinks.

---

## 2. Baseline facts

| Fact | Location |
|------|----------|
| `kOpcodeUsedCount = 79` (M4 appends `DECL` → 80) | `include/qlang/QLangBytecode.h:267` |
| Deprecated identities: `MOVC` 0, `JMPIFFALSE` 45, `JMPIFTRUE` 46, `MOVI` 57, `ADDI` 58, `SUBI` 59, `CMPI` 61 | `include/qlang/QLangBytecode.h` (`enum class Opcode`) |
| Reader accepts `kQlcVersion` only ("unsupported version") | `include/qlang/QLangBytecodeIO.h:34`, `src/QLangBytecodeIO.cpp:469` |
| Compiler still emitted `JMPIFFALSE`/`JMPIFTRUE` at 15 sites (**all converted in S1**) | `src/QLangCompiler.cpp:858, 1044, 1918, 1935, 1949, 1973, 2110, 2121, 2130, 2162, 2252, 2284, 2316, 3066, 3945` |
| VM handlers to delete (`MOVC`/`MOVI` share one case) | `src/QLangVM.cpp:2027-2028, 2449, 2454, 2490, 2529, 2539` |
| Jump class includes the `JMPIF*` pair (width selector + 1-byte condition register) | `src/QLangBytecode.cpp:20-21, 57, 68-69` |
| Permitted-kind table pins the suffix kinds (`MOVI` → immediate only, …) | `src/QLangBytecode.cpp:346-351` |
| Mnemonics `movc`/`jmpfalse`/`jmptrue`/`movi`/`addi`/`subi`/`cmpi` | `src/QLangBytecode.cpp:1196, 1244-1245, 1253-1257` |
| Disassembler special cases ("historical spellings", pseudo-text) | `examples/qlang_run.cpp:390-393, 551-561, 657, 895` |
| §11 item 6 legacy-execution suite (hand-built deprecated stream) | `tests/unit/codec_check.cpp:1749` ff. |
| Hand-built `MOVI` streams in round-trip cases | `tests/unit/codec_check.cpp:828-836, :912-919` |
| `encoding_v6` deprecated-spelling cases (kinds, base forms, sweep rows, rejects) | `tests/unit/encoding_v6_test.cpp:384-431, 459-461, 526-527, 559-578, 624-635, 671-675, 823, 903` |
| `spec_check2` "movi:/addi:/subi:" cases are **behavioral** (compile source, check values) — labels stale since M2 | `tests/unit/spec_check2.cpp:902-997` |
| `value_semantics` L5 comments name the `MOVC` clone (behavior now on `mov` kind 2) | `tests/unit/value_semantics.cpp:34, 285, 298` |
| `tst` implemented, **not emitted** (M3 D4) — **emitted since S1**; the spec sentence is stale until S6 | `docs/spec/13-bytecodes.md:397` |
| Spec `## opCode scope` + `## future plans` already describe Phase 3 (removal, v7) | `docs/spec/13-bytecodes.md:746` ff. |
| SERIALIZATION: version 6 at `:95, :101, :260`; "79 defined" at `:132, :152, :185`; identity rule `< kOpcodeUsedCount` at `:152` | `docs/SERIALIZATION.md` §4 |
| No checked-in bytecode fixtures: root `/*.qlc`, `/*.qla` and `examples/qla/` are gitignored | `.gitignore:18-20` |

**Measured today** (`qlang_run --asm-level 4`, 2026-11-07) — the bare-truthiness population
S1 converts, each site +1 byte / +1 instruction (parent §5.4):

| Script | `jmpfalse` | `jmptrue` |
|--------|-----------:|----------:|
| `hello.ql` | 1 | 3 |
| `flags.ql` | 10 | 0 |
| `perf_stress.ql` | 11 | 33 |
| **corpus (22 scripts)** | — | **83 total** |

---

## 3. Decisions to settle before coding

| # | Decision | Recommendation | Rationale |
|---|----------|----------------|-----------|
| D1 | Bare truthiness lowering | `tst r[a]` + `je`/`jne` at all 15 sites; retire the pair here. Rejected: keep `jmpfalse`/`jmptrue` until a hypothetical v8 (the §12 item-6 default) — it keeps two identities, the jump-class condition-register special case and the spec's stale "not emitted" note alive, and blocks its own removal. | `tst` sets `flags = truth(r[a]) ? EQ : none`; `je` takes `EQ`, `jne` takes the none-set state — exact subsumption (parent §5.4). Spec `13-bytecodes.md:397` already normative. |
| D2 | Ordering vs M4 | **M4 first** (baseline `kOpcodeUsedCount` 80; after removal 73 = the §3 budget). `DECL` keeps its documented identity 79 → 72; the declaration table ships inside v6 (M2 D4); one renumbering event. Rejected: Phase 3 first (79 → 72, `DECL` later appended at 72) — legal, but `DECL`'s identity would differ from every document written for M4. Fallback if the order flips: drop the `DECL` row from §4 and end at `TST` → 71, count 72. |
| D3 | Renumbering scheme | Compact in relative order: `new = old − |{removed ids < old}|`; no holes left. Rejected: keep holes — wastes 7 of the 128-slot budget and leaves identities that are "defined" (`< kOpcodeUsedCount`) but unimplemented, which the load validator would have to special-case. | Parent §6: "a one-time renumbering, since removal leaves holes". |
| D4 | Removed identities at decode | No special case. After compaction every removed identity is either ≥ `kOpcodeUsedCount` (rejected by the existing load rule, `SERIALIZATION.md` §4 `:152`) or belongs to a live opcode. Old v6 streams die at the **version field** before any byte decodes (`QLangBytecodeIO.cpp:469`) — byte-level detection of old spellings is impossible (identities are reused) and unnecessary. |
| D5 | Fate of the legacy tests | Delete the `codec_check` §11-item-6 suite; retarget the two hand-built `MOVI` streams to `mov` kind 0; delete the `encoding_v6` deprecated-spelling cases; add version-boundary cases (v6 rejected below, v8 rejected above). Rejected: keep them as must-fail tests — they would exercise identities the decoder no longer defines. |
| D6 | Mnemonics | Remove the seven mnemonics from `Bytecode::name()` and the disassembler. A mnemonic for an undecodable opcode only invites hand-built streams that cannot load. |
| D7 | `spec_check2` labels | Keep every assertion (the cases are behavioral and already pass on the unified spellings); rename the labels `movi:`/`addi:`/`subi:` → `mov-imm:`/`add-imm:`/`sub-imm:` and fix the `value_semantics` L5 comments. |

---

## 4. Identity compaction (D3)

Removed: **0, 45, 46, 57, 58, 59, 61**. Shift by old identity: `1–44 → −1`;
`47–56 → −3`; `60 → −6`; `62–79 → −7`. Boundary anchors (the formula covers the rest;
`encoding_v6`'s exhaustive sweep cross-checks size/encode/decode for every identity):

| Opcode | old | new |
|--------|----:|----:|
| `MOVC` | 0 | **removed** |
| `MOV` | 1 | 0 |
| `JMP` | 44 | 43 |
| `JMPIFFALSE` / `JMPIFTRUE` | 45 / 46 | **removed** |
| `CONTINUE` | 47 | 44 |
| `YIELD` | 56 | 53 |
| `MOVI` / `ADDI` / `SUBI` | 57 / 58 / 59 | **removed** |
| `CMP` | 60 | 54 |
| `CMPI` | 61 | **removed** |
| `JE` | 62 | 55 |
| `JLE` | 67 | 60 |
| `Nop` | 68 | 61 |
| `INDEXPUT` | 72 | 65 |
| `EQF` | 73 | 66 |
| `TST` | 78 | 71 |
| `DECL` (M4) | 79 | 72 |

`kOpcodeUsedCount` **80 → 73** (fallback without M4: 79 → 72). The escape bit (bit 7) and
the 127-identity ceiling are unaffected.

---

## 5. Steps

### S1 — Compiler: `tst` + `je`/`jne` for bare truthiness

Replace all 15 emission sites (`QLangCompiler.cpp` list in §2): `jmptrue r,t` → `tst r; je t`,
`jmpfalse r,t` → `tst r; jne t`. Update the jump-width selector (`:858`) and the negative-jump
mapping default (`:1044`) to operate on `je`/`jne` (plain jump class — no condition register).
Corpus stdout byte-identical (22 scripts); re-record `codegen_size_check` counts (+1
instruction per site: hello +4, flags +10, perf_stress +44); re-measure `perf_stress`
(baseline 10.382 ms; expect a small increase from +44 instructions — record the number, do
not silently accept a regression beyond ~5 %).

*Exit criteria:* `grep -c "Opcode::JMPIFFALSE\|Opcode::JMPIFTRUE" src/QLangCompiler.cpp` == 0;
suite green (the legacy decode suites hand-build streams and are untouched until S3).

**Landed 2026-11-08.** 15 references converted — 13 emission sites plus the two
non-emission references §2 counts:

| Site | count |
|------|------:|
| `compileFor` — direction select, step-zero guard, same-sign guard, back-edge re-test | 4 |
| `compileForIn` — constant-bound path and runtime-bound path (same four shapes) | 4 |
| `compileWhile`, `compileIf` — fallback after `emitCmpJumpFalseExpr` returns −1 | 2 |
| `compileGotoIf`, `compileSwitch` (case dispatch), `compileExpr` (ternary) | 3 |
| `emit()` — the condition-register jump form deleted; `cmpJumpIfFalse` default → `Nop` | 2 |

`emitJumpIfTrue`/`emitJumpIfFalse` (`QLangCompiler.cpp:1093, 1099`) emit `tst r` +
`je`/`jne` and return the index of the **jump**; the `tst` sits at index − 1, which is
where a loop back-edge must point to re-evaluate the condition. Every hardcoded `+2`/`+3`
label offset in the loop prologues became an explicitly captured index — patching the
`tst` (not a jump) would set `jumpTarget` on a non-jump and corrupt the width fixpoint.
The jump-width selector keeps its 7-byte conditional form for now
(`isConditionalJumpOpcode` is dead code after S1; S3 deletes it with the enumerators).

Gate and measurement output (2026-11-08, Release):

```
$ grep -c "Opcode::JMPIFFALSE\|Opcode::JMPIFTRUE" src/QLangCompiler.cpp
0
$ ctest --test-dir build                     # 50/50 passed (doc_hashes skipped)
$ build/codegen_size_check                   # ceilings re-recorded, reason in the file header
hello.ql 211 -> 215 (+4)   money.ql 151   cast.ql 163   perf_stress.ql 521 -> 565 (+44)
corpus stdout: 23/23 scripts byte-identical to the pre-change build, all exit 0
```

`perf_stress` execute, median of 5, three interleaved A/B rounds against a `f192ad0`
worktree built in the same session:

| build | per-round medians (ms) | median |
|-------|------------------------|-------:|
| `HEAD` `f192ad0` | 10.436 / 10.442 / 10.458 | **10.442** |
| S1 | 13.311 / 10.960 / 11.349 | **11.349 (+8.7 %)** |

**+8.7 % is above this step's ~5 % caution, so it is recorded, not accepted.** The cause
is structural, not noise: the inner `for j = 1 to 200` back-edge lands on the loop's
direction re-test, one instruction before (`jmptrue r23`) and two after (`tst r23; je`),
so each of the 40 000 iterations executes one extra dispatch — 7 → 8 instructions on the
taken path, +14 % on that loop, +8.7 % for the script (its other loops have no
bare-truthiness test). `perf_bench`'s own gate passes (12/12 stages within ±20 %, worst
+8.7 %, 0.5 ms epsilon). The direction test is loop-invariant: hoisting it out of the
back-edge is a `compileFor`/`compileForIn` change, not a `tst` change, and would take the
loop below the pre-S1 count — filed in §8.

Found while re-reading the runtime-bound `compileForIn` path (pre-existing, **unchanged**
by S1, marked in the code): the skip `JMP` after the backward guard is never patched, so a
descending `for … in range(hi, lo) step -1` with non-constant bounds spins at instruction
0 and trips the step guard. Filed in `todo.md`.

### S2 — Producer-free gate

Record in this plan: the grep gate over `src/` + `examples/` is zero for all seven opcodes,
and the `--asm-level 4` scan of all 22 corpus listings contains no `movi`/`movc`/`addi`/
`subi`/`cmpi`/`jmpfalse`/`jmptrue`. No checked-in `.qlc` fixtures exist (§2, `.gitignore`) —
the parent §6 precondition ("no producer emits the old spellings") is then discharged.

*Exit criteria:* gate output pasted below this step during execution.

**Recorded 2026-11-08.** The literal grep over `src/` + `examples/` is **not** zero and
cannot be at this step: the remaining hits are the *consumer* side (codec tables, VM
handlers, disassembler), which is exactly S3's deletion list. The **producer** — the
compiler — is zero, which is what the parent §6 precondition asks for.

```
$ grep -rn "Opcode::JMPIFFALSE\|Opcode::JMPIFTRUE\|Opcode::MOVI\|Opcode::MOVC\|Opcode::ADDI\
\|Opcode::SUBI\|Opcode::CMPI" src/ examples/ | awk -F: '{print $1}' | sort | uniq -c
     22 src/QLangBytecode.cpp        # jump class, width/permitted-kind tables, mnemonics (S3)
      8 examples/qlang_run.cpp       # disassembler special cases (S3)
      7 src/QLangVM.cpp              # handlers (S3)
$ grep -rn … src/QLangCompiler.cpp include/qlang/QLangCompiler.h | wc -l
1                                  # isConditionalJumpOpcode (QLangCompiler.h:194) — dead
                                   # since S1, removed with the enumerators in S3
$ for f in tests/scripts/*.ql; do build/qlang_run --asm /tmp/p3/gate2/$(basename $f).qla \
      --asm-level 4 "$f"; done ; grep -lE "\b(movi|movc|addi|subi|cmpi|jmpfalse|jmptrue)\b" …
asm gate OK                        # 23 listings (22 + flags.ql, added by M3 S6), 0 hits
mnemonics in the corpus: tst 83   je 59   jne 51   jmpfalse 0   jmptrue 0
```

`tst` 83 matches §2's predicted population exactly (83 bare-truthiness sites). `je`/`jne`
exceed it because M3 already emits them for fused comparisons. No checked-in `.qlc`
fixture exists (§2), so nothing else can carry an old spelling.

Re-verified 2026-11-08 at `b86460b` (clean tree, incremental build already up to date):
producer gate unchanged (22 / 8 / 7), `src/QLangCompiler.cpp` 0 with `QLangCompiler.h:194`
the only compiler-side hit; 23/23 corpus listings emitted, asm gate 0 hits, tally unchanged
(`tst` 83 / `je` 59 / `jne` 51); `git ls-files` lists 0 `.qlc`/`.qla` (47 on-disk dumps, all
matched by `.gitignore`); `ctest` 50/50 passed (`doc_hashes` skipped). The parent §6
precondition — no producer emits the old spellings — holds at this commit.

### S3 — Enum removal + compaction + tables (one atomic change)

The tests reference the enumerators, so enum, tables, VM, disassembler and test edits land
together (compile atomicity):

- `QLangBytecode.h`: delete the 7 enumerators, renumber per §4, `kOpcodeUsedCount` → 73.
- `QLangBytecode.cpp`: jump class (`:20-21, 57, 68-69`), operand-count/width tables
  (`:100, 274`), permitted kinds (`:346-351`), names (`:1196, 1244-1245, 1253-1257`).
- `QLangVM.cpp`: delete cases `:2027-2028` (the shared `MOVC`/`MOVI` case — `mov` by kind
  already covers both), `:2449`, `:2454`, `:2490`, `:2529`, `:2539`.
- `examples/qlang_run.cpp`: delete the special cases at `:390-393, 551-561, 657, 895`.
- `QLangCompiler.h:193` `isConditionalJumpOpcode` and its three uses in the width
  fixpoint (`QLangCompiler.cpp:60, 88, 104`): dead since S1 (the compiler emits no
  conditional jump), they exist only to name the two enumerators — delete them with the
  enum.
- Tests (D5): delete the `codec_check` legacy-execution suite and the `encoding_v6`
  deprecated-spelling cases; retarget the hand-built `MOVI` streams (`codec_check.cpp:836,
  919`) to `mov` kind 0 (`e = 0x40`).

*Exit criteria:* build green; suite green **except** the `codec_check` version asserts
(S4 owns them); `encoding_v6` sweep re-run over 73 opcodes × 256 modifiers — record the new
accepted/rejected tallies (was 1 756 / 14 138 at 73 pre-M3 identities; re-measure, do not
estimate).

**Landed 2026-11-08 (uncommitted, on `b86460b`).** Enum, tables, VM, disassembler and test
edits landed as one change — 9 files, **135 insertions / 485 deletions**:
`QLangBytecode.h` +21/−39, `QLangBytecode.cpp` +3/−71, `QLangVM.cpp` +9/−30,
`QLangCompiler.h` +3/−7, `QLangCompiler.cpp` +10/−13, `qlang_run.cpp` +3/−22,
`codec_check.cpp` +61/−234, `encoding_v6_test.cpp` +23/−67, `flag_flow_test.cpp` +2/−2.

Deviations from the text above, all forced:

1. **`kOpcodeUsedCount` → 72, not 73** (the §1 D2 fallback: M4 has not landed, so the `DECL`
   row is dropped and `TST` is the last defined identity). Compaction removed ids
   **0, 45, 46, 57, 58, 59, 61**; `new = old − |{removed < old}|` gives `mov` 1→0,
   `jmp` 44→43, `continue` 47→44, `yield` 56→53, `cmp` 60→54, `je` 62→55, `jne`→56,
   `jg`→57, `jge`→58, `jl`→59, `jle` 67→60, `nop` 68→61, `emit`→62, `deepcopy`→63,
   `indexget`→64, `indexput`→65, `eqf` 73→66, `ltf`→67, `gtf`→68, `ltef`→69, `gtef` 77→70,
   `tst` 78→71.
2. **The suite is fully green (50/50), not "green except the version asserts".** S3 does not
   touch `kQlcVersion`, so nothing in S3's scope can break them; they stay at v6 until S4.
3. **D5's test list was incomplete.** Besides the legacy-execution suite and the two
   hand-built `MOVI` streams (both retargeted to `mov` + `e = 0x40`), the identity pins had
   to be re-recorded: `codec_check` `nop` 68→61, `deepcopy` 70→63, `indexget`/`indexput`
   71/72→64/65, `eqf..gtef` 73..77→66..70, `tst` 78→71 and their byte-level asserts
   (`[43]`, `[C2]`, `[C6]`, `[47]`), and `flag_flow_test:863` `kOpcodeUsedCount` 79→72.
   `codec_check`'s invalid-opcode probe now uses `kOpcodeUsedCount` instead of the literal
   79, so it tracks the boundary instead of pinning it twice.

Measured (re-measured, not estimated):

| metric | before S3 | after S3 |
| --- | --- | --- |
| `encoding_v6` exhaustive pairs validated | 2 029 | **1 872** |
| `encoding_v6` rejected pairs cross-checked | 15 401 | **14 274** |
| `encoding_v6` decoder/table parity | 30 298 rej / 2 470 acc / 15 731 trunc | **30 392 rej / 2 376 acc / 15 144 trunc** |
| `encoding_v6` checks | 37 859 | **35 083** |
| `flag_flow` checks | 1 324 | **1 324** (unchanged) |
| `codegen_size_check` (hello/money/cast/perf_stress) | 215/151/163/565 | **215/151/163/565** (unchanged — §6 row 3) |
| `ctest` | 50/50 | **50/50** (`doc_hashes` skipped) |

Sweep coverage is now 72 opcodes × 256 modifiers = 18 432 pairs; the rejected cross-check
skips the 9 jump opcodes, so it walks 63 × 256 = 16 128. The two tallies are consistent:
16 128 − 14 274 rejected = 1 854 accepted non-jump pairs, and 1 872 − 1 854 = 18 = the 9
jump opcodes × their 2 width modifiers. The parity sweep still covers the whole
128 × 256 identity space (30 392 + 2 376 = 32 768), so the 7 retired identities moved 94
modifier pairs from accepted to rejected; the decoder accepts fewer of them than the size
table does (157), because a retired identity has no payload the encoder could build.

Compiler diagnostics are unchanged in kind: `QLangBytecode.cpp`'s three `default`-less
switches in `baseOperandCount` emit `-Wswitch` noise at HEAD too — **209** warnings at
`b86460b`, **189** now (7 fewer enumerators × 3 switches). No new warning class, and the
build is not `-Werror`.

Left for later steps (deliberately out of S3's scope): the stale comments naming the deleted
enumerators — `include/qlang/QLangVM.h:112, 114, 128, 154` (`CMP/CMPI`, `mov/movc`),
`tests/unit/spec_check2.cpp:902-1007` labels (D7), `tests/unit/value_semantics.cpp:34, 285,
298`, `tests/unit/codegen_size_check.cpp:56` → **S5**; spec + compliance + hashes → **S6**.

### S4 — `.qlc` v7

`kQlcVersion` 6 → 7 (`QLangBytecodeIO.h:34`); the writer emits 7; the reader rejects 6 with
the standard "unsupported version" (the `:469` path is version-agnostic — no logic change).
`codec_check`: version-boundary asserts (v6 below rejected, v8 above rejected, v7
round-trip) and the `.qlc` round-trip + execute for all 22 scripts (stdout identical).

*Exit criteria:* suite fully green; `SERIALIZATION.md` §4 validation rule matches the
implementation (doc edit lands in S6).

### S5 — Tallies, labels, perf

Re-record: `encoding_v6` check count + sweep tallies, `codec_check`, `flag_flow` (unchanged),
ctest entry count (50); rename the `spec_check2` labels (D7) and fix the `value_semantics`
comments; re-measure `perf_stress` against the S1 number (**11.349 ms**, see the S1
record) and record the pair.

*Exit criteria:* suite green; tallies recorded under each step.

### S6 — Documentation (only after the suite is green)

- `docs/spec/13-bytecodes.md`: `## opCode scope` — the deprecated column becomes "removed in
  v7", the `tst` row becomes **emitted**; `## future plans` Phase 3 bullet → landed; the
  `### bytecode encoding` "a deprecated jmpfalse / jmptrue additionally carries its 1-byte
  condition register" note deleted; `### OpCode tst` status → emitted (M3 D4 closed).
- `docs/SERIALIZATION.md`: §4 title + version field → **7** (`:95, :101, :260`); "79
  defined" → **73** (`:132, :152, :185`); identity rule keeps `< kOpcodeUsedCount`.
- `docs/compliance/`: `history.md` round entry (next available number — M4 may take one
  first); `status.md` extended-encoding item → Phase 3 landed (M4 row per its actual state);
  `README.md` round range; `tests.md` tallies.
- Parent plan: §3 fate column → removed; §5.4 → done; §6 phase table + a §6.1 row → **DONE**
  with SHAs; §11 item 6 marked inverted/retired; §12 item 6 marked superseded by D1.
- This plan → DONE; `todo.md` §3 checked.
- Then `bash update-doc-hashes.sh` — **never before** the tests (AGENTS.md).

*Exit criteria:* `doc_hashes` green; no spec sentence left claiming the deprecated spellings
still decode, none claiming `tst` is un-emitted.

### S7 — Housekeeping and commits

Suggested names (AGENTS.md: suggest, never auto-commit), staged by concern:

1. `compiler: lower bare truthiness to tst + je/jne (Phase 3 S1)`
2. `bytecode: retire movi/movc/addi/subi/cmpi/jmpfalse/jmptrue; compact identities (Phase 3 S3)`
3. `io: .qlc v7 — reader accepts v7 only (Phase 3 S4)`
4. `tests: retire legacy-spelling cases; re-record tallies (Phase 3 S5)`
5. `docs: opcode retirement shipped (Round N)` — S6, plan-status edits included, hashes after green

---

## 6. Verification gate

```bash
cmake -B build && cmake --build build
ctest --test-dir build                      # 50 entries (48 with -E perf)
# producer gate (S2)
grep -rn "Opcode::JMPIFFALSE\|Opcode::JMPIFTRUE\|Opcode::MOVI\|Opcode::MOVC\|Opcode::ADDI\|Opcode::SUBI\|Opcode::CMPI" src/ examples/ \
  && echo "PRODUCER GATE FAIL" || echo "producer gate OK"
# corpus disassembly gate
for f in tests/scripts/*.ql; do build/qlang_run --asm /tmp/p3/x.qla --asm-level 4 "$f" >/dev/null; done
grep -lE "\b(movi|movc|addi|subi|cmpi|jmpfalse|jmptrue)\b" /tmp/p3/*.qla \
  && echo "ASM GATE FAIL" || echo "asm gate OK"
# v6 image rejected (write one before S4, keep it as a fixture under /tmp)
build/qlang_run /tmp/p3/v6.qlc               # exit != 0, "unsupported version"
```

| Parent §11 item | Discharged by |
|-----------------|---------------|
| 1 corpus byte-identical | S1, S5 |
| 2 round-trip → execute (now v7) | S4 |
| 3 size/decode agreement at every `pc` | unchanged (layout invariant) — S3 sweep |
| 4 zero-byte immediates | unchanged |
| 5 flag matrix | S1 — `tst` now appears in compiled code; `flag_flow` + `flags.ql` cover it |
| 6 deprecated spellings still execute | **inverted** (deviation 2): v6 rejected by version; removed spellings never produced (gate above) |
| 7 reserved kinds/forms rejected at load | unchanged; identity rule `< kOpcodeUsedCount` now 73 |
| 8 `$$` | — M4 |
| 9 disassembly well-formed | S3 (mnemonics removed) |
| 10 pool shrink | unchanged |

---

## 7. Risks and effort

| Risk | Mitigation |
|------|------------|
| Identity reuse makes old v6 byte streams ambiguous | The version field rejects v6 before any byte decodes (`QLangBytecodeIO.cpp:469`); clean-break precedent (v1–v5 all rejected since Round 56) |
| +1 B / +1 instruction per truthiness test (83 corpus sites; 44 in `perf_stress`) | **Measured (S1): +8.7 %** on `perf_stress` execute, 10.442 → 11.349 ms — inside the AGENTS.md ±20 % gate, above this plan's ~5 % caution, root-caused to one extra dispatch in the `for` loop's per-iteration direction re-test (see the S1 record). Mitigation filed in §8; `tst` dispatches through the same table-driven decoder as every non-jump op |
| Test compile breakage when enumerators vanish | S3 is atomic (enum + tables + VM + disassembler + tests in one change) |
| M4 ordering drifts | D2 fallback recorded; §4 table recomputed if the order flips |
| A codec-table row missed for a shifted identity | `encoding_v6` exhaustive sweep (every opcode × 256 modifiers, size/encode/decode cross-checked) re-run in S3 |
| Docs claim removed spellings still decode | S6 exit criteria + `doc_hashes` |

Effort: S1 1 d, S2 0.25 d, S3 1 d, S4 0.5 d, S5 0.5 d, S6 0.5 d, S7 0.25 d ≈ **4 days**.

---

## 8. Follow-ups recorded (deliberately not Phase 3)

- **Hoist the `for` loop's direction re-test out of the back-edge** (recovers
  S1's +8.7 %). `compileFor`/`compileForIn` end the loop with a jump back to the test
  that picks the bound comparison (`jl` when the step is positive, `jg` when negative).
  That test reads `step_init > 0`, computed once in the prologue and never written in
  the body, so the loop can branch to one of two specialized back-edges before entering
  and drop the test from the iteration entirely (8 → 7 instructions in `perf_stress`'s
  inner loop, i.e. below the pre-S1 count). Independent of `tst`: the same saving
  applies to the old `jmptrue` spelling, so it is a codegen improvement, not a Phase 3
  obligation.
- **Offset-table plan bumps v6 → 7 too** (`offset-table-elimination-plan.md` §10). If its §9
  decision lands while this plan is open, decide whether **one bump carries both** (one
  reader change, one round) or two sequential bumps. Decision belongs to that plan, not here.
- Spec `## opCode divi` / `## OpCode: swap` snippets still show `movi`/`divi`-era listings —
  spec-drift cleanup, not a Phase 3 obligation.
- `ja`/`jae`/`jb`/`jbe` aliases stay spec/disassembly-only until an assembler exists (M3 D6).
- `Long`/`Qword`/`Unsigned` literal widening (M2 D3 follow-up).
- `Opcode::Nop` casing (odd one out) — cosmetic, separate change.

---

## 9. Housekeeping and commits

See S7. Plan-file status edits ride the docs commit; `update-doc-hashes.sh` runs only after
a green suite (AGENTS.md); `todo.md` §3 is checked when this plan closes.
