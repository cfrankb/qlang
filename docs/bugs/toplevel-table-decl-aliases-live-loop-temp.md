# Top-level name declaration can alias a live main-frame loop temp (register overwrite)

**Date:** 2026-11-15
**Severity:** high (correctness — an inner loop variable silently shares the
enclosing loop's hidden counter/bound register)
**Status:** open — discovered while landing the nested-loop break/continue fix
(`docs/bugs/resolved/nested-loop-break-continue-hijack.md`); per the Q54
convention (divergences found mid-round are bug reports, not drive-by fixes)
it is filed, not fixed. The round's `nest forin-array-for break` test runs
inside a function to test the hijack semantics without tripping this bug.

## Symptom

```ql
local s = ""
for x in [1,2] {
  for y = 1 to 3 {
    if y == x { break }
    s = s + toStr(x) + toStr(y) + ","
  }
}
print(s)
```

- Expected: `21,`
- Actual:   `` (empty — the outer for-in visits only `x = 1`)

Repro: `/tmp/opencode/probeE.ql`; run with `build/qlcc --run`. The same
program inside `def f()` prints `21,` correctly
(`/tmp/opencode/probeF.ql`) — the bug is top-level (main) only.

## Evidence

Trace (`qlcc --trace-level 3`): the inner loop's `y` takes global-table slot 2
while the outer collection for-in's hidden index counter `idx` is a live temp
on r2:

```
[DECL] line=1 's' -> r=0 (table)
[DECL] line=2 'x' -> r=1 (table)
[DECL] line=3 'y' -> r=2 (table)   ; == the live idx temp
```

Disassembly (`qlcc -a --asm-level 4`): `seqget 01, 03, 02` (outer read of
`x = [1,2][idx]`), the inner guard/step and the outer `inc 02` all operate on
r2 — the inner `for y` clobbers the outer counter mid-loop. (The BREAK at
`002d` correctly targets the *inner* exit `0053`; the break-patch fix is fine
— this is an independent defect.)

## Root cause

`QLangCompiler::declareVar`'s top-level table path
(`src/QLangCompiler.cpp:987`, `globalSlotFor`) accepts whatever slot the
global table appends; unlike the two other named-declaration paths
(`QLangCompiler.cpp:959` block-scoped, `:1013` function-local), it never
checks `m_liveTmps`. At top level the main frame *is* the global table
(the table slot IS the register), so a name first assigned inside a loop body
gets the next table slot — and any loop temp the compiler holds live across
the body can sit exactly there. Shapes with body-live hidden temps:
the collection `for-in`'s `idx`/`colReg`/`lenReg` counters and the hoisted
`for`'s `endReg`/`stepReg`.

## Candidate fix (its own round)

The table slot and the frame register are the same cell, so the fix is not
just a skip-loop in `declareVar`: either the top-level temp allocator must
stay above the table's *current* high-water mark and `declareVar` must reclaim
downward as today, or a table-slot collision must force the live temp to be
reallocated/moved before the table entry materializes. Needs red-first tests
covering: collection for-in + inner loop-var decl, hoisted `for` with runtime
bounds + inner decl, and repeated decls inside the same body.
