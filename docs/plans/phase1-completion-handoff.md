# Phase 1 Completion Handoff (qlang-sandbox)

Date: 2026-11-03
Repo: `/home/cfrankb/toolkit/gcc/qlang-sandbox`, branch `master`, base commit
`973eab5` ("phase 0"). All work below is **uncommitted** in the working tree
(on top of the pre-existing uncommitted Phase-0 VM rewrite).

## Status at a glance

Phase 1 (per-call generator instances, `GeneratorState`, `MAKEGEN`
constant-pool closure, `resumeGenerator()` in the single `runFrames()`
dispatch loop) is **complete**. The two remaining spec_check2 failures
identified in `phase1-handoff.md` are fixed:

- **spec_check2: 463/463** (was 460/3).
- **Full ctest: 32/34.** The only failures are `phase0_scope` and
  `phase0_api` — both verified **pre-existing at base commit `973eab5`**
  (built in a throwaway worktree; identical failure set). Not regressions.
- Perf baseline re-recorded; `perf` test green.
- `/tmp` worktrees and repro scripts cleaned up.

## What changed this session (on top of the prior handoff state)

### 1. Nested-fn / closure-callee clobber fix — `src/QLangCompiler.cpp`

The `ExprKind::Call` case had two clobber hazards for register-based callee
loads (nested def-fns, arrow-closure vars, generator vars), e.g.
`def outer(a) def inner(b) return a+b end; return inner(10) end`:

1. **Arg-slot clobber:** the callee's register `cr` lies in the arg-slot
   range `[argBase, argBase+argCount-1]`; `compileArgsToSlots` overwrites
   the closure before the call.
2. **Live-alias clobber:** `dest` is a register the callee's capture cells
   live-alias (e.g. `return inner(10)` compiles into reg 0, which is also
   `outer`'s param `a` that `inner`'s cells alias via `Env::owner`
   register-aliasing). Writing the callee into `dest` before the callee runs
   corrupts the captured value.

Fix: a hoist block before `compileArgsToSlots` moves the callee into a
`tmpRegister()` (never an arg slot) when either hazard applies — for
captured def-fns when `cr` is in the slot range **or** `dest` equals a
capture-source register (checked via `fn.captureNames` → `regOf`); for
closure/generator vars **always** (source registers are baked into the
runtime closure, unknown at compile time). The call is then emitted as
`CALLCLOS calleeTmpR, dest, argCount` so the result lands in `dest` only at
`RETURN` (after the callee ran).

### 2. CALLCLOS operand rework — `src/QLangVM.cpp`, `include/qlang/QLangVM.h`

Previously `CALLCLOS i.a` was simultaneously callee source, arg base
(args at `i.a+1..`), and result destination — which made hazard 2
unfixable. New convention:

- `i.a` — callee source register.
- `i.b` — **result destination AND arg base** (args at `i.b+1..i.b+argc`).
  Normally `i.b == i.a` (the callee's home register).
- `i.c` — argc.

`pushCall` signature changed to
`pushCall(calleeIndex, callerRegs, resultDest, argBase, argc)`; the
`CALLCLOS` Function branch passes `(i.b, i.b, i.c)`. The Generator branch
writes its result to `i.b`; the BoundMethod branch (never hoisted —
table-stored calls always emit `i.a == i.b`) also writes to `i.b`.
`CALLFN` passes `(i.a, i.a, i.c)`. **All compiler `CALLCLOS` emit sites were
updated** to the 3-operand form (`dest, dest, argc` in the normal paths).

### 3. Set append — `src/QLangVM.cpp`

`Opcode::ARRAPP` now routes through the previously-unused `appendValue()`
helper: arrays append unconditionally; **sets insert with dedup**
(numbers/strings only, `vmThrow` otherwise) per SPEC §sets. This matches
the test expectation (`{1,2}` + append 2 → `{1,2}`).

### 4. Perf baseline re-recorded — `tests/perf/baseline.json`

`perf_stress` execute went **18.6ms → 13.4ms (-28%)** — a genuine
improvement from the Phase-1 redesign (the workload's 1000 closure calls
now go through `CALLCLOS` in the single dispatch loop instead of the old
thunk path). The old baseline predated Phase 1, so it was stale, not a
regression. Updated via `./perf_bench --update-baseline` (Release build).

## Test evidence

```
ctest: 32/34 passed
  FAIL phase0_scope  (5 failures: S2×3 via one parse error, P2, S1)
  FAIL phase0_api    (3 failures: P1, P3, F1)
```

Pre-existing proof: base commit `973eab5` built in a temp worktree shows the
same defects — `phase0_scope`: S2 (r2=nil), S3 (b=11, c=12), P2, S1;
`phase0_api`: P1, P3, F1.

**Note:** Phase 1 *fixed* S3 (two instances of the same generator no longer
clobber each other — exactly the per-call-instance defect) as a side
effect. S2 changed form: at base it silently returned the wrong value
(caller-frame capture rebinding); now it fails at compile time with
`parse error: cannot call captured function 'f'` because a `global`-captured
def-fn called from another def-fn has neither a local register (`regOf`
→ -1) nor a capture cell (`m_captureRegs` miss). Same root defect
(`global` capture resolution, see remediation plan F3/Phase 2), different
manifestation. S1, P2, P1, P3, F1 are unchanged.

## Remaining work

1. **Document the Phase-0 gaps** in
   `docs/plans/architecture-remediation-plan.md`: mark Phase 1 as done
   (S3 fixed; S1/S2/P2 remain for Phase 2, P1/P3 for Phase 2, F1 for
   Phase 4). Suggested: add a status line under the Phase 1 heading and an
   "as-of 2026-11-03" evidence note in §2. Nothing to fix now — all six
   are Phase 2 / Phase 4 scope.
2. **Commit.** The working tree holds the full Phase-0 VM rewrite +
   Phase-1 work (11 files, ~+746/−1428). Suggest splitting: (a) Phase-0
   VM rewrite, (b) Phase-1 generator instances + call fixes + set append,
   (c) perf baseline update ("deliberate act" per the harness output).
   `docs/plans/phase1-handoff.md` (and this file) are untracked.
3. **Phase 2** (global table: one home for variables) is the next
   remediation-plan phase — it is what turns the remaining
   `phase0_scope`/`phase0_api` reds green (S1, S2, P1, P2, P3).

## Key invariants for future edits

- `CALLCLOS` is now 3-operand (`calleeSrc, resultDest+argBase, argc`).
  Any new emit site must pass `dest, dest, argc` in the normal path.
- Never write the callee/call-result into a register that the callee's
  capture cells live-alias *before* the callee runs. The hoist block in
  the `Call` case is the single place this is handled — keep it in sync
  if new register-based call paths appear.
- `tmpRegister()` never allocates inside an active arg-slot range
  (`reserveArgSlots`/`endArgSlots` + quarantine); rely on that, don't
  re-check.
- `Env::owner` is a `Frame*` into `m_frames`, which is a `std::deque`
  (pointer-stable across push/pop) — safe; don't change `m_frames` to a
  `vector` without reworking the capture model.
- `ARRAPP` = collection append (array: push; set: dedup insert).
  `ARRAPPV` is the array-only strict variant (used by `join`).
