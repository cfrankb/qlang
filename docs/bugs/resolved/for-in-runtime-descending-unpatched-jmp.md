# For-in loop with runtime-bound descending range: never-patched skip-JMP spins to the step limit

**Date:** 2026-11-14
**Severity:** high (correctness — a spec'd loop form hangs instead of iterating)
**Status:** open — fix implemented in working tree (uncommitted), verified 2026-11-14
**Cited code at commit:** `5b15ba7` (`QLangCompiler.cpp` line numbers below are at that commit)

## Symptom

```ql
local n = 3
for i in range(5, n) step -1
    interp i
end
```

`build/qlcc --run` → `runtime error at line 2,1: infinite loop (step limit)`
(repro: `/tmp/opencode/defects/d1.ql`). The literal-bounds twin
(`range(5, 1) step -1`) works, which is why the corpus passes.

## This is the recorded Defect

On record in `docs/plans/todo.md` §6 ("`compileForIn` emits an unpatched
skip-JMP … fires for runtime-bound descending loops") and retired-clause
pending in `docs/compliance/status.md` Round 70. The comment itself:

```cpp
// src/QLangCompiler.cpp:2825-2827 — Defect (pre-existing, unchanged here):
// this skip JMP is never patched, so its target stays instruction 0 — a
// descending loop with non-constant bounds spins there. Tracked in
// docs/plans/todo.md.
emit(Opcode::JMP, 0);                                    // :2828
```

## Root cause

`QLangCompiler::compileForIn` (`src/QLangCompiler.cpp:2675`), runtime path
(taken when the step is not a foldable constant **or** the bounds are not —
`:2752+`). The runtime path first derives the loop's *natural direction*
from the bounds (`:2762-2775`) and splits three ways: `step == 0` → an
unguarded infinite loop; step sign **disagreeing** with the natural
direction (`sameSign` test, `:2788`) → a deliberate **run-once** prologue
(`:2789-2796`, body executes exactly once — mirroring the literal-bounds
disagreement branch at `:2714`); step sign **agreeing** → the guarded loop:

| line | instruction | role |
|---|---|---|
| 2814 | `MOV x, start` | init counter once (`guardInit`) |
| 2816 | `guardLabel` | loop head |
| 2818-2819 | `GT posDir, dir, 0` + `JMP_IF_TRUE → 2830` | ascending → forward guard |
| 2823 | `emitCmpJumpFalse(GE, x, end)` | descending guard: out of range → `guardExit` (patched :2845) |
| **2828** | **`JMP 0`** | **descending + in-range → should enter the body; jumps to instruction 0 forever** |
| 2830-2831 | forward guard `LE` (exit → `guardExit`, patched :2846) | |
| 2833 | `bodyStart` | the body, and the jump target the JMP never got |
| 2837-2838 | `ADD x, step` ; `JMP guardLabel` | continue edge |

Three facts make the bug fire only for **direction-agreeing descending**
runtime loops:

1. Ascending: the direction test (2827) jumps over the JMP to the forward
   guard — the JMP is dead for ascending loops. (Matches observation:
   ascending runtime bounds work.)
2. Step-vs-direction **disagreement** (e.g. `range(5, n=9) step -1`): the
   `sameSign` test (`:2788`) skips the guarded loop entirely — the run-once
   prologue handles it, one body execution, by design. The JMP is never
   reached. (A runtime-bounds range loop therefore never iterates zero
   times: either the start passes the first guard — it always does when the
   direction was *derived* from the bounds, since `dir < 0` implies
   `start > end` — or the mismatch path runs once.)
3. Descending **with agreeing step**: every iteration that passes the `GE`
   guard falls into `JMP 0` and spins toward instruction 0; the guard itself
   never spins (it exits correctly). The loop never reaches the body.


`QLangBytecodeEmitter::patchJump(idx, target)` rewrites `operands[0]` to
`target - idx - 1`; the JMP's index was simply never captured, so nothing
ever patched it — the Defect comment itself records exactly that.

## Fix (implemented in working tree, verified)

Two hunks in `src/QLangCompiler.cpp::compileForIn`:

```cpp
int backSkipIdx = static_cast<int>(m_emitter.size());   // capture, before emit
emit(Opcode::JMP, 0);
...
m_emitter.patchJump(backExitIdx, guardExit);
m_emitter.patchJump(fwdExitIdx, guardExit);
// descending + in-range fell through the GE guard: enter the body
m_emitter.patchJump(backSkipIdx, bodyStart);
```

The stale Defect comment gets replaced with the rationale for the patch.
Targeting `bodyStart` is safe: the only fallthrough into the JMP is
"descending guard passed", which is exactly what the continue edge
(`JMP guardLabel` → guard → fallthrough) re-enters every iteration; failed
guards exit at the `GE` compare, and the direction-disagreement shape never
reaches the JMP (it takes the run-once prologue).

**Verified:** `d1.ql` prints `5 4 3` and exits 0; full
`ctest --test-dir build` **67/67 green** (including `codegen_size` — the
operand patch adds no instructions — and `doc_hashes`). The literal-bounds
twin and ascending cases are byte-identical.

## Test guard (added with the fix, `tests/unit/range_check.cpp`)

`range_check.cpp` had **no `step` cases at all** before this fix:

```cpp
expectVal("runtime desc 5..n(3) sum", "local n=3\nlocal s=0\nfor i in range(5,n) step -1{ s += i }\n", "s", 12); // 5+4+3
expectVal("runtime dir-mismatch run-once", "local n=9\nlocal c=0\nfor i in range(5,n) step -1{ c += 1 }\n", "c", 1); // run-once, by design
expectVal("runtime step expr", "local st=-2\nlocal s=0\nfor i in range(10,5) step st{ s += i }\n", "s", 24); // 10+8+6
```

(The second case corrected a false expectation mid-fix: a runtime-bounds
range loop never iterates zero times — direction mismatch is the
run-once prologue, direction agreement always passes the first guard.
That behavior is by design at `:2788-2796` and unchanged by this fix.)

## Suggested commits

- `fix(compiler): patch the descending-guard skip-JMP in compileForIn` (code + range_check cases)

## Related records to update when the fix commits

- `docs/plans/todo.md` §6: flip the "`compileForIn` emits an unpatched
  skip-JMP" row.
- `docs/compliance/status.md` Round 70 "still open" clause: retire.
- Move this file to `docs/bugs/resolved/` per AGENTS.md once committed.
