# MOVI for compiler-generated Int constants (step 4.4) — implementation plan

Status: **proposal, not implemented**. Follow-up to
`movi-implementation-plan.md` (MOVI itself is implemented, Round 30); this is
the deferred optional step 4.4 from that plan.

Spec: `docs/spec/13-bytecodes.md` § "new opcode: MOVI" (implemented). No new
opcode — pure codegen change.

## What this does

The compiler emits `CONST` + a constant-pool `Int` slot for values it
generates itself (loop counters, class metadata markers). All of them are
`Type::Int` and fit `i32`, so each becomes a `MOVI` — same value, same
instruction count (1:1), **no pool slot**. Behavior is unchanged by
construction: `MOVI r, N` produces exactly `Int(N)`, which is what the pool
path produced.

## The 13 sites (line numbers at planning time; re-verify)

All in `src/QLangCompiler.cpp`, all of the form
`emit(Opcode::CONST, r, emitConst(QLangValue::Int(N)))` →
`emitImm(Opcode::MOVI, r, N)`:

| Line(s) | Site | Value |
|---------|------|-------|
| 1085, 1278 | `for` loop default step reg (two codegen branches) | 1 |
| 1088, 1281 | `for` loop zero reg | 0 |
| 1096, 1288 | `for` loop direction reg (descending) | -1 |
| 1102, 1294 | `for` loop direction reg (ascending) | 1 |
| 1390 | `for-in` index counter init | 0 |
| 1894 | class `__static_const__` marker value | 1 |
| 1924 | class `__const_fields__` marker value | 1 |
| 1973 | class `__fields__` index value (per field, `Int(i)`) | 0..n |
| 3071 | `ab[]` append index (unused in the append branch, but emitted) | 0 |

## Impact assessment

| Area | File(s) | Change | Risk |
|------|---------|--------|------|
| Compiler | `src/QLangCompiler.cpp` | 13 one-line swaps to `emitImm(Opcode::MOVI, …)` | **Low.** Same value, same instruction count; `emitImm` already stamps line/col |
| Bytecode / VM / codec | — | none — MOVI already lands end-to-end | — |
| Tests | `examples/codec_check.cpp` | Pool-shrink assertion (below) | — |
| Docs | `docs/spec/13-bytecodes.md` (one-line note in the MOVI section), `docs/spec/README.md` hash, `docs/compliance/status.md` entry | Per AGENTS.md: test first, then hash | — |

Expected effect: pool entries `0`, `1`, `-1` (deduped — one slot each no
matter how many sites use them) and the per-class `0..n` field indices
disappear from the pool. Instruction count is unchanged (CONST→MOVI is 1:1).
Reference: `hello.ql` compiled pool 48 → ~45 constants (main 42 → 39).

Not affected: user literals (already MOVI since Round 30), `INC`/`DEC`,
register allocation, jump backpatching, `valueKey`/pool dedup.

## Implementation steps

1. `src/QLangCompiler.cpp` — swap the 13 sites (grep
   `emitConst(QLangValue::Int(` to find them all; the list above is complete
   at planning time).
2. Tests — see below. Build `cmake -B build && cmake --build build`, run
   `ctest --test-dir build` plus `build/spec_check2` and `build/codec_check`.
3. Docs — one-line note in the spec MOVI section ("compiler-generated loop /
   class-metadata Ints also use MOVI"), hash rows, compliance entry.

## Test cases

- `codec_check` (bytecode level): compile a program with a `for` loop and a
  class, e.g. `for x = 1 to 3 { local s = 0 }` + a two-field class → assert
  the main function's constant pool contains **no `Type::Int` entries at all**
  (user literals were already MOVI; this removes the last compiler-generated
  ones).
- `spec_check2`: no new behavior cases needed (behavior is unchanged) — the
  existing loop/class suites are the regression guard.
- Existing `spec_check2` MOVI section + all suites stay green.

## Risks / open questions

1. **Tiny, deduped win** — the pool saves ~3–5 slots per program, not one per
   site. The point is hygiene (no Int slot in the pool, period), not size.
2. **L1973 is data-driven** (`Int(i)` per field) — `MOVI` handles any i32,
   no special case needed; field count > INT32_MAX is impossible in practice.
3. Independent of the ADDI plan (`addi-implementation-plan.md`) — either
   order works; both touch only `QLangCompiler.cpp` codegen.
