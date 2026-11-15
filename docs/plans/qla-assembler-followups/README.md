# Implementation plan: .qla assembler follow-ups — remaining milestones (M3 → M2 → M4)

Derived: 2026-11-15 (post-Round 108) from `docs/plans/qla-assembler-followups-plan.md`.
M1 LANDED (Round 108): all machine sections print/parse, `writeQlc` sorts vars/constVars,
`qla_roundtrip` is whole-byte — **29 byte-identical, 5 DEFERRED** (`class_export`, `neo`,
`ql_module`, `ql_module_const`, `ql_star_import`). ctest baseline: **69**.
This file is the build plan for what is left, in the parent's sequence **M3 → M2 → M4**.
Repo is C++20; all work is in `examples/qlcc.cpp` (driver `qlcc`: disassembler + assembler),
plus the guard test and tracking docs. No new files in `src/` beyond test/corpus additions.

## M3 — pool tag fidelity (small; empties the "cannot round-trip" list)

**LANDED 2026-11-15 (Round 109).** Empirical probes before code: the parent's sketch
(`L<dec>`) under-scoped the problem — folded `enum` members of narrow base types
(`enum E:byte { A }` → `{A: 0}` pool entry) and an integral float in a table literal
(`{x = 3.0}` → `{x: 3}`) reassemble with wrong tags as **ordinary-script byte differences**,
and a `Set` pool entry from `readQlc` **crashed** the disassembler (`asArray()` on a Set).
Shipped grammar: only `Int` prints bare; all other integer types print `<type>=<dec>`
(one uniform `name=` rule for the whole family beats seven ad-hoc prefixes); integral
floats force `.0`; `set[...]` (crash fix via `asSet()`); blobs `<N: hexbytes>` replace the
`<N bytes>` refusal. Parser inverses keep the bare-decimal hand-written rule (int32-wide
Int, wider Qword). Proof: corpus `pool_constants.ql` (ctest entry 70; corpus now 30/35
byte-identical, same 5 DEFERRED) + the hand-written `tags` fixture in `qla_roundtrip`
(Set/Blob/typed-integer pool — the tags no `.ql` producer reaches). ctest **70/70**.

Ambiguities in `valueText` (examples/qlcc.cpp ~l.1022) vs `parsePoolValue` (~l.2110), verified
at HEAD:

| value | printed today | parsed back as | fix |
|---|---|---|---|
| `Set` | `[a, b]` — same as `Array` | `Array` | print `set[...]` when `v.type() == T::Set`; parser: `set[` prefix → Set (same element parse; check the `QLangValue::Set` constructor shape) |
| `Long` | plain decimal | `Int` (int32-wide) or `Qword` | print `L<dec>`; parser: leading `L` → `Long` |
| negative below `INT32_MIN` | plain decimal | `Qword` (strtoll result reinterpreted — silently wrong today) | signed parse: negative out of int32 → `Long`; positive out of int32 → `Qword` (unchanged) |
| integral `Float` (e.g. `3.0`) | `%.17g` → `3` | `Int` | after `%.17g`, append `.0` when the text has no `.`/`e`/`E`/inf/nan; parser already routes `.`-containing tokens to `Float` |
| `Blob` | `<N bytes>` | refused (`"a blob constant is printed as a size only…"`, ~l.2297) | print `<N: hexbytes>` (N = byte count, lowercase hex); parser: `<`-branch inverse; drop the refusal |

Print tag markers **only where bare print is ambiguous** — `Bool`/`Char`/`Byte`/`Word`/
`Short`/`Int`/`Unsigned`/`Qword` decimals stay as-is where the existing int32-wide rule is
already lossless for pool contents (comments at ~l.2103–2109 record the rule; update them).

- Table keys containing `#` or `: `: **declared limitation** (comment strip + the `find(": ")`
  key scan at ~l.2233). Do not build escaping. Revisit only if a corpus script trips.
- **Corpus proof** (the guard proves nothing today — the corpus has no set/blob/long/integral-
  float constant): add ONE script `tests/scripts/pool_constants.ql` with one constant of each
  fixed kind at top level (so each lands in the pool). Note: `tests/scripts/*.ql` is globbed
  by other suites too — the script must be self-contained and deterministic like its siblings.
- Files: `examples/qlcc.cpp` (`valueText`, `parsePoolValue`), `tests/scripts/pool_constants.ql`.
- Exit: `qla_roundtrip` 30 byte-identical (corpus grew by one), still 5 DEFERRED.
- Suggested commit: `feat(qlcc): .qla pool tag fidelity — set[]/L-prefix/hex blobs/integral floats (qla-followups M3)`

## M2 — module tables: pair the entry listing with the entry image (unfreezes 5 DEFERRED)

Two decisions, both pre-verified at HEAD:

1. **Pairing (parent plan option 1 — confirmed safe).** `-a` currently disassembles the
   *merged* `compiled` (~l.3801) while `-c` writes the snapshotted `entryImage` (~l.3667–3673,
   3707) — two artifacts, nothing to compare. `cmake/check_qlcc_cli.cmake` pins only *file
   existence* for module listings ("`-a` still lists the entry", ~l.332), never their content,
   so switching is contractually free.
   - Make the entry-image snapshot unconditional when `listing` is set (today gated on
     `saveImage || !packageFile.empty()`, ~l.3669) and disassemble `*entryImage` (with
     `entryNativeIds`) instead of `compiled` for the entry's `.qla`.
   - Non-importing programs: `entryImage` already *is* `compiled`'s value — zero change for
     the 29 green rows and all `qlcc_cli` content expectations (which use plain scripts).
   - The level-5 module breakdown then describes the entry module only; merged detail stays
     where it is (stdout paths unaffected — listings are artifacts, not stdout).
2. **`imports:` machine section.** `exports:` already round-trips (M1). Print `imports:` for
   the entry image in `.qlc` import-table order, one entry per line in the shape
   `readQlc`/the linker contract already speaks (module, exported name, kind, ref — mirror the
   writer's field order; native-slot text stays level-5 breakdown's job). Values map straight
   onto `prog.imports`.
   - Parser: a new section mode beside the M1 `globals:`/`natives:`/`exports:` modes in
     `assembleQla` (~l.2563+); empty section / absent section → empty `prog.imports`.
   - **Drop the up-front refusal** in `parseCodeInstr` (~l.2397: `GETIMPORT`/`CALLIMPORT`
     "refers to the module import table…" ~l.2399). The operands are already printed as plain
     numbers; with the table present they mean the same thing again.
   - Guard cleanup: `tests/unit/qla_roundtrip.cpp` defers on `"import table"` in stderr
     (~l.129) and on `!want.imports.empty()` (~l.153). After the pairing switch the second
     deferral key is obsolete — remove both DEFERRED paths.
- Do **not** chase assembled *merged* images: `.qlc` has no init-sequence section
  ("merged program is not serializable", ~l.3663–3666 note + parent plan). Closed without action.
- Files: `examples/qlcc.cpp`, `tests/unit/qla_roundtrip.cpp`, `docs/spec/18-qlang-run.md`
  (`--assemble` refusal sentence + `-a` pairing wording), `docs/SERIALIZATION.md` note if the
  listing-section table moved.
- Exit: `qla_roundtrip` **34/34 byte-identical, 0 DEFERRED**; `qlcc_cli` 91 checks green
  (update the two module-listing checks only if an existence path moved — none should).
- Suggested commit: `feat(qlcc): .qla imports section + entry listing describes the entry image (qla-followups M2)`

## M4 — `--asm-level >1` input symmetry: decide, do not build

- Resolution text into the design record `docs/plans/encoding-v6-and-indirect-plan.md`:
  *the machine contract is the level-1 print plus the M1/M2 sections; levels 2–5 print human
  annotations (source interleaves, pseudo-mnemonics, register maps, merged breakdowns) and are
  never assembler input. Building it means parsing prose.*
- Half an hour of prose; no code. Fold into the M2 commit or stand alone
  (`docs(qlcc): close --asm-level >1 input symmetry as never-build (qla-followups M4)`).

## Explicitly closed without action (from the parent's deferral table)

- Assembled merged images — blocked by codec law, promote only if `.qlc` v11+ grows an
  init-sequence section.
- M3 table-key escaping — promote when a corpus script has a `#`/`: ` table-key constant.

## Verification (AGENTS.md, every milestone)

`cmake -B build && cmake --build build`; `ctest --test-dir build` (69-test baseline; M3 adds
one corpus script but no new test target). Print-shape changes (M3 `valueText`, M2 pairing):
they reach artifact files only, never stdout — the `qla_roundtrip` byte-equality + `qlcc_cli`
are the proof; a manual A/B of one module listing at HEAD vs after is cheap for M2.
History `docs/compliance/history.md` Round entry + `docs/compliance/tests.md` row +
`docs/plans/todo.md` row flips + `update-doc-hashes.sh` **after green, never before**.
Update this file's status markers per milestone; when all three land, move both this folder and
the parent plan to `docs/plans/archive/`. One commit per milestone; suggest names, do not commit.

## Sequencing & cost

M3 (small) → M2 (medium, the only behavior decision) → M4 (prose). Parent ponytail caveat
still stands: the guard already proves what v1 promises; M2 earns its keep the first time a
hand-edited module `.qla` is real — but the 5 DEFERRED rows are the last gap to a clean
34/34, and M3 is a prerequisite for the corpus to keep meaning that.
