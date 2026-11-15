# Plan: .qla assembler follow-ups — the recorded skips of Phase 5 (Round 103)

Proposed: 2026-11-15 (post-Round 103). Parent: `docs/plans/archive/encoding-codegen-followups-plan.md`
Phase 5 (landed Round 103: `qlcc --assemble`, `qla_roundtrip` guard, 28/32 byte-round-trip,
4 module programs DEFERRED). Design record: `docs/plans/encoding-v6-and-indirect-plan.md` (D6).

The skips are: (a) import/export-table (module) assembly, (b) `--asm-level >1` input
symmetry, and (c) the recorded v1 print losses (loc values, `vars`/`constVars`, signature
block, module name, native-slot names, untagged pool values). (a) and (c) are the same work:
**teaching the listing to carry the sections it currently drops.** The endpoint we walked
back from is the plan's original promise: **full `.qlc` byte-equality** for
`assemble(disassemble(P)) == P`.

## M1 — machine sections: close the metadata losses (highest value, smallest delta)

**LANDED 2026-11-15 (Round 108).** All five sections print in the level-1 listing and
parse back (`module:`, `globals:`, `natives:`, `exports:` — included because module
scripts' export tables must round-trip — plus per-function `locs:`/`sigs:`); `writeQlc`
serializes vars/constVars sorted so a compiled image and its rebuild encode identically;
`qla_roundtrip` is in whole-byte mode (29/34 byte-identical, the same 5 module scripts
DEFERRED). The trigger check was moot: whole-byte equality makes `locs`/`sigs` print
mandatory regardless of their run-time impact. M2–M4 below; concrete build plan for the
remainder: `docs/plans/qla-assembler-followups/README.md`.

Makes non-linked scripts round-trip to **whole-file byte equality**, letting `qla_roundtrip`
graduate from "compared set" to `cmp(1)`-style equality.

- Print additive sections in the level-1 listing (header stays `(v1)` — additive; hand-written
  listings may omit them, the assembler tolerates absence):
  - `module: <name>` — the compiler writes the entry stem; today the assembler cannot know it.
  - `globals:` — the `vars` map (name → register) + `constVars` markers.
  - `natives:` — slot ↔ name pairs (`call -id` operands are meaningless without them; this is
    what `writeQlc`'s `nativeIds` table serializes).
  - `locs:` — per-instruction `(line, col)` as a parallel indexed section (the reader already
    enforces parallelism; assembled images ride `(0,0)` placeholders today).
  - `sigs:` — per-function `paramTypes`/`paramNames`/`returnType` (the `.qlc` v10 section).
- Assembler parses each; values map straight onto `CompiledProgram` fields.
- **Trigger check first:** does a missing `vars`/`sigs`/loc section actually degrade a *run* of
  an assembled image (globals by name, strict-typing diagnostics on loaded modules, error line
  numbers)? If the answer is "only diagnostics", the `sigs`/`locs` parts can slip; the
  `module`/`globals`/`natives` parts are what byte-equality and native calls need.
- Files: `examples/qlcc.cpp` (`disassemble` + `assembleQla`), `tests/unit/qla_roundtrip.cpp`
  (full-bytes mode), `docs/spec/18-qlang-run.md` + a `docs/SERIALIZATION.md` note.

## M2 — module tables: assemble the entry module (unfreezes 4 DEFERRED scripts)

- Print `imports:` (module, exported name, native slot) and `exports:` (kind, name,
  module-local ref) for the **pre-link entry image** — the form `readQlc`/the linker contract
  already speaks; the assembler fills `prog.imports`/`prog.exports`.
- Guard: `ql_module_const`, `ql_star_import` move from DEFERRED to compared (their listings
  already carry `getimport` and are rejected by the assembler's up-front refusal, which drops).
- **Design decision for `neo`/`ql_module`:** `-a` lists the *merged* program while `-c` writes
  the *entry* image — two artifacts, two programs, nothing to compare. Options:
  1. Make the entry's `.qla` describe the entry image (matching `-c`); merged detail stays in
     the level-5 breakdown only. Smaller, honest artifact pairing — but changes existing
     listing behavior; check the `qlcc_cli` contract (`cmake/check_qlcc_cli.cmake`) first.
  2. Emit a second listing beside it.
  Recommend 1 unless the contract pins current behavior.
- Do **not** chase assembled *merged* images — `.qlc` has no section for the link init
  sequence (parent plan: "merged program is not serializable"). Record as closed-without-action.

## M3 — pool tag fidelity (small; empties the "cannot round-trip" list)

- Print tag markers only where the bare print is ambiguous: `set[...]`, `L<dec>` for `Long`
  and below-`int32` negatives, integral floats forced to carry a `.` (`%.17g` + append), blobs
  as `<N: hexbytes>`. Inverse in `parsePoolValue`; the blob refusal drops.
- Add one corpus script exercising a set/blob/long constant so the guard proves it (the corpus
  holds none today — that is why it could be recorded as a deviation at all).
- Table keys containing `#`/`: `: keep as **declared limitation** (comment strip + key scan);
  revisit only if a corpus script trips.

## M4 — `--asm-level >1` input symmetry: decide, do not build

- Recommended resolution: **never build it.** Levels 2–5 print human annotations
  (pseudo-mnemonics, register maps, merged breakdowns); the machine contract is the level-1
  print plus the explicit M1/M2 sections. Close the row with that sentence recorded in the
  design record (`encoding-v6-and-indirect-plan.md`). Building it means parsing prose.

## Explicitly deferred (promote on trigger)

| item | why not now | promote when |
|---|---|---|
| M2 (module tables) | no host hand-edits a module `.qla` today; the guard already records the DEFERRED reason | a module listing is hand-edited, or a module `.qlc` must be rebuilt from its listing |
| assembled merged images | codec law: no init-sequence section in `.qlc` | a `.qlc` v11+ grows one |
| M1 `sigs`/`locs` detail | diagnostics-only impact (verify in the M1 trigger check) | an assembled image's run/diagnostics are observably worse |
| M3 table-key escaping | no corpus producer | a script with a `#`/`: ` table-key constant |

## Verification (AGENTS.md, every milestone)

`cmake -B build && cmake --build build`; `ctest --test-dir build` (68/68 baseline). Print-shape
changes: corpus stdout byte-identity A/B vs the HEAD worktree (listing-only changes need the
corpus proof only if any print reaches stdout; none should). `qlcc_cli` (91 checks) and
`qla_roundtrip` green and updated (M1: full-bytes mode; M2: DEFERRED count drops toward 0
except linked-program rows pending the pairing decision). `codegen_size` budgets untouched
(no compiler change). History Round entry + `tests.md` tally + `todo.md` rows flip +
`update-doc-hashes.sh` after green, never before. Commits: one per milestone; suggest names,
do not commit.

## Sequencing

M1 → M3 → M2 → M4 (closure prose). Cost: M1 medium, M3 small, M2 medium (the only milestone
with a behavior decision), M4 half an hour of prose.

**Ponytail caveat:** none of this is paged-at-3am work today — the guard already proves what
v1 promises. M1 earns its keep the moment anyone *runs* an assembled image or needs
reproducible `.qlc` bytes; M2 earns it the first time a hand-edited module `.qla` is real. If
neither is near, the honest alternative is: record the M2/M4 decisions in `todo.md` with
promote-triggers and build nothing.
