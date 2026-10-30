# MOVI opcode — impact assessment & implementation plan

Status: **implemented** (Round 30, 2026-10-29; all steps 1–7 landed, suites
green: `spec_check2` 381, `codec_check` all pass, ctest 5/5).
Spec: `docs/spec/13-bytecodes.md` § "new opcode: MOVI" (SHA256
`7a6ae7c5…`, recorded in `docs/spec/README.md`).
Deviations: compiler entry point is `emitImm()` (a 3-arg `emit`/`add` overload
would be ambiguous with the defaulted b/c parameters); the optional
step-4.4 compiler-generated `Int(0/1/-1)` sites were not converted (pure pool
shrink, no behavior change — deferred).

## What MOVI does

`movi` loads a 32-bit integer literal straight into a register, skipping the
constant pool:

- `Instruction.a` = destination register, `Instruction.i32` = immediate.
- Fits the existing 12-byte `Instruction` layout exactly — **no struct change**.
- Only `Type::Int` (int32) values are eligible. Everything else (nil, Bool,
  Byte/Word/Short/Char, Unsigned, Long/Qword, Float, strings, tables, …)
  stays in the pool via `CONST` (`movc`).

Eligibility is deliberately `Type::Int` only, even though Byte/Word/Short/
Char/Unsigned also fit in 32 bits: MOVI must produce a value with the same
type tag as the pool path would. A `true` literal is `Type::Bool` stored as
Int 1/0; emitting it via MOVI would change its tag and break `type()` and
serialization. `Long`/`Qword` (64-bit) do not fit `i32` at all.

## Impact assessment

| Area | File(s) | Change | Risk |
|------|---------|--------|------|
| Opcode enum | `include/qlang/QLangBytecode.h` | Add `MOVI` **before `kOpcodeCount`** (append at end); add to `usesI32()`; update header comment (i32 users) | **Enum position is load-bearing**: `.qlc` writes the raw opcode number. Inserting mid-enum renumbers every later opcode and breaks all existing `.qlc` files. Appending keeps old files valid. |
| Bytecode core | `src/QLangBytecode.cpp` | `name()`: `case MOVI: return "movi"`. New `Bytecode::add(Opcode, int a, int32_t imm)` overload — the existing `add(op,a,b,c)` maps `i32` from `a`/`b` for the jump family only; MOVI needs its own path | Low. `usesI32` already drives the raw-layout serialization, so no codec change for the operand encoding itself. |
| VM dispatch | `src/QLangVM.cpp` | Add `case Opcode::MOVI: frame.regs[i.a] = QLangValue::Int(i.i32); break;` at **all four** dispatch sites (next to each `case Opcode::CONST`: lines ~1371 in `run()`, ~1698 in `runFrames()`, ~2070, ~2407 — generator/step-limit loops). Missing one = silent fall-through to the unknown-op throw | Medium: 4 sites, easy to miss one; a test that exercises a generator containing an int literal covers it |
| Compiler | `src/QLangCompiler.cpp` | 1) `emit()` overload for i32 immediates (stamps line/col like the existing one). 2) `compileExpr` `ExprKind::Literal` (line ~2216): `Type::Int` → `emit(MOVI, dest, v.asInt())`, else current `CONST` path. 3) Optional: inline Int literals in `compileAssignTarget` (line ~3017) so `local a = 0` emits one `movi` into the target reg instead of `movc tmp` + `mov` — this is the spec's "−2 instructions" example. 4) Compiler-generated `Int(0/1/-1)` sites (~12, loop codegen lines ~814, 1056–1073, 1249–1265, 1361, 1865, 1895) may also switch to the immediate form — pure pool shrink, no behavior change | Low. In-place `CONV dest,dest` (for `local a:byte = 0`) is safe — VM reads `regs[b]` before writing `regs[a]` |
| .qlc codec | `src/QLangBytecodeIO.cpp` | Writer: bump format version 1 → 2. Reader: accept `version <= 2`. MOVI serializes automatically (u16 op + 12-byte raw layout); reader's `op >= kOpcodeCount` check (line ~419) already rejects unknown ops | **Version bump is the clean break**: a new `.qlc` containing MOVI loaded by an old tool fails with "unsupported version" instead of an opaque unknown-opcode error. Old files (v1, no MOVI) load fine in the new tool. |
| Tests | `examples/spec_check2.cpp`, `examples/codec_check.cpp` | New cases (below) | — |
| Docs | `docs/spec/13-bytecodes.md`, `docs/spec/README.md`, `docs/compliance/status.md` (+ history) | Finalize MOVI section (drop "DO NOT IMPLEMENT YET"), add MOVI row to the operand map, update `usesI32` user list, record hash, add compliance round entry | Per AGENTS.md: **test first, then bump hashes** |

Not affected: register allocation (MOVI uses one dest reg like CONST), jump
backpatching (MOVI is not a jump), `valueKey`/pool dedup, `setConstants`,
the decompiler (not in the repo — the spec's decompiler-mnemonics and
line/col-separation sections remain separate DO-NOT-IMPLEMENT proposals).

## Implementation steps

1. **`QLangBytecode.h`** — append `MOVI` before `kOpcodeCount`; add to
   `usesI32()`; comment.
2. **`QLangBytecode.cpp`** — `name()` case; `add(Opcode, int, int32_t)`
   overload setting `inst.i32`.
3. **`QLangVM.cpp`** — MOVI case in all four dispatch loops.
4. **`QLangCompiler.cpp`** — `emit` i32 overload; `compileExpr` literal
   branch (`Type::Int` → MOVI); assign-target literal inlining (step 4.3
   above); optional: switch the ~12 compiler-generated `Int` sites.
5. **`QLangBytecodeIO.cpp`** — writer version 2, reader accepts ≤ 2.
6. **Tests** — see below. Build `cmake -B build && cmake --build build`,
   run `ctest --test-dir build` plus `build/spec_check2`.
7. **Docs** — finalize spec section, hash rows, compliance entry.

## Test cases

- `local x = 0` / `= 1` / `= -1` / `= INT32_MAX` / `= INT32_MIN` → bytecode
  contains `MOVI` with the right `i32`, constant pool **shrinks** (no Int
  slot), value runs correctly.
- `local b = true` → still `CONST`/pool with `Type::Bool` (regression guard
  for the type-fidelity rule).
- 64-bit literal (Long) → still pool.
- Int literal used inside a **generator** and inside a **loop** → exercises
  all four dispatch sites.
- `local a:byte = 0` → `movi` + in-place `conv` (or `movi` only) runs clean.
- `.qlc` round-trip of a program containing MOVI (codec_check): write v2,
  read back, instruction-for-instruction equality; a hand-crafted v1 file
  with an out-of-range op number is rejected.

## Risks / open questions

1. **Enum append vs. spec wording** — spec doesn't state placement; append is
   the only choice that keeps existing `.qlc` files valid.
2. **Version bump policy** — bumping to 2 is a judgment call (spec is silent);
   alternative is staying at 1 and relying on the unknown-opcode check.
   Recommend bump.
3. **Unsigned literals** — `Type::Unsigned` fits in `i32` bits but needs a
   `MOVIU`-style variant or a type side-band to keep the tag; defer.
4. The spec's "decompiler mnemonics" (`r`/`c` prefixes) and the 8-byte
   instruction / parallel `SourcePos` rework are **separate proposals** in the
   same file — do not bundle into this change.
