# M3 — Flag Flow: `eqf`/`ltf`/`gtf`/`ltef`/`gtef`, `tst` and the Flag Jumps

Status: **in progress — S1 landed (`4ead571`), S2–S3 landed (uncommitted).** Baseline `HEAD = 28707db` ("perf: re-record
baseline after the decoder fast path"). M1 **DONE 2026-11-06** (Round 55), M2 **DONE 2026-11-07**
(Round 56 + the Round 57 decoder fast path). Suite at this baseline: ctest **47** entries,
`encoding_v6` **34 470** checks (sweep tallies 1 756 accepted / 14 138 rejected
`(opcode, modifier)` pairs), `codec` **199** checks, `perf_stress` execute **10.382 ms**.
Parent plan: [encoding-v6-and-indirect-plan.md](encoding-v6-and-indirect-plan.md) — this file
executes its **§6.1 milestone M3** and cites its sections (§2.5 permitted kinds, §2.6 arity,
§2.9 concrete forms, §4 the new opcodes + flag model + type domain + aliases + savings, §5
phasing out `jmpfalse`/`jmptrue`, §10 impact, §11 verification gate item 5, §12 item 6).
Reference spec: `docs/spec/13-bytecodes.md` (`## alternate conditional jump OpCode`,
`### shared registers`, `### OpCode jg` / `je` / `jl`, `### OpCode tst`,
`### extended OpCode eq, lt, gt, lte, gte`, `## opCode scope`, `## future plans`).
Precedent for this document's structure: [m2-pipeline-switch-plan.md](m2-pipeline-switch-plan.md).

Two deviations from the parent plan, both recorded up front:

1. **`ja`/`jae`/`jb`/`jbe` land as specification + disassembly rules only** (D6). The repo has
   **no assembler** — `qlang_run` offers `--asm <file.qla>` / `--asm-level <n>` *disassembly*
   only — so an alias has no code surface to live on.
2. **The "base form" rows for an immediate right operand are corrected** (D5). Parent plan
   §2.9 lists `eqf r12, 5` as base **3 B** (`≡ e = 0x40`) and `13-bytecodes.md:864` prints
   `eqf e=0x40 0c, 05 … # 3 bytes (base form, no e emitted)`. In the shipped codec a base form
   can only mean `defaultRightKind(op)` (= `Register` for every flag compare), so an immediate
   right operand **must be announced**: `eqf r12, 5` is `[EQF|80][40][0C][05]` = **4 bytes**.
   Same class of correction M2 recorded for `mov e=0x08 … pool[N]`.

---

## 1. Scope

**In scope (M3):**

1. Six new opcode identities appended last in the enum (parent §4): `EQF`, `LTF`, `GTF`,
   `LTEF`, `GTEF`, `TST` → `kOpcodeUsedCount` **73 → 79** of 128 (`DECL` is M4 → 80).
2. The codec tables that make them legal instructions: operand count, permitted kinds
   (§2.5 "flag compare" row), the in-place arity rule (§2.6), `Bytecode::name()` mnemonics.
3. The VM handlers: the five flag compares over **general** values (§4.3) and `TST`
   (§5.1), writing `Frame::flags`. `flagsSatisfied` and the six flag jumps are **not touched**.
4. The compiler: comparison conditions in `if`/`while` position lower to
   `*f` + flag jump for every type domain the current `cmp` gate rejects, freeing the temporary
   and (for a literal right operand) the load instruction (§4.5).
5. The disassembler (`examples/qlang_run.cpp`) in the same change — AGENTS.md and
   `13-bytecodes.md` `## disassembler` mandate it whenever the encoding changes.
6. `ja`/`jae`/`jb`/`jbe`: spec bullets flipped from "M3, pending" to landed; disassembly keeps
   printing the canonical mnemonic (§4.4, D6).
7. Tests and documentation: the §11 item 5 flag matrix, a script suite for general comparisons,
   compliance Round 58, hash re-record after green.

**Out of scope:**

| Item | Where it belongs |
|------|------------------|
| `DECL`, `$$`, frame name map + declaration bitset, declaration table in the function record, two-operand `INDGET`/`INDPUT` | **M4** (parent §7, §8) |
| Removing `JMPIFFALSE`/`JMPIFTRUE` and the `i`/`c` suffix identities, renumbering opcodes | **Phase 3**, `.qlc` v6 → v7 (parent §5.4, §6) |
| Emitting `tst` for a bare truthiness test (`if x`, `while x`, `&&`/`\|\|`) | follow-up; parent §12 item 6 default is to keep `jmpfalse`/`jmptrue` (D4) |
| `nef`, and value-producing `lte`/`gte` | rejected (parent §4.1) |
| A genuinely unsigned relation on mixed-signedness operands; the compiler emitting immediate forms 0–3 | future version (parent §4.4) |
| Widening the remaining ALU families to kinds 0/2 beyond `mov`/`add`/`sub`/`cmp` | follow-up (parent §2.5 last paragraph) |
| 64-bit / unsigned immediate **emission** | M2 D3 follow-up |
| `switch` case checks (`EQ` + `JMPIFTRUE`) and `compileGotoIf` (`JMPIFTRUE`) moving to flags | follow-up, measure separately (§7) |

**Invariants:** `QLangVM::flagsSatisfied` unchanged; `cmp` stays `Int`-only; the flag model
stays one-hot (at most one of `GR`/`EQ`/`LT`, "none" legal); every
`tests/scripts/*.ql` keeps byte-identical stdout; `.qlc` stays **v6** (M3 adds identities, not
layout — the reader's `opcode < kOpcodeUsedCount` guard widens automatically).

---

## 2. Where the code stands today (baseline facts)

| Fact | Location |
|------|----------|
| Enum tail is `INDEXGET, INDEXPUT`; `kOpcodeUsedCount = 73` | `include/qlang/QLangBytecode.h:230-236` |
| `baseOperandCount`: 2-operand list contains `CMP`; 1-operand list `INC/DEC/…` | `src/QLangBytecode.cpp:66-116` |
| `isAluOrFlagCompare` (ADD…`INx`, `CMP`) drives `permittedKinds = imm\|reg\|const` **and** `isInPlaceForm` | `src/QLangBytecode.cpp:290-371` |
| `defaultRightKind` defaults to `Register`; `baseModifier(op)` = `makeModifier(Byte, defaultRightKind(op), kind==Imm ? I8 : 0)` | `src/QLangBytecode.cpp:260-285` |
| Cold-built 128-entry `OpMeta` fast path mirrors those tables (`nOps`, `defKind`, `permitted`, `aluCmp`) — new opcodes need **no** fast-path edit, they inherit the tables | `src/QLangBytecode.cpp:735-760` |
| `Bytecode::name()` mnemonic table ends at `indexget`/`indexput` | `src/QLangBytecode.cpp:1249-1250` |
| Flags: `Frame::flags`, `CMP_GR=1`, `CMP_EQ=2`, `CMP_LT=4` | `include/qlang/QLangVM.h:112-133` |
| `cmpFlags` (Int-only, throws `cmp operand must be an int`), `flagsSatisfied` (the six predicates) | `src/QLangVM.cpp:1094-1120` |
| General relation rules to reuse: `valueEqual` (`__str__` dispatch then `isEqual`), `cmp(a, b, op, vm)` (numeric / string / string-vs-object / array / table), `isTrue` | `src/QLangVM.cpp:240`, `1073`, `1467-1502` |
| **One** dispatch switch in the exec loop (parent §10's "four exec loops" is stale); `CMP`/`CMPI` case, flag-jump case, `JMPIFFALSE`/`JMPIFTRUE` cases | `src/QLangVM.cpp:1998`, `2461-2493` |
| Fusion site: `cmpJumpIfFalse` (negated jump for `cmp`'s total order), `emitCmpJumpFalse`, `emitCmpJumpFalseExpr` (gated on `inferType(...) == Int` for **both** operands) | `src/QLangCompiler.cpp:981-1044` |
| Callers: `compileWhile`, `compileIf` (fall back to `compileExpr(cond, tmp)` + `JMPIFFALSE`), `for`/`for-in` guards (always Int) | `src/QLangCompiler.cpp:1768-2112`, `2141`, `2172` |
| Literal right operands already go through `emitLiteral(op, a, value)` → `literalImmOf` else pool (M2 D3) | `src/QLangCompiler.cpp` (M2 S5) |
| Disassembler `pseudoText()` is driven by `rightKind` + `isInPlaceForm` (`right()`, `sources()`, `bin()`, `cmp()` lambdas) | `examples/qlang_run.cpp:288-570` |
| `codec_check` asserts `kOpcodeUsedCount == 73`; `encoding_v6` sweeps `i < kOpcodeUsedCount` in three places | `tests/unit/codec_check.cpp:1238`, `tests/unit/encoding_v6_test.cpp:409,980,1032` |
| `docs/SERIALIZATION.md` states "73 defined" three times | `docs/SERIALIZATION.md:132,152,185` |

**Measured today** (`--asm-level 4` on a string-comparison probe, 2026-11-07):

| Source condition | Emitted today | Bytes | Temps | M3 shape | Bytes | Temps |
|--------------------|--------------------------------------------------|-------|-------|------------------------------|-------|-------|
| `if a < b` (String) | `lt 02,00,01` + `jmpfalse 02,0a` | 4 + 3 = 7 | `r2` | `ltf 00,01` + `jne` | 3 + 2 = 5 | 0 |
| `while a != b` (String) | `ne 02,00,01` + `jmpfalse 02,06` | 4 + 3 = 7 | `r2` | `eqf 00,01` + `je` | 3 + 2 = 5 | 0 |
| `if a == 3` (String vs Int) | `mov e=0x40,03,3` + `eq 02,00,03` + `jmpfalse 02,0a` | 4 + 4 + 3 = 11 | `r2`,`r3` | `eqf e=0x40, 00, 3` + `jne` | 4 + 2 = 6 | 0 |
| `if 1 == 1` (Int vs Int) | `mov e=0x40, 03, 1` + `cmp e=0x40, 03, 1` + `jne` (already fused) | 4 + 4 + 2 = 10 | `r3` | unchanged (D2) | 10 | `r3` |

The third row is the headline: the literal load disappears because the compare's right operand
may be an immediate. The fourth row proves the fusion already exists for `Int` (compare + flag
jump, no `jmpfalse`) and proves D5 — an immediate right operand is emitted **escaped**
(`e=0x40`), never as a 3-byte base form. Its leading `mov` is the *left* operand: `inPlaceReg`
returns nothing for a literal, so `emitCmpJumpFalseExpr` stages it into a temp
(`src/QLangCompiler.cpp:1016-1022`) — the left operand of a compare must be a register, so M3
does not remove that load either.

The `Int` gate is conservative in the right direction: a variable later reassigned to a `Float`
stops being `Int` to `inferType`, so the guard falls back to `lt` + `jmpfalse` rather than
emitting a `cmp` that would throw. M3's `*f` path is exactly the domain that gate rejects.

---

## 3. Decisions to settle before coding

| # | Decision | Recommendation | Rationale |
|---|----------|----------------|-----------|
| **D1** | The **negative** branch of a `*f` compare is always `jne` (or `je` for `!=`) — never `jge`/`jle`/`jg`/`jl`. | **Yes** — normative table below | After a *partial* comparator the "relation does not hold" state is **no bits set**. `jge`/`jle` require a bit (`(f & (GR\|EQ)) != 0`), `jg`/`jl` require `f == GR`/`f == LT`; with `f = 0` **none of them fires**, so `if a <= b` lowered as `ltef` + `jg` would silently never take the else path. `cmp`'s total order is what makes today's `cmpJumpIfFalse` mapping (`<`→`jge`, `<=`→`jg`, …) correct; that mapping must **not** be reused for `*f`. This is the single most error-prone rule in M3, which is why the flag matrix (S4) lands **before** the emission change (S5). |
| **D2** | Keep `cmp` when both operands are statically `Int`; use `*f` only where the current gate refuses to fuse. | **Yes** | Same byte count for the compare, but `cmp` sets exactly one bit, so the flags stay reusable (three-way branches, loop guards); its error text (`cmp operand must be an int`) and the corpus shapes are existing behaviour. The change is then purely *additive coverage*: everything the `inferType == Int` gate rejects today (String, Float, Array, Table, object, mixed) stops going through a temporary + `jmpfalse`. |
| **D3** | `*f` reuses the value-comparison rules **verbatim**: `EQF` → `valueEqual`; `LTF`/`GTF`/`LTEF`/`GTEF` → `cmp(a, b, op, vm)` sign test with the **same op-name strings** `doBinaryVm` uses (`"<"`, `">"`, `"<="`, `">="`). | **Yes** | Parent §4.3 says the `*f` family "reuses the existing `EQ`/`LT`/`GT` value-comparison rules — including the `ObjectOps` dispatch". Routing through the same two functions inherits `__str__` dispatch, array/table ordering and every error message, so the corpus byte-identity check is a real proof of equivalence rather than a hope. No new comparison semantics, no new error text. |
| **D4** | `TST` is **implemented but not emitted** in M3. | **Yes** (parent §12 item 6 default, spec `### OpCode tst`) | Codec + VM + disassembler + tests, so the flag matrix (§11 item 5) covers it and Phase 3's removal of `jmpfalse`/`jmptrue` has a target ready. Emitting it for bare truthiness costs +1 byte per test (`tst`+`je` = 4 B vs `jmpfalse` = 3 B) and would churn every corpus `.qla` for no gain. |
| **D5** | `EQF`…`GTEF` join `isAluOrFlagCompare`; `TST` does **not**. Consequences: the five compares get kinds {0,1,2} + the in-place arity rule; `TST` keeps the default `kReg` (so `tst r, imm` is a decode error); an immediate/pool right operand on a compare is **escaped** (4 B for `i8`), and the parent §2.9 / spec `:864` "base 3 B" rows are corrected. | **Yes** | One switch entry buys both §2.5's permitted-kind row and §2.6's arity rule, and the `OpMeta` fast path inherits it for free. `TST` has one operand and no right operand at all, so any kind other than register must stay a load-time rejection. The byte-count correction is the same `defaultRightKind` consequence M2 recorded for pool `mov`. |
| **D6** | `ja`/`jae`/`jb`/`jbe` ship as spec text + a "no identity exists" guard, not as code. | **Yes** | No assembler exists in the repo (§ header). The spec already records the aliases (`### shared registers`, `## opCode scope`); M3 flips the two `## future plans` bullets to landed and asserts in `encoding_v6` that no opcode identity or mnemonic string corresponds to an alias, so a future assembler has a stated contract instead of a guess. |
| **D7** | Extend `emitCmpJumpFalseExpr` in place rather than adding a parallel lowering path. | **Yes** | `compileIf`/`compileWhile` keep one entry point and their existing `-1` fallback (non-comparison conditions: truthiness, `&&`/`\|\|`, `!`, `in`, calls). The return contract (index of the jump to backpatch) and the jump class (unchanged since v5) mean backpatching is untouched. |

**D1 normative table** (what `emitCmpJumpFalseExpr` returns is the jump taken when the
condition is **false**):

| Condition | Comparator | Negative jump (M3) | Positive jump (reference) | Today's `cmp` negative |
|-----------|-----------|--------------------|---------------------------|------------------------|
| `a == b` | `eqf` | `jne` | `je` | `jne` |
| `a != b` | `eqf` | `je` | `jne` | `je` |
| `a < b` | `ltf` | `jne` | `jl` | `jge` |
| `a > b` | `gtf` | `jne` | `jg` | `jle` |
| `a <= b` | `ltef` | `jne` | `jle` | `jg` |
| `a >= b` | `gtef` | `jne` | `jge` | `jl` |

---

## 4. Steps

Each step ends with a green build. S1–S3 are **additive** (nothing emits the new opcodes yet, so
the corpus cannot move); S4 is the test that proves the semantics; S5 switches instruction
selection. A regression in S5 therefore cannot be an encoding bug.

### S1 — Identities and codec tables

> **DONE (`4ead571`), 2026-11-07.** The six identities appended last (73–78),
> `kOpcodeUsedCount` **73 → 79**; `EQF`…`GTEF` in the 2-operand list and `isAluOrFlagCompare`,
> `TST` in the 1-operand list and *not* in the ALU family (D5); mnemonics `eqf`/`ltf`/`gtf`/
> `ltef`/`gtef`/`tst`. No `OpMeta` fast-path edit was needed — it inherits the tables.
> Every S1 expected fact holds: `baseModifier(EQF) == 0x04`, `permittedKinds(EQF) == 0x7`,
> `permittedKinds(TST) == 0x2`, `isInPlaceForm(EQF, Immediate) == true`,
> `isInPlaceForm(TST, Immediate) == false`, `instructionSize(EQF, false, 0x04) == 3`,
> `instructionSize(EQF, true, 0x40) == 4`, `instructionSize(TST, false, 0x04) == 2`
> (and the escaped `tst` is **3** bytes — the modifier byte is not free).
> `codec_check` **274** checks (was 199): the count assertion, the malformed-opcode byte moved
> `73 → 79`, a new `testFlagFlowIdentities` (identity/name/arity/kinds/in-place/sizes/
> encode-decode for all six, incl. the golden `[C9][40][0C][05]` for `eqf r12, 5`), and five
> load-time rejections + four legal load spellings in `testV6LoadValidation` (§11 item 7).
> `encoding_v6` **37 859** checks (was 34 470) — new tallies: sweep **2 029 accepted /
> 15 401 rejected** (was 1 756 / 14 138), decoder/table parity **30 298 rejected / 2 470
> accepted / 15 731 truncations** (was 30 517 / 2 251 / 14 401; the parity sweep spans all 128
> identities, so the six new ones moved 219 pairs from rejected to accepted).
> `ctest` **47/47** green, corpus untouched (nothing emits the new opcodes yet).

`include/qlang/QLangBytecode.h`, `src/QLangBytecode.cpp`:

- Append `EQF, LTF, GTF, LTEF, GTEF, TST` after `INDEXPUT` with a comment block citing
  `13-bytecodes.md` `### extended OpCode eq, lt, gt, lte, gte` / `### OpCode tst` and parent §4;
  bump `kOpcodeUsedCount` **73 → 79**.
- `baseOperandCount`: `EQF`…`GTEF` → the 2-operand list (next to `CMP`); `TST` → the 1-operand
  list.
- `isAluOrFlagCompare`: add the five compares (D5). No other table needs an edit —
  `permittedKinds`, `isInPlaceForm`, `escapedInstructionSize` and the `OpMeta` fast path all
  derive from it.
- `Bytecode::name()`: `eqf`, `ltf`, `gtf`, `ltef`, `gtef`, `tst`.
- `tests/unit/codec_check.cpp:1238`: `kOpcodeUsedCount == 79`; add name/arity checks for the six
  identities.

*Exit criteria:* build green; `encoding_v6` green — the three `kOpcodeUsedCount` sweeps now cover
79 opcodes automatically, so record the **new** accepted/rejected tallies (baseline 1 756 /
14 138) and the parity tallies (30 517 / 2 251 / 14 401); `codec` green. Expected new facts:
`baseModifier(EQF) == 0x04`, `permittedKinds(EQF) == 0x7`, `permittedKinds(TST) == 0x2`,
`isInPlaceForm(EQF, Immediate) == true`, `isInPlaceForm(TST, Immediate) == false`,
`instructionSize(EQF, false, 0x04) == 3`, `instructionSize(EQF, true, 0x40) == 4`,
`instructionSize(TST, false, 0x04) == 2`.

### S2 — VM: the flag compares and `TST`

> **DONE (uncommitted), 2026-11-07.** `QLangVM::flagCompare` (`src/QLangVM.cpp:1117`,
> declared next to `cmpFlags`) — a **member**, not static as `cmpFlags` is, because
> `valueEqual`/`displayString` carry the `__str__` dispatch. The masks are implemented
> with the two existing functions and nothing else (D3): `EQF` → `valueEqual`;
> `LTF`/`GTF` → `cmp(a, b, "<" | ">", this)` sign test; `LTEF`/`GTEF` → the same call
> with `"<="` / `">="`, mapping `c < 0 → LT`, `c == 0 → EQ`, `c > 0 → GR` so the result
> stays one-hot or zero. Dispatch: one case group beside `CMP`/`CMPI`
> (`src/QLangVM.cpp:2496`) taking the right operand through
> `operandValue(i, frame, consts, false)` (register / pool / immediate, no clone, D6)
> and never reading `i.c`; `TST` (`:2510`) is `isTrue(r[a]) ? CMP_EQ : 0`.
> `flagsSatisfied`, the six flag jumps and `JMPIFFALSE`/`JMPIFTRUE` are untouched.
> The six `-Wswitch` warnings S1 left in the exec loop are gone; the only unhandled
> identities in that switch are now the M4 placeholders `INDGET`/`INDPUT`, as before M3.
> `ctest` **47/47** green. Corpus: all **22** `tests/scripts/*.ql` byte-identical
> **stdout and exit code** against a `git worktree` of `28707db` (§11 item 1, taken
> early because S2 is the first step that executes new code).
> Semantics proven ahead of S4 by a throwaway harness (`/tmp`, not committed):
> **287 checks, 0 failures** — 5 comparators × {less, equal, greater} ×
> {register, immediate, pool} × 6 jumps = **270** taken/not-taken assertions, which
> include the none-set state (`ltf` with `a > b` fires `jne` only; `ltef` with
> `a == b` fires `je` **and** `jle`, never `jl`/`jg`/`jge`), `tst` truthy/falsy × 6
> jumps, the `cmp`-throws-on-`String` regression, and error-text parity with the
> value-producing ops (`operator '<=' cannot compare table and int`, D3).
> `pseudoText()` had no case for the six identities when this step finished (level 4
> printed an empty pseudo-op, reading no operands, so nothing desynchronised); S3
> landed in the same pass, as AGENTS.md requires.

`src/QLangVM.cpp` (+ `include/qlang/QLangVM.h` if the helper is a member, as `cmpFlags` is):

- `int flagCompare(Opcode op, const QLangValue &a, const QLangValue &b)` next to `cmpFlags`
  (`src/QLangVM.cpp:1094`), implementing the capability masks of
  `### extended OpCode eq, lt, gt, lte, gte` (D3):
  `EQF` → `valueEqual(a, b) ? CMP_EQ : 0`;
  `LTF` → `cmp(a, b, "<", this) < 0 ? CMP_LT : 0`;
  `GTF` → `cmp(a, b, ">", this) > 0 ? CMP_GR : 0`;
  `LTEF` → `c < 0 ? CMP_LT : c == 0 ? CMP_EQ : 0` with `op = "<="`;
  `GTEF` → `c > 0 ? CMP_GR : c == 0 ? CMP_EQ : 0` with `op = ">="`.
- Dispatch case beside `CMP`/`CMPI` (`src/QLangVM.cpp:2461`): right operand through
  `operandValue(i, frame, consts, false)` (register / pool / immediate, no cloning — M2 D6),
  `frame.flags = flagCompare(op, frame.regs[i.a], rhs)`. The compares never read `i.c`.
- `TST`: `frame.flags = isTrue(frame.regs[i.a]) ? CMP_EQ : 0;`
- `flagsSatisfied`, the six flag jumps, `JMPIFFALSE`/`JMPIFTRUE`: **no change** (spec: "The mask
  reading needs no change to `QLangVM::flagsSatisfied`").

*Exit criteria:* build green; full `ctest` green with **zero corpus change** (nothing emits the
new opcodes yet — if any script's stdout moves, something was touched that should not have
been).

### S3 — Disassembler (same change as S1–S2, AGENTS.md mandate)

`examples/qlang_run.cpp`:

- `pseudoText()`: `EQF`…`GTEF` print `flags = (l <op> r)` using the existing `sources()` lambda
  (it already honours `isInPlaceForm` + `rightKind`); `TST` prints `flags = truth(rA) ? eq : 0`.
  The existing `cmp` lambda prints a **destination** (`r[a] = (…)`), so the flag form needs its
  own printer — reusing it verbatim would claim a register write that never happens.
- `operandText()` / `operandMask()`: expected to need **no** change (driven by
  `baseOperandCount` + `rightKind` + `isInPlaceForm`, which S1 extends). Verify, do not assume.
- `examples/hello.cpp` decodes generically (only `Opcode::CALL` is special-cased) — verify.

*Exit criteria:* `--asm-level 0–5` well-formed on a hand-built stream containing each of the six
opcodes in each permitted kind (the S4 harness can dump it); no out-of-bounds operand read
(`--asm-level 4` on the S4 stream under ASan if the build config allows).

> **DONE (uncommitted), 2026-11-07.** `pseudoText()` gained a `flagCmp` lambda
> (`examples/qlang_run.cpp:360`) and six cases beside `CMP`/`CMPI` (`:544`–`:561`):
> `EQF`…`GTEF` print `flags = (r[a] <op> right())`, `TST` prints
> `flags = truth(r[a]) ? eq : 0` — byte-for-byte the spec's own listings
> (`eqf 0c, 0d ; flags = (r12 == r13)`, `tst 06 ; flags = truth(r6) ? eq : 0`).
> **Plan correction:** `sources()` cannot be used here. It implements the 3-operand
> ALU convention, and for kind 1 (`Register`) `isInPlaceForm` is **false**, so it would
> print `r[b]` / `r[c]` — but `c` is not in a flag compare's stream (§2.6): the left
> operand is always `r[a]` and the right one is kind-selected, i.e. `cmp`'s shape. The
> new lambda therefore calls `right()` directly, as the `cmp` / `cmpi` lines do.
> `operandText()` needed **no** change (verified on real output, not assumed: it is
> driven entirely by `baseOperandCount` + `rightKind` + `isInPlaceForm`); `operandMask()`
> does not exist in the tree (the deviation M2 recorded). `disassemble()` also needed
> nothing — the level-3 pool note keys off `rightKind`, so `eqf r0, pool[0]` gets its
> `# 10`. `examples/hello.cpp` is generic (only `Opcode::CALL` is special-cased) and was
> left alone; it still runs clean.
> Gate (§11 item 9): a throwaway harness (`/tmp`, not committed) hand-built a
> 38-instruction program — the five comparators × {register, immediate, pool}, the
> immediate payload shapes `i8`/`i32`/`u16`/`float`/`true`/`nil` (item 4), string and
> float operands, `tst` feeding the six flag jumps, and `cmp` for contrast — serialized
> it with `writeQlc`, and drove the real `qlang_run` over the `.qlc` at `--asm-level`
> **0–5**: all six listings well-formed, exit 0, and no third operand printed. Under an
> out-of-source ASan build (`Debug`, `-fsanitize=address`) all six levels are clean and
> byte-identical to the normal build — no out-of-bounds operand read.
> No regression to existing listings: HEAD's `qlang_run.cpp` compiled against the current
> library produced **identical** `.qla`, stdout and exit code for all **22**
> `tests/scripts/*.ql` × levels 0–5 — **132** pairs, **0** differences. `ctest` **47/47**.

### S4 — Flag matrix harness (parent §11 item 5) — before any emission

New `tests/unit/flag_flow_test.cpp` + `add_executable` / `add_test(NAME flag_flow …)` in
`CMakeLists.txt` (ctest **47 → 48**). It is behavioural (needs the VM), so it does not belong in
`encoding_v6`; the codec-side facts stay in S1's suites.

- Build streams by hand with `encodeInstruction` (the pattern `codec_check.cpp` already uses for
  the deprecated spellings) and run them on a `QLangVM`.
- Coverage: **7 comparators** (`cmp`, `eqf`, `ltf`, `gtf`, `ltef`, `gtef`, `tst`) ×
  **6 jumps** (`je`, `jne`, `jg`, `jge`, `jl`, `jle`) × **{less, equal, greater}** ×
  **{register right, immediate right, pool right}**, asserting taken / not-taken.
- Assert the **none-set** state explicitly: after `ltf` with `a > b`, `je`/`jl`/`jg`/`jle`/`jge`
  do not fire and `jne` does; after `ltef` with `a == b`, `je` **and** `jle` fire while `jl`,
  `jg`, `jge` do not (this is the case that would fail if anyone reintroduced the rejected
  two-bit `lte` state).
- General-domain cases: String vs String, String vs object (`__str__`), Float vs Int,
  Array vs Array, Table vs Table, and the error paths (`lt` on incomparable types must raise the
  same message the value-producing op raises — D3).
- `cmp` regression: Int-only contract still throws for a String operand.
- Aliases (D6): no identity and no mnemonic for `ja`/`jae`/`jb`/`jbe`; `Bytecode::name()` returns
  the canonical `jg`/`jge`/`jl`/`jle`.
- Reserved slots (§11 item 7): `tst` with kind 0/2 and any compare with kind 3 rejected at load.

*Exit criteria:* the matrix passes against S2's VM. **This is the guard that makes S5 safe** —
if the negative-jump map is wrong, this suite says so while nothing in the corpus depends on it.

### S5 — Compiler: general comparison-condition fusion

`src/QLangCompiler.cpp`:

- `emitCmpJumpFalseExpr` (`:1000`): replace the two `inferType(...) != Int → return -1` gates
  (`:1013-1014`) with a selection — both operands statically `Int` → today's
  `cmp`/`cmpi` + `cmpJumpIfFalse` (D2); otherwise → `*f` + the D1 negative jump.
- Add `flagCompareOpcode(Opcode rel)` (`EQ`→`EQF`, `LT`→`LTF`, `GT`→`GTF`, `LE`→`LTEF`,
  `GE`→`GTEF`, `NE`→`EQF`) and `flagJumpIfFalse(Opcode rel)` (D1: `NE`→`JE`, everything
  else →`JNE`). Keep `cmpJumpIfFalse` untouched for the `cmp` path; the two maps must not be
  merged.
- Right operand: literal → `emitLiteral(op, ra, literal)` (already folds `Int`/`Float`/`Bool`/
  `nil` to immediates and everything else to a pool entry); otherwise `inPlaceReg`/temp as today.
- Non-comparison conditions keep returning `-1` → the existing `compileExpr` + `JMPIFFALSE`
  fallback (D4). `compileGotoIf`, the `switch` case checks and the `for`/`for-in` guards are not
  touched.

*Exit criteria:* build green; `ctest` green; **corpus stdout byte-identical** to a
`git worktree` of `28707db` (§11 item 1) — the fusion is behaviour-preserving by construction
(D3), so any diff is a bug in the jump map or in the operand staging, not an acceptable
"improvement"; record the instruction-count and code-byte delta per script (expect fewer
instructions and one fewer live temp for every non-Int guard).

### S6 — Script suite and corpus re-verification

- New `tests/scripts/flags.ql`, added to the explicit `QL_SCRIPTS` list
  (`CMakeLists.txt:244-266` — deliberately **not** a `file(GLOB)`, so a new script is a
  deliberate CMake edit) → ctest **48 → 49**:
  String `<`/`==`/`!=` in `if` and `while` position, Float vs Int, Array and Table
  comparisons in control position, object `__str__` comparison, mixed String/Int `==`, a loop
  whose guard is a string compare (proves `ltf` + `jne` iterating), and truthiness / `&&` / `||`
  cases that must **still** take the `jmpfalse` path (D4).
- Corpus: every `tests/scripts/*.ql` stdout byte-identical to `28707db` (§11 item 1);
  `--output` → load → run round-trip for all scripts (§11 item 2); `--asm-level 0–5` well-formed
  on `flags.ql`, `cmp.ql`, `hello.ql`, `imm_kinds.ql`, `jump_widths.ql` (§11 item 9).
- Perf: the `perf` ctest entry **skips outside Release** (`SKIP_RETURN_CODE 77`,
  `CMakeLists.txt:289`; the baseline is calibrated for the CI gxx job), so measure in a
  Release build (`cmake -B build-release -DCMAKE_BUILD_TYPE=Release`) against
  `tests/perf/baseline.json` — Round 57 baseline `perf_stress` execute **10.382 ms**. Re-record
  the baseline only if it moves materially, and say which way.

*Exit criteria:* all of the above recorded with numbers, in this file, under a `> **DONE**` note
per step (M2's convention).

### S7 — Documentation (only after the suite is green)

- `docs/spec/13-bytecodes.md`: flip the two `## future plans` bullets from "**— M3, pending**"
  to landed (Round 58); correct `:864` to the escaped 4-byte `eqf` form (D5); note in
  `### OpCode tst` that `tst` is implemented but not emitted in v6 (D4); state the D1 rule
  (negative branch of a `*f` compare is `jne`/`je`) next to the jump table, since the spec's
  `lte`/`gte` rows invite the wrong reading.
- `docs/plans/encoding-v6-and-indirect-plan.md`: §6.1 M3 row → **DONE** with commit SHAs and the
  verification tally; correct the §2.9 rows `mov r3, 5`, `add r3, 12`, `cmp r6, 0`,
  `eqf r12, 5` to the escaped forms (D5); §10's "four exec loops" → one dispatch switch.
- `docs/SERIALIZATION.md`: "73 defined" → **79** at `:132`, `:152`, `:185`; version stays **6**.
- `docs/compliance/history.md`: **Round 58** entry; `docs/compliance/status.md`: the
  extended-encoding open item from "milestones 1–2 of 4" to "1–3 of 4", §11 item 5 flipped to
  verified, tally and hash checkpoint rows updated; `docs/compliance/README.md:22` module table
  from "audit rounds 7–56" to "7–58" (already stale at the baseline — `README.md:4` says
  "last audit Round 57").
- Then `./update-doc-hashes.sh` — **never before** the tests (AGENTS.md: testing proves
  correctness, the hash merely detects change).

*Exit criteria:* `ctest --test-dir build` green including `doc_hashes`; no spec statement left
describing `*f`/`tst` as "not yet emitted" that is now false, and none describing it as emitted
that is not (D4).

### S8 — Housekeeping and commits

Suggested commit names (AGENTS.md: suggest, do not commit; stage by concern):

1. `bytecode+vm: flag-flow opcodes eqf/ltf/gtf/ltef/gtef/tst (M3 S1-S3)`
2. `tests: flag × jump matrix for the *f family and tst (M3 S4, plan §11 item 5)`
3. `compiler: lower comparison conditions to flag compares + flag jumps (M3 S5)`
4. `tests: flags.ql script suite, corpus and perf re-verified (M3 S6)`
5. `docs: flag flow shipped (Round 58, M3 S7)`

Keep the plan-file status edits in the docs commit, not in the feature commits. Nothing moves to
`docs/plans/archive/` until M4 lands — the parent plan stays open.

---

## 5. Verification gate (parent §11, M3 subset)

```bash
cmake -B build && cmake --build build
ctest --test-dir build                      # 49 entries after S4 + S6
build/qlang_run --asm /tmp/flags.qla --asm-level 5 tests/scripts/flags.ql
build/qlang_run --output /tmp/t.qlc tests/scripts/flags.ql   # then round-trip /tmp/t.qlc
```

| §11 item | M3 obligation | Step |
|----------|---------------|------|
| 1 corpus byte-identical | all `tests/scripts/*.ql` vs `28707db` | S5, S6 |
| 2 `.qlc` v6 round-trip | all scripts, incl. `flags.ql` | S6 |
| 3 size/decode agreement | the `encoding_v6` sweeps now cover 79 opcodes × 256 modifiers | S1 |
| 4 zero-byte immediates | unchanged since M2; re-covered by the sweep and by `eqf r, true` | S1 |
| **5 flag matrix** | **7 comparators × 6 jumps × {less, equal, greater} × {reg, imm, pool} + the none-set state + the aliases** | **S4** |
| 6 deprecated spellings | `jmpfalse`/`jmptrue` still emitted for truthiness (D4); `codec` legacy stream green | S5, S6 |
| 7 reserved slots | `tst` kind 0/2 and compare kind 3 rejected at load | S1, S4 |
| 8 `$$` | **M4** — not M3 | — |
| 9 disassembly | `--asm-level 0–5` for every new (opcode, kind) shape | S3, S6 |
| 10 pool shrink | unchanged since M2; record that a pool-right `*f` reuses existing entries (no new ones) | S6 |

---

## 6. Risks and effort

| Risk | Mitigation |
|------|------------|
| **Wrong negative jump after a partial comparator** (`jge` instead of `jne`) → a silently never-taken branch, and the corpus may not even notice if no script exercises it | D1's normative table; S4's matrix lands **before** S5 and covers all 7 × 6 × 3 plus the none-set state; `flags.ql` exercises the shape in a loop |
| Fusion changes behaviour for object/string comparison (`==` routes through `valueEqual`/`isEqual`, `<` through `cmp`) | D3: call the same two functions with the same op-name strings; corpus byte-identity is the proof |
| Someone reintroduces the rejected two-bit `lte`/`gte` flag state | S4 asserts `ltef` with `a == b` yields `f == EQ` only (`je` taken, `jle` taken, `jl`/`jge`/`jg` not); spec `### extended OpCode …` keeps the rejection paragraph |
| Flags clobbered between compare and jump | The jump is always the instruction immediately after the compare (the emitter emits the pair back-to-back and backpatches the target); a condition containing calls stages its operands into registers **before** the compare, as today. Add a `flags.ql` case `if f(x) == g(y)` |
| `kOpcodeUsedCount` bump breaks `codec_check` and the three `encoding_v6` sweeps | S1 updates the assertion and records the new tallies in this file |
| Perf regression (Round 57 baseline 10.382 ms) | S6 runs `perf_stress` explicitly; the fusion removes instructions, so a regression would mean the new dispatch case is mis-scheduled |
| Disassembler forgotten or reading out of bounds for the new operand shapes | S3 in the same commit as S1–S2 (AGENTS.md); `--asm-level 0–5` gate |
| Opcode budget | 79/128 after M3, 80 after M4 — no pressure |

Effort: S1 0.5 d, S2 0.5 d, S3 0.25 d, S4 0.5 d, S5 0.5 d, S6 0.5 d, S7 0.5 d → **≈ 3 days**.

---

## 7. Follow-ups recorded (deliberately not M3)

1. `tst` emission for bare truthiness, together with removing `JMPIFFALSE`/`JMPIFTRUE`
   (Phase 3, `.qlc` v7) — parent §5.4.
2. `switch` case checks (`EQ` + `JMPIFTRUE`, `src/QLangCompiler.cpp:2941`) and `compileGotoIf`
   (`:2208`) could move to `eqf` + `je` / `tst` + `je`; each frees a temp but changes corpus
   shapes — measure separately.
3. `&&` / `||` short-circuits in control position via flags (parent §5.1 mentions `tst` covers
   them; no emission today).
4. Genuinely unsigned relations, and the compiler emitting immediate forms 0–3 (parent §4.4).
5. Widening the remaining ALU families to kinds 0/2 beyond `mov`/`add`/`sub`/`cmp`
   (parent §2.5).
6. An assembler (`qlang_run --assemble`?) is the prerequisite for `ja`/`jae`/`jb`/`jbe` to be
   more than documentation; until then D6's "no identity" guard is the contract.
