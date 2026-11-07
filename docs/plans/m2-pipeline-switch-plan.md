# M2 — Switch the Pipeline to v6 (Discard the v5 Codec)

Status: **In progress — S1 committed (`3bc55c7`), S2 (`35edb7a`), S3 (`e0a8323`), S4 (`337584c`),
S6 (`d577612`); S5 done (uncommitted)** (S6 was taken first because it is the AGENTS.md-mandated companion of
the S1–S4 encoding change and is independent of S5: it renders whatever kind/form the emitter
produces, so it is correct both before and after the pool shrinks). Baseline
`HEAD = 0b4a4fb` ("encoding: v6 modifier byte (M1) — 2/2/4 layout, kind/form tables, \*V6 codec +
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

> **DONE (uncommitted).** `operandValue` is a **private static member** of `QLangVM`
> (`QLangVM.h`, declared after `flagsSatisfied`) rather than a file-static: `Frame` and
> `RegFile` are private nested types, so a free function in the anonymous namespace cannot name
> it. Its definition sits just after that namespace closes (`QLangVM.cpp`, after the
> `} // namespace` at the end of `callBuiltin`) so it still sees `freshContainerValue`.
> `MOVC`/`MOVI`/`MOV` collapsed into one case, `CMP`/`CMPI` into one case, `ADDI`/`SUBI` keep
> `doBinary` and take their right operand from the helper, and the ALU family gained the §2.6
> branch. *Verification:* the script suite is still red for the S4 reason (the compiler emits
> v5 modifiers), so S3 was verified with a throwaway harness (`/tmp/vm_s3_check.cpp`, **23
> checks, all pass**) that assembles v6 streams with `encodeInstruction` and runs them through
> the real dispatch loop: immediate forms `i8/i16/i32/i64/u32/float/true/nil`, pool and register
> right operands, pool-container cloning on store (incl. nested) vs. no clone for ALU, in-place
> `add`/`sub` with immediate and pool right operands, 3-operand `add`/`mul`, and `cmp`
> (immediate and register kinds) plus legacy `cmpi` driving `je`/`jne`/`jg` both taken and not
> taken. `ctest` is unchanged at 6/46 (40 failures, all `invalid CALL escape modifier` /
> `right-operand kind not permitted` — the S4-owned v5-modifier streams).

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

> **DONE (uncommitted).** `RawInstr` carries `kind`/`form`/`payload`/`immUnsigned`/`immFloat`;
> `emit` derives `kind = defaultRightKind(op)` (and `payload = Signed` for an immediate kind), so
> every existing call site keeps its historical encoding, and `emitJump` builds the struct by
> field instead of by aggregate order. `computeModifier` returns `pair<uint8_t, bool>` and is
> driven by `r.kind`, not by `isImmediateOpcode`: `CALL` → `makeModifier(left, Immediate, I16)`
> always escaped, `baseOperandCount(op) == 0` → never escaped, `== 1` → the modifier only widens
> the left index with kind/form pinned to their base values, otherwise the base form requires
> `kind == defaultRightKind(op) && left == Byte && (immediate ? form == I8 : right == Byte)`.
> `finish()` sizes with `instructionSize(op, escaped, mod)` and mirrors the modifier's kind/form
> back into the `DecodedInstruction`; the jump path is byte-for-byte unchanged.
> **Codec change:** `baseOperandCount` was promoted from a file-static to a public declaration in
> `QLangBytecode.h` — the emitter must know whether an opcode *has* a right operand before it
> picks a modifier, and `INC`/`DEC`/`RETURN`/`YIELD`/`TABCREATE`/`TABFINALIZE`/`DELETEx` are
> emitted as `emit(op, r, r)`, so a naive right-width computation produced a modifier the codec
> rejects ("invalid escape modifier for a single-operand opcode") for any register > 255.
> *Verification:* `ctest` **45/46 pass, 1 skipped (`perf`)** — the only failure left is the
> S7-owned `movi: format version 5` assert (`codec_check.cpp:319`; its `readQlc` round-trip
> already passes). A throwaway emitter harness (`/tmp/emitter_s4_check.cpp`, **73 checks, all
> pass**) covers base forms, escape-by-immediate-form (`i8/i16/i32`), escape-by-left/right width,
> single-operand opcodes at `r300/r400/r600`, the §2.6 in-place `add`/`mul` (no trailing `c`),
> `mov` with `u8`/`true`/`nil`/`float` immediates, `call` at `e=0x50`/`0x51`, jump width
> selection (modifier always 0/1), pool dedup, and the three rejected combinations. Disassembly
> of `tests/scripts/cmp.ql` shows the expected v6 sizes (`movi` 3 B, `lt` 4 B, `movc`/`mov` 3 B,
> `call` 6 B) and a `.qlc` write → re-run round-trip reproduces the script output exactly.

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

> **DONE (uncommitted).** The helpers exist as specified (`emitImmI`/`emitImmF`/`emitImmB`/
> `emitImmNil`/`emitPool`), plus `emitLiteralImm(op, a, LiteralImm)` and
> `emitLiteral(op, a, value)` = `literalImmOf` else `emitPool(op, a, emitConst(value))` — the one
> call site every literal load now goes through. The old positional `emitImm` is **deleted**; the
> explicit-descriptor entry point is `FnEmitter::emitRight(op, a, const RawInstr &right, line,
> col)`, which copies `kind`/`form`/`payload`/`imm*`/`b` from a `RawInstr` built by the caller.
> `literalImmOf` (now `static`, next to its neighbours) is the D3 selector: `Int` within ±2³¹ →
> narrowest of `i8`/`i16`/`i32`, `Float` → form 8, `Bool` → 9/10, `nil` → 11, anything else
> `nullopt` → pool. `foldNegLiteral` folds float literals as well as ints now
> (`isNumericLiteral`/`negateLiteral`), so `-1.5` is a single float immediate. **Zero**
> `MOVI`/`MOVC`/`ADDI`/`SUBI`/`CMPI` references and zero `emitImm(` call sites remain in
> `QLangCompiler.cpp`; the deprecated identities still decode and execute (§3). The dead
> `RawInstr::hasRight` field S4 added (set, never read) was removed.
> **Encoding consequence worth recording:** `defaultRightKind(MOV) == Register`, so a *pool* `mov`
> names a kind the opcode would never mean on its own and must be announced — `mov e=0x08, r,
> pool[NN]` is 4 bytes where the old `movc` base form was 3. Measured below.
> **Two pre-existing selection limits surfaced (not S5 regressions, both follow-ups):**
> (a) `inferType`/`updateVarType` records only `Array`/`Table` for variables, so
> `emitCmpJumpFalseExpr` fuses `cmp` + immediate only for **literal-vs-literal** conditions — a
> variable-vs-literal guard still emits register `cmp` + flag jump; (b) compound `+=`/`-=` in-place
> folding stays **Int**-only, so a float RHS is loaded with `mov` (float immediate) and the ALU op
> remains 3-operand register-right.
> *Verification:* build green; `ctest` **45 passed / 1 skipped (`perf`) = 100 %** (`codec_check`
> **164 checks**). Corpus (§11 item 1): all **21** `tests/scripts/*.ql` run against a `git worktree`
> of `d577612` — **stdout byte-identical (2 255 B), exit codes identical (all 0)**. Pool diff
> (§11 item 10, throwaway harness `/tmp/poolstat.cpp` built against both trees; 18 scripts compile
> standalone, 3 need the `math` module): pool entries **515 → 477 (−38)**, `bool` **5 → 0**, `nil`
> **27 → 0**, `float` **6 → 0**, while `string` 441, `array` 15, `table` 5, `function` 15 and
> `unsigned` 1 are unchanged — exactly the D3 set. Code bytes **52 604 → 53 618 (+1 014, +1.9 %)**:
> the 11-byte float immediate and the pool-`mov` modifier byte cost more than the pool slot they
> free (`ext_consts.ql` 807 → 1 063 B for 258 pool loads). Disassembly shapes confirmed:
> `mov e=0xb0, 00, nil` (3 B), `e=0xa0` `true`, `e=0x90` `false`, `e=0x50` `300` (`i16`),
> `e=0x80` `1.5` (11 B), `e=0x08 … pool[00]`, register `mov 06, 00` base, `call e=0x50`.
> `--asm-level 0..5` well-formed on `hello.ql`, `cmp.ql`, `ext_regs.ql`, `ext_consts.ql`,
> `jump_widths.ql` (§11 item 9; `imm_kinds.ql` is S7). `.qlc` write → load → run round-trip for all
> 21 scripts: stdout identical, exit 0. **S7 overlap done here because the run had to be
> trustworthy:** the hand-built malformed images in `codec_check.cpp` still declared version 5, so
> every `rejects()` case passed vacuously — they now write version 6 and the version probe asserts
> that **5** is rejected.

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

> **DONE (`d577612`).** `operandText()` is driven by `rightKind`, not by `isImmediateOpcode`:
> two new helpers, `doubleText()` (shortest round-tripping `%.*g`, so `3.5` prints as `3.5` and
> not `3.500000`) and `immediateText()` (switches on `DecodedInstruction::immPayload` → signed
> decimal / unsigned decimal / float / `true` / `false` / `nil`), then the operand list is
> `a [, right] [, c]` with the arity taken from `baseOperandCount(op)` and the trailing `c`
> dropped when `isInPlaceForm(op, rightKind)` — the same rule the encoder applies, so the
> listing can never read past an in-place instruction. `CALL` keeps its own branch (its signed
> function id travels in `b`, not in `imm`): `dest, id, argc`. Kind 2 prints `pool[NN]`, kind 1
> (and the reserved kind, which the codec rejects before this point) prints `rN`.
> `pseudoText()` gained `right()` / `sources()` (the latter honours the in-place rule), the
> `bin`/`cmp` lambdas now read their sources through it, and `MOV`/`MOVC`/`MOVI`,
> `DEEPCOPY`/`ADDI`/`SUBI`/`CONV`/`CAST`/`NEG`/`NOT`/`EVALKEY`/`ARRAPP*`/`ARRLEN`/`LEN`,
> `LAND`/`LOR`/`INx`, `CMP` and `CMPI` were merged or rewritten around `right()`.
> **Three pre-existing gaps fixed:** `SUBI`, `JNE` and `Nop` had no `pseudoText` case at all, so
> their pseudo column was empty; they now print `r3 = r3 - 5`, `pc += off` and `noop`.
> The level-3 constant note is keyed on `rightKind == ConstPool` instead of an opcode list
> (`emit` is special-cased: its `b` is a pool index although its kind stays register), and the
> header/usage wording is "QLC1 v6". `examples/hello.cpp` gained `immText()` and its
> `dumpBytecode()` prints the right operand by kind (`imm=…` / `= const[i] (type)` / `rN`) with
> the same in-place arity rule.
> **Plan deviations:** there is no `operandMask()` in the tree (the fields are read directly at
> the two decode sites in `disassemble()`); `tests/scripts/imm_kinds.ql` does not exist yet
> (S7 creates it), so form coverage came from a throwaway hand-encoded image instead;
> `CALLCLOS` now prints all three stream operands (it printed `a, c` before, hiding the callee
> register).
> *Verification:* `ctest` **45/46 pass, 1 skipped (`perf`)** — unchanged from the S4 baseline, the
> only failure still being the S7-owned `movi: format version 5` assert. `--asm-level 0..5` is
> well-formed on `hello.ql`, `cmp.ql`, `ext_regs.ql`, `ext_consts.ql`, `jump_widths.ql` (and on
> every other `tests/scripts/*.ql`). A throwaway harness (`/tmp/asm_s6_check.cpp`, built with
> `g++ -std=c++20 -Iinclude … build/libqlang.a`) hand-encodes a 2-function v6 `.qlc` covering
> **every (kind, form) pair** — `u8/i8/u16/i16/u32/i32/u64/i64/float/true/false/nil`, pool and
> register right operands, `Word`/`DWord` left and right widths (`r70000`, `pool[65000]`), an
> out-of-range pool index, the deprecated `movi`/`addi`/`subi`/`cmpi`, in-place `add`/`sub`
> versus 3-operand `add`, `cmp` against an immediate and against a register, `conv`/`cast`
> keeping their type id, jumps of all three widths including a negative offset, `call` with a
> positive and a negative id, `nop` — writes it through `writeQlc`, re-reads it (`readQlc`
> round-trip ok), **executes it** (`qlang_run`, exit 0) and disassembles it at all six levels;
> `mov e=0xa0, 0a, true`, `mov e=0xb0, 0c, nil`, `mov e=0x30, 07, 4000000001`,
> `mov e=0x32, 11170, 18446744073709551615`, `add e=0x60, 06, 3 ; r6 = r6 + 3` and
> `cmp e=0x60, 06, 100 ; cmp r6, 100` all render. `./build/qlang_example` still runs.
> *Formatting:* the two `examples/` files use Allman braces throughout (116 brace-only lines in
> `qlang_run.cpp` at `HEAD`), so the new code matches the file; running `clang-format -i` on
> them would rewrite ~1 900 untouched lines, which AGENTS.md ("minimize unnecessary diffs")
> argues against. New lines stay within the 80-column limit.

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

> **DONE (working tree; one `git rm` still outstanding — see below).**
> **Retired suite.** `tests/unit/encoding_test.cpp` lost its CMake target in S1 and is orphaned.
> Both of its unique cases are ported into `encoding_v6_test.cpp`: the truncated-stream decode
> errors (§13 — truncated `i32`/`float` immediate, extended index, third operand, `jmp32` offset,
> `call` id and argc) and the jump-width tiling (§17, folded into the (kind, form) sweep). The
> file itself still needs `git rm tests/unit/encoding_test.cpp && rm -f build/encoding_test` —
> the agent shell denies `rm`, so this is a manual step. Nothing builds it, so the suite is green
> either way.
> **`encoding_v6_test`** gained §16, the §2.9 golden wire bytes (fixed prefix + little-endian
> operand bytes per row, `mov`/`add`/`cmp`/`jmp`/`call`, escaped and unescaped), and §17, one
> stream tiled by **every** (kind, form) pair plus the three jump widths, walked pc-by-pc to
> assert `instructionSize(op, escaped, e)` equals the encoder's own emitted length at every
> offset (§11 item 3). 34 249 → **34 466** checks.
> **`codec_check`** gained `testV6LoadValidation()` (20 checks: the §2.9 golden stream loads and
> round-trips; versions 3/4/5/7 rejected; `kind = 3`, left form 7 and right forms 12–15 rejected;
> a kind not permitted for the opcode (`arrput` + immediate) rejected; an unescaped `CALL`
> rejected; the size guard refuses an oversized `codeSize` while the widest legal instruction —
> `CONV`, 15 B — still loads) and `testDeprecatedSpellings()` (15 checks: a two-pass assembler
> emits 24 instructions using `movi`/`movc`/`addi`/`subi`/`cmpi`/`jmptrue`/`jmpfalse`, writes
> them through `writeQlc`, re-reads them with `readQlc` and **executes** them — §11 item 6).
> 164 → **199** checks. The version assertions (5 → 6) and the malformed-version probe byte had
> already moved during the S5 overlap, so there was nothing left to change there.
> **`tests/scripts/imm_kinds.ql`** (new, registered in `QL_SCRIPTS`) drives every immediate form
> end to end; `--asm-level 4` over it shows `e=0x40/0x50/0x60/0x80/0x90/0xa0/0xb0` plus the pool
> fallback `e=0x08`.
> **Plan deviations:** (a) the plan's boundaries "255/256, 65 535/65 536" do not straddle a v6
> form — v6 dropped `u8`/`u16`/`u32`, so the real ones are **127/128** (`i8`→`i16`) and
> **32 767/32 768** (`i16`→`i32`); 255 is `i16` and 65 536 is `i32`, and the script says so.
> (b) ±2³¹: `2147483647` is the last `i32`, `2147483648` falls back to the pool and
> `-2147483648` compiles to `pool` + `neg`; `-2147483649` wraps inside `neg`, so the script
> asserts nothing past the `i32` range. (c) `cmp` against an immediate is **not** fused —
> `z == 7` emits `mov e=0x40, tmp, 7` then `cmp r, tmp` — so the script asserts the behaviour,
> while the in-place `add`/`sub` immediates (which *are* fused, `add e=0x40, r0, 5`) are checked
> directly. (d) unsigned and qword literals (`44u`, `4294967295u`) are never immediate-folded;
> they always take the pool.
> *Verification:* `cmake -B build && cmake --build build` clean (Release, no new warnings).
> `ctest --test-dir build` **46/47 pass** — every unit suite and all 22 script tests, including
> the new `script_imm_kinds`. The one failure is `perf`, which S7 neither owns nor affects (the
> working tree touches no `src/` or `include/` file): this build dir is `Release`, so the gate
> runs instead of skipping (77), and `tests/perf/baseline.json` was last written before the v6
> switch (`c9acf81`) — `perf_stress.ql` execute is +31 % against it, which is what the wider
> encoding costs. Re-baseline with `./build/perf_bench --update-baseline`, or use a non-Release
> build dir.
> *Formatting:* the new code keeps the compact one-row-per-case braced tables the file already
> uses. `clang-format --dry-run` flags those rows, but it flags 158 of them at `HEAD` in
> `encoding_v6_test.cpp` and 394 in `src/QLangCompiler.cpp` — the tree is not clang-format clean,
> so running `-i` here would explode every table one field per line and rewrite untouched code
> (AGENTS.md: match the tree, minimise diffs). Only the three added lines over the 80-column
> limit were re-wrapped; `clang-format -i --lines=…` on the added ranges was tried and reverted.


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
