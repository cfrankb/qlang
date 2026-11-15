# Plan: encoding-plan codegen follow-ups (todo §7)

Proposed: 2026-11-14 (post-Round 94). Status: **in progress** — Phase 0 landed 2026-11-15
(`05-operators.md` short-circuit + exact-integer relation sentences, `13-bytecodes.md` alias
paragraph restated to the decided rule; hashes re-recorded on green `ctest` 67/67).
Phase 1 landed 2026-11-15 (Round 99: `compileCondBranch` short-circuit lowering for
`&&`/`||`/`!` in control **and** value position, fused `switch`/`compileGotoIf` compares,
`emitCmpJumpExpr(cond, jumpIfFalse)` two-direction fold; corpus 32/32 byte-identical,
budgets unchanged as measured).
Phase 2 landed 2026-11-15 (Round 100: literal→form selector signs by literal type —
`Unsigned` literals ride the narrowest of forms 0–2, `Qword` literals form 3; corpus
32/32 byte-identical, budgets unchanged as measured, `codec_check`'s D3 pin flipped).
Phase 3 landed 2026-11-15 (Round 101: every `isAluOrFlagCompare` family takes a literal
right inline — the in-place 2-operand form replaces the literal→temp `mov`; budgets
re-recorded `hello` 217→214, `perf_stress` 565→563, corpus 32/32 byte-identical).

Provenance: `todo.md` §7 "Codegen follow-ups recorded by M3 / Phase 3 (no plan yet)" — the six
rows below are that list, in dependency order; design record is
[`encoding-v6-and-indirect-plan.md`](encoding-v6-and-indirect-plan.md) (D3 immediate policy,
D5 escaped compare immediates, D6 + §4.4 unsigned jumps, §5 forms 0–3).

| todo row | phase |
|---|---|
| `switch` / `compileGotoIf` → `eqf`+`je` | P1 |
| `&&` / `||` short-circuit in control position | P0 + P1 |
| compiler emitting immediate forms 0–3 | P2 |
| ALU families widened to kinds 0/2 | P3 |
| genuinely unsigned relations | P0 + P4 |
| assembler (`ja`/`jae`/`jb`/`jbe` beyond aliases) | P5 |

## Baseline facts (verified Round 94, 2026-11-14)

- Flag machinery is shipped: `emitJumpIfTrue/False` = `tst` + `je`/`jne`
  (`QLangCompiler.cpp:1402/1407`); `compileIf`/`compileWhile` fuse comparisons via
  `emitCmpJumpFalseExpr` (`:1414`, M3 D2: statically-Int pair → `cmp`, else the dual
  `flagCompareIfFalse` compare, `:1379`) — **and that fold already rides a literal right
  operand inside the compare** (inline immediate or constant-pool kind 2, escaped, M3 D5,
  `:1444`+). The fold machinery to reuse exists.
- `compileGotoIf` (`:2979`) never fuses: condition → temp → `tst`+`je`, even for comparisons.
- `compileSwitch` (`:4234`): per case `EQ` (or `INx`) into a temp + `tst`+`je` + skip `JMP`.
- `&&`/`||` are **eager**: the Binary lowering (`:4502`–`:4509`) compiles both operands into
  registers and emits `LAND`/`LOR`, whose VM impl (`QLangVM.cpp:1763`) tests both. The spec
  never says short-circuit (`05-operators.md` just lists them). The todo's "short-circuits in
  control position" therefore **adds behavior**, not just instructions — hence Phase 0.
- Immediate selector is signed-only: `narrowestSignedForm` (`:28`) + `emitImmI/emitImmF/
  emitImmNil`; unsigned forms U8/U16/U32/U64 (forms 0–3) decode in the codec and materialize
  in the VM (Round 55 shipped the full set) but **nothing in the compiler emits them**.
  Unsigned/Qword/word-literal values go through the constant pool (D3 policy).
- General binary expressions never fold a literal right except compound assignment
  (`a += k` → immediate `ADD`/`SUB`, `:1045`/`:2391`/`:2456`): `a * 3` is `mov` + reg-right
  `MUL` today, even though `isAluOrFlagCompare` grants every ALU family kinds 0/2 in the
  codec and the VM materializes by kind.
- `cmp()` (`QLangVM.cpp:240`) compares **all integer types through `toInt64` as signed
  int64**; a `Qword` above `INT64_MAX` throws. §4.4's conclusion holds: flags carry no
  signedness — unsignedness is a property of the compare *helper*, not of the jump encoding.
  `ja`/`jae`/`jb`/`jbe` exist **only in spec prose** (D6); no mnemonic, no identities.
- `qlcc -a` writes `.qla` v1 (`examples/qlcc.cpp`): offsets, mnemonic, verbatim `e=0x..`
  modifier, operands with pool-index annotations, `je 02 -> 0072` (form + resolved target),
  `;`/`#` comments. No assembler exists anywhere.
- `.qlc` v10 unchanged by any phase below; every emission-shape change re-records the
  `codegen_size` budgets **as measured** and the corpus stdout byte-identity check.

## Phase 0 — two spec sentences (docs-only gate, own commit)

Both code-change phases below are blocked on semantics the spec never fixed:

1. **`&&` / `||` evaluation** (`05-operators.md`): proposed — *operands short-circuit*
   (universal convention; today's eager `LAND`/`LOR` is an artifact, and the todo row
   presumes short-circuit). Decide whether **value position** also short-circuits; it should
   — one expression must not change side-effect behavior by context. Recommended landing:
   spec says short-circuit everywhere, P1 implements control position, and the value-path
   `LAND`/`LOR` materialization gains its branch shape **in the same round** (skipping it
   ships the split-brain).
2. **Mixed signed/unsigned relations** (`01-types.md` / `05-operators.md` compare table):
   proposed — relations are **exact mathematical-integer comparisons** (a negative `Int` is
   less than any `Unsigned`; `Qword` vs unsigned-family compares without sign loss). Rejected
   alternatives to note: C-style reinterpretation (surprising), compile-time refusal (breaks
   working today). No encoding change either way (§4.4): flags stay signless.

## Phase 1 — emission shapes (compiler-only, one round)

- **`compileSwitch`**: non-`in` cases → one compare (`cmp` val,case statically-Int, else
  `eqf`) + `je` body — drops the `EQ`-into-temp and the `tst`; `INx` cases keep `tst`.
- **`compileGotoIf`**: route the condition through the `emitCmpJumpFalseExpr` family (the
  true-side dual of `flagCompareIfFalse` is the same six jumps); comparison conditions
  become one compare + one relation jump to the label, non-comparisons unchanged
  (`tst`+`je` already).
- **Control-position `&&`/`||`/`!`** (after Phase 0): recursive `compileCondJump(cond, target,
  whenTrue)` — `&&`/`||` chain the jump targets, `!` flips, comparisons reuse the existing
  fold, everything else materializes + `tst` + flag jump. Deletes the bool temps for guards.
- Guards: `codegen_size` budgets re-recorded as measured; corpus stdout byte-identical;
  `perf_stress` A/B (12 medians) recorded in `history.md` — re-baseline only per the
  deliberate-gate rule; new `spec_check2` short-circuit cases pin Phase 0 (side effect in the
  right operand must not run) for control **and** value position.

## Phase 2 — immediate forms 0–3 (one round)

- Widen the literal→form selector (`narrowestSignedForm` + `emitLiteralImm`) to emit
  `U8`/`U16`/`U32`/`U64` payloads for `Unsigned`/`Qword`-typed literals (word/dword/unsigned/
  qword decls, masks) and keep signed forms for `Int`/`long` — literal **type**, not just
  value, picks sign. Pool fallback only where no form fits (strings/blobs/closures, unchanged).
- No codec/VM change (all forms shipped and covered by `codec_check` since Round 55); flip the
  D3 policy note wherever it's restated (spec `## encoding extended` reserves only kind 3 +
  forms 12–15, so likely plan/prose-only).
- Guards: `spec_check2` literal-materialization cases (`cast<dword>`, qword above `INT64_MAX`
  as immediate), `codegen_size` re-record, corpus byte-identity.

## Phase 3 — ALU families widened to kinds 0/2 (one round)

- In the Binary lowering (`:4502`), route a literal right operand through the **same fold
  helper the compare path uses** for every family `isAluOrFlagCompare` admits (`MUL`/`DIV`/
  `MOD`/`AND`/`OR`/`XOR`/`SHL`/`SHR` gain what `CMP`/`EQF` already do): inline immediate
  (kind 0, sign per Phase 2) when the type has a form, constant-pool right (kind 2) otherwise.
  The `mov`-into-temp at `:4505` disappears for literals; non-literal rights unchanged.
- Safety: pool holds scalar/string constants only (no container identities to alias per
  execution) — verify and state; VM needs nothing (materializes by kind already).
- Guards: `spec_check2` fold cases per family, `codegen_size` re-record (expect a visible
  shrink — this is the phase worth an A/B), corpus byte-identity.

## Phase 4 — genuinely unsigned relations (one round, after Phase 0)

- Fix `cmp()` (and `isEqual` if it shares the path): `Unsigned`/`Qword` vs `Int` per the
  Phase 0 mathematical-integer rule; `Qword` > `INT64_MAX` stops throwing in relations.
  One shared helper — every compare opcode, flag compare, `sort`, `min`/`max` route through
  it; grep every `toInt64` in comparison contexts, not just `cmp()`.
- **No** `JA`/`JAE`/`JB`/`JBE` identities, **no** flag change (§4.4 + D6 stand: distinct
  unsigned jumps would encode a signedness the flags don't carry).
- Guards: `value_semantics`/`spec_check2` mixed-relation cases incl. qword-edge and negative-
  Int-vs-unsigned; script regression for the error-message paths unchanged.

## Phase 5 — assembler (largest item; lands last, own multi-round option)

- Host: **`qlcc --assemble <file>.qla -o <out>.qlc`** — qlcc owns the `.qla` format, the
  codecs and the writer. (Todo's `qlang_run --assemble?` rejected: qlang_run runs programs.)
- Scope MVP: parse `.qla` v1 — strip `;`/`#` comments; consume the registers/params/constants/
  function sections verbatim (pool printed verbatim → pool indices taken literally); code
  lines: mnemonic + verbatim `e=0x..` + operands, jump targets from the printed absolute
  offsets (the `-> target` is authoritative, the printed form byte from `e=`), encode through
  `encodeInstruction`. **`ja`/`jae`/`jb`/`jbe` accepted here as D6 aliases → encode to
  `jg`/`jge`/`jl`/`jle`** — that is what makes them real; disassembly stays canonical.
- Byte-exact round-trip is achievable because `.qla` prints modifiers and pool verbatim:
  guard = for every `tests/scripts/*.ql`, `assemble(disassemble(P))` bytes == writer bytes of
  `P` (deviations recorded, not waved). Error reporting: line-numbered parse diagnostics
  (kind `Parse`), the only new user-facing surface.
- Deferred with the module story: assembling images with import/export-table sections
  (the `--asm-level 5` dump) and `--asm-level` input symmetry.

## Explicitly deferred (promote on trigger)

| item | why not now | promote when |
|---|---|---|
| distinct unsigned jump identities | §4.4: flags carry no signedness; the encodings would be dead weight | flags ever become sign-carrying |
| value-position `&&`/`||` short-circuit | — it rides P1 if Phase 0 approves (recommended); listed here only if the spec says eager | spec decision says value-position stays eager |
| assembler input for module images | needs export/import-table parsing + linker contract | a host hand-edits a `.qla` module |
| loop-direction hoist | tracked separately (`todo.md` §6 perf; archived plan §8) | its own round |

## Verification (AGENTS.md, every phase)

`cmake -B build && cmake --build build`; `ctest --test-dir build` (67/67 at the Round 94
baseline); `codegen_size` budgets + `tests.md`/`harness.md` tallies re-recorded as measured;
corpus stdout byte-identity per corpus-touching round; Release `perf_stress` A/B with the
±gate re-baseline rule; `update-doc-hashes.sh` after green, never before. Commits: one per
phase; `todo.md` §7 rows flip to `[x]` with the phase that lands each.
