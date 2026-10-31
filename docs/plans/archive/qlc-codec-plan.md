# Plan: replace `.qlc` format + consolidate serializer into the library

Status: **proposed** (2026-07) — not started
Supersedes: the POC `QlcWriter` in `examples/qlang_run.cpp`
Reference spec: `docs/SERIALIZATION.md` (draft) — this plan amends §3 and §4.1
Operand map: `docs/spec/13-bytecodes.md` — "Instruction operand map"
section (per-opcode parameters for the proposed 12-byte layout)

## Context

- `docs/SERIALIZATION.md` already specs the `QLC1` v1 program image (header,
  functions, native-slot table, vars, constVars, registerCount) and the load
  contract. `include/qlang/QLangBytecodeIO.h` already declares
  `serializeValue` / `deserializeValue` / `writeQlc` / `readQlc` — orphaned:
  no `src/QLangBytecodeIO.cpp`, not in CMakeLists, no callers.
- The app's `QlcWriter` is the POC reference (spec §10): it works but is
  missing the §4.2 native-slot table and uses a 28-byte instruction encoding.
- `QLangErrorKind::Io` already exists (spec §10 status row is stale).
- This plan pairs with the `Instruction` restructure below: a 12-byte struct
  with a `b/c | i32` union so any opcode can carry a 32-bit parameter
  (today: jump targets).

## New `Instruction` layout

```cpp
struct Instruction {
  Opcode op = Opcode::CONST; // uint16_t
  int16_t a;
  union {
    int16_t b, c;
    int32_t i32;  // combined b/c for opcodes with 32-bit operands
  };
  uint16_t line, col;
};
```

- 12 bytes, alignment 4, **no padding** (union sits at offset 4).
- Anonymous union: `ins.b` / `ins.i32` alias. Convention: each opcode writes
  only the form it uses (data opcodes → `b`/`c`; jump family → `i32`); no
  accessor helpers.
- **Operand-slot rule:** any opcode that needs a 32-bit parameter uses the
  combined `i32` slot; all others use `a`/`b`/`c` as `int16_t` (registers ≤
  32,767, native ids via negative `b`, arg counts / type ids / pool indices
  in `b`/`c`). The single source of truth is a `usesI32(Opcode)` helper in
  `QLangBytecode.h`, shared by compiler, VM, disassembler, and codec.
  - **Per-opcode parameters:** the full table lives in
    `docs/spec/13-bytecodes.md` ("Instruction operand map") — the plan
    defers to it; update the table there first if an assignment changes.
  - **Current 32-bit users: jump targets** (pc can exceed 32,767):
    `JMP` / `CONTINUE` / `BREAK` (target in `i32`, `a` unused);
    `JMPIFFALSE` / `JMPIFTRUE` (condition in `a`, target in `i32`).
  - Future opcodes with 32-bit parameters join by adding their case to
    `usesI32` — no struct change needed.

## Ratified deviations from the spec

1. **§4.1 instruction encoding → 12 bytes on disk:**
   `(u16 op, i16 a, i32 b/c, u16 line, u16 col)` — i.e. the struct's raw
   bytes (little-endian, padding-free). Replaces the specced
   `u8 op + 5×i32` (28 bytes). The format never shipped (no reader exists;
   the only v1 files are the regenerable `test.qlc` / `test-.qlc` in the repo
   root), so draft-v1 is amended rather than a v2 invented.
2. **§3 tag table corrected to the real `QLangValue::Type` enum:**
   `0=Nil 1=Bool 2=Char 3=Byte 4=Word 5=Short 6=Int 7=Unsigned 8=Qword
   9=String 10=Blob 11=Array 12=Float 13=Table 14=Function 15=Generator
   16=Object 17=Eof 18=Long 19=Set`.
   Non-durable → emit tag 0 (Nil): **Generator** (R2) and **Object** (class
   instances carry runtime state/finalizers — lossy by design; confirm before
   implementing). **Set** is durable with the Array payload shape
   (`u32 count` + values).
3. Depth limit is **64** per spec §8; the header comment says 128 — fix the
   comment to match the spec.

## Commit 1 — restructure `Instruction` (prerequisite)

- [ ] `include/qlang/QLangBytecode.h`:
  - [ ] `enum class Opcode : uint16_t`
  - [ ] `Instruction` per the layout above (12 bytes)
  - [ ] add `kOpcodeCount` sentinel (spec §5.1 references it; does not exist)
  - [ ] add `usesI32(Opcode)` helper (jump family for now)
- [ ] `src/QLangCompiler.cpp`:
  - [ ] emit jump-family targets via `i32` (per the slot assignment above)
  - [ ] per-opcode range check at the single emit/addInstruction site:
        `int16_t` bounds for `a`/`b`/`c` opcodes (throw `QLangError`, never
        wrap); `i32` opcodes need no practical bound
- [ ] `src/QLangVM.cpp`: decode jump-family targets from `i32`
- [ ] `examples/qlang_run.cpp`: disassembler `operandMask` / `pseudoText` /
      operand printing for the jump family (target from `i32`)
- [ ] Verify compiler emit sites, VM decode sites, and disassembler operand
      text against the operand map in `docs/spec/13-bytecodes.md`
- [ ] Build (`cmake -B build && cmake --build build`) + `ctest --test-dir build`
- Commit: `refactor: 12-byte Instruction with b/c|i32 union; 32-bit jump targets`

## Commit 2 — library codec + format replacement

- [ ] New `src/QLangBytecodeIO.cpp`: implement the declared API per spec
      §3–§5 and §8:
  - [ ] `serializeValue` / `deserializeValue` (tag table per deviation 2;
        non-durable → tag 0; tables in `std::map` order per R3)
  - [ ] `writeQlc` — full §4 layout **including the §4.2 native-slot table**
        (name per slot, ascending, no duplicates, non-empty names, null slots
        omitted); instructions written as raw 12-byte struct records
        (little-endian, padding-free — no per-field encoding needed)
  - [ ] `readQlc` — untrusted-input reader: magic/version check
        (`version > 1` → unsupported, never parse), bounds-checked reads with
        0-based byte offsets in errors, structural invariants (spec §5.1:
        `funcCount >= 1`, `functions[0].name == "main"`, `op < kOpcodeCount`,
        closure `funcIndex` in range, unique native slots, non-negative
        counts), §8 limits (depth 64, 64 Mi values, 64 Mi instructions);
        out-param left untouched on any failure
- [ ] `include/qlang/QLangBytecodeIO.h`: fix depth comment 128 → 64
- [ ] `CMakeLists.txt`:
  - [ ] add `src/QLangBytecodeIO.cpp` to the `qlang` library
  - [ ] new `codec_check` executable + `add_test`
- [ ] `examples/qlang_run.cpp`: delete `QlcWriter` (~120 lines); call
      `writeQlc(os, compiled, compiler.nativeFuncIds())`
- [ ] New `examples/codec_check.cpp` (spec §9, scaled):
  - [ ] value round-trip: every durable type, nested arrays/tables (depth ≥ 60),
        closures, empty containers → structural equality
  - [ ] program round-trip: compile a script using natives → `writeQlc` →
        `readQlc` → structural compare → execute with natives wired (the
        `qlang_run.cpp` wiring pattern) → stdout byte-identical to direct run
  - [ ] malformed suite (each → clean `Io` error, no crash, no partial
        out-param): truncation at section boundaries, bad magic, version 2,
        `op >= kOpcodeCount`, dangling closure index, duplicate native slot,
        negative count, 100-deep nested array
- [ ] `docs/SERIALIZATION.md`: update §3 tag table, §4.1 encoding, §10 status
- [ ] Regenerate repo-root `test.qlc` / `test-.qlc` with the new writer
- [ ] Build + full `ctest`
- Commit: `feat: .qlc codec in library (writeQlc/readQlc); retire app QlcWriter`

## Deferred (explicitly out of scope)

- `QLangContext::loadProgram` / `execute` (spec §6) — the round-trip test
  wires natives manually; the header comment referencing `loadProgram` stays
  as a draft pointer.
- State snapshot `QLS1` v2 (spec §7).

## Risks

- Union aliasing: `b`/`c`/`i32` are the same bytes; a future opcode that
  mixes forms (e.g. reads `c` after writing `i32`) silently misdecodes. The
  per-opcode convention above is the only guard — the `usesI32(Opcode)`
  helper in `QLangBytecode.h` is the single source of truth so the VM,
  compiler, disassembler, and codec agree by construction.
- Existing `test.qlc` / `test-.qlc` are unreadable by the new codec until
  regenerated (step in commit 2).
- Object → Nil is lossy; any test corpus persisting class instances in
  constants will show it in round-trip.
- Spec doc and codec land in the same commit — acceptable because the spec is
  a draft, but it rewrites a "fixed" section.
