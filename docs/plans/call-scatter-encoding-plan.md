# CALL encoding v12 — short function id + scatter call variant

Status: **Proposed — not started.** The encoding is specified in
`docs/spec/13-bytecodes.md` (`### bytecode encoding`, the *"if opCode is call
(proposal / not finalized — the 'scatter call' variant)"* block); this plan
implements it. **Phase 1 and Phase 2 are separable; Phase 2 is gated on a
measurement (Phase 0) and may be dropped.**
Builds on: `.qlc` v11 (class export meta, Round 106–107), modifier 2/2/4 layout
`.qlc` v6 (Round 56) — [encoding-v6-and-indirect-plan.md](encoding-v6-and-indirect-plan.md) §2.8
is the design record for today's fixed-form CALL.
Reference spec: `docs/spec/13-bytecodes.md` (`### bytecode encoding`, `## encoding
extended`), on-disk contract: `docs/SERIALIZATION.md` §4.5.

---

## 0. Why split: the measured case

Corpus: the 36 scripts in `tests/scripts` (10,731 instructions, disassembled
with `qlcc -a`):

| metric | value |
|-|-|
| `call` (builtin/native) sites | 467 — 4.4% of instructions, 91% of the call family |
| fn ids that fit in signed 1 byte | **467 / 467 (100%)** (builtins 0–22; native slots −1…−11) |
| arities | 1×228, 2×173, 3×35, 4×17, 5×11, 8×3 (max 8, no 0-arg call) |
| call stream bytes today | 2,802 (467 × 6 fixed: `op e base fn16 argc`) |
| same stream with a 1-byte fn id | 2,335 (**−16.7%**) |
| same stream with scatter used for *every* call | 3,161 (+12.8%) — scatter never wins on bytes |

So: the function-id width is a measured win with zero layout risk; the scatter
payload saves only the staging `mov` + temp in the register-window collision
case, whose frequency the corpus **cannot show** — that is what Phase 0 measures.

---

## 1. Settled decisions (from the spec proposal)

| # | Question | Decision |
|---|----------|----------|
| D1 | Modifier layout | **Standard 2/2/4**, no call-specific fields: bits 0–1 base-register width, bits 2–3 kind = `0` (immediate fn id), bits 4–7 form = fn id width. |
| D2 | Accepted CALL forms | `{4 = i8 (new), 5 = i16 (shipped), 6 = i32 (future, rejected until needed)}`; every other form → decode error. Compiler picks 4 when the id fits, else 5. |
| D3 | Wide native slots | Keep form 5 for fn ids outside `[-128, 127]` (negative native slots −128…); no new limit is imposed on registration. |
| D4 | Scatter discriminator | The payload byte (today's argc): bit 7 clear = contiguous `argc 0–255` (shipped); bit 7 set = scatter, `n = byte & 0x7f` (0–127), then `n` delta bytes. No modifier ambiguity (form 4 never existed). |
| D5 | Scatter legality | Every param register within `base −128 .. base +127` and `n ≤ 127`; otherwise the compiler MUST emit the contiguous form (staging as today). |
| D6 | Scope | **`CALL` only.** `callfn` / `callimport` / `callclos` / `emit` keep `r[a+1 .. a+c]`; extending them is a separate later decision. |
| D7 | Reader | Single-version, clean break, one version bump per landed phase (v11 → v12 in Phase 1; v12 → v13 only if Phase 2 lands). No migration shim. |

---

## 2. Phase 0 — the measurement (gates Phase 2)

Instrument `QLangCompiler::slotsWindowBusy` call sites in `compileCallExpr`
(`src/QLangCompiler.cpp`): count (calls emitted, rehomed-to-temp) under
`TRACE_FULL` — the existing gated `trace()`, no new plumbing. Run the script
suite + the corpus programs and record the ratio.

- **≥ ~5% rehomed** → Phase 2 lands after Phase 1.
- **< ~5%** → Phase 2 is **dropped**; archive this plan with Phase 1 only and
  move the spec proposal block to a historical note. (A `swap`-opcode staging
  fix, already sketched in `docs/spec/13-bytecodes.md` `## OpCode: swap`, is
  the cheaper alternative for a collision-heavy corpus.)

Deliverable: a round entry in `docs/compliance/history.md` with the measured
ratio. Revert the instrumentation afterwards (it is scaffolding, not shipped
code) unless the number needs periodic re-checking.

---

## 3. Phase 1 — short function id (form 4). `.qlc` v11 → v12

No behavior change; pure width selection. The fn id stays `int16_t` in the
decoded `Instruction` — only the byte width changes.

| Area | Change |
|-|-|
| codec tables | `src/QLangBytecode.cpp`: the CALL modifier is no longer *fixed* — extend the `m.call` validation (~`OpcodeClass::call`, `decodeModifier`, `escapedInstructionSize`, `readInstruction`, writer escape check) to accept forms {4, 5} and read the fn id at `kWidthBytes[form]` as signed. Writer: emit form 4 when `-128 <= b <= 127`, else 5. |
| header | `include/qlang/QLangBytecode.h`: `baseModifier(Opcode::CALL)` keeps returning the form-5 modifier (it is the *default* announcement); update the §470 comment. `kQlcVersion` → **12** (`QLangBytecodeIO.h`). |
| compiler | `QLangCompiler::compileCallExpr` / the CALL emission site: select the form from the id (`b`); no other compiler change. |
| VM | none — `i.b` is already a decoded `int16_t`. |
| disassembler | `examples/qlcc.cpp`: print `e=0x40` calls with an `i8` fn id annotation (AGENTS.md: mandatory). |
| assemble | `qlcc --assemble` / the `.qla` reader must round-trip the new spelling. |
| tests | `tests/unit/codec_check.cpp`: pin `call` sizes (id fits i8 → 5 bytes; native slot < −127 → 6 bytes), round-trip both; the `builtin_table` guard's negative-slot check still passes. Re-measure `codegen_size_check` / perf baseline and record the new numbers. Run `ctest --test-dir build` whole-matrix green. |
| docs | `docs/spec/13-bytecodes.md`: rewrite the "call is the one documented exception" rule (form ∈ {4, 5}, scatter pending), flip the proposal block's status note; `docs/SERIALIZATION.md` §4/§4.5 version line; `docs/compliance/history.md` round entry; `./update-doc-hashes.sh`. |

Net expected: −467 bytes over the corpus, no other measurement should move.

---

## 4. Phase 2 — scatter payload (only if Phase 0 says so). v12 → v13

| Area | Change |
|-|-|
| codec | CALL payload byte: bit 7 splits contiguous/scatter; scatter sizes at `5 + fnW + n`… i.e. `escapedInstructionSize` and `readInstruction` grow the delta branch (bounds-checked, same truncation errors). The decoded delta list needs a home on the decoded instruction — smallest: reuse the code bytes themselves at exec time (see VM row) rather than a new `std::vector` field, keeping `Instruction` POD. |
| compiler | `compileCallExpr`: after arg registers are known, if the window `dest+1..dest+n` is not naturally the arg set, check D5 legality and emit scatter (params = `argReg − base` deltas); the existing rehome path stays as the fallback for illegal scatter. `reserveArgSlots` / `slotsWindowBusy` logic stays — it still serves the fallback. |
| VM | `QLangVM.cpp` frame build (~1879) and the `CALL` handler's `callArgs` gather (~2468): contiguous loop unchanged; scatter walks the delta bytes at exec. Range-check every `base + delta` against the frame's register count (the reader cannot know the frame size). Read all params **before** the result is written to `base` at RETURN — already true (args are copied into the new frame at call time). |
| disassembler | print scatter calls with deltas relative to base (`call 0x40, 20, 18i8, n=2, -15, -11`) plus the pseudo-mnemonic unchanged. |
| tests | codec round-trip (scatter ≤ and > window, boundary deltas ±128, `n = 127`, malformed bit-7 combinations rejected); a script-suite regression where a busy-window call site loses its staging `mov` (assert in the `.qla` listing or an instruction-count check); register-aliasing case: a param **is** the base register. |
| docs | same sweep as Phase 1; the spec block drops "(proposal / not finalized)". |

---

## 5. Verification gate (every phase, per AGENTS.md)

```
cmake -B build && cmake --build build
ctest --test-dir build            # full matrix green, doc_hashes gate Skipped-not-Failed
bash update-doc-hashes.sh         # after the doc sweep, last
```

Compliance: on landing, flip the spec proposal block to **Implemented (Round
NN)**, record the round in `docs/compliance/history.md`, refresh the
`docs/spec/README.md` hash row for `13-bytecodes.md`.

## 6. Commits

- Phase 1: `feat(bytecode): CALL short function id (form 4), .qlc v12` — code +
  spec rule rewrite + serialization contract + tests in one commit; doc hashes
  as the trailing docs concern.
- Phase 0 round entry: `docs(compliance): measure CALL window-rehome frequency`.
- Phase 2 (if taken): `feat(bytecode): CALL scatter payload, .qlc v13`.
- On full landing, move this plan to `docs/plans/archive/`.

## 7. Explicitly out of scope

- `callfn` / `callimport` scatter (D6).
- Form 6 (i32 fn id): specified as future width, not decoded — reject it.
- Any `.qlc` compatibility shim (single-version reader, `## technical
  specifications` 6).
- Revisiting the 127-opcode identity space: this plan adds **no opcode**.
