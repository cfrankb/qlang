# Nested loops: outer loop's break/continue scan re-patches the inner loop's already-patched jumps

**Date:** 2026-11-15
**Severity:** high (correctness — a nested `break` exits *both* loops)
**Status:** open — discovered while implementing the direction-hoist plan
(`docs/plans/archive/for-direction-hoist-plan.md` M1); deliberately **not** fixed there
(Q54: divergences found mid-plan are bug reports, not drive-by fixes).
**Cited code at commit:** `b247fbf` (reproduces identically with the uncommitted
M1 working tree — the behavior predates and is untouched by that change)

## Symptom

```ql
local s = ""
for i = 1 to 3 {
  for j = 1 to 5 {
    if j == 3 { break }
    s = s + toStr(i) + toStr(j) + ","
  }
}
print(s)
```

- Expected: `11,12,21,22,31,32,` (inner `break` exits only the `j` loop)
- Actual:   `11,12,` (first inner `break` exits both loops)

Repro: `/tmp/opencode/nest2.ql`; run with `build/qlcc --run`.

## Disassembly evidence

`build/qlcc -a --asm-level 4` of the repro:

```
  L2:  for i = 1 to 3 {
  000f  cmp          01, 03     ; outer guard
  0012  jg           42 -> 0056 ; outer exit = 0056

  L3:    for j = 1 to 5 {
  001f  cmp          02, 05     ; inner guard
  0022  jg           2c -> 0050 ; inner exit = 0050
  ...
  L4:      if j == 3 { break }
  002a  break        2a -> 0056 ; <-- patched to the OUTER exit (should be 0050)
  ...
  0050  add          e=0x40, 01, 1 ; r1 = r1 + 1   ; outer step
  0054  jmp          -71 -> 000f
  0056  ...print...               ; program tail
```

The inner loop compiles fully (and patches its own BREAKs to 0050) *while
inside* the outer loop's body region. The outer loop then scans the same
instruction range looking for its own BREAK/CONTINUE markers and finds the
inner loop's already-patched ones, re-patching them to the outer exit.

## Root cause

Every loop's body scan keys only on the opcode, e.g.

```cpp
// src/QLangCompiler.cpp:2890-2891 (compileFor bucket-3 path; same pattern at
// ~2754, 2768, 2800, 2897, 2905, 2981, 3020, 3027, 3072, 3085, 3108, 3171,
// 3206, 3218 and in compileForIn/while/do-while):
if (ci.op == Opcode::CONTINUE) m_emitter.patchJump(j, bodyEnd);
else if (ci.op == Opcode::BREAK) m_emitter.patchJump(j, loopExit);
```

A BREAK/CONTINUE that an inner loop already patched (non-zero `jumpTarget`) is
indistinguishable from a fresh marker, so any enclosing loop steals it. The
first emitted loop to patch "wins", and outer loops scan last, so outer always
wins. (BREAK happens to encode as a jump; CONTINUE has the same hijack, see
`/tmp/opencode/nest.ql`.)

## Candidate fix (its own round)

Guard every scan site with the zero-target sentinel:

```cpp
else if (ci.op == Opcode::BREAK && ci.jumpTarget == 0)
    m_emitter.patchJump(j, loopExit);
```

(a `continue` that legitimately jumps to offset 0 would need a separate
sentinel check first, or switch `patchJump` to record patch state). ~14 scan
sites; each loop's scan then claims only its unpatched markers. Needs nested
break+continue tests across all loop forms (`for`, `for ... step`, `for in`,
`while`, `do`) at two and three nesting levels before landing.

## Why filed, not fixed here

The direction-hoist plan (Q54) freezes semantics: this bug exists identically
on HEAD `b247fbf` and after the M1 change, so it must be fixed in a round with
its own red-first tests, not smuggled into the hoist commit.
