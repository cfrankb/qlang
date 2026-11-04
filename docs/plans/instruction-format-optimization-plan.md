# Instruction Format Optimization Plan

Status: **proposed** (2026-11-03)
Supersedes: the 12-byte `Instruction` layout in `include/qlang/QLangBytecode.h`
Reference spec: `docs/spec/13-bytecodes.md` (operand map)

## Current State

```cpp
struct Instruction {          // 12 bytes, align 4
  Opcode op;                  // uint16_t  (2 B)  ~60 values used
  int16_t a;                  // 2 B
  union { int16_t b, c; int32_t i32; };  // 4 B
  uint16_t line, col;         // 4 B
};
```

- Register indices: `int16_t` → up to 32 767 registers per frame.
- `QLangValue` is ~96 B (variant with `ExternalRef` as largest alternative).
- A 32 K-register frame = 3 MB of register file (no cache level holds it).
- Line/col is embedded in every instruction but only read on error.
- The VM decode loop reads 12 B/instruction; 21 instructions per 256 B L1 line.

## Goals

1. Shrink the hot-path instruction to ≤ 4 B for the common 3-operand case.
2. Cap the register file at 256 so it is L2-resident (256 × 96 B = 24 KB).
3. Introduce a **universal scratch register** range the compiler can use for
   temporaries without allocation/save-restore overhead.
4. Keep the decode loop fixed-width (no variable-length decode in the hot path).
5. Preserve `.qlc` backward compatibility via a format-version bump.

---

## Phase 1 — Side-table for line/col (12 → 8 B)

**What changes**

- `Instruction` drops `line`/`col`; becomes 8 B:
  ```cpp
  struct Instruction {         // 8 bytes, align 4
    Opcode op;                 // uint16_t
    int16_t a;
    union { int16_t b, c; int32_t i32; };
  };
  ```
- `Function` gains `std::vector<std::pair<uint16_t,uint16_t>> loc;`
  (parallel to `instructions`; empty entries = no source location).
- The `.qlc` codec writes a location section after the instruction stream.
- The VM's `throwAt(pc)` looks up `loc[pc]` only when building an error.

**Files touched**

| File | Change |
|------|--------|
| `include/qlang/QLangBytecode.h` | Shrink `Instruction`, add `loc` to `Function` |
| `src/QLangBytecodeIO.cpp` | New location section in writer/reader |
| `src/QLangVM.cpp` | `throwAt` reads `loc[pc]` instead of `ins.line/col` |
| `src/QLangCompiler.cpp` | Emit into `loc` vector alongside instructions |
| `tests/unit/` | Update any test that constructs `Instruction` literals |

**Execution cost**

| Metric | Before | After | Δ |
|--------|--------|-------|---|
| Bytes/instruction (hot path) | 12 | 8 | −33 % |
| Instructions per 256 B L1 line | 21 | 32 | +52 % |
| Decode memory reads | 12 B | 8 B | −33 % |
| Error-path cost | 0 (inline) | 1 extra vector index | negligible (cold) |
| Register file | unchanged | unchanged | — |

**Risk:** Low. No decode-logic change; the VM loop shape is identical.

---

## Phase 2 — 8-bit opcode + 8-bit registers (8 → 4 B)

**What changes**

- `Opcode` becomes `uint8_t` (256 values; ~60 used, room to grow).
- Register indices become `uint8_t` (0–255).
- `Instruction` becomes 4 B for the 3-operand form:
  ```cpp
  struct Instruction {         // 4 bytes, align 4
    uint8_t op;
    uint8_t a;
    union {
      struct { uint8_t b; uint8_t c; };
      // i32 opcodes use a 4-byte extension (see below)
    };
  };
  ```
- **i32 opcodes** (JMP, MOVI, ADDI, SUBI, CMPI, JE…JLE) need a 32-bit
  payload. Two options:
  - **(a) Fixed 8-byte form:** `uint8_t op, a; int32_t i32;` (6 B, padded to 8).
    The decode loop checks `usesI32(op)` and reads 8 B instead of 4 B.
  - **(b) Extension byte:** `op == 0xFF` signals "next 4 B are i32".
    Keeps the common path at 4 B; rare i32 ops cost 6 B.
  - **Recommendation: (a)** — fixed-width, no branch in the inner loop.
    The instruction stream is a flat array of 4-B or 8-B records; the
    compiler emits 8-B records for i32 ops. The VM decodes with a single
    `if (usesI32(op))` that is predictable (i32 ops are < 10 % of stream).

- `Function::registerCount` is now `uint8_t` (max 256).
- The compiler's register allocator asserts `regCount ≤ 256`.

**Files touched**

| File | Change |
|------|--------|
| `include/qlang/QLangBytecode.h` | New 4 B / 8 B `Instruction`, `uint8_t` fields |
| `src/QLangCompiler.cpp` | Emit 8-bit regs; assert ≤ 256; 8-B records for i32 |
| `src/QLangVM.cpp` | Decode 4 B / 8 B; `uint8_t` index into `regs` |
| `src/QLangBytecodeIO.cpp` | New wire format (v3) |
| `include/qlang/QLangVM.h` | `Frame::regs` stays `vector<QLangValue>` (size ≤ 256) |
| `tests/unit/` | Update all `Instruction` construction sites |

**Execution cost**

| Metric | Phase 1 | Phase 2 | Δ |
|--------|---------|---------|---|
| Bytes/instruction (3-op) | 8 | 4 | −50 % |
| Bytes/instruction (i32) | 8 | 8 | 0 % |
| Instructions per 256 B L1 line | 32 | 64 (3-op) / 32 (i32) | +100 % (3-op) |
| Decode memory reads | 8 B | 4 B (3-op) / 8 B (i32) | −50 % (3-op) |
| Register file (256 regs) | 24 KB | 24 KB | L2-resident |
| Register file (32 K regs, old) | 3 MB | N/A (capped) | eliminated |
| Index width | 16-bit → 32-bit addr | 8-bit → 32-bit addr | 1 fewer add |

**Cache analysis (256-register frame, 96 B/QLangValue):**

- Register file = 24 KB → fits in L2 (typically 256 KB–1 MB).
- Hot working set (params + temps, ~32 regs) = 3 KB → fits in L1 (32–64 KB).
- Old 32 K-register frame = 3 MB → every register access was an L3/DRAM miss.
- **Net effect:** register accesses drop from ~100 ns (DRAM) to ~4 ns (L1)
  for the hot set. This dominates the instruction-size savings.

**Risk:** Medium. Every emit/decode site changes. The 256-register cap may
force the compiler to spill in pathological cases (deeply nested expressions
with many live variables). Mitigation: the compiler already linear-scans;
add a spill-to-stack path if `regCount > 240`.

---

## Phase 3 — Universal scratch registers

**Concept**

Partition the 256-register file into two ranges:

| Range | Size | Role |
|-------|------|------|
| 0 – 239 | 240 | **Named registers** — allocated by the compiler for script variables, params, temps. Saved/restored across calls. |
| 240 – 255 | 16 | **Universal scratch** — compiler-owned temporaries. Never saved/restored. Always available. |

**Properties**

- Scratch registers are **caller-saved**: the callee may clobber them
  freely. The compiler must not rely on a scratch value surviving a call.
- Scratch registers are **always allocated** (no per-frame `resize` cost
  beyond the fixed 256).
- The compiler uses scratch for:
  - Intermediate results in expression evaluation (reduces `MOV` count).
  - Temporary storage for `CALL` argument marshalling.
  - Loop counters in `for` loops (avoids allocating a named reg).
  - Type-conversion temporaries (`CONV`, `CAST`).
- Because scratch is 1-byte addressed (like all registers in Phase 2),
  there is **no encoding penalty** for using it.

**Compiler changes**

- Register allocator reserves 0–239 for named vars; 240–255 are a free
  pool for temporaries.
- At each call site, the compiler knows scratch is clobbered → any live
  value in scratch must be moved to a named reg before the call.
- The allocator's liveness analysis treats scratch as "dead after call".
- Net effect: fewer named-register allocations → fewer `MOV` instructions
  → shorter instruction streams.

**VM changes**

- None in the decode loop. The VM already treats all 256 registers
  uniformly. The scratch/named distinction is a **compiler-only** concept.
- `Frame::regs` is always sized to 256 (fixed allocation, no `resize`
  per call). This eliminates the `vector::resize` cost in `pushCall`.

**Execution cost**

| Metric | Phase 2 | Phase 3 | Δ |
|--------|---------|---------|---|
| Instruction size | 4 / 8 B | 4 / 8 B | 0 % |
| `MOV` instruction count | baseline | −10 to −20 % | fewer temps |
| Register allocation cost | linear scan over ≤ 240 | linear scan over ≤ 240 + free scratch | ~0 % |
| Frame setup cost | `resize(registerCount)` | fixed 256 (pre-allocated) | −1 alloc |
| Register file cache | 24 KB (L2) | 24 KB (L2) | 0 % |
| Hot-set cache (32 regs) | 3 KB (L1) | 3 KB (L1) | 0 % |

**Estimated throughput impact (combined Phases 1–3):**

- Instruction fetch: 3× fewer bytes → 3× more instructions per cache line.
- Register access: L1 hit (4 ns) vs L3/DRAM (100 ns) for hot set.
- Fewer `MOV` instructions: −10 to −20 % instruction count.
- **Net estimated speedup: 2–4×** for compute-heavy scripts (arithmetic,
  loops). I/O-bound scripts see less benefit (I/O dominates).

**Risk:** Low. The VM is unchanged; the compiler's allocator gets a
simpler problem (fixed 256, 16 free scratch). The only new failure mode
is a compiler bug that leaks a scratch value across a call — caught by
the existing test suite.

---

## Phase 4 — Variable-length encoding (optional, 2–6 B)

**Concept**

Encode only the operands each opcode actually needs:

| Operand class | Opcodes | Bytes (8-bit op+regs) |
|---------------|---------|-----------------------|
| 0-op + i32 | JMP, BREAK, CONTINUE, JE…JLE | 5 |
| 1-op | INC, DEC, RETURN, TABCREATE, DELETE | 2 |
| 2-op | MOVC, MOV, ADD…IN, CMP, MOVI, ADDI, SUBI, CMPI | 3 |
| 3-op | CALL, ARRGET, TABGET, YIELD, CONV, CAST | 4 |
| 1-op + i32 | (none currently; JMPIFFALSE/TRUE use a+i32) | 6 |

A 1-byte **operand-count tag** is derived from the opcode (a static
`opcodeInfo[op].operandCount` table), so no extra byte is needed.

**Decode loop**

```cpp
uint8_t op = stream[pc];
uint8_t nOps = opcodeInfo[op].nOps;  // 0, 1, 2, or 3
// read nOps bytes of operands
// if usesI32(op): read 4 B i32 instead of b/c
pc += 1 + nOps + (usesI32(op) ? 4 : 0);
```

The variable length makes the decode loop **harder to pipeline** (the CPU
can't prefetch the next instruction until it knows the current one's
length). This partially offsets the cache benefit.

**Execution cost**

| Metric | Phase 3 (fixed) | Phase 4 (variable) | Δ |
|--------|-----------------|--------------------|---|
| Avg bytes/instruction | 4.4 (est.) | 3.2 (est.) | −27 % |
| Instructions per 256 B line | 58 | 80 | +38 % |
| Decode pipelining | excellent (fixed stride) | degraded (variable stride) | −15 % est. |
| Branch prediction | trivial | opcode → length (predictable) | ~0 % |
| **Net throughput** | baseline | +10 to +20 % | modest |

**Risk:** Medium-high. The variable-length decode is a significant change
to the VM's inner loop. Debugging, disassembly, and the `.qlc` codec all
become more complex. The gain over Phase 3 is modest (+10–20 %) because
the decode-pipeline penalty eats much of the cache benefit.

**Recommendation:** **Defer Phase 4.** Phases 1–3 deliver 80 % of the
total benefit with a fixed-width decode loop. Revisit Phase 4 only if
profiling shows instruction-fetch bandwidth is still a bottleneck.

---

## `examples/qlang_run.cpp` — Required Updates

The driver (1 231 lines) reads `Instruction` fields directly in its
disassembler and profile reporter. Each phase touches a different set of
functions.

### Phase 1 — line/col side-table

| Function | Line(s) | Change |
|----------|---------|--------|
| `disassemble()` | 729, 734 | `ins.line` → `f.loc[j].first` (side-table lookup). Guard: `j < f.loc.size()`. |
| `disassemble()` | 732 | Source-line interleave uses `f.loc[j].first` instead of `ins.line`. |
| `pseudoText()` | — | No change (does not read line/col). |
| `operandText()` | — | No change. |
| `writeBytecode()` | — | No change (delegates to `writeQlc` in the library). |

**Verification:**
- `--asm-level 2` on any `.ql` script must produce identical source-line
  interleave to the pre-change output.
- `--asm-level 0` and `1` must be byte-identical (they do not use line/col).
- Error-path: a script that throws at a known line must report the same
  `line:col` in stderr as before.

### Phase 2 — 8-bit opcode + 8-bit registers

| Function | Line(s) | Change |
|----------|---------|--------|
| `pseudoText()` | 240–241 | `rn()` lambda: `int r` → `unsigned r` (cosmetic; `uint8_t` promotes to `int` anyway). No logic change. |
| `pseudoText()` | 257–264 | `bin()` / `cmp()` lambdas: `ins.a`, `ins.b`, `ins.c` are now `uint8_t`. The `rn()` calls still work (implicit promotion). No logic change. |
| `pseudoText()` | 266 | `snprintf(pc, …, "%.4x", ins.i32)` — unchanged (i32 is still `int32_t`). |
| `pseudoText()` | 270–453 | All `ins.a`/`ins.b`/`ins.c` reads: type narrows from `int16_t` to `uint8_t`. **No logic change** — every use is a read into an `int`-typed lambda parameter or a `std::to_string` call. |
| `operandMask()` | 469–515 | No change (operates on `Opcode` enum, not on operand types). |
| `operandText()` | 519–552 | `ins.a`/`ins.b`/`ins.c` are `uint8_t`. The `fmt()` lambda takes `int v` — implicit promotion works. **One subtlety:** `ins.b` was `int16_t` and could be negative (native-call slot: `b = -1 - id`). With `uint8_t`, the negative encoding is lost. **Fix:** the compiler must encode native slots differently (e.g. a separate `nativeSlot` field, or keep `b` as `int8_t` for CALL only). See Open Question 5. |
| `disassemble()` | 680 | `prog.registerCount` is now `uint8_t` (max 256). Display unchanged. |
| `disassemble()` | 696 | `f.registerCount` is now `uint8_t`. Display unchanged. |
| `disassemble()` | 780 | `snprintf(addr, …, "%.4x", j)` — unchanged (address is still a `size_t` index). |
| `disassemble()` | 792–795 | `Bytecode::name(ins.op)` — unchanged. `operandText(ins)` — see above. |
| `writeBytecode()` | 143–160 | No change (delegates to `writeQlc`). |
| `profileText()` | 890–909 | No change (reports counts, not operand values). |

**Verification:**
- `--asm-level 0` through `5` on the full `tests/scripts/*.ql` corpus must
  produce listings that are **semantically identical** to the pre-change
  output (register numbers may differ if the allocator changes, but the
  opcode mnemonics, operand roles, and pseudo-mnemonics must match).
- `--output test.qlc && qlang_run test.qlc` round-trip: compile → write →
  read → execute → stdout byte-identical to direct `ctx.run(source)`.
- Native-call disassembly: a script that calls a native function must show
  the correct native slot in the `CALL` operand (regression test for the
  `uint8_t b` encoding issue).

### Phase 3 — Universal scratch registers

| Function | Line(s) | Change |
|----------|---------|--------|
| `disassemble()` | 680 | Header: add `scratch: r240–r255` to the register count line. |
| `disassemble()` | 696 | Per-function header: unchanged (scratch is a global concept, not per-function). |
| `disassemble()` | 804–883 (level 5) | Register breakdown: add a **"scratch"** category for registers 240–255. New count slot in `counts[]`. The `RegisterReport` struct gains a `scratchRange` field (or the disassembler hard-codes 240–255). |
| `disassemble()` | 877–880 | Summary line: add `N scratch` to the category summary. |
| `pseudoText()` | — | No change (scratch registers are addressed like any other register). |
| `operandText()` | — | No change. |
| `profileText()` | — | No change. |

**`RegisterReport` changes** (in `QLangCompiler.h`):
- Add `int scratchBase = 240;` and `int scratchCount = 16;` (or a
  `std::pair<int,int> scratchRange`).
- The compiler's allocator must report which scratch registers were used
  (for the level-5 breakdown).

**Verification:**
- `--asm-level 5` on a script with complex expressions must show registers
  240–255 labelled "scratch" in the breakdown.
- The summary line must account for all 256 registers (named + free +
  quarantine + deferred + live + call-arg + scratch + unallocated = 256).
- A script that uses many temporaries (e.g. a large expression with 20+
  live sub-expressions) must compile without exceeding the 240 named
  register limit; if it does, the compiler spills to scratch or emits a
  clear error.

### Cross-phase: build & test gate

After **each** phase, the following must pass before the phase is considered
done:

```bash
cmake -B build && cmake --build build
ctest --test-dir build
# Driver smoke test:
build/examples/qlang_run --asm-level 5 --profile tests/scripts/hello.ql
build/examples/qlang_run --output /tmp/t.qlc tests/scripts/hello.ql
# Round-trip (if a .qlc reader example exists):
#   build/examples/qlang_run /tmp/t.qlc   (stdout must match direct run)
```

Specifically for `qlang_run.cpp`:
1. **Compiles cleanly** with no warnings (`-Wall -Wextra`).
2. **`--asm-level 0–5`** produces well-formed listings on every script in
   `tests/scripts/`.
3. **`--output`** writes a `.qlc` that the library reader accepts.
4. **`--profile`** reports sane instruction counts (non-zero, matches the
   disassembly line count).
5. **Exit codes** 0–5 behave as documented in the header comment.

---

## Summary

| Phase | Instr size | Reg file | Scratch | Decode | Est. speedup | Risk |
|-------|-----------|----------|---------|--------|-------------|------|
| Current | 12 B | 3 MB (32 K) | — | fixed | 1× (baseline) | — |
| 1: loc side-table | 8 B | 3 MB | — | fixed | 1.2× | Low |
| 2: 8-bit op+regs | 4 / 8 B | 24 KB (256) | — | fixed | 2–3× | Medium |
| 3: scratch regs | 4 / 8 B | 24 KB | 16 regs | fixed | 2–4× | Low |
| 4: variable-length | 2–6 B | 24 KB | 16 regs | variable | 2.5–4.5× | Med-High |

**Recommended path: Phases 1 → 2 → 3.** Ship as three independent
commits, each with its own `.qlc` format version bump (v2 → v3 → v3,
since Phase 3 is compiler-only and changes no wire format).

## Open Questions

1. **256-register cap:** Is 240 named registers enough for the worst-case
   script in the test suite? Profile the current `registerCount`
   distribution before committing.
2. **Scratch size:** 16 is a starting point. 8 is enough for most
   expression evaluation; 32 gives more headroom. Benchmark.
3. **i32 encoding (Phase 2):** Fixed 8-B records vs extension byte.
   Fixed is simpler; extension saves 4 B on i32 ops (< 10 % of stream).
   Profile the i32-op ratio in the test corpus.
4. **`.qlc` versioning:** Phase 1 → v3, Phase 2 → v4. Phase 3 is
   compiler-only (no wire change). Confirm the reader rejects v > current.
5. **Native-slot encoding with `uint8_t b`:** `CALL` operand `b` is
   negative for native slots (`b = -1 - id`). A `uint8_t` cannot hold
   negative values. Options: (a) keep `b` as `int8_t` for CALL only
   (breaks the uniform `uint8_t` operand model); (b) encode native slots
   as a separate 1-byte field after the 3-operand prefix (CALL becomes a
   4-byte instruction: `op, a, b, c, nativeSlot`); (c) use a high-bit
   convention: `b >= 0x80` means native, `slot = 0x7F - b`. Option (c)
   is simplest and keeps the 4-byte fixed width.
