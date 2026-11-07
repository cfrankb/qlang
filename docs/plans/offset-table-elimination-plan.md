# Offset Table — Compression / Elimination (exploration)

Status: **exploration only — not scheduled, no code changed.** Baseline
`HEAD = 78d717c` ("qlang_run: add level-5 module import/export breakdown"),
`kQlcVersion = 6` (`include/qlang/QLangBytecodeIO.h:34`). Date 2026-11-07.
Reference spec: `docs/spec/13-bytecodes.md` (`### bytecode encoding`, the `loc`
side-table note) · `docs/SERIALIZATION.md` §4.1 (function record, offset table,
loc side table).

The originating hypothesis: instead of an index holding a 32-bit byte offset for
**every** instruction, keep a *forward-looking* index of a few **key**
instructions (absolute offset stored) and locate the instructions that follow a
key by relative offsets, packed two per byte because "the relative offset is
always less than 16".

This document records what was measured, the one correction the hypothesis
needs, and the options it opened up. No decision is taken here (§9).

---

## 1. What the table is today

`Function::instrOffsets` (`include/qlang/QLangBytecode.h:455`) is
`std::vector<size_t>` — **8 B per instruction in RAM** — one strictly increasing
byte offset per instruction in the flat v6 code stream. `Function::loc`
(`:456-458`) is a parallel `std::vector<std::pair<uint16_t, uint16_t>>` —
**8 B per instruction in RAM** — read only on the error path.

| Role | Site | Access pattern |
|------|------|----------------|
| Producer | `src/QLangCompiler.cpp:75-144` (`recomputeOffsets` + iterative backpatch) | built once, sequential |
| Wire | `src/QLangBytecodeIO.cpp:405-418` writer, `:507-547` reader | `offCount × u32` = **4 B per instruction on disk** |
| Load validation | `src/QLangBytecodeIO.cpp:531-547` | decodes **every** instruction from the table |
| pc → instruction index | `src/QLangVM.cpp:1022` (`instructionIndexAt`), binary search | **error / backtrace path only** |
| Disassembler | `examples/qlang_run.cpp:838-949`, `:1052` | sequential iteration |
| Tests | `tests/unit/codec_check.cpp` (round-trip equality), `tests/unit/codegen_size_check.cpp` (instruction counts) | sequential / `.size()` |

**The VM hot loop never touches it.** `pc` is a byte offset into `fn.code` and
the interpreter decodes on demand; jump targets and generator resume points are
byte offsets too. Random access is needed only to map a faulting `pc` back to a
source location.

## 2. Measured cost

**Reference program: `tests/scripts/perf_stress.ql`** — the largest of the four
perf-corpus scripts (`tests/perf/perf_bench.cpp:93` lists
`hello.ql, money.ql, cast.ql, perf_stress.ql`). `hello.ql` is a smoke test
(229 instructions) and is not a basis for a savings claim, so it is not used
here. Everything was compiled exactly as `qlang_run` compiles, i.e. through
`QLangCompiler::compile()`; the whole `tests/scripts/*.ql` corpus (22 scripts,
**10,257 instructions, 54,809 code bytes, average 5.34 B, largest 11 B**) is
kept as the second column.

`perf_stress.ql` compiles to **526 instructions** (`main` 522 + `square` 4) in
**1,831 code bytes**, average 3.48 B, largest 7 B. `qlang_run --output` writes a
**6,719 B** image:

| section | bytes | share |
|---------|-------|-------|
| code stream | 1,831 | 27.3% |
| offset table (`offCount × u32`) | **2,104** | 31.3% |
| `loc` side table (`locCount × u16 u16`) | **2,104** | 31.3% |
| header + function metadata + constants + native slots | 680 | 10.1% |

**The two side tables are 4,208 B — 62.6% of the image; the instructions
themselves are 27%.** The offset table alone is 115% of the code it indexes
(230% of it in RAM, where it is 8 B/instr).

Candidate encodings, measured:

| encoding | perf_stress | corpus |
|----------|-------------|--------|
| today, `u32` per instruction | 2,104 B | 41,028 B |
| ULEB delta per instruction | 526 B | 10,257 B |
| anchors + nibbles, K=4 — `u32` / ULEB anchors | 791 / 395 B | 15,454 / 7,699 B |
| … K=8 | 531 / **330** B | 10,314 / **6,414** B |
| … K=16 | 399 / 297 B | 7,782 / 5,781 B |
| … K=32 | 335 / 281 B | 6,526 / 5,736 B |
| `loc` today (`u16` line + `u16` col) | 2,104 B | 41,028 B |
| `loc` as ULEB `(Δline, col)` | 1,088 B | 20,613 B |

Instruction-size histograms (size → count): perf_stress
`2:68 3:174 4:260 5:12 6:11 7:1`; corpus
`2:193 3:735 4:1659 5:516 6:7128 7:13 11:13`.

Three things stand out:

1. **The tables, not the code, dominate the image** (62.6%), and `loc` is
   byte-for-byte as large as the offset table while serving the same
   error-path-only consumer. Compressing one and leaving the other at 4 B/instr
   merely re-ranks the wall (§7.4).
2. **Anchors + nibbles beat plain ULEB deltas at every stride once the anchors
   are varints too** (K=8: 330 B vs 526 B on perf_stress, 6,414 vs 10,257 on the
   corpus). With fixed `u32` anchors the crossover is K≈16, because
   `N/2` nibble bytes + `N/8 · 4` anchor bytes = `N` exactly at K=8. Anchor
   deltas average `K × 3.5–5.3` B, so a ULEB anchor is usually one byte.
3. The savings are **flat-rate, not proportional to program size**: 3.5 B
   recovered per instruction, so a 526-instruction program saves 1.8 KB and a
   10k-instruction program saves 35 KB.

## 3. The invariant holds — for deltas, not for offsets from a key

`escapedInstructionSize` (`src/QLangBytecode.cpp:432-490`) bounds any
instruction at `2 (op + modifier) + 4 (left operand) + 8 (immediate) + 1 (third
operand) = 15` B; a conditional `jmp32` is 7. So **the distance between two
consecutive instructions is always 1..15 and fits one nibble**, with `0` free as
a padding sentinel. The measurements agree: perf_stress never exceeds 7 B and
the corpus maximum is 11 B.

The hypothesis needs one correction: the nibble cannot be "the offset of
instruction *k* from its key". With an average instruction of 3.48 B
(perf_stress) to 5.34 B (corpus), the cumulative distance from an anchor passes
15 after 3–4 instructions, so a block of any useful length cannot express
offsets from its key in 4 bits.

What *does* fit is a **run-length size stream**: each nibble is the size of one
instruction, i.e. the delta to the next one. Absolute positions then come from
periodic anchors, and the "key instructions" are simply every K-th instruction
rather than a semantically chosen set (there is no reason to key on jump
targets — jump operands already carry byte offsets, so they need no lookup).

## 4. Design sketch (option B below)

Per function:

```
uleb  instrCount
uleb  anchorStride K            // fixed per file, or per function
uleb  anchorCount = ceil(instrCount / K)
      anchorCount × uleb offset // absolute byte offset of instruction i*K
      ceil(instrCount / 2) bytes// nibble[i] = size of instruction i, 1..15; last
                                // instruction's size is implied by codeSize
```

- `index → offset`: `anchors[i / K] + Σ nibbles[i % K .. ]` — at most `K-1`
  additions. Sixteen nibbles live in one `uint64_t`, so a SWAR/SIMD prefix sum
  is available if it ever shows up in a profile; a plain loop is fine at
  K ≤ 32.
- `offset → index` (what `instructionIndexAt` does): binary search over
  `instrCount / K` anchors, then scan ≤ K nibbles forward. `main` of
  perf_stress (522 instructions) searches 33 anchors at K=16 instead of 522
  offsets; a 10k-instruction function searches 625.
- Anchor deltas average `K × 3.5–5.3` B, so a ULEB anchor is usually one byte
  and the `u32`-anchor rows in §2 are pessimistic (perf_stress K=8: 531 B →
  330 B).

Measured **wire** density of the scheme (perf_stress): 0.63 B/instr at K=8,
0.56 B at K=16, 0.53 B at K=32 — versus 4 B today. The corpus gives the same
0.56 B/instr at K=16. In **RAM**, where anchors would be plain `uint32_t`
rather than varints, the density is `0.5 + 4/K` B/instr: 1.0 at K=8, 0.75 at
K=16, 0.63 at K=32 — versus 8 B today. The two strides differ for a reason: a
varint anchor is cheap enough to place every 8 instructions, a 4-byte anchor is
not.

## 5. The table is derivable — the bigger win

The `.qlc` reader **already decodes every instruction** to validate the stream
(`src/QLangBytecodeIO.cpp:531-547`), and `decodeInstruction` computes
`instructionSize(op, escaped, e)` as it goes. The offset table therefore carries
**no information that the code stream does not already have**: it can be
reconstructed during a pass the loader performs anyway, for zero wire bytes and
zero extra asymptotic work.

Rebuilding by scanning is also a *stronger* validator than the current
table-driven one. Today the reader rejects overlap
(`lastEnd > instrOffsets[j + 1]`) and trailing bytes (`lastEnd != codeSize`), but
a **gap** between `lastEnd` and the next declared offset is accepted, leaving
dead bytes inside the stream. A scan from offset 0 cannot express a gap at all.

With the table off the wire, `locCount` becomes the instruction count and
"decoded count == locCount" is the cross-check that replaces the
`locCount == offCount` check (`src/QLangBytecodeIO.cpp:522-524`).

## 6. Where the random-access requirement actually lands

`instructionIndexAt` runs only from `vmThrowAt` / the builtin error path
(`src/QLangVM.cpp:1050`, `:1062`, `:2230`) — i.e. once per raised error, per
frame, on a path that is already building a backtrace and formatting a message.
A linear walk over a few thousand instruction sizes (1–2 byte reads each) is
microseconds there. Nothing in the design needs O(log n) lookup for
correctness; that is a preference, not a constraint, and it is what makes
options A and C viable.

## 7. Options

### 7.1 A — drop the table from the wire, rebuild it at load

`offCount`/offsets leave the format; the reader's existing validation walk
populates the in-memory table. Change `instrOffsets` to `std::vector<uint32_t>`
(code streams are already capped at `kMaxInstructions × 15`, and offsets are
`u32` on the wire today).

- Disk: perf_stress 6,719 B → 4,615 B (**−2,104 B, −31.3%**); the corpus table
  is −41,028 B.
- RAM: 8 → 4 B/instr (perf_stress 4,208 B → 2,104 B for this table).
- Validation: strictly stronger (gaps become unrepresentable).
- Diff: smallest of the three; no new data structure, no lookup rewrite.

### 7.2 B — A, plus the anchor/nibble layout in memory

`Function` stores nibbles + anchors instead of a vector of offsets
(§4). RAM: 4 → ~0.6 B/instr. Lookup becomes "binary search anchors + ≤K nibble
adds" in `instructionIndexAt`, and a sequential iterator replaces the
`for (j : instrOffsets)` loops in the disassembler and in `codec_check`.

Buys 3.25–3.4 B/instr of **RAM** over A (perf_stress `main`: 2,088 B → 392 B at
K=16; the corpus: 41,028 B → 7,782 B). It changes nothing on disk once A has
removed the table from the wire. Worth it if resident instruction counts get
large; marginal at the current scale, and it is the only option that adds a
new encode/decode invariant to get right.

### 7.3 C — no table at all

Store nothing; derive sizes from `fn.code` on demand (a `sizeOfAt(code, pc)`
helper over `decodeInstruction`/`instructionSize`). 0 B everywhere, but
`index → offset` is O(n) from 0, `offset → index` is O(n), and every consumer
(disassembler, `codec_check`, `codegen_size_check`, `instructionIndexAt`) goes
through the helper. Simplest possible state, worst access, and it makes the
"one instruction per `loc` entry" coupling implicit rather than structural.

### 7.4 The `loc` table is the co-equal target

`loc` is 4 B/instr on disk and 8 B in RAM, and it is *also* only read on the
error path. Lines are non-decreasing along the stream, so `(Δline: uleb,
col: uleb)` measures **1,088 B on perf_stress (2.07 B/instr, −48%)** and
20,613 B on the corpus (2.01 B/instr). Doing the offset table without `loc`
just re-ranks the same wall: after option A, perf_stress would be 1,831 B code,
2,104 B `loc`, 680 B rest — `loc` becomes the largest single section.

## 8. Impact and risks

| Area | Consequence |
|------|-------------|
| `.qlc` version | 6 → **7**. The reader accepts `version == kQlcVersion` only (`src/QLangBytecodeIO.cpp:469`), so v6 files are rejected — same one-version policy as the v5 → v6 bump. Every checked-in `.qlc`/`.qla` artifact is stale afterwards. |
| Codec tests | `tests/unit/codec_check.cpp` compares `a.instrOffsets == b.instrOffsets` (`:228`, `:423`, `:513`, `:562`, `:640`) and walks the table (`:259-262`); it must compare the rebuilt table or the code bytes instead. |
| Budget gate | `tests/unit/codegen_size_check.cpp:93-95` counts instructions via `instrOffsets.size()`; needs an explicit count accessor. |
| Disassembler | `examples/qlang_run.cpp` must be updated in the same change (AGENTS.md / `13-bytecodes.md` `## disassembler`). |
| Docs | `docs/SERIALIZATION.md` §4.1 (record layout, §7 rules, §4 checklist) and `docs/spec/13-bytecodes.md`; then re-record the spec SHA256 and the `docs/compliance/` checkpoint rows **after** a green build — per AGENTS.md, testing proves correctness, the hash merely detects change. |
| Risk | Low for A (deletion + a type change). B adds a new index abstraction whose encode/decode must agree exactly, the same class of invariant as `instructionSize` (§10 of the escape-byte plan). |

## 9. Recommendation (pending sign-off)

1. Take **A** first: it is pure deletion, it takes 2,104 B (31.3%) out of the
   perf_stress image, it halves the in-memory table, and it strengthens load
   validation.
2. Revisit **B** only if resident instruction counts grow enough that another
   3.25–3.4 B/instr of RAM matters. The hypothesis is sound once restated as
   "nibble = delta to the next instruction, anchors every K-th instruction",
   with **K ≥ 16 in RAM** (u32 anchors) and **K = 8 on the wire** (varint
   anchors).
3. Fold **§7.4** (`loc` delta-encoding) into the same step: it is worth another
   1,016 B on perf_stress, and leaving it alone makes `loc` the largest section
   of the image. A and §7.4 together take perf_stress from **6,719 B to ~3,600 B
   (−46%)**.

## 10. Checklist if this is pursued

- [ ] Decide A / B / C and the `loc` treatment; record the decision here.
- [ ] `kQlcVersion` 6 → 7; reader/writer changes; reject v6 with the standard
      "unsupported version" message.
- [ ] `Function` representation + `instructionIndexAt`; disassembler and
      `examples/hello.cpp` in the same change.
- [ ] `codec_check` round-trip and `codegen_size_check` count accessor; add a
      case for a stream with an interior gap (must now be rejected) and, for B,
      a K-boundary/`K`-straddling lookup case.
- [ ] `cmake -B build && cmake --build build`, `ctest --test-dir build`.
- [ ] Re-measure perf_stress (image bytes + the §2 encoding table) and record
      the before/after.
- [ ] `docs/SERIALIZATION.md` §4.1 + `docs/spec/13-bytecodes.md`; re-record the
      hash and the compliance checkpoint rows after green.

---

### Appendix — how the numbers were produced

Two throwaway tools, kept out of the tree and not added to CMake, linked
against `build/libqlang.a`:

- `/tmp/idxstats/idx_stats2.cpp` compiles the given scripts the way
  `codegen_size_check` does and tallies `fn.code.size()`,
  `fn.instrOffsets.size()`, the delta histogram, and each candidate encoding
  (ULEB deltas, anchors + nibbles at K = 4/8/16/32 with both `u32` and ULEB
  anchors, and `loc` as ULEB `(Δline, col)`). Run it as
  `idx_stats2 tests/scripts/perf_stress.ql` or `idx_stats2 tests/scripts/*.ql`.
- `/tmp/idxstats/idx_fn.cpp` prints the per-function breakdown (§2's
  `main`/`square` split).

The 6,719 B image size is `qlang_run --output /tmp/idxstats/perf_stress.qlc
tests/scripts/perf_stress.ql`; the section bytes come from the tool, and the
"rest" row is the difference. Re-create them from §2/§4 if the numbers need
refreshing.
