# ADDI — add 32-bit immediate to register — implementation plan

Status: **implemented** (Round 31, 2026-10-29; all steps landed, suites
green: ctest 5/5, `spec_check2` 391/0, `codec_check` all pass). Companion to
`movi-implementation-plan.md` (MOVI is implemented, Round 30); reuses its
exact operand layout and every part of its plumbing.

Spec: `docs/spec/13-bytecodes.md` § "new OpCode addi" (added; hash row in
`docs/spec/README.md` stays stale until implementation + tests land, per
AGENTS.md).

## What ADDI does

`addi a, imm` → `r[a] = r[a] + Int(imm)` — in-place addition of a 32-bit
integer immediate.

- Operand layout is **identical to MOVI**: `a` = register, `i32` = immediate,
  `b`/`c` unused. `usesI32()` covers it. No instruction-struct change.
- Semantics are **exactly** the current `movi tmp, imm; add a, a, tmp`
  sequence: the VM routes ADDI through the same ADD binary path
  (`doBinary(Opcode::ADD, …)`), so string-left concat, float promotion,
  array+number type error, and int32 wrap-around all behave identically.
- Only `Type::Int` literals are eligible (same rule as MOVI). Bool, Float,
  string, etc. keep the pool path.

## Impact assessment

| Area | File(s) | Change | Risk |
|------|---------|--------|------|
| Opcode enum | `include/qlang/QLangBytecode.h` | `ADDI` appended **before `kOpcodeCount`** (same load-bearing rule as MOVI); `usesI32()`; comment | **Low** — append-only; existing `.qlc` opcode numbers unchanged |
| Bytecode core | `src/QLangBytecode.cpp` | `name()` → `"addi"` | Low |
| VM | `src/QLangVM.cpp` | 4 dispatch sites (exec + 3 inline copies): `case Opcode::ADDI: frame.regs[i.a] = doBinary(Opcode::ADD, frame.regs[i.a], QLangValue::Int(i.i32), this); break;` | **Medium** — 4 sites, same pattern as MOVI; generator + loop tests are the guard |
| Compiler | `src/QLangCompiler.cpp` | Compound `+=` with an Int-literal RHS (see scope below) | Low |
| `.qlc` codec | `src/QLangBytecodeIO.cpp` | See version-bump question below — likely **no change** | Policy decision |
| Decompiler | `examples/qlang_run.cpp` | `operandText`: ADDI shares MOVI's `a, immi` form; `pseudoText`: `r[a] = r[a] + imm` (spec § "new OpCode addi" shows `r0 = r0 + 12`, matching the `add` pseudo style); `operandMask`: MOVI/ADDI share the a+i32 case | Low (missed for MOVI — fixed after the fact; included here up front) |
| Tests | `examples/spec_check2.cpp`, `examples/codec_check.cpp` | Below | — |
| Docs | `docs/spec/13-bytecodes.md` (§ + operand-map row + i32-users list), `docs/spec/README.md` hash, `docs/compliance/status.md` | Per AGENTS.md: test first, then hash | — |

Not affected: register allocation (in-place, **no temp**), `INC`/`DEC`
(still the `++`/`--` path), jump backpatching, pool dedup, `setConstants`.

## Compiler emission scope (deliberately narrow)

`op == "+=" && isIntLiteral(s->assignValue)` — four sites:

| Line(s) at planning | Site | Before | After |
|---|---|---|---|
| ~970 | plain `a += 12` | `movi t, 12; add a, a, t` | `addi a, 12` (−1 instr, −1 temp) |
| ~923 | captured var `cap += 12` (closures) | `refload cur; movi t, 12; add cur, cur, t; refstore` | `refload cur; addi cur, 12; refstore` |
| ~3076 | index `a[i] += 12` (after `ARRGET tmp`) | `movi t, 12; add tmp, tmp, t` | `addi tmp, 12` |
| ~3105 | member `t.k += 12` (after `TABGET tmp`) | `movi t, 12; add tmp, tmp, t` | `addi tmp, 12` |

Explicitly **out of scope**:

- General binary expressions `e1 + 12` — `compileExpr` already puts `e1` in
  a temp, so `addi tmp, 12; mov dest, tmp` is the same instruction count as
  `movi + add`. No win.
- `+= <non-Int literal>` (float, string) — stays on the pool/ADD path.
- `+= 1` → `INC` — no; ADDI is uniform, `INC`/`DEC` stay dedicated to
  `++`/`--`.

## Pre-existing bug to fix in the same edit

`src/QLangCompiler.cpp` ~L923 (captured-variable compound branch):

```cpp
Opcode opCode = (op == "++") ? Opcode::INC : Opcode::DEC;   // BUG
```

Any compound op other than `++` on a captured variable compiles to
`INC`/`DEC` — `cap += 5` currently behaves as `cap++`. `binOp()` is never
consulted. The ADDI captured-var site must select
`opCode = (op == "++") ? INC : (op == "--") ? DEC : binOp(op.substr(0, op.size()-1))`
(with the Int-literal `+=` shortcut to ADDI) — a behavior change, so it gets
its own `spec_check2` case (`closure var += 5` → +5, not +1).

## `.qlc` version-bump question

MOVI landed at format **v2**, which is **uncommitted** as of this plan.
Decision:

- **Default (recommended): no bump.** ADDI ships in the same uncommitted
  v2. Reader keeps `accept ≤ 2`.
- If v2 gets committed/released before ADDI lands: bump writer to 3, reader
  to `accept ≤ 3`, and move the `codec_check` "unsupported version" case to
  4.

## Implementation steps

1. `include/qlang/QLangBytecode.h` — `ADDI` before `kOpcodeCount`,
   `usesI32()`, comment.
2. `src/QLangBytecode.cpp` — `name()`.
3. `src/QLangVM.cpp` — 4 dispatch sites (grep `case Opcode::ADD:` — the
   ADDI case goes directly above it in each).
4. `src/QLangCompiler.cpp` — 4 emission sites + the L923 bug fix (above).
5. `src/QLangBytecodeIO.cpp` — only if the v2-shipped case applies.
6. `examples/qlang_run.cpp` — `operandText` / `pseudoText` / `operandMask`.
7. Tests — see below. Build `cmake -B build && cmake --build build`, run
   `ctest --test-dir build` plus `build/spec_check2` and `build/codec_check`.
8. Docs — spec § + operand-map row + i32-users list, hash rows, compliance
   entry.

## Test cases

`spec_check2`:

1. `local a = 10; a += 12` → 22
2. `local a = 10; a += -5` → 5 (negative immediate)
3. `local f = 1.5; f += 1` → float 2.5 (float promotion preserved)
4. `local s = "x"; s += " y"` → `"x y"` (string path untouched, no ADDI)
5. `local a = [10, 20]; a[1] += 5; local r = a[1]` → 25 (index target)
6. `local t = {k: 1}; t.k += 4; local r = t.k` → 5 (member target)
7. closure: `local x = 1` captured, `x += 5` inside → 6 (**regression for
   the L923 bug** — previously would be 2)
8. loop: `local s = 0; for x = 1 to 10 { s += 3 }` → 30 (all 4 dispatch
   sites)
9. generator: `g()` with `y += 7` across yields → value check
10. overflow wrap: `local a = 2147483647; a += 1` → −2147483648
    (int32 wrap, identical to ADD)

`codec_check`:

1. `a += 12` compiles to ADDI with `i32 == 12`; no Int(12) pool slot
2. `a += 1.5` does **not** become ADDI (stays pooled + ADD)
3. `writeQlc`/`readQlc` round-trip of an ADDI program — instruction-for-
   instruction equality (opcode, a, i32)

## Risks / open questions

1. **VM dispatch ×4** — the inline-loop copies are the classic source of
   divergence; the generator/loop test cases are the guard.
2. **Version policy** — resolve before step 5 (see above).
3. **L923 bug fix is a behavior change** — `cap += n` / `cap -= n` /
   `cap *= n` etc. on captured variables start working correctly. Flag it in
   the compliance entry so the tally change is explained.
4. Independent of `compiler-int-immediates-plan.md` — either order works.
