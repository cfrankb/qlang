# Plan M4: `$$` indirect access (A3)

Status: **plan — not started**.
Baseline `HEAD = a32f066` ("imported names name the import table (Q31, Phase 3c)");
`cmake --build build` clean, `ctest --test-dir build` **53/53** green including `doc_hashes`.
Parent plan: [encoding-v6-and-indirect-plan.md](encoding-v6-and-indirect-plan.md) — this plan
executes **§7** (`$$` inside the new framework, which closes A3 **Q6**), the declaration-table
half of **§8**, **§11 item 8** and **§11 item 9** for the new forms, and the `$$`-relevant
defaults of **§12** (items 3, 4, 5). Inventory item **A3** of
[missing-features-plan.md](missing-features-plan.md) **Phase 2**; that plan owns **Q7–Q11**, all
five of which are ratified here (§3). Backlog entry: [todo.md](todo.md) §2.
Spec: [../spec/05-operators.md](../spec/05-operators.md) `## operator $$` (the normative text:
examples, restrictions, the six-stage pipeline, resolution order, declaration state),
[../spec/13-bytecodes.md](../spec/13-bytecodes.md) (`## encoding extended` → the `$$` paragraph at
:1280–1290 already records the design), [../SERIALIZATION.md](../SERIALIZATION.md) §4/§4.1/§5.1.
Structural precedent: [archive/m3-flag-flow-plan.md](archive/m3-flag-flow-plan.md).

---

## 0. Deviations from the parent plan (stated up front)

Three, all of them consequences of Q11 and of measuring the cost of §7.2 as written.

1. **§7.2 piece 2 — the frame name map is not built per frame.** The parent text builds
   `name → {register, kind}` "once when the frame is created from the function's declaration
   table". That is O(declarations) hashing on **every call** in a `$$`-using function, to produce
   a map that never changes for that function. M4 splits it: the `name → [site id]` index is
   built **once per function at load** and shared by all its frames; the only per-frame state is
   the declaration bitset (D7). Same observable semantics, no per-call cost.
2. **§7.3 — no dominance analysis.** The parent allows omitting `DECL` where a declaration site
   dominates every `$$` use of the name. Its only consumer was the literal fast path; with Q11
   not shipping (deviation 3) the saving is one instruction per declaration in a body that
   already opted into `$$`. `DECL` is emitted unconditionally there (D8). §12 item 5's
   conservative rule stays a recorded follow-up.
3. **§7.5 — `$$"literal"` does not ship** (**Q11** ratified *not as defaulted* against the
   parent, **as defaulted** for missing-features). `## operator $$` never shows a literal form;
   every example goes through a string-valued variable. Shipping it would add a parser form, a
   compile-time resolution path and a dominance test for behaviour the spec does not ask for
   (D9). It is recorded in §7 as a follow-up, not rejected outright.

Two smaller notes, not deviations: the parent's §7.4 "identities inside the defined range" is
still true but the numbers moved twice since it was written (Round 60 compaction, Round 65
`getimport`/`callimport`), so `DECL` takes **74**, not 72/79 (**Q31** handover, D1); and §8's
"declaration table in the `.qlc` v6 function record" is now **v10** (D4).

---

## 1. Scope

**In scope**

1. `DECL siteId` — opcode identity **74**, `kOpcodeUsedCount` 74 → 75, codec tables, mnemonic,
   disassembler pseudo-text.
2. `INDGET` / `INDPUT` become real opcodes: moved to the **2-operand** class, emitted by the
   compiler, executed by the VM, disassembled as `r[a] = $$r[b]` / `$$r[a] = r[b]`.
3. `$$` in the parser and the AST: `$$expr` as an expression (read) and `$$expr = value` as a
   statement (write). Nothing else.
4. Per-function **declaration table** (`{name, kind, reg-or-slot}`, site id = index) built by the
   compiler for bodies that contain `$$`, plus the `DECL` emission at every declaration event.
5. `.qlc` **v10**: the table in the function record (§4.1), load-time validation, single-version
   reader, linker passthrough.
6. VM frame state: the per-frame declaration bitset, the load-time `name → [site id]` index, the
   static global/const view, and the **six-stage `indirect variable error:` pipeline** in both
   handlers, including generator suspend/resume of the bitset.
7. Tests: a dedicated `indirect` suite, the `codec_check` version matrix, a 24th script regression.
8. Docs and compliance: `05-operators.md` (**Q9** opcode names, **Q28** duplicate-declaration
   wording), `13-bytecodes.md`, `SERIALIZATION.md`, `coverage.md`, `status.md`, `tests.md`,
   `history.md`, `todo.md` §2.

**Out of scope**

| Item | Where it belongs |
|---|---|
| `$$"literal"` fast path (**Q11**) | follow-up (§7); needs its own parser decision |
| Dominance-based omission of `DECL` | follow-up (§7), parent §12 item 5 |
| `$$a += 1`, `$$a++`, `delete $$a` | follow-up (§7) — spec shows plain assignment only (D10) |
| `$$` through a table member or subscript (`$$a.b`) | not in the spec's grammar; §7 |
| Runtime module registry, module search paths, `searchCallback` | **C8** (`missing-features-plan.md`, Q23–Q26) |
| Offset-table elimination / `instrOffsets` removal | `offset-table-elimination-plan.md` (takes `.qlc` **v11**) |
| `kind = 3` / immediate forms 12–15 | stay **reserved** (parent §7.6, D7 of that plan) |
| Serializing the declaration table for functions without `$$` | deliberately omitted (D14) |

**Invariants:** the corpus (`tests/scripts/*.ql`, 23 scripts) stays **byte-identical** in stdout,
exit code **and instruction count** — no script uses `$$`, and D14 means a function without a
`$$` use gets no table, no `DECL` and no bitset, so nothing is emitted where `$$` is absent. The
`codegen_size` budgets (215 / 151 / 163 / 565) must not move. No perf re-baseline. The v6 2/2/4
modifier byte is untouched: `kind = 3` and forms 12–15 stay reserved. `DECL` is **appended
last** — no existing identity is renumbered (Q31's rule). Error messages of existing features do
not change; M4 only adds messages behind the `indirect variable error:` prefix.

---

## 2. Where the code stands today (baseline facts)

| Fact | Location |
|---|---|
| `$$` is already a **token** and nothing else: `TokenType::DollarDollar`, produced by the lexer, referenced nowhere else in `include/`, `src/`, `examples/`, `tests/` | `include/qlang/QLangLexer.h:90`, `src/QLangLexer.cpp:391` |
| Reserved-word oracle is already a free function the VM can call (pipeline stage 3) | `keywordType`, `include/qlang/QLangLexer.h:167`, `src/QLangLexer.cpp:10` |
| `INDGET` / `INDPUT` are reserved placeholders **inside** the defined range | `include/qlang/QLangBytecode.h:213-214` |
| Their mnemonics exist (`indget`, `indput`) | `src/QLangBytecode.cpp:1280-1281` |
| They fall through to the **3-operand default** class | `src/QLangBytecode.cpp:214-216` |
| `permittedKinds` default is `kReg` (mask `0x2`) — already pinned for `INDGET` | `src/QLangBytecode.cpp:428-461`, `tests/unit/encoding_v6_test.cpp:436` |
| `defaultRightKind` has an immediate precedent (`CALL`) | `src/QLangBytecode.cpp:362-372` |
| `operandRole`: slot 0 is `Register` for every opcode except `Nop`; jump class stores only `b` | `src/QLangBytecode.cpp:105-135`, `include/qlang/QLangBytecode.h:503-513` |
| `kOpcodeUsedCount = 74`, and the comment already hands **74** to `DECL` | `include/qlang/QLangBytecode.h:352-356` |
| `isValidOpcode` is `< kOpcodeUsedCount` (contiguous used range) | `src/QLangBytecode.cpp:85` |
| `Function` record: `name, paramCount, registerCount, code, instrOffsets, loc, captures, captureNames, isGenerator` — no declaration metadata | `include/qlang/QLangBytecode.h:566-587` |
| `.qlc` is **v9**, single-version reader (`version != kQlcVersion` → reject) | `include/qlang/QLangBytecodeIO.h:37`, `src/QLangBytecodeIO.cpp:394`, `:502` |
| Function record writer / reader (where the table is inserted) | `src/QLangBytecodeIO.cpp:396-425`, `:528-548` |
| Version allocation is recorded in `SERIALIZATION.md` §4 (v8 pool, v9 module tables, "next free number belongs to whoever lands next") | `docs/SERIALIZATION.md:131-136`; §4.1 record at `:140-151` |
| The linker copies whole `Function` objects and rewrites operands **by `OperandRole`** — table-free passthrough | `src/QLangLinker.cpp:302`, `:356` |
| `Frame` = `funcIndex, regs, captures, envs, pc, callDest, flags` — no declaration state | `include/qlang/QLangVM.h:109-123` |
| Frame creation sites to touch: closure call, generator resume, module-init, main, direct call | `src/QLangVM.cpp:1818`, `:1949`, `:1998`, `:2005`, `:2666` |
| `GeneratorState` = `funcIndex, regs, captures, pc, exhausted` — the bitset must ride here too | `include/qlang/QLangVariant.h:71-77` |
| Single dispatch loop; `GETIMPORT` is the shape to copy (reads a global-table slot through a table index) | `QLangVM::runFrames()` `src/QLangVM.cpp:2024`, `GETIMPORT` `:2540` |
| Parser: `parseUnary` accepts `Minus/Not/Dollar/++/--` (where the read goes); the identifier statement dispatch handles `=`, compound-assign, `[...] =`, `.name =` (where the write goes) | `src/QLangParser.cpp:1277-1289`, `:322-337`, `:299-319` |
| `ExprKind` / `StmtKind` enumerations (no indirect node today) | `include/qlang/QLangParser.h:17-31`, `:113-135` |
| Compiler: `unOp` throws `unsupported unary operator '$$'`; unary lowering at `ExprKind::Unary` | `src/QLangCompiler.cpp:4993-5000`, `:3795` |
| Declaration events to mark: `compileDeclaration`, `compileConst`, `compileAssignment` (first plain assignment), `compileDef`, `compileGlobal` | `src/QLangCompiler.cpp:1934`, `:1977`, `:1996`, `:2659`, `:3550-3587` |
| Inside a function a `global` name is a **capture index** (`REFLOAD`/`REFSTORE`) *and* a global-table slot from `globalSlotOf` — the slot is what `$$` needs (read-only) | `src/QLangCompiler.cpp:3579-3584`, `:2021-2044` |
| `containsYield` is the pre-scan precedent (body walk + two call sites) | `src/QLangCompiler.cpp:5009`, callers `:1808`, `:2718` |
| Disassembler pseudo-text maps `INDGET`/`INDPUT` to the `r[a] = r[b][r[c]]` family | `examples/qlang_run.cpp:500-511` |
| `codegen_size` budgets | `tests/unit/codegen_size_check.cpp:68-73` |
| `codec_check` version matrix (currently "written at v9, reader accepts v9 only") | `tests/unit/codec_check.cpp:412-427` |
| Spec implementation notes still name the unimplemented `OP_INDIRECT_READ`/`OP_INDIRECT_WRITE` (**Q9**) | `docs/spec/05-operators.md:369-370` |
| Spec duplicate-declaration sentence is garbled (**Q28**: "is a compile warning declares into the enclosing function frame") | `docs/spec/05-operators.md:329-331` |
| Compliance rows to flip | `docs/compliance/coverage.md:53`, `docs/compliance/status.md:287`, `:361` |

**Measured today** (`HEAD = a32f066`, Debug build, commands re-run in this round):

| Measurement | Value |
|---|---|
| `qlang_run --profile` instructions (hello / money / cast / perf_stress) | **215 / 151 / 163 / 565** — exactly the `codegen_size` ceilings |
| `encoding_v6` | **36127** checks (parity sweep: 30392 rejected / 2376 accepted / 15135 truncations) |
| `codec_check` | **358** checks |
| `flag_flow` / `linker` / `export` / `builtin_table` / `spec_check2` / `gen` | 1324 / 126 / 88 / 151 / 529 / 7 |
| `tests/scripts/*.ql` | **23** script regressions |
| `ctest -N` | **53** entries, 53/53 green |
| `$$` today | `local a = "b"` / `local b` / `$$a = 1` → `parse error at line 3,1: unexpected token '$$' at start of statement`, exit **3** |

The last row is the whole feature's starting point: the lexer recognizes `$$`, the parser does
not, and the compiler's `unOp` would reject it too.

---

## 3. Decisions to settle before coding

| # | Decision | Recommendation | Rationale / rejected alternative |
|---|---|---|---|
| **D1** | `DECL` identity | **74**, `kOpcodeUsedCount` **74 → 75** | **Q31** (Round 65) explicitly handed 74 to `DECL`; the enum's used range must stay contiguous because `isValidOpcode` is `< kOpcodeUsedCount` and `encoding_v6` probes "identity ≥ count is rejected". Rejected: 75 with a hole at 74. |
| **D2** | `DECL` operand shape | 2-operand class; `a` unused (`OperandRole::None`), `b` = site id **immediate**, `permittedKinds = kImm`, `defaultRightKind = Immediate`; i8 base form (2 bytes), escaped i16 above 255 | Follows `CALL`, the only existing immediate-`b` opcode, so the codec, `baseModifier` and the OpMeta fast path need no new shape. Rejected: a 1-operand form carrying the id in `a` — `a` is the destination register everywhere the codec, the disassembler and the linker assume it, and making it an immediate would be the first exception. |
| **D3** | `INDGET` / `INDPUT` arity (**Q10**) | **2 operands**: `indget dst, nameReg`, `indput nameReg, srcReg`; move both to the 2-operand list in `baseOperandCount`; roles `Register, Register, None`; `permittedKinds` stays `kReg` | They were never emitted, so there is no compatibility cost (parent §7.4). The third operand of the placeholder was a table subscript that `$$` does not have. `encoding_v6_test.cpp:436` (`{INDGET, 0x2}`) stays valid unchanged. |
| **D4** | `.qlc` version (**Q34** follow-on) | **v9 → v10**, table in the **function record** (§4.1) | The reader is single-version, so any new section forces a bump; the question is only *where* the data goes. Per-function data belongs with the function (parent §8). Rejected: an optional image-tail section parallel to §4.6 — same bump, worse locality, and it would have to defer validation like the import table does. `offset-table-elimination-plan.md` takes **v11**; `SERIALIZATION.md` §4 "Version allocation" records both. |
| **D5** | Table entry encoding | `u32 declCount`, then `declCount × { u32 nameLen + name bytes, u8 kind, i32 regOrSlot }`; **site id = index** (dense by construction) | Inline strings are validated in place while reading. Rejected: naming the pool (§4.2) — the pool section comes *after* the function records, so every name check would need the deferred-validation machinery the import table already pays (§4.6), for tables of a handful of entries. |
| **D6** | `kind` vocabulary | `DeclKind : uint8_t { Local = 0, Const = 1, Function = 2, Global = 3, GlobalConst = 4 }`; `Local`/`Const`/`Function` carry a **frame register**, `Global`/`GlobalConst` a **global-table slot** | Parent §7.2's `var/const/function/global`, split so a global const reports `is a constant` (stage 5) rather than `global write denied` (stage 6), which is what the spec's `const year` example requires. Globals are read through `m_scope.globalTable().slots[slot]` — `GETIMPORT`'s shape — never through a capture, so `$$` works at any frame depth. Rejected: reusing the capture index, which only exists inside the function that declared `global`. |
| **D7** | Declaration state representation (**Q7**) | Per-frame **packed `uint64_t` words**, sized `ceil(declCount/64)`, **lazily allocated** (null when the function has no table); bit index = **declaration-site id**, not register | Parent §12 item 3 default. Keyed by site id because registers are reused across branches and loop iterations — the spec's own `if cond { local a = 1 }` example and the block-scope register recycling in `declareVar` (`:622`) both make a register-keyed bit wrong. Rejected: `std::vector<bool>` (proxy reference, per-frame allocation of the same size, no cheaper). |
| **D8** | `DECL` emission policy | Emit at **every** declaration event in a body that contains `$$`; **no** dominance analysis | Deviation 2. Idempotent under loops (the spec's re-declaration case). Rejected: parent §7.3's dominance omission — it saves one instruction in functions that already opted in, and it is the kind of static claim that silently breaks `$$` when the analysis is wrong. |
| **D9** | `$$"literal"` (**Q11**) | **Does not ship** | Deviation 3. `## operator $$` has no literal example; missing-features' own default is "v1 supports only the dynamic form". Rejected: parent §7.5's narrow shipping, which needs the dominance test D8 declines. |
| **D10** | Write forms | Plain `$$a = value` only. `$$a += 1`, `$$a++`, `delete $$a`, `$$a.b = v` → parse error | The spec's restrictions and examples only ever assign. A compound form needs a read-modify-write that would run the pipeline twice with different stages; that is a separate decision (§7). |
| **D11** | Where the pipeline runs | **In the VM**, in both handlers; stage 1 type check, stage 2 `QLangLexer` over the string (identifier + EOF), stage 3 `keywordType`, stages 4–6 against the index/bitset/global view. Errors: `QLangErrorKind::Runtime`, message prefix `indirect variable error:` | The name is only known at run time, so no compile-time check is possible (except for D9's literal form, which is not shipping). Stage 2/3 reuse the lexer rather than re-implementing identifier rules, so `$$"123"` and `$$"local"` report exactly what the spec says. |
| **D12** | The static global/const view | Built by a **post-pass at the end of `compile()`** over every function: names this function exposed via `global` (kind `Global`) + **all** global consts (kind `GlobalConst`, visible everywhere without `global`) | "Global const is visible everywhere" is a *static* scope fact, not a sequential declaration event, so those entries must not be bitset-gated. A post-pass makes source order irrelevant — a `def` compiled before the `const` later in the file still sees it. Rejected: filling it when `compileGlobal` runs (misses the everywhere-visible consts and depends on source order). |
| **D13** | Generators | `GeneratorState` gains the bitset; it is moved out of the frame on suspend and back on resume, next to `regs`/`captures` | A generator that declared `local a` before a `yield` must still see `a` declared after the resume; otherwise `$$` silently regresses to `not declared` inside every generator. |
| **D14** | Functions without `$$` | **No** declaration table, **no** `DECL`, **no** bitset (gate = a `containsIndirect` body pre-scan, the `containsYield` shape) | Parent §12 item 4 default. This is also what keeps the corpus byte-identical and the `codegen_size` budgets unmoved, which is the cheapest possible proof that M4 cannot regress existing programs. |

---

## 4. Steps

Ordered so the additive layers land before any semantics exist, and the semantics test lands
before the docs — a regression therefore localizes to one step.

### S1 — Opcode layer and disassembler (additive, nothing emitted)

`DECL` at 74 + `kOpcodeUsedCount` 75 (D1); `DECL` codec rows (`baseOperandCount` → 2,
`defaultRightKind` → `Immediate`, `permittedKinds` → `kImm`, `operandRole(DECL, 0)` → `None`,
mnemonic `decl`); `INDGET`/`INDPUT` moved to the 2-operand list (D3); `qlang_run` pseudo-text
split so `INDGET`/`INDPUT` print `r[a] = $$r[b]` / `$$r[a] = r[b]` instead of joining the
`r[a] = r[b][r[c]]` group, and `decl` prints its site id.

Expected new facts (assert them, do not eyeball): `baseOperandCount(DECL) == 2`,
`permittedKinds(DECL) == 0x1`, `kindPermitted(DECL, Immediate) && !kindPermitted(DECL, Register)`,
`baseOperandCount(INDGET) == baseOperandCount(INDPUT) == 2`, `isValidOpcode(74) &&
!isValidOpcode(75)`, `instructionSize(DECL, false, baseModifier(DECL)) == 2`.

`encoding_v6` grows: the exhaustive `(opcode, modifier)` sweep walks one more identity (Round 65
measured +1 044 checks for two identities, so expect roughly **+522**), plus new explicit cases —
`decl` with a register kind is rejected, an unescaped `decl` decodes to an i8 immediate, an
escaped `decl` with a 2-byte id round-trips, and `indget`/`indput` now reject a third operand.

*Exit criteria:* build clean; `ctest` **53/53**; corpus 23/23 byte-identical; `codegen_size`
budgets unmoved (215/151/163/565); `encoding_v6` tally re-recorded.

### S2 — Declaration table in the model and `.qlc` v10 (still nothing emitted)

`DeclKind` + `DeclEntry {std::string name; DeclKind kind; int32_t regOrSlot;}` in
`QLangBytecode.h`; `std::vector<DeclEntry> decls` and `std::vector<DeclEntry> statics` on
`Function` (D5, D6); `kQlcVersion = 10`; writer appends both vectors to the function record after
`loc`; reader parses them with load-time validation — `kind > GlobalConst` → reject, `regOrSlot`
outside `[0, registerCount)` for the frame-register kinds and outside `[0, globalSlotCount)` for
the global kinds → reject, empty or non-identifier name → reject, `declCount` bounded by the
instruction count (a `DECL` must exist for every site).

Both vectors are **empty for every function** in this step: the compiler does not fill them yet,
so the whole step is format-only. `codec_check`: the version matrix flips to "written at v10,
reader accepts v10 only (v1–v9 rejected)" and gains a hand-built-program case that round-trips a
non-empty table, plus the four rejection cases. `SERIALIZATION.md` §4 (version allocation: v10 =
the declaration table, v11 = next free), §4.1 (the two new fields), §5.1 (v1–v9 rejected).
Linker: verify by test, not by assumption, that a table survives merge unchanged (it copies whole
`Function`s, `QLangLinker.cpp:302`) — add a `linker` case with a non-empty table.

*Exit criteria:* build clean; `ctest` **53/53**; corpus byte-identical; budgets unmoved;
`codec_check` tally re-recorded; a v9 image is rejected with `unsupported version (byte offset 8)`.

### S3 — Parser, AST and compiler (emit the instructions and the table)

Parser: `DollarDollar` accepted in `parseUnary` (read) and at statement start followed by
`Identifier`, `=`, expression (write). Representation: reuse `ExprKind::Unary` with `uop.text ==
"$$"` for the read (so `isCallFree`, `inferType` and the constant folder see a unary node they
already handle) and `StmtKind::Assignment` with a new `indirectTarget` flag for the write — no
new `ExprKind`, one new `Stmt` field. `$$a += 1` and friends stay parse errors (D10).

Compiler: `containsIndirect` pre-scan (the `containsYield` shape) sets a per-function flag; while
it is set, every declaration event (`compileDeclaration`, `compileConst`, the first plain
assignment in `compileAssignment`, `compileDef`) appends a `DeclEntry` and emits `DECL siteId`
immediately after the declaration's store (D8); `compileGlobal` contributes to the function's
`statics` instead of the bitset (D12, which runs once at the end of `compile()` over
`m_functions`). `$$expr` lowers to `compileExpr(operand, tmp)` + `INDGET dest, tmp`; `$$expr = v`
lowers to `compileExpr(operand, rName)` + `compileExpr(v, rVal)` + `INDPUT rName, rVal`.
`unOp` keeps its throw for `$$` (it is no longer reachable through the unary path).

No VM handler exists yet, so a `$$` program compiles and then fails at run time with
`unknown opcode` — acceptable *inside this step only*, and it is why S3 has no script test; the
semantics are pinned in S4/S5.

*Exit criteria:* build clean; `ctest` **53/53** (no script uses `$$`); corpus byte-identical;
budgets unmoved; `qlang_run --asm --asm-level 4` on a scratch `$$` sample shows `decl`, `indget`
and `indput` with sane pseudo-text and the table present in the compiled program.

### S4 — VM: frame state and the six-stage pipeline

`Frame` gains `uint64_t *declBits` (or a small `std::vector<uint64_t>` — decide against D7's
lazy-allocation rule and record it) plus a pointer to the function's load-time index; the index
(`name → [site id]`, built once per function on first use, cached per `funcIndex`) and the static
global/const view are VM-side, not frame-side (deviation 1). All five frame-creation sites get
the allocation; `GeneratorState` gets the bitset (D13).

`DECL siteId` sets one bit. `INDGET` / `INDPUT` run the pipeline in spec order and stop at the
first failure:

| stage | check | message |
|---|---|---|
| 1 | operand is a string | `indirect variable error: invalid name <value>` |
| 2 | string lexes as an identifier (lexer + EOF) | `indirect variable error: invalid name <value>` |
| 3 | not a reserved word (`keywordType`) | `indirect variable error: reserved word <name>` |
| 4 | declared and visible (locals by bitset, then `statics`) | `indirect variable error: <name> not declared` |
| 5 | not `Function` (read and write); not `Const`/`GlobalConst` (write) | `indirect variable error: <name> is a function` / `… is a constant` |
| 6 | write target is a frame local, not a global slot | `indirect variable error: global write denied` |

Resolution order per the spec: writes consult the current frame's locals only; reads consult
locals, then `statics` (`global`-exposed variables and every global const). A read that misses
every level is an error — `$$` never yields nil. Multiple set bits for one name resolve to the
**highest site id** (the latest declaration that executed), which is what "declarations are
sequential" means once registers are recycled.

*Exit criteria:* build clean; `ctest` **53/53**; corpus byte-identical; budgets unmoved; every
example in `## operator $$` reproduces exactly (the eight successes and the seven errors),
verified by hand before S5 encodes them.

### S5 — Semantics harness (parent §11 item 8)

New `tests/unit/indirect_test.cpp`, ctest entry `indirect`, following the `class_static_test`
precedent (compile a source string, run, assert stdout or the exact error message). Coverage:
the six stages one at a time; resolution order (local shadows global; `global`-exposed read;
global const without `global`; read miss → `not declared`); **branch-dependent declaration**
(untaken → `not declared`, taken → success) and the same in a loop (re-declaration); `const`
read ok / write refused; function refused on read **and** write; global write denied; the
sequential-declaration example (`local aa = "bb"` before `local bb` → `bb is not declared`);
generators across a `yield`; nested functions (a `$$` inside a `def` resolving a `global`);
`.qlc` round-trip of a program with a non-empty table executing identically; and the load-time
rejections from S2.

*Exit criteria:* build clean; `ctest` **54/54**; the suite's tally recorded in
`docs/compliance/tests.md`.

### S6 — Script regression and corpus re-verification

`tests/scripts/indirect.ql` (24th script): the successes only (a script must exit 0), covering
the spec's headline cases — `$$a = "toto"` writing through a name held in a string, `f = $$a`,
the `global module_owner` read, the local const read. Then re-verify the corpus: 23 existing
scripts byte-identical in stdout **and** instruction count, `--asm-level 0–5` well-formed for the
new forms (parent §11 item 9), and a `compile → .qlc → load → run` round-trip of `indirect.ql`
through `codec_check`'s `qlc-exec` gate (which becomes 24 scripts).

*Exit criteria:* build clean; `ctest` **55/55**; 24/24 scripts byte-identical; `perf` gate 12/12
with no re-baseline.

### S7 — Documentation (only after the suite is green)

`docs/spec/05-operators.md`: **Q9** — the implementation notes at `:369-370` name `indget` /
`indput` and `decl` instead of `OP_INDIRECT_READ`/`OP_INDIRECT_WRITE`, and note 2's "declaration
bit on the store opcode" is marked superseded (parent §7.1); **Q28** — the garbled
duplicate-declaration sentence at `:329-331` is rewritten to point at the strict-typing level
table instead of promising a "compile warning". `docs/spec/13-bytecodes.md`: operand-map rows for
`decl` / `indget` / `indput`, the `$$` paragraph at `:1280-1290` flipped from design to shipped
with **v10**, `## opCode scope` count 74 → 75. `docs/SERIALIZATION.md`: §4 version allocation,
§4.1 record, §5.1 rejection list. `docs/compliance/coverage.md:53` and `status.md:287`/`:361`
flipped to implemented, `tests.md` tallies, `history.md` Round 66 entry, `todo.md` §2 closed.
Then `./update-doc-hashes.sh` (AGENTS.md: after the green run only).

*Exit criteria:* `ctest` **55/55** with `doc_hashes` green; no spec module edited without a
compliance row describing it.

### S8 — Housekeeping

Status header of this plan → **DONE 2026-11-xx (Round 66)** with the real tallies; `> **DONE**`
note under each step; parent plan §6.1 M4 row and §11 item 8 marked; `todo.md` §2 replaced by the
next milestone. Suggested commits (§8) — suggested, not committed (AGENTS.md).

---

## 5. Verification gate

```bash
cmake -B build && cmake --build build
ctest --test-dir build                                   # 55/55
build/qlang_run --profile tests/scripts/hello.ql         # 215 instructions (budget ceiling)
build/qlang_run --asm /tmp/m4.qla --asm-level 5 tests/scripts/indirect.ql
build/qlang_run --output /tmp/m4.qlc tests/scripts/indirect.ql && build/qlang_run /tmp/m4.qlc
./update-doc-hashes.sh --check
```

| Parent §11 item | Discharged by |
|---|---|
| 1 full corpus byte-identical | S1–S6 (invariant, §1) |
| 2 `.qlc` round-trip + execute | S2 (format), S6 (`qlc-exec`, 24 scripts) |
| 3 size/decode agreement for every (kind, form) | S1 (`encoding_v6` sweep + explicit `decl` cases) |
| 4 zero-byte immediates | — (M2 shipped it) |
| 5 flag matrix | — (M3 shipped it) |
| 6 deprecated spellings rejected | — (Phase 3 shipped it; v10 rejects v1–v9) |
| 7 reserved `kind = 3` / forms 12–15 rejected at load | S1 (unchanged guard, re-run) |
| **8 `$$`: branch-dependent declaration, loop re-declaration, const read-only, function/global rejection, the six messages, literal fast path** | **S4 + S5**; the literal fast path is **not** verified because it does not ship (D9) |
| 9 disassembly well-formed `--asm-level 0–5` | S1 (`decl`), S6 (all levels on `indirect.ql`) |
| 10 pool shrink | — (M2 shipped it) |

---

## 6. Risks and effort

| Risk | Mitigation |
|---|---|
| `DECL` emitted where the compiler's declaration-event notion and the spec's disagree (e.g. `local` re-declaration in the same frame, block-scope register reuse) | D7 keys the bit by **site id**, never by register; S5 tests re-declaration and the loop case explicitly |
| A `$$` in a function whose table was omitted (pre-scan misses a use inside a closure body or an f-string) | The pre-scan must recurse through every child statement **and** expression (closure bodies, f-string parts, ternary branches); S5 covers `$$` inside a `def` and inside a closure |
| Global resolution inside a function reads the wrong cell (capture vs table slot) | D6 always uses the table slot from `globalSlotOf`, the same value `compileGlobal` records in `m_globalRegMap`; S5 tests a `$$` read of a `global` name from inside a `def` |
| v10 breaks every existing `.qlc` | Accepted and consistent with the compatibility rule (exact same compiler version); the reader rejects v9 with the standard message, `codec_check` pins it |
| Bitset allocation cost in the hot call path | D7 lazy allocation + D14 (no table ⇒ no allocation ⇒ zero cost for existing programs); `perf` gate 12/12 with no re-baseline is the check |
| Generators silently lose declaration state across a `yield` | D13; S5 has a generator case that would fail without it |
| `encoding_v6`'s exhaustive sweep changes shape when `INDGET`/`INDPUT` lose an operand | Run S1 alone first and record the new tally before any other change |
| Spec examples are ambiguous about stage precedence (e.g. a global const write: `is a constant` vs `global write denied`) | D6 fixes the order (stage 5 before stage 6) and S5 pins both messages; if the intent differs, the fix is one `if` and a test edit |

Effort: S1 0.5 d · S2 1 d · S3 1.5 d · S4 1.5 d · S5 1 d · S6 0.5 d · S7 0.5 d ≈ **6.5 days**
(missing-features estimates A3 at 1–2 weeks; the settled design puts it at the low end).

---

## 7. Follow-ups recorded (deliberately not M4)

- `$$"literal"` fast path (**Q11**, D9) — parser form + compile-time resolution + the dominance
  test; only worth doing together with D8's omission of `DECL`, since both need the same analysis.
- `$$a += 1`, `$$a++`, `delete $$a`, `$$a.b` (D10) — needs a decision on running the pipeline once
  or twice for a read-modify-write.
- Real CFG dominance for `DECL` omission (parent §12 item 5).
- Declaration-table compression (name dedup against the pool) if tables ever get large — the
  current inline-string choice (D5) is deliberate for small tables.
- `$$` interaction with `Env`/reference captures (`REFLOAD` names) if captures ever become
  targetable — today they are deliberately invisible (D6).
- Archiving `external-modules-plan-v4.md` to `docs/plans/archive/` (AGENTS.md: a fully
  implemented plan moves to the archive) — it is complete as of Round 65; the move needs the live
  links in `docs/README.md`, `todo.md`, `missing-features-plan.md` and
  `encoding-v6-and-indirect-plan.md` re-pointed, so it belongs in its own docs commit.

---

## 8. Housekeeping and commits

Suggested names (AGENTS.md: suggest, never commit), staged by concern:

1. `bytecode: DECL opcode and 2-operand indget/indput (M4 S1)` — `QLangBytecode.h/.cpp`,
   `examples/qlang_run.cpp`, `tests/unit/encoding_v6_test.cpp`
2. `bytecode: per-function declaration table, .qlc v10 (M4 S2)` — `QLangBytecode.h`,
   `QLangBytecodeIO.h/.cpp`, `tests/unit/codec_check.cpp`, `tests/unit/linker_test.cpp`,
   `docs/SERIALIZATION.md`
3. `compiler: $$ parses and lowers to decl/indget/indput (M4 S3)` — `QLangParser.h/.cpp`,
   `QLangCompiler.h/.cpp`
4. `vm: frame declaration state and the $$ check pipeline (M4 S4)` — `QLangVM.h/.cpp`,
   `QLangVariant.h`
5. `tests: indirect access suite and script regression (M4 S5–S6)` — `tests/unit/indirect_test.cpp`,
   `tests/scripts/indirect.ql`, `CMakeLists.txt`
6. `docs: $$ indirect access shipped (Round 66)` — `docs/spec/{05-operators,13-bytecodes}.md`,
   `docs/SERIALIZATION.md`, `docs/compliance/*`, `docs/plans/todo.md`, this plan's status header,
   `docs/spec/README.md` hashes

No `*.orig` / `*.rej` artifacts; out-of-source build only (`cmake -B build`).
