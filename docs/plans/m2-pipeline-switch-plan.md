# M2 — Switch the Pipeline to v6 (Discard the v5 Codec)

Status: **In progress — S1 done (codec-only, uncommitted).** Baseline `HEAD = 0b4a4fb`
("encoding: v6 modifier byte (M1) — 2/2/4 layout, kind/form tables, \*V6 codec +
`encoding_v6` suite").
Parent plan: [encoding-v6-and-indirect-plan.md](encoding-v6-and-indirect-plan.md) — this file
executes its **§6.1 milestone M2** and cites its sections (§2 layout, §3 coexistence, §6
phasing, §8 file format, §9 pool, §10 impact, §11 verification gate, §12 open questions).
Reference spec: `docs/spec/13-bytecodes.md` (`## encoding extended`, `### bytecode encoding`,
`## constant pool`) · `docs/SERIALIZATION.md` §4.
Deviation from the parent plan: **the v5 codec is deleted here** rather than kept side by side
until Phase 3. M1 shipped the v6 layer additively (`kQlcVersion = 5`, v5 codec untouched); M2
makes v6 the only encoding.

---

## 1. Scope

**In scope (M2):**

1. v6 becomes the only instruction codec; the v5 size/encode/decode functions are deleted.
2. `.qlc` format version 5 → **6**; the reader accepts v6 only (v5 rejected like v1–v4).
3. The VM decodes v6 and materializes a kind-selected right operand (register / pool /
   immediate of any form, including the zero-byte `bool`/`nil`).
4. The compiler emits the **unified** `mov`/`add`/`sub`/`cmp` opcodes with the right-operand
   kind and the immediate form selected per literal, so `bool`, `nil` and `float` leave the
   constant pool (§9).
5. The disassembler (`examples/qlang_run.cpp`) and `examples/hello.cpp` decode and print v6
   (mandatory in the same change — `13-bytecodes.md` `## disassembler`, AGENTS.md).
6. Tests and documentation updated to v6-only.

**Out of scope:**

| Item | Where it belongs |
|------|------------------|
| The 7 new opcodes `eqf`/`ltf`/`gtf`/`ltef`/`gtef`, `tst`, and the `ja`/`jae`/`jb`/`jbe` aliases | **M3** (parent plan §4, §5) |
| `DECL`, `INDGET`/`INDPUT`, frame name map, declaration bitset, declaration table in the function record | **M4** (parent plan §7, §8) |
| Deleting `MOVI`/`MOVC`/`ADDI`/`SUBI`/`CMPI`/`JMPIFFALSE`/`JMPIFTRUE` from the enum and renumbering identities | **Phase 3**, `.qlc` v6 → v7 (parent plan §6) |
| `jmpfalse`/`jmptrue` emission changes | M3 (parent plan §12 item 6 default: keep them for bare truthiness) |
| Emitting 64-bit / unsigned immediates | follow-up (see D3) |

**Invariant:** the deprecated spellings keep decoding and executing (§3 coexistence, §11 item
6). M2 removes the *v5 encoding*, not the *deprecated opcodes*.

---

## 2. Decisions to settle before coding

| # | Decision | Recommendation | Rationale |
|---|----------|----------------|-----------|
| **D1** | Drop the `V6` suffix now that exactly one codec exists: `encodeInstructionV6` → `encodeInstruction`, `decodeInstructionV6` → `decodeInstruction`, `instructionSizeV6` → `instructionSize(op, escaped, e)`, `escapedInstructionSizeV6` → `escapedInstructionSize`, `makeModifierV6` → `makeModifier`, `baseModifierV6` → `baseModifier`. | **Yes** | 69 occurrences inside `QLangBytecode.{h,cpp}` + 44 in `encoding_v6_test.cpp`, but **zero churn at the 14 external call sites** (VM, IO, `qlang_run`, `hello`, `codec_check`): the v5 and v6 `encode`/`decode` signatures are identical, so the rename lands on the callers for free. The suffix would also be wrong after Phase 3 (v7 changes identities, not the layout). |
| **D2** | `DecodedInstruction` gains `ImmForm immForm` (filled by the decoder). | **Yes** | `ImmPayload::Signed` alone cannot distinguish `Int` (forms `i8`/`i16`/`i32`) from `Long` (`i64`); re-reading `modifier` at every dispatch site is cheaper to avoid than to audit. One byte, decode stays cheap (§12 item 1). |
| **D3** | Immediate **emission** restricted to literals whose `QLangValue` type round-trips exactly: `Int` within ±2³¹ (forms `i8`/`i16`/`i32`), `Float` (form 8), `Bool` (9/10), `nil` (11). `Long`/`Qword`/`Unsigned` literals stay in the pool. | **Yes** | The codec already supports `i64`/`u64`, but the type mapping is ambiguous (a 64-bit literal is `Qword` or `Long` depending on how it was produced). Restricting emission keeps value semantics byte-identical; widening is a separate, measurable follow-up. |
| **D4** | `.qlc` v6 in M2 **without** the declaration table; M4 adds it as an optional section inside v6; Phase 3 bumps v7. | **Yes** | No external `.qlc` images exist (only stale local artifacts). Parent plan §8 lists the declaration table under v6 — annotate it as "added in M4". Alternative: M4 takes v7 and Phase 3 takes v8. |
| **D5** | Delete `tests/unit/encoding_test.cpp` and its `encoding` ctest target (pure v5 codec); fold its two cases with no v6 analogue into `encoding_v6_test.cpp`. | **Yes** | `encoding_v6` (34 249 checks) already covers base/escaped round-trips, immediate boundaries and the exhaustive (opcode, modifier) sweep. Unique cases to port: jump-width tiling, truncated-stream decode error. |
| **D6** | Pool reads: `mov`/`movc` clone containers (`freshContainerValue`); ALU/`cmp` right operands read straight from the pool do **not** clone. | **Yes** | `freshContainerValue` deep-copies arrays/sets/tables/blobs (`QLangVM.cpp:171`); cloning per arithmetic op would be a real regression, and the ALU right operand is read-only. |

---

## 3. Steps

Each step ends with a green build; S1–S4 switch the **encoding**, S5 switches the **instruction
selection**. Splitting them means a corpus regression in S5 cannot be an encoding bug.

### S1 — v6 becomes the only codec

> **DONE (uncommitted).** `encoding_v6` 34 265 checks pass; `codec` 133/135 (the two
> failures are `invalid CALL escape modifier` from the still-v5 emitter — S4).
> `tests/unit/encoding_test.cpp` is orphaned (no CMake target) and still needs
> `git rm tests/unit/encoding_test.cpp`.

`include/qlang/QLangBytecode.h`

* Delete the v5 declarations: `escapedInstructionSize(Opcode, uint8_t)` (L375),
  `instructionSize(Opcode, uint8_t modifier = 0)` (L376), and the v5
  `encodeInstruction` / `decodeInstruction` prototypes.
* Apply the D1 renames to `makeModifierV6` (L313), `baseModifierV6` (L333),
  `escapedInstructionSizeV6` (L386), `instructionSizeV6` (L387), `encodeInstructionV6` (L410),
  `decodeInstructionV6` (L412).
* Add `ImmForm immForm` to `DecodedInstruction` (L348-365) — D2.
* Rewrite the migration comment block (L246-255, "both exist side by side during the
  migration") to state that v6 is *the* encoding, and the `instructionSizeV6` comment (L378-385)
  which explains the explicit `escaped` flag (M1 decision 1) — keep the reasoning, drop the
  v5 comparison.

`src/QLangBytecode.cpp`

* Delete v5 `escapedInstructionSize` (L121), `instructionSize` (L173), `encodeInstruction`
  (L455), `decodeInstruction` (L626).
* Rename the v6 definitions: `makeModifierV6` (L212), `baseModifierV6` (L328),
  `escapedInstructionSizeV6` (L804), `instructionSizeV6` (L864), `encodeInstructionV6` (L1011),
  `decodeInstructionV6` (L1109); fill `immForm` in the decoder.
* Keep `baseInstructionSize` and `baseOperandCount` (shared by both versions, unchanged).
* Keep `permittedKinds` (L372), `kindPermitted` (L417), `isInPlaceForm` (L420),
  `defaultRightKind` (L313), `isAluOrFlagCompare` — these are the normative §2.5/§2.6 tables.

`tests/unit/encoding_v6_test.cpp`

* Rename the 44 `*V6` occurrences.
* Delete `sameBytesAsV5()` (L178) and its 10 call sites (L509-571) — they compare against the
  deleted v5 encoder. Replace with **golden byte vectors** transcribed from parent plan §2.9,
  e.g. `mov r3, 200` → `[MOV|80][00][03][C8]`, `mov r3, 1.5` → 11 B, `mov r3, true` →
  `[MOV|80][A0][03]`, `mov r3, nil` → `[MOV|80][B0][03]`, `mov r3, pool[300]` →
  `[MOV|80][18][03][2C 01]`, `call f, 2` → `[CALL|80][50][01][id:2][02]`.

*Verify:* `cmake --build build && ctest --test-dir build -R "encoding_v6|codec"` (the codec
suite still fails on version assertions until S7 — expected; note it and continue).

### S2 — `.qlc` v6

> **DONE (uncommitted).** `kQlcVersion = 6`; reader/writer wording updated; load guard is
> `kMaxInstructions * 15` (measured widest instruction = 15 B, `CONV`). Verified with a
> throwaway harness (`/tmp/qlc_v6_check.cpp`, 25 checks): version 6 written, hand-encoded
> v6 image round-trips, versions 3/4/5/7 rejected with a version error, reserved kind /
> left form / immediate form and an unpermitted kind rejected at load, unescaped `CALL`
> rejected, oversized code stream rejected, a 15-byte instruction still loads.
> `codec` is 132/135: the two `invalid CALL escape modifier` failures (S4) plus
> `movi: format version 5` (`tests/unit/codec_check.cpp:319`, updated in S7).

`include/qlang/QLangBytecodeIO.h`

* `constexpr uint32_t kQlcVersion = 5;` (L34) → `6`.
* Header comments: "QLC1 v5 … readers accept v5 only" → v6.

`src/QLangBytecodeIO.cpp`

* L384 comment ("v5 is the escape-byte …") → v6 wording; `putU32(os, kQlcVersion)` (L387)
  unchanged.
* L469 `version != kQlcVersion` stays as-is ⇒ **v5 is now rejected** with the existing
  "unsupported format version" error (Round 37 precedent: single-version reader, clean break,
  no migration shim).
* L500 load guard `codeSize > kMaxInstructions * 7` → `* 15`. v6 maximum instruction =
  `2 (op + e) + 4 (dword left) + 8 (u64/float immediate) + 1 (third operand)` = 15 bytes
  (`CALL` is 9, conditional `jmp32` is 7). The stale comment "Max instruction is 7 bytes" must
  go.
* L533 validation loop calls the (renamed) v6 decoder, which routes every escaped form through
  the size table ⇒ reserved `kind = 3`, reserved left form 3, immediate forms 12–15, kinds not
  permitted for the opcode, and an unescaped `CALL` **fail at load**, satisfying §8 "Validation".
* No new sections in M2 (D4).

*Verify:* `qlang_run --output /tmp/t.qlx hello.ql` then load it back; a v5 image must fail with
the version error.

### S3 — VM decodes v6

`src/QLangVM.cpp`

* The single decode site `runFrames()` L1948 needs **no textual change** after D1
  (`decodeInstruction(code.data(), pc, code.size(), i)`), which is the point of D1.
* Add a static helper next to `freshContainerValue` (L171):

  ```cpp
  // Materialize the right operand of a decoded v6 instruction (plan §2.2/§2.3).
  // `cloneContainers` is true only where the value is *stored* into a register
  // (mov/movc), so a pool literal cannot be aliased and mutated; ALU/compare
  // operands are read-only and are returned without copying (decision D6).
  static QLangValue operandValue(const DecodedInstruction &i, Frame &frame,
                                 const std::vector<QLangValue> &consts,
                                 bool cloneContainers);
  ```

  Mapping: `Register` → `frame.regs[i.b]` · `ConstPool` →
  `cloneContainers ? freshContainerValue(consts[i.b]) : consts[i.b]` · `Immediate` → switch on
  `immPayload`/`immForm`: `Signed` → `Int` (`i8`/`i16`/`i32`) or `Long` (`i64`), `Unsigned` →
  `Unsigned` (`u8`/`u16`/`u32`) or `Qword` (`u64`), `Float` → `Float`, `True`/`False` → `Bool`,
  `Nil` → `Nil`.
* Route the existing handlers through it so the deprecated spellings stay correct for
  hand-built streams: `MOVC` (L1956), `MOVI` (L1961), `MOV` (L1964), `ADDI` (L2381),
  `SUBI` (L2386), `CMPI` (L2416).
* ALU family (L2391-2411) and `CMP` (L2413) gain the arity rule (§2.6):

  ```cpp
  if (isInPlaceForm(op, i.rightKind))
    frame.regs[i.a] = doBinaryVm(op, frame.regs[i.a], operandValue(i, frame, consts, false));
  else
    frame.regs[i.a] = doBinaryVm(op, frame.regs[i.b], frame.regs[i.c]);
  ```

  and for `CMP`: kind `Register` → `cmpFlags(regs[a], regs[b])`, otherwise
  `cmpFlags(regs[a], operandValue(...))`.
* `CONV`/`CAST` keep `c` as a plain 1-byte type id — no kind field consulted (§12 item 2).
* `cmpFlags` / `flagsSatisfied` (L1063-1074) and the one-hot flag model
  (`CMP_GR`/`CMP_EQ`/`CMP_LT`, `QLangVM.h:131-133`) are **unchanged**; flag jumps untouched.

*Verify:* the 21-script suite, especially `cmp.ql` (flag semantics must be identical) and
`ext_regs.ql` (dword operands).

### S4 — Compiler plumbing (still emitting the old spellings)

`include/qlang/QLangCompiler.h`

* `RawInstr` (L62-68) gains: `RightKind kind`, `ImmForm form`, `ImmPayload payload`,
  `uint64_t immUnsigned`, `double immFloat`.
* `FnEmitter::emit` (L82-86) keeps its positional shape (register-right default) and gains
  immediate/pool variants; `computeModifier` returns `std::pair<uint8_t, bool>` (modifier,
  escaped).

`src/QLangCompiler.cpp`

* `FnEmitter::computeModifier` (L114-165) rewritten for 2/2/4:
  * `CALL` → `makeModifier(left, RightKind::Immediate, ImmForm::I16)`, **always escaped**
    (§2.8); keep the existing range checks (register ≤ 65 535, id in `int16`, arg count ≤ 255).
  * immediate ops → `makeModifier(left, Immediate, formOf(payload))`, escaped whenever the form
    is not the base `i8` or the left index exceeds 255.
  * register/const ops → `makeModifier(left, defaultRightKind(op), rightForm)`, escaped only when
    an index exceeds 255 or the kind differs from the opcode default.
  * **Never** return `0` to mean "base form" — under 2/2/4 `e = 0x00` is a legal escaped
    modifier (M1 decision 1). The `escaped` flag is the only base-form signal.
* `FnEmitter::finish()` (L25-105): sizing via `instructionSize(op, escaped, modifier)`; the
  `DecodedInstruction` handed to `encodeInstruction` must carry `escaped`, `modifier`,
  `rightKind`, `immPayload`, `immForm`, `imm`, `immUnsigned`, `immFloat`.
* Jump path **unchanged**: the jump class is byte-for-byte v5 (§2.7), `escapedInstructionSize`
  reads `e` as an offset-width selector only (`e > 1` throws), so
  `di.escaped = sizes[i] > baseInstructionSize(op)` and `di.modifier = (sizes[i] >= 6) ? 1 : 0`
  remain correct (jmp16 = 4/5 B, jmp32 = 6/7 B). The backpatch fixpoint and its post-loop
  validation are untouched.

*Verify:* **the whole script suite must pass with v6 bytes while the compiler still emits
`movi`/`movc`/`addi`/`subi`/`cmpi`** (they encode as v6 base forms via `defaultRightKind`).
This is the milestone's cheapest high-value checkpoint.

### S5 — Unified kinds + immediate selection (the pool shrinks)

New emission helpers on `QLangCompiler` (next to `emit`/`emitImm`, L747-768):
`emitImmI(op, a, int32_t)`, `emitImmF(op, a, double)`, `emitImmB(op, a, bool)`,
`emitImmNil(op, a)`, `emitPool(op, a, poolIdx)`; `emit(op, a, b, c)` stays register-right.
Generalize `isIntLiteral` (L770) into `literalImmOf(const QLangValue&)` returning
`{form, payload, value}` or `nullopt` (pool path) — D3.

Central decision point `ExprKind::Literal` (L2852-2856): dispatch on `QLangValue::Type` —
`Int` → `i8`/`i16`/`i32` · `Float` → form 8 · `Bool` → 9/10 · nil → 11 ·
`String`/`Blob`/`Array`/`Table`/`Set`/closure → pool (kind `ConstPool`).

Site conversions (line numbers at baseline `0b4a4fb`):

| Current | New | Sites |
|---------|-----|-------|
| `emitImm(MOVI, r, v)` | `emitImmI(MOV, r, v)` | 706, 713, 1352, 1357, 1491, 1624, 1627, 1635, 1641, 1817, 1820, 1827, 1833, 1929, 2495, 2525, 2573, 2981, 3974 |
| `emitImm(ADDI/SUBI, r, k)` | `emitImmI(ADD/SUB, r, k)` (in-place by §2.6) | 679, 684 (`foldSelfArith`), 1446, 1509, 3986, 4029 |
| `emitImm(CMPI, ra, v)` | `emitImmI(CMP, ra, v)` | 849 (`emitCmpJumpFalseExpr`) |
| `emit(MOVC, r, emitConst(QLangValue()))` (nil) | `emitImmNil(MOV, r)` — 3 bytes, **no pool entry** | 419 (`releaseTemp`), 1202, 1394, 2148, 2160, 2180, 3338, 3353 |
| `emit(MOVC, r, emitConst(x))` (string/container) | `emitPool(MOV, r, idx)` | 717, 1293, 1411, 2388, 2431, 2456, 2479, 2493, 2501, 2508, 2523, 2531, 2546, 2549, 2555, 2571, 2579, 2688, 2863, 3112, 3116, 3161, 3194, 3207, 3219, 3231, 3233, 3244, 3257, 3271, 3475, 3478, 3503, 3514, 3574, 3608, 3611, 3625, 3804, 4018 |
| `emit(ADD, d, b, c)` and friends | unchanged (kind `Register`) | — |
| `MAKEFUNC`/`MAKEGEN` | unchanged (kind `ConstPool` only, §2.5) | — |
| `JMPIFFALSE`/`JMPIFTRUE` | unchanged in M2 (§12 item 6) | — |

Also touched: `compileArgsToSlots` (L706-717), `foldNegLiteral` (L2981), the class-metadata
builders (L2388-2579) where `MOVI 1` / `MOVI i` become `MOV` immediates while the string keys
stay pool entries.

*Verify:* corpus stdout byte-identical to the pre-change build (§11 item 1) **plus** a pool-size
diff showing bool/nil/float entries gone (§11 item 10) — record the before/after numbers in the
commit message.

### S6 — Disassembler and demos (same change, AGENTS.md mandate)

`examples/qlang_run.cpp`

* `operandText()` (L476): replace the `isImmediateOpcode` branch with `rightKind`-driven output —
  signed decimal / unsigned decimal / `float` / `true` / `false` / `nil` for kind 0,
  `pool[0xNN]` for kind 2, `a, b[, c]` for kind 1; keep the `e=0xNN` modifier prefix and the
  jump form (`offset -> target`).
* `pseudoText()` (L245): `MOV` kind 0 → `r3 = 200`, kind 2 → `r3 = pool[0]`; ALU in-place →
  `r3 = r3 + 5`; `CMP` with an immediate right operand; the `bin`/`cmp` lambdas need a
  right-operand variant.
* `disassemble()` (L676, decode at L694 and L739) and `operandMask()` for the new fields.
* Header/usage text "QLC1 v5" (L2, L58) → v6.

`examples/hello.cpp:126` — same treatment (it prints `isImmediateOpcode(ins.op)` today); it must
at least keep compiling and printing sane operands.

*Verify:* `--asm-level 0..5` well-formed (§11 item 9) on `hello.ql`, `cmp.ql`, `ext_regs.ql`,
`ext_consts.ql`, `jump_widths.ql`, `imm_kinds.ql`.

### S7 — Tests

* Delete `tests/unit/encoding_test.cpp` and its CMake target (`CMakeLists.txt` L170-174,
  `encoding`); port jump-width tiling and the truncated-stream decode error into
  `tests/unit/encoding_v6_test.cpp` (D5).
* `tests/unit/codec_check.cpp`: version assertions 5 → 6 (L310-324, 405-413, 455-463, 513-521);
  the malformed-version probe byte `4` → `5` (L957, "v5 is the current format" → v6); the
  decode call sites (L258, 653, 707, 750, 795, 825, 1142, 1157, 1194, 1216) keep their names
  after D1.
* New `codec_check` cases: load-time rejection of `kind = 3`, immediate forms 12–15, a kind not
  permitted for the opcode, and an unescaped `CALL` (§11 item 7); a **hand-built** v6 stream
  containing `movi`/`movc`/`addi`/`subi`/`cmpi`/`jmpfalse`/`jmptrue` that still executes (§11
  item 6).
* New `encoding_v6` cases: §2.9 golden bytes; size == encoder length at every `pc` over a stream
  exercising every (kind, form) pair (§11 item 3).
* New `tests/scripts/imm_kinds.ql`: bool/nil/float literals, mixed-width immediates
  (255/256, 65 535/65 536, ±2³¹), in-place compound assignment, `cmp` against an immediate.
* `CMakeLists.txt`: keep one codec suite target (`encoding_v6`); update the comment above it.

*Verify:* `ctest --test-dir build` fully green (M1 baseline: 47/47 with `perf` skipped).

### S8 — Documentation (only after the suite is green)

* `docs/SERIALIZATION.md` §4: magic `QLC1` version **6**, "readers accept 6 only", the 2/2/4
  modifier layout and kind/form tables, opcode identity bound `< kOpcodeUsedCount` (**73**, not
  the stale 68), §5/§9/§10 touch-ups.
* `docs/spec/13-bytecodes.md`: `## encoding extended` (L693-727) — flip "Specified below —
  `.qlc` v6, not implemented" to **shipped**; `### bytecode encoding` rewritten from the v5
  nibble description to 2/2/4; `## opCode scope`; `## constant pool` (L872) — bool/nil/float
  have left the pool; `## future plans` Phase 1 status.
* `docs/compliance/{status,history,tests,harness,README}.md`: M2 round entry, the removed
  `encoding` target row, `codec_check` "format version 5" → 6, `encoding_v6` check count,
  Open Items flipped; then re-hash the `docs/spec/README.md` rows
  (`update-doc-hashes.sh`). **Never bump a hash before the tests pass** (AGENTS.md).
* Parent plan `encoding-v6-and-indirect-plan.md` §6.1: M2 row → **DONE** with the settled
  decisions (D1–D6 above) recorded next to M1's two; §8 annotated per D4.

### S9 — Housekeeping and commits

* Stale v5 images become unreadable: `hello_*.qlc`, `test-hello_*.qla` in the repo root and
  `examples/qla/hello_*.qlc`. Delete or gitignore — **ask the user which**.
* Suggested commit split (stage by concern, never auto-commit):
  1. `encoding: make v6 the only codec; .qlc v6 (M2a)` — S1, S2, S7 (codec parts)
  2. `compiler: unified mov/add/sub/cmp kinds + immediate selection (M2b)` — S3, S4, S5
  3. `tools: v6 disassembler` — S6
  4. `docs: v6 encoding shipped` — S8
* Remove `*.orig` / `*.rej` artifacts if any.

---

## 4. Verification gate (parent plan §11, M2 subset)

```bash
cmake -B build && cmake --build build
ctest --test-dir build
build/qlang_run --asm-level 5 --profile tests/scripts/hello.ql
build/qlang_run --output /tmp/t.qlc tests/scripts/hello.ql   # round-trip + execute
```

| §11 item | Check | Step |
|----------|-------|------|
| 1 | every `tests/scripts/*.ql` compiles, runs, stdout byte-identical to the pre-change build | S4, S5 |
| 2 | compile → `.qlc` v6 → read → execute → stdout matches the direct run | S2, S7 |
| 3 | `instructionSize()` == the encoder's emitted length at every `pc` over every (kind, form) | S1, S7 |
| 4 | `true`/`false`/`nil` cost 3 bytes and add **no** pool entry | S5, S7 |
| 6 | hand-built stream with the deprecated spellings still executes | S7 |
| 7 | `kind = 3` / forms 12–15 rejected **at load** | S2, S7 |
| 9 | `--asm-level 0–5` well-formed for every new form | S6 |
| 10 | corpus diff showing bool/nil/float entries gone from the pool | S5 |

Items 5 (flag matrix) and 8 (`$$`) belong to M3 and M4.

---

## 5. Risks

| Risk | Mitigation |
|------|-----------|
| `pc` desynchronises if size / encode / decode drift | both codec paths already route through `escapedInstructionSize`; §11 item 3 test; the exhaustive (opcode, modifier) sweep from M1 |
| Disassembler forgotten → silent mis-decode or out-of-bounds reads | S6 lands in the same change (AGENTS.md / `13-bytecodes.md` `## disassembler`) |
| Value-type drift when a literal moves from the pool to an immediate | D3 restricts emission to types that round-trip exactly; corpus stdout diff (§11 item 1) |
| In-place ALU folding changes evaluation order or register liveness | S4/S5 split: the encoding switch is proven green before any instruction selection changes |
| Load guard too small for v6 (15 B) → false "code stream too large" | S2 raises it and a dword-left + `u64` case is added to `codec_check` |
| Deprecated spellings accidentally broken | §11 item 6 hand-built stream |
| Docs drift from the shipped format | S8 with hashes re-hashed only after a green run |

Effort estimate: S1–S3 mechanical-to-small, S4 small, **S5 is the bulk** (~70 emission sites,
mostly one-line), S6 medium, S7 medium, S8 docs. Two reviewable commits minimum (M2a / M2b).
