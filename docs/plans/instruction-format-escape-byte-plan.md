# Instruction Format — Escape-Byte Encoding Plan

Status: **Implemented** (`.qlc` v5, Round 37) — §4, §5.3 and §13.4 are **superseded** by
`docs/plans/encoding-v6-and-indirect-plan.md` (`.qlc` v6, specified 2026-11-06, not
implemented); everything else in this document remains the authoritative record of the shipped
format
Supersedes: Phase 2–4 (fixed-width 4/8 B) of `docs/plans/instruction-format-optimization-plan.md`
Builds on: Phase 1 (line/col side-table, `.qlc` v4) — **DONE** (Round 36)
Reference spec: `docs/spec/13-bytecodes.md` (operand map, `## encoding extended`)

---

## 1. Design goals

1. Compact common case without a fixed-width tax on small immediates / nearby jumps.
2. **No hard register cap.** 256 is an *encoding boundary* (1-byte vs 2-byte), not a limit —
   the register file may grow to 65 535. This removes the need for a register spill path.
3. Variable-width immediates that map directly onto QLang's integer types.
   **This version: signed `Int` only** (1/2/4-byte, sign-extended). `Unsigned`/`Long`/`Qword` and
   the unsigned forms are reserved in the encoding for future versions.
4. **Relative** short/long jumps (asm-compiler style), not absolute 32-bit targets.
5. **One** register pool and **one** constant pool. "Main / extended" is only the
   *addressing width* (1-byte vs 2-byte), not separate pools.
6. A **single escape mechanism** (top opcode bit + one modifier byte) for every widened form.
   **No escape bit ⇒ both operands are 1-byte.**

---

## 2. Byte-stream model

The instruction stream is a **flat byte array**; `pc` is a **byte offset**. This replaces the
current fixed 8-byte `std::vector<Instruction>` (`include/qlang/QLangBytecode.h:232`). Variable
length is what enables relative jumps and narrow immediates. The decode loop reads a per-opcode
width (table-driven), not a fixed stride.

---

## 3. Opcode encoding

- **Opcode byte:** bit 7 = escape flag; bits 0–6 = opcode identity (**0–127**, 128 values).
- **68 opcodes** are currently used (`QLangBytecode.h:42-195`); **60 spare**. The 7-bit identity
  space (0–127) is **final** — deliberately not expanded; 128 is already more than enough.
- **Base form** (bit 7 = 0): `[op]` + default-form operands (§7).
- **Escaped form** (bit 7 = 1): `[op | 0x80]` + **modifier byte** `e` (§4) + operands.

The escape bit is a pure "modifier follows" flag; it does not extend the opcode identity space.

---

## 4. Modifier byte `e`

> **Superseded by `docs/plans/encoding-v6-and-indirect-plan.md` §2 (`.qlc` v6, specified
> 2026-11-06).** The nibble split below (bits 0–3 left form / bits 4–7 right form) is what v5
> ships. v6 re-splits the byte into **2/2/4** — left form (0–1), right-operand **kind** (2–3:
> immediate / register / const pool / reserved), right-operand form (4–7) — which is what makes
> the `i`/`c` suffix opcodes redundant. §4.1 (jump class) is unchanged in v6.

`e` is interpreted **per opcode class**. Three classes:

### 4.1 Jump class
`JMP, JMPIFFALSE, JMPIFTRUE, CONTINUE, BREAK, JE, JNE, JG, JGE, JL, JLE`

| `e` | Form | Offset width | Range |
|-----|------|--------------|-------|
| 0 | jmp16 | 2-byte relative | ±32 767 |
| 1 | jmp32 | 4-byte relative | ±2 GB |

(The base, no-escape form is jmp8 — 1-byte relative, ±127, §7. The escape selects the wider
jmp16 / jmp32 variants.)

### 4.2 Immediate class
`MOVI, ADDI, SUBI, CMPI`

| Field | Bits | Meaning |
|-------|------|---------|
| left operand (`a`, dest) | 0–3 | register addressing (§4.3 left) |
| right operand (immediate) | 4–7 | width × signedness (table §5.3) |

**This version implements signed `Int` only** (right values 4/5/6 = 1/2/4-byte signed). The
unsigned (0–3) and 8-byte (7) forms are reserved in the encoding for future versions.

### 4.3 Register / constant class
`MOV, MOVC, MAKEFUNC, MAKEGEN`, and 3-operand ALU (`ADD, SUB, …`)

**Left operand (`a`, dest) — always a register, bits 0–3:**

| Value | Addressing | Bytes |
|-------|------------|-------|
| 0 | register 0–255 | 1 |
| 1 | extended register | 2 |
| 2 | extended register | 4 (future) |

**Right operand (`b`), bits 4–7 — interpreted by opcode:**

| Operand kind | Opcodes | Value 0 | Value 1 | Value 2 |
|--------------|---------|---------|---------|---------|
| register | `MOV`, ALU | reg 0–255 (1 B) | extended reg (2 B) | extended reg (4 B, future) |
| constant | `MOVC`, `MAKEFUNC`, `MAKEGEN` | const 0–255 (1 B) | extended const (2 B) | extended const (4 B, future) |

> **Note:** `MOVC`/`MAKEFUNC`/`MAKEGEN` operand `b` is a **constant-pool index**, not an inline
> immediate — it uses the constant branch above, *not* the immediate-width branch (§4.2).

The third operand `c` of 3-operand ops has **no form field** in `e` and is always a 1-byte
register (0–255). An extended `c` is expected to be rare; defer encoding support until needed.

---

## 5. Operand encodings

### 5.1 Registers — one pool, width by index
| Index range | Addressing | Bytes |
|-------------|------------|-------|
| 0–255 | default | 1 |
| 256–65 535 | extended | 2 |
| 65 536+ | extended | 4 (future) |

A single contiguous register pool (no main/spill split, no cap, no spill path). This version uses
1-byte (≤255) and 2-byte (≤65 535) addressing; 4-byte addressing is reserved for future. The
per-frame register file is an unbounded `std::vector<QLangValue>` sized to the high-water mark.

### 5.2 Constants — one pool, width by index (mirrors registers)
| Index range | Addressing | Bytes |
|-------------|------------|-------|
| 0–255 | default | 1 |
| 256–65 535 | extended | 2 |
| 65 536+ | extended | 4 (future) |

A single contiguous constant pool (no main/spill split), addressed exactly like registers. The
compiler appends constants in first-use order; an index ≤255 is addressed with 1 byte, an index
≥256 with 2 bytes (4-byte reserved for future). No reordering or partitioning is required.

### 5.3 Immediates (width × signedness → QLang type)

> **Superseded by `docs/plans/encoding-v6-and-indirect-plan.md` §2.3 (`.qlc` v6).** v6 ships
> the whole table — unsigned (0–3) and 8-byte signed (7) included — plus three new zero-byte
> forms: `8` = `float` (8 B), `9`/`10` = `bool` false/true, `11` = `nil`. The base form stays
> value 4 (1-byte signed `Int`). Effect: `bool`, `nil` and `float` literals leave the constant
> pool.

| `e` right | Bytes | Signed | QLang type | This version |
|-----------|-------|--------|------------|--------------|
| 0 | 1 | unsigned | `Unsigned` | future |
| 1 | 2 | unsigned | `Unsigned` | future |
| 2 | 4 | unsigned | `Unsigned` | future |
| 3 | 8 | unsigned | `Qword` | future |
| 4 | 1 | signed | `Int` | **yes** (base form) |
| 5 | 2 | signed | `Int` | **yes** |
| 6 | 4 | signed | `Int` | **yes** |
| 7 | 8 | signed | `Long` | future |

**This version: signed `Int` only** (values 4/5/6; the base no-escape form is value 4). Signed
values sign-extend to `Int`. The unsigned (0–3) and 8-byte (7) forms are reserved in the encoding
for future versions. The compiler emits the narrowest signed form whose range fits the literal.

### 5.4 Jumps (relative)
| Form | Layout | Bytes | Range |
|------|--------|-------|-------|
| jmp8 (base) | `[op][off8]` | 2 | ±127 |
| jmp16 (escaped, `e`=0) | `[op\|0x80][0x00][off16]` | 4 | ±32 767 |
| jmp32 (escaped, `e`=1) | `[op\|0x80][0x01][off32]` | 6 | ±2 GB |

`off = target_pc − (jump_pc + jump_size)`; VM executes `pc += jump_size + off`. The compiler
emits the narrowest form whose range fits the offset (jmp8 for tight loop back-edges, jmp16 for
most branches, jmp32 for far targets).

---

## 6. Concrete instruction forms

| Instruction | Form | Bytes | Layout |
|-------------|------|-------|--------|
| `MOV a, b` (idx ≤255) | base | 3 | `[MOV][a][b]` |
| `MOV a, bExt` (idx ≥256) | escaped | 5 | `[MOV\|80][e=a0,b1][a][bExt:2]` |
| `MOVC a, c` (idx ≤255) | base | 3 | `[MOVC][a][c]` |
| `MOVC a, cExt` (idx ≥256) | escaped | 5 | `[MOVC\|80][e=a0,b1][a][cExt:2]` |
| `MOVI a, i8` (signed, base) | base | 3 | `[MOVI][a][i8:1]` |
| `MOVI a, i16` (signed) | escaped | 5 | `[MOVI\|80][e=a0,imm5][a][i16:2]` |
| `MOVI a, i32` (signed) | escaped | 7 | `[MOVI\|80][e=a0,imm6][a][i32:4]` |
| `ADD a, b, c` (idx ≤255) | base | 4 | `[ADD][a][b][c]` |
| `JMP` near (±127) | base | 2 | `[JMP][off8:1]` |
| `JMP` mid (±32 767) | escaped | 4 | `[JMP\|80][0x00][off16:2]` |
| `JMP` far | escaped | 6 | `[JMP\|80][0x01][off32:4]` |
| `JE` near / mid / far | base / escaped | 2 / 4 / 6 | as `JMP` |

---

## 7. Default (no-modifier) operand forms

**Rule: if the opcode top bit is not set, both the left and right operands use 1-byte encoding.**
A base opcode (bit 7 = 0) therefore decodes with:

| Operand kind | Default form |
|--------------|--------------|
| Register | 1-byte (index 0–255) |
| Pool index | 1-byte (index 0–255) |
| Immediate (`MOVI/ADDI/SUBI/CMPI`) | 1-byte signed (`Int`, −128..127) |
| Jump offset | 1-byte relative (jmp8, ±127) |

The escape form (top bit set + modifier byte) is used **only** when an operand exceeds its 1-byte
default: a register/pool index ≥256 (2-byte), a wider / unsigned immediate (2/4/8-byte), or a jump
offset beyond ±127 (jmp16 / jmp32).

---

## 8. Compiler changes (`src/QLangCompiler.cpp`)

- **Emit** into a flat byte buffer; choose base vs escaped form per operand.
- **Relative backpatch:** replace absolute `patchJump` (`:44`) with relative offsets. Three-tier:
  lay out assuming jmp32, then downgrade each jump to the narrowest form that fits — jmp8
  (`|off| ≤ 127`), jmp16 (`|off| ≤ 32 767`), else jmp32; iterate to a fixpoint (converges in 1–2
  passes because narrower forms are shorter).
- **Immediate selection:** pick the narrowest width × signedness whose QLang type and range fit
  the literal (§5.3); emit the base form for a 1-byte signed value, the escaped form otherwise.
- **Constant pool:** append in first-use order into the single pool; index ≤255 → 1-byte,
  index ≥256 → 2-byte. No partitioning.
- **Extended registers:** emit the 2-byte form when a register index ≥ 256 (no spill, no cap).
- Drop `usesI32()` (`QLangBytecode.h:202`) — replaced by the per-opcode width/form table.

## 9. VM changes (`src/QLangVM.cpp`, `include/qlang/QLangVM.h`)

- Decode the flat byte stream at a byte-offset `pc`; read the opcode, then the modifier byte if
  bit 7 is set, then operands per the form table.
- Execute relative jumps: `pc += jump_size + off`.
- Index `Frame::regs` and the constant pool with 1- or 2-byte indices per the operand form.
- `Frame::regs` stays an unbounded `std::vector<QLangValue>`.

## 10. `.qlc` codec changes (`src/QLangBytecodeIO.cpp`)

- Write the flat instruction byte stream (no per-instruction struct framing).
- Write the single constant pool (one contiguous section; the 1/2-byte addressing is a decode-time
  concern, not a file-layout split).
- **Version bump** (v4 → v5); reader accepts v1..v5 and rejects newer.
- Reader validates pool size and operand-form consistency.

## 11. Files touched

| File | Change |
|------|--------|
| `include/qlang/QLangBytecode.h` | Escape-byte `Instruction` encoding; `Opcode` → 7-bit + escape; modifier byte; operand form/width tables; drop `usesI32` |
| `src/QLangCompiler.cpp` | Flat-stream emit; relative iterative backpatch; immediate width/sign selection; 1/2-byte pool & register addressing; extended-register emit |
| `src/QLangVM.cpp` | Flat byte-stream decode; modifier handling; relative jumps; extended reg/pool indexing |
| `src/QLangBytecodeIO.cpp` | Flat stream + single constant pool; v5; reader validation |
| `include/qlang/QLangVM.h` | `Frame::regs` unbounded; decode state |
| `examples/qlang_run.cpp` | Disassembler + profile reporter for the escape-byte format — **must be updated with every encoding change** (§16) |
| `tests/unit/` | Update `Instruction` construction sites; add >256-register and >256-constant stress tests |

## 12. Verification gate

After implementation, all of the following must pass:

```bash
cmake -B build && cmake --build build
ctest --test-dir build
build/qlang_run --asm-level 5 --profile tests/scripts/hello.ql
build/qlang_run --output /tmp/t.qlc tests/scripts/hello.ql   # then round-trip /tmp/t.qlc
```

Specifically:
1. **Full corpus:** every `tests/scripts/*.ql` compiles, runs, and produces byte-identical stdout
   to the pre-change build.
2. **Round-trip:** compile → write `.qlc` (v5) → read → execute → stdout matches direct run.
3. **Extended-register stress:** a script with **>256 simultaneously-live registers** compiles
   (exercising the 2-byte extended encoding) and runs correctly.
4. **Extended-pool stress:** a script with **>256 distinct constants** compiles (exercising the
   2-byte pool index) and round-trips.
5. **Jump forms:** a script with nearby (±127), mid, and far (>32 767 B) branches emits jmp8,
   jmp16, and jmp32 respectively and runs correctly.
6. **Disassembly:** `--asm-level 0–5` produces well-formed listings for the new format.

## 13. Scope (this version) — confirmed

1. **Opcode space is final at 128** (0–127); deliberately not expanded.
2. **Extended addressing = 2 bytes** (to 65 535) for both the single register pool and the single
   constant pool; **no cap** (256 is an addressing-width boundary only). 4-byte addressing is
   reserved for future. → removes the register spill path entirely.
3. **Base form (no escape bit) = 1-byte operands** — register/pool index, 1-byte signed `Int`
   immediate, and 1-byte relative jump (jmp8). The escape+modifier widens any of them.
4. **Immediates: signed `Int` only** (1/2/4-byte, sign-extended). `Unsigned`/`Long`/`Qword` and
   the unsigned forms are reserved in the encoding for future versions.
   → **Superseded:** v6 ships them, plus `float`/`bool`/`nil` (`encoding-v6-and-indirect-plan.md`
   §2.3). Items 1–3 above stand unchanged in v6.

## 14. Open questions / future work

1. **Extended `c` operand:** 3-operand ops currently force `c` to a 1-byte register. Defer until
   a real need appears. (Still deferred in v6 — `encoding-v6-and-indirect-plan.md` §2.6.)
2. **`.qlc` version:** v4 → v5. **Resolved as a clean break, not a range:** the reader accepts
   v5 **only** (`version != kQlcVersion` is rejected, `src/QLangBytecodeIO.cpp:469`) — v1..v4
   streams do not load. v6 follows the same precedent.
3. **Future versions (out of scope now):** unsigned / 8-byte immediates (`Unsigned`/`Long`/
   `Qword`), 4-byte register/constant addressing, extended `c` operand. → the immediates are
   **in scope for v6**; the two addressing items stay open.

## 15. Summary

| Aspect | Old plan (Phase 2–4) | This plan |
|--------|----------------------|-----------|
| Width model | fixed 4 / 8 B | variable, escape-byte |
| Register cap | 256 (hard) + spill | **none** — 256 = encoding boundary |
| Register spill path | required | **not needed** |
| Immediates | 32-bit only | signed `Int` 1/2/4 B (base = 1 B); unsigned / 8-B future |
| Jumps | absolute 32-bit | relative jmp8 / jmp16 / jmp32 (base = jmp8) |
| Register / const pool | unaddressed | single pool, 1-B (≤255) / 2-B (≥256) addressing, no cap |
| Extended-form mechanism | one opcode per form (79 ops) | 1 escape bit + 1 modifier byte (128 base ops) |

**Migration:** single `.qlc` version bump (v4 → v5). Compiler, VM, codec, and disassembler change
together; ship as one feature with the §12 gate. Phase 1 (loc side-table) is unchanged and carries
over.

## 16. Maintenance invariants

- **The disassembler must track the encoding.** Every change to the opcodes or the bytecode
  encoding — a new opcode, an operand form, an operand width, a jump form, or a pool-addressing
  change — **must** update the disassembler in `examples/qlang_run.cpp` in the *same* change.
  `pseudoText()`, `operandText()`, `operandMask()`, and `disassemble()` all read the encoding
  directly and will silently mis-decode (or read out of bounds) if left stale. A bytecode-encoding
  change that does not touch `qlang_run.cpp` is **incomplete** and must not be considered done.
