# Plan: `subi` + alternate conditional jumps (`cmp`/`cmpi` + `je/jne/jg/jge/jl/jle`)

Spec: `docs/spec/13-bytecodes.md` (SHA `52b0a616…`, updated flag tables). Precedents: MOVI (Round 30),
ADDI (Round 31) — `subi` is a near-verbatim clone of the ADDI pattern.

## Part 1 — `subi` (small, self-contained; do first)

Semantics: `r[a] = r[a] - Int(i32)`, in place, `Type::Int` only. Same operand
layout as ADDI (`a` = dest reg, `i32` = literal; b/c unused).

1. **`include/qlang/QLangBytecode.h`**
   - Append `Opcode::SUBI` after `ADDI` (before `kOpcodeCount`) — existing `.qlc`
     opcode numbers unchanged.
   - Add `case Opcode::SUBI:` to `usesI32()`.
2. **`src/QLangBytecode.cpp`** — `Bytecode::name()`: `case Opcode::SUBI: return "subi";`
   (next to `movi`/`addi` at ~line 245).
3. **`src/QLangVM.cpp`** — add `case Opcode::SUBI:` in **all four exec loops**
   (ADDI sites: ~1625, ~1929, ~2317, ~2629):
   `frame.regs[i.a] = doBinary(Opcode::SUB, frame.regs[i.a], QLangValue::Int(i.i32));`
   Inheriting `doBinary` gets int32 wrap, float promotion, array-number error, etc.
4. **`src/QLangCompiler.cpp`** — extend the existing ADDI check to SUB at the
   four compound-assignment sites (pattern at ~980):
   `if ((opCode == Opcode::ADD || opCode == Opcode::SUB) && isIntLiteral(s->assignValue))`
   → `emitImm(opCode == Opcode::ADD ? Opcode::ADDI : Opcode::SUBI, r, …)`.
   Sites: plain `a -= 12` (~980), captured var (~925), index `a[i] -= 12` (~3093),
   member `t.k -= 12` (~3127). Non-Int RHS stays on pool + SUB. `--` stays `DEC`.
5. **`.qlc` codec** — no change. Format version stays 2 (raw 12-byte layout,
   `usesI32` already the single source of truth for encoding).
6. **Tests**
   - `examples/spec_check2.cpp` — new SUBI block mirroring the ADDI block (~line 900):
     plain `n = n - 1` / `a -= 12`, captured var, index, member, non-Int RHS
     (float) stays pooled, loop body, generator path.
   - `examples/codec_check.cpp` — mirror ADDI checks: SUBI present in compiled
     bytecode, no new pool slot, float RHS not SUBI, v2 write/read round-trip.
7. **Docs** — `13-bytecodes.md`: add SUBI row to the operand table + "Implemented
   (Round N)" note on the `## new OpCode: subi` section. `docs/spec/README.md`:
   new SHA256 for 13-bytecodes.md. `docs/compliance/status.md`: tally + row.

## Part 2 — alternate conditional jumps (bigger; do after subi)

### Semantics (per the spec's flag tables)

Flags live in **`Frame`** (new `int flags = 0;` bitfield: `GR=1, EQ=2, LT=4`).
Each `cmp`/`cmpi` **wipes** the flags, then sets each bit **independently**:
`GR = (a > b)`, `EQ = (a == b)`, `LT = (a < b)`. Because the three relations
are mutually exclusive and exhaustive, this yields exactly the spec's table:
`a > b` → `gr`; `a < b` → `lt`; `a >= b` → `gr`+`eq`; `a <= b` → `lt`+`eq`;
`a == b` → `eq`; `a != b` → none. More than one flag can be set simultaneously
(only the `>=`/`<=` pairs), which is what makes `jge`/`jle` decodable.

| op | jump when (flag pattern) |
|----|--------------------------|
| `jg`  | `gr` only (a > b) |
| `jge` | `gr` and `eq` (a >= b) |
| `jl`  | `lt` only (a < b) |
| `jle` | `lt` and `eq` (a <= b) |
| `je`  | `eq` only (a == b) |
| `jne` | `eq` not set (a != b) |

The "only" qualifiers are redundant given exhaustiveness, but implement the
test as written (e.g. `JG`: `f == GR`) — it is identical and self-documenting.

`j*` consumes the flags; a `CALL` between `cmp` and `j*` would clobber them —
the compiler must never emit one (true by construction, below).

All six `j*` opcodes take the pc target in `i32` (like `JMP`).

### Compilation strategy: branch-site fusion (not peephole)

Emit `cmp`/`cmpi` + `j*` only where a branch condition **is** a binary
comparison whose both operands are compile-time `Type::Int`:

- In `if`/`while`/`for`-condition compilation: if the condition expression is
  `E1 <op> E2` with `op ∈ {==,!=,<,>,<=,>=}` and both sides `Type::Int` →
  compile E1→ra, E2→rb (or a literal → `cmpi ra, i32`), emit
  `CMP`/`CMPI` + `jg/jge/jl/jle/je/jne <target>`.
- Everything else (non-Int operands, compound conditions) keeps the existing
  `EQ/GT/… + JMPIFFALSE` path. Those opcodes are **not** removed — range
  guards, switch, and non-Int comparisons still use them.
- Because the fused pair is emitted atomically at the branch site, no `CALL`
  (or other flag-clobbering code) can land between `cmp` and `j*`.
- Bonus: range guards (~lines 1165–1174, 1356–1364: `LE/GE x, endReg` +
  `JMPIFFALSE`) are Int-vs-Int and will fuse automatically once the branch
  site handles them.

### File changes

1. **`include/qlang/QLangBytecode.h`** — append `CMP, CMPI, JE, JNE, JG, JGE,
   JL, JLE` after `SUBI`; add all eight to `usesI32()` (CMP/CMPI use i32? —
   **no**: CMP is `a,b` registers, CMPI is `a` + i32 literal, the six jumps are
   i32 targets. Only `CMPI, JE, JNE, JG, JGE, JL, JLE` go in `usesI32`; `CMP`
   uses a/b).
2. **`src/QLangBytecode.cpp`** — `name()`: `"cmp","cmpi","je","jne","jg","jge","jl","jle"`.
3. **`include/qlang/QLangVM.h`** — `Frame::flags` (int, 0) + `CMP_GR/CMP_EQ/CMP_LT`
   constants.
4. **`src/QLangVM.cpp`** — in all four exec loops:
   - `CMP`: `frame.flags = cmpFlags(frame.regs[i.a], frame.regs[i.b]);`
   - `CMPI`: `frame.flags = cmpFlags(frame.regs[i.a], QLangValue::Int(i.i32));`
   - `j*`: `if (flagsSatisfied(frame.flags, op)) i.pc = (size_t)i.i32;`
   - `cmpFlags(x, y)` helper: Int-only (error otherwise, per spec "can only be
     used if both are int"), returns `(x > y ? GR : 0) | (x == y ? EQ : 0) |`
     `(x < y ? LT : 0)` (independent bits, wipe implicit).
   - `flagsSatisfied`: `JG: f==GR`, `JGE: f==(GR|EQ)`, `JL: f==LT`,
     `JLE: f==(LT|EQ)`, `JE: f==EQ`, `JNE: !(f & EQ)`.
5. **`src/QLangCompiler.cpp`** — branch-site fusion as above (if/while/for
   condition paths; switch case checks stay on `EQ + JMPIFTRUE` for now —
   non-Int values must still match).
6. **`.qlc` codec** — no version bump (stays 2).
7. **Tests** — `spec_check2`: int comparisons in if/while/for take the new path
   (values correct: all six relations, both directions, boundary ±1), non-Int
   (float, string) comparisons unchanged, `cmp` between a call and its `j*`
   impossible (structural). `codec_check`: opcode present, round-trip v2.
8. **Docs** — same as Part 1 step 7 (flag tables are already final in the
   spec; no interpretation notes needed).

## Verification (both parts, per AGENTS.md)

- `cmake -B build && cmake --build build`
- `ctest --test-dir build` (all suites, incl. `codec_check`)
- Update compliance hash/tally/rows **after** green run.

## Explicitly out of scope

- Decompiler mnemonics (`r`/`c` prefixes) — spec says DO NOT IMPLEMENT YET.
- Separating line/col from `Instruction` — spec says DO NOT IMPLEMENT.
- Removing `JMPIFFALSE`/`JMPIFTRUE`/binary comparison ops — still needed.
- `cmp` for non-Int types (float ordering etc.) — spec restricts to int.
