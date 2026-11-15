# Plan: hoist the `for` loop direction test out of the back-edge (perf codegen round)

Status: **fully landed 2026-11-15 (Round 113)** — M1 (compileFor buckets),
M2 (compileForIn), M3 (perf −10 % recovered, baseline re-recorded, budgets
tightened, compliance recorded). Promoted from
`docs/plans/todo.md` §7 ("Hoist the `for` loop's direction re-test out of the
back-edge; recovers the Round 60 +8.7 % A/B cost") and the §6 Round 93 note
("the loop-direction hoist … stays **parked** — land it as a codegen round of
its own, not for a number"). Source observation:
`docs/plans/archive/phase3-opcode-retirement-plan.md` §8 — the S1 `tst` flip
put the inner `for j = 1 to 200` of `perf_stress` at 8 dispatches per
iteration (pre-S1 7); "the direction test is loop-invariant: hoisting it out
of the back-edge is a `compileFor`/`compileForIn` change, not a `tst` change,
and would take the loop below the pre-S1 count."

## Current lowering, grounded

`compileFor` (`src/QLangCompiler.cpp:2713`) specializes only when
**step is a literal *and* bounds are foldable** (`:2731`) — then it emits the
tight loop `guard: CMP x,end; J*→exit; body; ADD; JMP guard` (4 control
instructions + body). Every other shape takes the runtime path (`:2791+`),
whose guarded loop (`:2869-2890`) re-tests the loop-invariant direction
**inside the loop**: the back edge is `ADD x,x,step; JMP guardOnceLabel` and
`guardOnceLabel` is `TST posDir; JE` (`emitJumpIfTrue`, 2 instructions) in
front of the direction-selected `CMP`+`J*` guard. **Per-iteration control
cost: 6 dispatches instead of 4** — that is the +8.7 % on `perf_stress`
(its inner `for j = 1 to 200` has *constant bounds but no explicit step*, so
it misses the `:2731` special case entirely).

`compileForIn` (`:2916`) has the same split: the foldable branch (`:2968-2991`)
is already the tight 4-dispatch shape; its runtime branch (`:2993+`) emits the
same `TST posDir2; JE` re-test at `guardLabel` inside the loop, plus
compile-time-reachable zero-step infinite-loop and run-once body copies.

**Key structural fact: the hoist costs no code size.** Today's runtime path
already compiles the body **twice** (a run-once copy for the wrong-direction
case + one shared guarded copy that re-tests the direction each pass). The
hoist replaces "shared guarded copy + per-iteration test" with "two
direction-specialized guarded copies + one test at loop entry" — same number
of body emissions. Nesting multiplies copies identically before and after
(the run-once copy already duplicates the full nested inner loop). No
body-size heuristic is needed; if corpus listings disagree, Q53 files one.

## Design — direction test outside the loop

Buckets by what the compiler knows, for both `compileFor` and `compileForIn`:

1. **Bounds foldable + (no step or literal step)** — already fully resolved
   (`:2731` / `:2938`); *extend the condition to `!hasStep`*: with no explicit
   step the loop's step *is* its natural direction (`for` normalizes
   `stepReg = dir`, `:2828-2829`; the `correctDir` formula at `:2727` is the
   same sign), so `sv`-checks (`step == 0`, wrong-direction run-once) are
   unreachable and the loop emits as the tight shape with a constant ±1 step
   register. This alone covers the `perf_stress` inner loop: **6 → 4 control
   dispatches per iteration**, below the pre-S1 5.
2. **Bounds runtime + step sign known at compile time** (no step, or a
   non-zero literal step): compute `dir` once (the existing `SUB`/`GT`
   sequence), test it once at entry, and jump into **one of two specialized
   loops**: matching-direction → guarded loop whose guard is chosen at compile
   time (`sv > 0` → `CMP LE`, else `CMP GE`; for `for`-to with no step both
   copies are bounded, stepping by +1 / −1 and guarded by the respective
   bound); mismatching-direction → the existing run-once copy. The
   per-iteration `TST`/`JE`, the `EQ sameSign` pair and (for the no-step `for`
   -to case) the zero-step normalization vanish from the loop entirely.
   Zero literal step with runtime bounds keeps today's dedicated infinite-loop
   copy, emitted *without* the now-dead direction test.
3. **Runtime step *and* runtime bounds** — **unchanged** (Q52). The three-way
   runtime choice (bounded-forward / bounded-backward / run-once, step-sign
   unknown) would need up to three body copies for the rarest loop shape.

`compileForIn`'s runtime branch mirrors buckets 2/3 with its own quirks kept
exactly: a `range` loop with no explicit step has step `+1` (a known sign —
bucket 2, so a descending runtime `range` is the mismatch copy, i.e. today's
run-once behavior, **not** normalized to the natural direction — the
`for`-to/`for`-in asymmetry is spec'd semantics, `:2826-2827` vs `:3003+`),
and its literal-zero-step infinite loop (`:3026-3032`) drops the direction
test.

`break`/`continue` patch loops already walk per-copy ranges
(`m_emitter.raw[j]` scans with explicit start/end) — the specialized copies
reuse that pattern verbatim.

No new opcodes, no `.qlc` change, no codec change, no VM change: the emitted
instruction *set* is the existing one; only the layout changes.

## Open questions (defaults recommended)

- **Q52 runtime step + runtime bounds.** Keep today's in-loop re-test (one
  shared guarded copy + run-once copy, 3 body-unknowns resolved at runtime) or
  clone three specialized loops? **Default: keep.** Rarest shape; three body
  copies for a shape no profiled program has shown hot. Promote when a real
  profile shows a hot double-runtime loop.
- **Q53 code size.** The claim "body-copy count is unchanged" (run-once copy
  traded for the opposite-direction guarded copy) is structural, but verify
  it against the checked-in corpus listings (`--asm-level 4` set) and
  `codegen_size`. **Default: no heuristic; add a body-size cap only if the
  corpus scan grows.**
- **Q54 semantics freeze.** This is a codegen round, not a semantics round:
  every shape keeps its exact visit sequence, including the pinned existing
  asymmetries — `for`-to no-step descending iterates downward (step
  normalized to `dir`), `for`-in no-step descending `range` runs once,
  literal `step 0` with foldable bounds is infinite while `step 0` with
  runtime bounds (and no explicit step) normalizes. **Default: pin all of
  them as-is in tests; a divergence found here is a bug report, not a drive-by
  fix.**
- **Q55 `.qla` corpus churn.** Codegen changes regenerate the corpus
  disassembly listings. **Default: regenerate in the same round and record it
  in the compliance entry; the `doc_hashes`/corpus check is the review
  surface.**

## Milestones

- **M1 — `compileFor` (the +8.7 % recovery).** Extend bucket 1 to
  `!hasStep` (single guarded loop, constant ±1 step, no run-once copy), add
  bucket 2 (entry test + two specialized loops). Red-first tests:
  - **Equivalence matrix** (the loop suite in `spec_check2`):
    {const, runtime} bounds × {none, +k, −k, 0, runtime} step × {asc, desc,
    start == end} × with `break`/`continue` — hand-written expected visit
    sequences, all green on HEAD *before* touching the compiler (this is the
    freeze Q54 pins), then unchanged after the lowering.
  - **Shape pin**: disassemble a no-step constant-bounds loop and assert the
    region between the back-edge `JMP` and the loop exit contains no `tst`
    (the direction test is gone), and one for the runtime-bounds
    known-sign-step shape pinning exactly one `tst` *before* the loop.
- **M2 — `compileForIn` runtime branch.** Same buckets with its quirks
  (step `+1` known-sign, descending-range run-once copy, zero-step infinite
  copy without the test); the same matrix extended over `range(...)` forms,
  including descending runtime bounds with and without `step -1` (the shape
  the Round 60 unpatched-`JMP` bug lived in — its regression case in
  `docs/bugs/resolved/for-in-runtime-descending-unpatched-jmp.md` must stay
  green).
- **M3 — perf + compliance round (per AGENTS.md).** A/B `perf_stress` (inner
  loop expected 8 → 6 total dispatches per iteration; script level must at
  least recover Round 60's +8.7 %, land the new `tests/perf/baseline.json` in
  its own commit per the Round 93 precedent); regenerate corpus listings
  (Q55); rebuild + full `ctest`; append the round to `history.md`; close the
  §7 and §6 lines in `todo.md` and the phase-3 plan §8 tail; archive this plan.

## Files touched (expected)

`src/QLangCompiler.cpp` (`compileFor`, `compileForIn` only) ·
`tests/unit/spec_check2.cpp` (or the loop suite's home) · corpus listings ·
`tests/perf/baseline.json` (own commit) · `docs/compliance/{status,history,tests}.md` ·
`docs/plans/todo.md`.

No spec change (codegen is not spec'd — `13-bytecodes.md` untouched), no
opcode, no `.qlc` version bump.

## Deferred (named, with triggers)

- **Bucket-3 cloning** (runtime step + runtime bounds, 3 specialized copies) —
  trigger: a profiled hot double-runtime loop (Q52).
- **Body-size cap on cloning** — trigger: corpus/`codegen_size` growth (Q53).
