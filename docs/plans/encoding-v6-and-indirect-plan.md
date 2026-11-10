# Encoding v6 — Unified Modifier Byte + `$$` Indirect Access

Status: **In progress — specification finalized 2026-11-06; milestones 1 (v6 encoding layer,
additive; 2026-11-06), 2 (pipeline switch, `.qlc` v6; 2026-11-07) and 3 (flag flow, `.qlc` still
v6; 2026-11-07) implemented and green — see §6.1. The shipped pipeline encodes and decodes the
v6 layout; the v5 codec is deleted. This plan's §6 **phase 3 (removal) also landed — Round 60,
2026-11-08** — executed by [archive/phase3-opcode-retirement-plan.md](archive/phase3-opcode-retirement-plan.md):
the seven `i`/`c`/`jmp*` spellings are gone, identities are compacted (`kOpcodeUsedCount` 72)
and `.qlc` is **v7**. Only **M4 (`$$`, §7)** is left.**
Supersedes: §4 (modifier byte), §5.3 (immediates) and §13.4 (signed-`Int`-only scope) of
`docs/plans/archive/instruction-format-escape-byte-plan.md` — that plan's byte-stream model, opcode
space, register/constant pooling and jump forms **stay in force**
Builds on: escape-byte encoding `.qlc` v5 — **DONE** (Round 37)
Reference spec: `docs/spec/13-bytecodes.md` (`## encoding extended`, `## opCode scope`,
`### bytecode encoding`, `### extended OpCode eq, lt, gt, lte, gte`, `## future plans`)
Inventory: `docs/plans/missing-features-plan.md` **A5** (this encoding) and **A3** (`$$`,
whose **Q6** is closed by §7 below)

---

## 1. Scope and settled decisions

Eight design questions were open when A5 was discovered (Round 53). All are settled:

| # | Question | Decision |
|---|----------|----------|
| D1 | Modifier-byte layout | Adopt the spec's **2/2/4** split (`bit 0-1` left register / `bit 2-3` right-operand **kind** / `bit 4-7` right-operand **form**), replacing v5's two nibbles. |
| D2 | `i`/`c` suffixes | **Folded into the base opcodes** via the kind field. Long term `MOVI`/`ADDI`/`SUBI`/`CMPI`/`MOVC` are **removed**; `ADDC`/`SUBC` are never added. |
| D3 | Base (unescaped) immediate | Stays **1-byte signed `Int`** (cheapest, most common). |
| D4 | `CALL` | Keeps a documented exception: always escaped, function-id immediate fixed at 2-byte signed, trailing 1-byte arg count. Expressible in the new layout (§2.8). |
| D5 | Flag-based flow vs `JMPIFFALSE`/`JMPIFTRUE` | Both decode in v6; `jmpfalse`/`jmptrue` are **deprecated** and phased out behind `TST` + `je`/`jne` (§5). |
| D6 | `ja`/`jb`/`jae`/`jbe` | **Assembler aliases** of `jg`/`jl`/`jge`/`jle` — no distinct encoding, no new identities (§4.4). A distinct unsigned compare is deferred. |
| D7 | Reserved slots | `kind = 3` and immediate forms `12`–`15` stay **reserved** — `$$` does not consume them (§7.6). |
| D8 | v6 immediate set | Ships the **full** set: unsigned/signed 1/2/4/8-byte, `float`, `bool`, `nil` — moving bool/nil/float out of the constant pool. |

Two consequences worth stating up front:

1. **The on-disk encoding changes ⇒ `.qlc` v5 → v6.** The reader accepts v6 only (Round 37
   precedent: single-version reader, clean break, no migration shim).
2. **`$$` needs no encoding space at all.** Its declaration bit is per-*event* runtime state,
   not per-instruction state (§7). A5 and A3 are therefore genuinely independent, which
   removes the `A5 layout ──> A3 Q6` edge from the dependency graph.

---

## 2. The v6 modifier byte `e`

Unchanged from v5: opcode byte bit 7 = escape flag, bits 0–6 = identity (128 values,
`kOpcodeUsedCount` 73 when this was written — **72 since the Round 60 compaction**, §3);
escaped form = `[op | 0x80][e]` + operands. **Only the interpretation of `e` changes.**

### 2.1 Field layout

| Field | Bits | Meaning |
|-------|------|---------|
| left operand (`a`) | 0–1 | always a **register**: `0` = 1 B, `1` = 2 B, `2` = 4 B (future), `3` = reserved |
| right-operand **kind** | 2–3 | `0` = immediate, `1` = register, `2` = constant pool, `3` = **reserved** |
| right-operand **form** | 4–7 | interpreted by kind (§2.3) |

v5's nibble split (`bit 0-3` left form / `bit 4-7` right form) is replaced. The left operand
never needed 4 bits — addressing widths 1/2/4 leave one value spare — and the freed two bits
become the kind selector, which is what makes the `i`/`c` suffixes redundant (D2).

### 2.2 Right-operand kind

| Kind | Operand is | Bytes | Selected by |
|------|-----------|-------|-------------|
| `0` immediate | inline literal | by form (§2.3) | the old `…i` suffixes (`movi`, `addi`, `cmpi`) |
| `1` register | another register | by form (§2.3) | the unsuffixed base form |
| `2` constant pool | pool index | by form (§2.3) | the old `…c` suffixes (`movc`, `addc`, `subc`) |
| `3` reserved | — | — | decode error (§2.5) |

### 2.3 Right-operand form

**Kind `0` — immediate** (width × signedness × type):

| Form | Bytes | Value | QLang type | Notes |
|------|-------|-------|------------|-------|
| 0 | 1 | unsigned | `Unsigned` | |
| 1 | 2 | unsigned | `Unsigned` | |
| 2 | 4 | unsigned | `Unsigned` | |
| 3 | 8 | unsigned | `Qword` | |
| 4 | 1 | signed | `Int` | **base form** (D3) |
| 5 | 2 | signed | `Int` | |
| 6 | 4 | signed | `Int` | |
| 7 | 8 | signed | `Long` | |
| 8 | 8 | IEEE-754 double | `Float` | new (D8) |
| 9 | 0 | `false` | `Bool` | new — value implied by the form, no operand bytes |
| 10 | 0 | `true` | `Bool` | new — value implied by the form, no operand bytes |
| 11 | 0 | `nil` | `Nil` | new — value implied by the form, no operand bytes |
| 12–15 | — | — | — | **reserved** (D7) |

Signed forms sign-extend to `Int` (1/2/4 B) or `Long` (8 B); unsigned forms zero-extend to
`Unsigned` (1/2/4 B) or `Qword` (8 B). The compiler emits the narrowest form whose type and
range fit the literal. Forms 9/10/11 are **zero-byte immediates**: the form *is* the value, so
`mov r12, nil` costs 2 bytes and touches no pool slot.

**Kind `1` — register** and **kind `2` — constant pool** share the addressing table (identical
to registers, §5.1 of the v5 plan):

| Form | Addressing | Bytes |
|------|-----------|-------|
| 0 | index 0–255 | 1 |
| 1 | extended index | 2 |
| 2 | extended index | 4 (future) |
| 3–15 | reserved | — |

### 2.4 Base form equivalence

"No escape bit ⇒ every operand is 1 byte" (v5 §7) is unchanged; it now means a specific `e`
value per opcode class:

| Opcode class | Base form equals | `e` |
|--------------|------------------|-----|
| register-right (`mov`, `add`, `cmp`, `eqf`, …) | left 1 B reg + kind 1 + form 0 | `0x04` |
| immediate-right (`movi`, `addi`, `cmpi`) | left 1 B reg + kind 0 + form 4 | `0x40` |
| const-right (`movc`, `makefunc`, `makegen`) | left 1 B reg + kind 2 + form 0 | `0x08` |
| jump class | jmp8 (1-byte relative) | — (§2.7) |

The escape is emitted **only** when an operand exceeds its 1-byte default.

### 2.5 Permitted kinds per opcode class (normative)

The kind field is *not* free for every opcode: each opcode declares the kinds it accepts, and
a modifier naming an unpermitted kind (or a reserved form, or reserved kind `3`) is a
**decode error** — the reader rejects the file, the VM never sees it.

| Class | Opcodes | Permitted kinds |
|-------|---------|-----------------|
| copy | `mov`, `conv`, `neg`, `deepcopy` | 0, 1, 2 |
| binary ALU | `add`, `sub`, `mul`, `div`, `mod`, `and`, `or`, `xor`, `shl`, `shr`, `land`, `lor`, `eq`, `ne`, `lt`, `gt`, `le`, `ge`, `pow`, `inx` | 0, 1, 2 |
| flag compare | `cmp`, `eqf`, `ltf`, `gtf`, `ltef`, `gtef` | 0, 1, 2 |
| pool-index | `makefunc`, `makegen` | 2 only |
| call | `call` | 0 only, form fixed to 5 (§2.8) |
| everything else | array/table/control-flow/closure/`indget`/`indput`/… | 1 only (today's behaviour) |

The **decoder** implements the whole table; the **compiler** is only required to emit widened
right operands for the families named in `13-bytecodes.md` `## opCode scope` (`mov`, `add`,
`sub`, `cmp`, the flag compares). Widening the remaining ALU ops is a follow-up, not a v6
requirement.

### 2.6 Third operand `c`

Unchanged from v5: 3-operand ops carry `c` as a trailing **1-byte register** with no form
field. Extended `c` stays future work (v5 §14.1).

**Arity rule (new, follows from D2):** for a 3-operand opcode, kind `1` means the full
3-operand form `r[a] = r[b] op r[c]`; kind `0`/`2` means the **in-place 2-operand** form
`r[a] = r[a] op right` — exactly today's `addi`/`subi` semantics, so `add a, imm` *is*
`addi a, imm`. Consequence: `r[x] = r[y] + imm` is not encodable in one instruction; the
compiler emits `mov x, y` + `add x, imm` (2 instructions — the same count as today's
`movi t, imm` + `add x, y, t`, with one fewer live temporary).

### 2.7 Jump class

Unchanged from v5 §4.1: base = **jmp8** (1-byte relative, ±127); escaped `e = 0` → **jmp16**
(2-byte relative); escaped `e = 1` → **jmp32** (4-byte relative). `off = target_pc −
(jump_pc + jump_size)`. The jump class does **not** use the 2/2/4 layout — `e` is the offset
width selector only. Flag jumps (`je`, `jne`, `jg`, `jge`, `jl`, `jle`, `jmp`, `continue`,
`break`) carry no condition operand; the deprecated `jmpfalse`/`jmptrue` keep their trailing
1-byte condition register (v5 behaviour) until they are removed (§6).

### 2.8 `CALL` (documented exception, D4)

`call` is always escaped. Its `e` is `left | kind 0 | form 5` — the builtin id / native slot is
a **2-byte signed immediate** — followed by the 1-byte argument count. So the exception is
expressed *within* the new layout rather than as a separate rule: the fixed form is simply
form 5, and the trailing `c` byte is the ordinary third operand.

### 2.9 Concrete instruction forms

Corrected 2026-11-06 against the milestone-1 implementation
(`tests/unit/encoding_v6_test.cpp`); the first draft of this table still carried v5's
two-nibble modifier bytes, which mis-stated `e` and every 8-byte immediate size. An escaped
instruction costs `1 (op) + 1 (e) + width(a) + immediate/operand bytes`.

| Instruction | Form | Bytes | Layout |
|-------------|------|-------|--------|
| `mov r3, r7` | base | 3 | `[MOV][03][07]` (≡ `e = 0x04`) |
| `mov r3, 5` | base | 3 | `[MOV][03][05]` (≡ `e = 0x40`) |
| `mov r3, pool[9]` | base | 3 | `[MOV][03][09]` (≡ `e = 0x08`) |
| `mov r3, 200` | escaped | 4 | `[MOV\|80][00][03][C8]` (kind 0, form 0 — `e = 0x00` **is** a legal modifier) |
| `mov r3, 500` | escaped | 5 | `[MOV\|80][50][03][F4 01]` (kind 0, form 5 = i16) |
| `mov r3, 4000000000` | escaped | 7 | `[MOV\|80][20][03][u32:4]` (kind 0, form 2 = u32) |
| `mov r3, 1.5` | escaped | 11 | `[MOV\|80][80][03][double:8]` (kind 0, form 8) |
| `mov r3, true` | escaped | 3 | `[MOV\|80][A0][03]` (kind 0, form 10, 0 operand bytes) |
| `mov r3, nil` | escaped | 3 | `[MOV\|80][B0][03]` (kind 0, form 11, 0 operand bytes) |
| `mov r3, pool[300]` | escaped | 5 | `[MOV\|80][18][03][2C 01]` (kind 2, form 1) |
| `mov r300, r7` | escaped | 5 | `[MOV\|80][14][01 2C][07]` (left form 1, kind 1) |
| `add r3, r4, r5` | base | 4 | `[ADD][03][04][05]` (≡ `e = 0x04`, three operands) |
| `add r3, 12` (in place) | base | 3 | `[ADD][03][0C]` (≡ `e = 0x40`, no `c`) |
| `add r3, 2.5` (in place) | escaped | 11 | `[ADD\|80][80][03][double:8]` (kind 0 ⇒ in place, no `c`) |
| `cmp r6, r9` | base | 3 | `[CMP][06][09]` (≡ `e = 0x04`) |
| `cmp r6, 0` | base | 3 | `[CMP][06][00]` (≡ `e = 0x40`) |
| `call f, 2` | escaped | 6 | `[CALL\|80][50][01][id:2][02]` (kind 0, form 5 — always escaped, §2.8) |
| `jmp` near / mid / far | base / escaped | 2 / 4 / 6 | v5 §5.4 — the jump class ignores the 2/2/4 fields (§2.7) |
| `eqf r12, r13` | base | 3 | `[EQF][0C][0D]` — **M3**, opcode not yet in the enum |
| `eqf r12, 5` | base | 3 | `[EQF][0C][05]` (≡ `e = 0x40`, in place) — **M3** |
| `tst r6` | base | 2 | `[TST][06]` — **M3** |
| `decl 7` | base | 2 | `[DECL][07]` — **M4** |

---

## 3. Opcode reconciliation — removing the `i`/`c` suffixes (D2)

| Today | v6 spelling | v6 encoding | Fate of the old identity |
|-------|-------------|-------------|--------------------------|
| `movi a, imm` | `mov a, imm` | `mov`, kind 0 | `MOVI` deprecated → **removed in v7 (Round 60)** |
| `movc a, idx` | `mov a, pool[i]` | `mov`, kind 2 | `MOVC` deprecated → **removed in v7 (Round 60)** |
| `addi a, imm` | `add a, imm` | `add`, kind 0 (in place) | `ADDI` deprecated → **removed in v7 (Round 60)** |
| `subi a, imm` | `sub a, imm` | `sub`, kind 0 (in place) | `SUBI` deprecated → **removed in v7 (Round 60)** |
| `cmpi a, imm` | `cmp a, imm` | `cmp`, kind 0 | `CMPI` deprecated → **removed in v7 (Round 60)** |
| `addc` / `subc` (spec families) | `add` / `sub`, kind 2 | no new opcode | **never added** |

**Coexistence rule for v6:** *(in force for v6 only — retired in Round 60)* the decoder accepts
*both* spellings — the deprecated identities remain in the enum and decode with kind restricted
to their historical kind (`MOVI` → kind 0 only, `MOVC` → kind 2 only, …), so a v6 stream that
still contains them is valid. The v6 **compiler emits only the unified spellings**. Removal
happens in a later version once no producer emits them, which is a second `.qlc` bump with no
encoding-layout change. **That bump landed in Round 60:** the seven identities are deleted, the
remaining ones are compacted (`new = old − |{removed ids < old}|`, no holes left), and a v6
image now dies at the version field before any byte decodes — the removed identities are
reused by live opcodes, so byte-level detection of the old spellings is impossible and
unnecessary. `JMPIFFALSE`/`JMPIFTRUE` went with them (§5.4).

Opcode budget: 73 today + 7 new (§4) = **80** of 128; **75** after the five suffix identities
are removed; **73** after `JMPIFFALSE`/`JMPIFTRUE` are removed (§5.4). *Actual outcome: M4 has
not landed, so `DECL` was never appended — 79 + 7 removed = **72** at Round 60. External-modules
plan v4 Phase 3c then appended `GETIMPORT`/`CALLIMPORT` as **72/73** (Round 65, its **Q31**,
"append last, never renumber"), so `kOpcodeUsedCount` is **74** and **`DECL` takes 74 (→ 75) when
M4 lands** — 72 is no longer available to it.*


---

## 4. New opcodes (7)

Appended last in the enum, as always, so existing identities stay valid.

| Opcode | Mnemonic | Operands | Effect |
|--------|----------|----------|--------|
| `EQF` | `eqf` | `a`, right | flags ← relation of `r[a]`, right (general types) |
| `LTF` | `ltf` | `a`, right | flags ← relation of `r[a]`, right |
| `GTF` | `gtf` | `a`, right | flags ← relation of `r[a]`, right |
| `LTEF` | `ltef` | `a`, right | flags ← relation of `r[a]`, right |
| `GTEF` | `gtef` | `a`, right | flags ← relation of `r[a]`, right |
| `TST` | `tst` | `a` | flags ← `truth(r[a]) ? EQ : 0` |
| `DECL` | `decl` | site id (1-byte imm) | sets the frame's declaration bit for that site (§7) |

### 4.1 Why `f`

`EQ`/`NE`/`LT`/`GT`/`LE`/`GE` already exist and produce a **boolean value in a register**
(`r[a] = (r[b] == r[c])`) — needed whenever a comparison is a value (`flag = a == b`). The new
family produces **flags, no register**. The spec calls the family `eq, lt, gt, lte, gte`;
because those mnemonics are taken, the assembly/disassembly spelling gains an `f` suffix
(`eqf`, `ltf`, `gtf`, `ltef`, `gtef`). `13-bytecodes.md` records the mapping.

There is no `nef`: "not equal" is `eqf` + `jne`. There is no `lte`/`gte` value-producing
counterpart either — `LE`/`GE` already cover that.

### 4.2 The flag model (normative — resolves the spec's ambiguous table)

The shared flags are three **independent relation predicates** on the frame
(`QLangVM.h:131-133`: `CMP_GR = 1`, `CMP_EQ = 2`, `CMP_LT = 4`), wiped before each compare. A
compare sets **exactly the bits whose relation holds**. Because `>`, `==` and `<` are mutually
exclusive, **at most one bit is ever set** — the existing jump predicates
(`QLangVM::flagsSatisfied`, `src/QLangVM.cpp:1063-1074`) depend on it:

| Jump | Predicate (implemented today) |
|------|-------------------------------|
| `je` | `f == CMP_EQ` |
| `jne` | `(f & CMP_EQ) == 0` |
| `jg` | `f == CMP_GR` |
| `jge` | `(f & (CMP_GR \| CMP_EQ)) != 0` |
| `jl` | `f == CMP_LT` |
| `jle` | `(f & (CMP_LT \| CMP_EQ)) != 0` |

The spec table in `### extended OpCode eq, lt, gt, lte, gte` lists `lte` as `eq=1, lt=1` and
`gte` as `eq=1, gt=1`. Read as a *flag state* that is **rejected**: a simultaneous two-bit
state is lossy and breaks the existing predicates — `je` (`f == CMP_EQ`) would not fire after
`lte` for `a == b`, and `jle` would fire for `a == b` after `cmp` only by accident. Read as a
**capability mask** — "which bits this opcode may set" — it is exactly right and needs no jump
change. The spec is rewritten to say so explicitly.

| Op | May set | Actual state |
|----|---------|--------------|
| `cmp` / `cmp imm` | gr, eq, lt | exactly one (total order, `Int` only) |
| `eqf` | eq | `eq` when equal, **none** otherwise |
| `ltf` | lt | `lt` when less, none otherwise |
| `gtf` | gr | `gr` when greater, none otherwise |
| `ltef` | eq, lt | `lt` when less, `eq` when equal, none otherwise |
| `gtef` | eq, gr | `gr` when greater, `eq` when equal, none otherwise |
| `tst` | eq | `eq` when truthy, none otherwise |

"None" is a legal state and means *the tested relation does not hold* — every jump predicate
above already handles it (`jne` jumps, `je`/`jl`/`jg`/`jle`/`jge` do not).

### 4.3 Type domain

`cmp` stays **`Int` only** (spec: "comparison (numbers only)"). The `*f` family is the
**general** comparison: it reuses the existing `EQ`/`LT`/`GT` value-comparison rules —
including the `ObjectOps` dispatch that `==`/`<` already perform on strings, tables and
external objects — and writes the relation into the flags instead of a register. `tst` uses
the VM's ordinary `truth()`.

### 4.4 `ja` / `jae` / `jb` / `jbe` (D6)

QLang's flags carry **no signedness** — the relation is computed from the operand values, and
`Int`/`Unsigned`/`Qword` compare within their own types. There is therefore nothing for a
distinct unsigned jump to test. They are **assembler-level aliases** — `ja`≡`jg`,
`jae`≡`jge`, `jb`≡`jl`, `jbe`≡`jle` — with no distinct encoding and no new identities;
disassembly prints the canonical mnemonic. A genuinely unsigned compare (bitwise/unsigned
relation on mixed operands) is deferred to a future version along with forms 0–3 being
emitted by the compiler.

### 4.5 Savings

The current shape of a comparison in control position is *compare into a temporary +
`jmpfalse` on that temporary* — two instructions and one live register. The v6 shape is
*flag compare + flag jump* — two instructions, **zero** temporaries, and the compare's right
operand may be an immediate or a pool entry. This is the "reducing temporary registers is a
priority" item in `13-bytecodes.md` `## future plans`.

---

## 5. Phasing out `jmpfalse` / `jmptrue` (D5)

### 5.1 `TST`

`jmpfalse r9, target` ≡ `tst r9` + `jne target`; `jmptrue r9, target` ≡ `tst r9` + `je target`.
`TST` sets `eq` from `truth(r[a])`, so the whole flag-jump set becomes reachable from any
truthy value, not just comparison results — which also covers `while x`, `if not x`, `&&`/`||`
short-circuits.

### 5.2 Cost and when it is not a win

`tst` + `je` is 2 + 2 = 4 bytes minimum against `jmpfalse`'s 1 + 1 + 1 = 3 bytes (base form).
The pair is a net win whenever the condition is already a comparison (`eqf` + `jne` replaces
`eq` + `jmpfalse` and frees a register) or when the flags are reused by a second branch. The
compiler is **not** required to emit `tst` for a bare truthiness test in v6 — it may keep
`jmpfalse` there. Deprecation is about the *spelling in new emission sites for comparisons*,
not about a mechanical rewrite. *(Phase 3 flipped this: with the pair removed there is nothing
left to keep, and the mechanical rewrite is what landed — see §5.4 and §12 item 6.)*

### 5.3 v6 behaviour

*(v6 only — superseded by Phase 3, Round 60.)* Both forms decode. The compiler emits flag jumps
for comparison conditions and keeps `jmpfalse`/`jmptrue` for bare truthiness tests.
`13-bytecodes.md` marks the pair deprecated.

### 5.4 Removal — **DONE (Round 60, 2026-11-08)**

`JMPIFFALSE`/`JMPIFTRUE` are removed together with the `i`/`c` suffix identities in the later
version (§6, phase 3): `TST` + `je`/`jne` fully subsume them, at the cost of one byte per bare
truthiness test. **Landed:** all 15 compiler emission sites now lower bare truthiness to
`tst` + `je`/`jne` (flipping §12 item 6's default), the pair's condition-register jump form is
deleted from `emit()`, and the cost measured exactly as predicted — +1 instruction per test
site, 83 sites across the corpus, `hello.ql` 211 → 215 and `perf_stress.ql` 521 → 565, corpus
stdout byte-identical.

---

## 6. Migration and phasing

| Phase | Content | `.qlc` |
|-------|---------|--------|
| **1 — v6 (this plan)** | 2/2/4 modifier byte; full immediate set; `mov`/`add`/`sub`/`cmp` kind folding; `eqf`/`ltf`/`gtf`/`ltef`/`gtef`; `tst`; `ja`/`jae`/`jb`/`jbe` aliases; `DECL` + `$$` (§7); frame name map + declaration table in the file | v5 → **v6** |
| **2 — compiler follow-up** | Compiler stops emitting `movi`/`movc`/`addi`/`subi`/`cmpi` (already required in phase 1) and stops emitting `jmpfalse`/`jmptrue` for comparisons; widened right operands extended to the remaining ALU ops; constant pool shrinks as bool/nil/float become immediates | unchanged |
| **3 — removal** | `MOVI`, `ADDI`, `SUBI`, `CMPI`, `MOVC`, `JMPIFFALSE`, `JMPIFTRUE` deleted from the enum; opcode identities compacted (a one-time renumbering, since removal leaves holes) | v6 → **v7 — DONE Round 60** |

Phase 3 is deliberately separate: it changes no layout, only the identity table, and it can
only land once no producer (compiler, tests, checked-in `.qlc` fixtures) emits the old
spellings. **It landed 2026-11-08** — the producer gate (compiler + disassembler grep, plus an
`--asm-level 4` scan of every corpus listing) was recorded clean before the enum was touched,
and no checked-in `.qlc`/`.qla` fixture exists (all such dumps are gitignored). Executed by
[archive/phase3-opcode-retirement-plan.md](archive/phase3-opcode-retirement-plan.md); see the P3 row below.

### 6.1 Milestone status (implementation decomposition actually used)

The phase table above is the *encoding* phasing. The work is landed in four reviewable
milestones; each keeps `ctest --test-dir build` green.

| # | Milestone | Scope | Status |
|---|-----------|-------|--------|
| **M1** | v6 encoding layer, **additive** | 2/2/4 modifier byte, kind/form tables, permitted-kind + in-place-arity rules, full immediate set, `CALL` as kind 0 / form 5, jump class untouched, new `*V6` codec + `encoding_v6` test suite. v5 codec, `kQlcVersion = 5` and the shipped pipeline are **not** modified. | **DONE 2026-11-06** |
| **M2** | Switch the pipeline | Compiler emits unified `mov`/`add`/`sub`/`cmp` kinds with immediate selection, VM decodes v6, `.qlc` v5 → v6, disassembler (`examples/qlang_run.cpp`) | **DONE 2026-11-07** |
| **M3** | Flag flow | `eqf`/`ltf`/`gtf`/`ltef`/`gtef` + `tst`, flag jumps, `ja`/`jae`/`jb`/`jbe` aliases (§4) | **DONE 2026-11-07** (Round 59; `4ead571`…`56998cf`, docs `51f9d33`) — plan [archive/m3-flag-flow-plan.md](archive/m3-flag-flow-plan.md) |
| **P3** | §6 phase 3 — removal | The seven deprecated identities deleted, identities compacted, `tst` emitted for bare truthiness, `.qlc` v6 → v7 | **DONE 2026-11-08** (Round 60; `b86460b` S1 / `801498c` S3 / `a6737b9` S4) — plan [archive/phase3-opcode-retirement-plan.md](archive/phase3-opcode-retirement-plan.md) |
| **M4** | `$$` (A3) | Declaration table, frame name map + bitset, `DECL`, two-operand `INDGET`/`INDPUT`, `$$"literal"` fast path (§7) | pending — `DECL` now takes identity **74** (`kOpcodeUsedCount` → 75): external-modules plan v4 Phase 3c took 72/73 in Round 65, so the 72 this row assumed is gone; the declaration table still ships inside the v6 layout, now carried by a v9 file (§8) |

P3 deviation from the phase table: the plan budgeted **73** remaining identities on the
assumption that M4 appended `DECL` first (80 − 7). M4 has not landed, so the count is **72** and
`DECL` will be appended at 72 — one renumbering event either way, and no document other than
this table's arithmetic depends on which.

Two decisions M1 settled, both deviations from the draft text:

1. **`instructionSizeV6(Opcode, bool escaped, uint8_t e)` takes an explicit `escaped` flag.**
   Under 2/2/4, `e = 0x00` is a *legal* escaped modifier (kind immediate, form `u8` — the only
   way to encode `mov r3, 200` in one operand byte), so v5's "`modifier == 0` ⇒ base form"
   convention cannot be reused. Without the flag that instruction sizes as 3 bytes while the
   encoder emits 4, desynchronising `pc`.
2. **`CALL` is rejected when unescaped** by both `encodeInstructionV6` and
   `decodeInstructionV6`, per §2.8 ("always escaped"), rather than silently taking a base form.

M1 verification: `encoding_v6` (34 249 checks) covers field layout and reserved-slot throws, the
width/form tables, base-form byte-for-byte equivalence with v5, the escaped-size table,
immediate boundaries (`u8`…`u64`, `i8`…`i64`, bit-exact `double`, zero-byte `bool`/`nil`), CALL,
the unchanged jump class, encoder range checks, truncated streams, and an exhaustive sweep of
1 756 accepted / 14 138 rejected `(opcode, modifier)` pairs cross-checked between size, encoder
and decoder. Full suite: 47/47 (`perf` skipped).

Six decisions M2 settled before coding (full rationale in §2 of `docs/plans/archive/m2-pipeline-switch-plan.md`):

1. **D1 — the `V6` suffix is dropped.** With exactly one codec, the plain names
   (`encodeInstruction`, `decodeInstruction`, `instructionSize(op, escaped, e)`, `makeModifier`,
   `baseModifier`) win; the v5 signatures were identical, so the 14 external call sites took
   zero churn. The suffix would also be wrong after Phase 3 (v7 changes identities, not layout).
2. **D2 — `DecodedInstruction` gains `immForm`.** `ImmPayload::Signed` alone cannot distinguish
   `Int` (`i8`/`i16`/`i32`) from `Long` (`i64`).
3. **D3 — immediate emission restricted to exactly-round-tripping literals:** `Int` within
   ±2³¹ (`i8`/`i16`/`i32`), `Float` (form 8), `Bool` (9/10), `nil` (11). `Long`/`Qword`/
   `Unsigned` stay in the pool — the codec supports `i64`/`u64`, but the value type of a
   64-bit literal is ambiguous; widening is a measurable follow-up.
4. **D4 — `.qlc` v6 ships without the declaration table.** M4 adds it as an optional section
   inside v6 (§8 annotated); Phase 3 bumps v7.
5. **D5 — the v5 harness `tests/unit/encoding_test.cpp` is retired** (its ctest target
   deleted); its two unique cases (jump-width tiling, truncated-stream decode error) moved
   into `encoding_v6_test.cpp`.
6. **D6 — pool reads clone containers only on store** (`mov`/`movc` → `freshContainerValue`);
   ALU/`cmp` right operands read straight from the pool are read-only and never copied.

M2 verification (2026-11-07): corpus stdout byte-identical to the pre-switch build (21
scripts, §11 item 1); `.qlc` v6 round-trip + execute for all 22 scripts (item 2);
`instructionSize` == encoder length at every `pc` over a stream tiled by every (kind, form)
pair (item 3); `true`/`false`/`nil` cost 3 B with no pool entry and the pool shrank
515 → 477, `bool`/`nil`/`float` entries gone (item 4/10); the deprecated spellings still
execute from a hand-built stream (item 6); reserved kinds/forms rejected at load (item 7);
`--asm-level 0–5` well-formed for every form (item 9). `encoding_v6` 34 466 checks,
`codec_check` 199. Full suite: 47/47. §11 items 5 (flag matrix) and 8 (`$$`) belong to
M3–M4.
**Round 57 follow-up (2026-11-07):** the +32.5 % `perf_stress` execute regression the switch
introduced was investigated rather than accepted — `decodeInstruction` is now a single-pass
table-driven decoder (16.4 → 6.68 ns/instruction, v5 was 6.03), behaviour-identical under an
exhaustive differential sweep, with a permanent decoder/size-table parity guard (§18) →
`encoding_v6` **34 470**, `perf_stress` execute 12.995 → 10.382 ms, full suite 47/47. See
`docs/plans/archive/m2-pipeline-switch-plan.md` §6.

---

## 7. `$$` indirect access inside the new framework (closes A3 **Q6**)

### 7.1 The problem is not the encoding

`05-operators.md` `## operator $$` note 2 puts the declaration bit "on the existing
store/init opcode". That was rejected in Q6 as unencodable in v5; A5 *does* create spare
encoding space (`kind = 3`, immediate forms 12–15), so the question has to be answered on its
merits rather than on availability — and the answer is still **no**, for a reason independent
of the layout:

**The declaration bit is per-*event* state, not per-instruction state.** The spec's own
example — `if cond { local a = 1 } … $$a = 2` is legal only if `cond` ran — means the bit
belongs to the *execution history of a frame*, and it is indexed by *declaration site*, not by
register (registers are reused across branches and loop iterations; **Q7**). No operand field
can express "has this declaration executed yet in this frame". An encoding bit could at best
say "this instruction is a declaration", which is static information the compiler already has.

### 7.2 Design

Three pieces, none of which touches the modifier byte:

1. **Declaration-site ids + per-function declaration table** (compile time). Every declaration
   that is visible to `$$` (a `local`/`let`/`def`/`global` in a frame-visible scope) gets a
   site id `0..n-1` and a table entry `{site id → register, name, kind}` where kind ∈
   `var` / `const` / `function` / `global`. The table is **function metadata**, emitted in the
   `.qlc` v6 function record (§8), and is also the source for the frame name map.
2. **Frame name map** (function metadata → frame state). `name → {register, kind}`, built once
   when the frame is created from the function's declaration table. This is spec note 1's
   "every name directly declared in the frame" — frame-wide, **Q8** resolved as defaulted; the
   existing asymmetry note (direct references are block-scoped since Round 10, indirect
   resolution is frame-wide) already documents the difference.
3. **Per-frame declaration bitset** (runtime). `std::vector<bool>` (or a packed bitmap) sized
   to the function's declaration count, **cleared when the frame is created** and set by the
   `DECL` opcode.

### 7.3 `DECL siteId`

The declaration bit is set by a dedicated marker opcode executed exactly where the declaration
*executes*:

```
if cond { local a = 1 }      →   …branch…  decl  <siteId(a)>   ; mov <reg a>, 1
$$a = 2                      →   indput <reg "a">, <reg 2>
```

`decl` takes a 1-byte site-id immediate (base form, 2 bytes; escaped 2-byte id for functions
with >255 declarations — in practice never). It has no other effect. Placing it is a compiler
concern and is trivial: emit it immediately after the declaration's store, in the same
block/scope the declaration lives in. Loops re-execute it, which is correct (idempotent).

Cost: one byte per declaration in a frame-visible scope. The compiler may omit `DECL` for a
declaration whose site **dominates** every `$$` use of that name (the bit is then statically
always set) — the same dominance reasoning as the literal fast path in §7.5.

### 7.4 `INDGET` / `INDPUT`

The reserved placeholders (`QLangBytecode.h:123-124`, identities inside the defined range,
mnemonics `indget`/`indput`) become **ordinary register-class opcodes** under the new layout —
kind 1 only, no special encoding (**Q9**: keep the implemented names and align the spec's
`OP_INDIRECT_READ`/`OP_INDIRECT_WRITE` wording). **Q10**: move them to the 2-operand class
(`indget dst, nameReg`, `indput nameReg, srcReg`) — they were never emitted, so there is no
compatibility cost; `qlang_run`'s pseudo-text is updated in the same change.

The six-stage check pipeline (string → identifier lexeme → reserved word → declared/visible →
binding kind → local-only writes) runs in the VM against the frame name map + bitset, with the
canonical `indirect variable error:` messages. No variable creation; functions/lambdas are
never targetable; `const` is read-only; a branch-dependent declaration reports `not declared`.

### 7.5 `$$"literal"` fast path (**Q11**)

Ships, but narrowly: when the literal name resolves to a declaration whose site **dominates**
the use, the compiler resolves the name at compile time and emits the direct register
operation — no `INDGET`/`INDPUT`, no bitset lookup. Without dominance the dynamic path is
emitted (the literal is just a constant string in a register). This keeps spec note 4 honest
without a parser commitment beyond the literal form.

### 7.6 Rejected alternative (recorded so it is not re-litigated)

`kind = 3` + a flag byte carrying `{declared, site id}` was considered. Rejected because it
(a) consumes the one reserved kind slot for a single feature, (b) still needs a site-id field
— so it does not avoid the declaration table, it just moves it into the stream, (c) restates
static information (which declaration an instruction refers to) as a runtime operand, and
(d) implies the bit is per-instruction, which §7.1 shows it is not. `kind = 3` and immediate
forms 12–15 therefore stay **reserved** (D7).

### 7.7 Sequencing

`$$` (A3) is **independent of A5**: the declaration table, name map and bitset are function
metadata and frame state, and `DECL`/`INDGET`/`INDPUT` are ordinary opcodes under either
modifier layout. A3 can land before, with, or after the encoding change. The only shared
constraint is the `.qlc` version bump — if A5 lands first, A3 rides on v6; if A3 lands first,
it bumps v5 → v6 itself and A5 takes v7.

---

## 8. `.qlc` v6 file-format changes

| Section | Change |
|---------|--------|
| Header | `kQlcVersion` 5 → **6**; reader accepts v6 only (`src/QLangBytecodeIO.cpp:469`). **6 → 7 in Round 60** (phase 3, §6): the reader is v7-only and the version comparison is unchanged — the constant is the only thing that moved |
| Instruction stream | unchanged framing (flat byte array); operand *decoding* follows §2 |
| Function record | **new**: declaration table (`site id → reg, name, kind`) — the source of the frame name map (§7.2). **Added in M4, not in M2** (decision D4: v6 shipped 2026-11-07 without it; the table will be an optional section inside v6) |
| Constant pool | unchanged framing; expected to shrink (bool/nil/float become immediates, §9) |
| Validation | reader validates every modifier byte against the §2.5 permitted-kind table and rejects reserved kinds/forms, so a malformed stream fails at load, not at execution |

`docs/SERIALIZATION.md` §4 is updated with the v6 layout in the same change.

---

## 9. Constant pool effects (D8)

- **Leaves the pool:** `Bool` literals (forms 9/10), `nil` (form 11), `Float` literals
  (form 8). Round 34/4.4 already removed `Int` literals via `MOVI`; this removes the rest of
  the scalar classes.
- **Stays in the pool:** strings, blobs, and closure/function constants (`makefunc`/`makegen`
  are pool-index-only, §2.5).
- Pool uniqueness/dedup (`13-bytecodes.md` `## constant pool`) is unchanged.
- Net effect on the common script: the pool holds only strings and closure consts, so most
  `movc`-style loads become 3-byte `mov` + immediate, and pool indices stay under 256 far more
  often (1-byte addressing).

---

## 10. Impact assessment

| Area | File(s) | Change | Risk |
|------|---------|--------|------|
| Encoding tables | `include/qlang/QLangBytecode.h`, `src/QLangBytecode.cpp` | 2/2/4 field decode; kind/form tables; permitted-kind table; `DecodedInstruction` gains the right-operand kind; `escapedInstructionSize`/`instructionSize`/`encodeInstruction` rewritten; 7 new identities | **High** — the size function must agree with the encoder and the decoder byte-for-byte, or `pc` desynchronises |
| VM | `src/QLangVM.cpp`, `include/qlang/QLangVM.h` | Decode loop reads the new modifier; `*f` compares + `TST`; flag predicates unchanged (`flagsSatisfied` is already correct); zero-byte immediates; frame declaration bitset + `DECL`; `INDGET`/`INDPUT` handlers | Medium — four exec loops all dispatch; flag semantics must not change |
| Compiler | `src/QLangCompiler.cpp` | Emit unified `mov`/`add`/`sub`/`cmp` kinds; immediate selection incl. float/bool/nil; `eqf`/`ltf`/`gtf`/`ltef`/`gtef` + flag jumps for comparison conditions; declaration-site ids + `DECL` emission + dominance analysis; pool stops receiving bool/nil/float | Medium–high — backpatching is unaffected (jump class unchanged); the fusion sites change |
| Codec | `src/QLangBytecodeIO.cpp`, `include/qlang/QLangBytecodeIO.h` | v6; declaration-table section; load-time kind validation | Medium |
| Disassembler | `examples/qlang_run.cpp` | **Must** be updated in the same change (AGENTS.md / `13-bytecodes.md` `## disassembler`): `pseudoText()`, `operandText()`, `operandMask()`, `disassemble()` for the new fields, the 7 opcodes, the aliases, and the zero-byte immediates | High if forgotten — silently mis-decodes or reads out of bounds |
| Frame / `$$` | `include/qlang/QLangVM.h`, `src/QLangVM.cpp` | Name map + bitset on `Frame`; six-stage pipeline | Medium (A3) |
| Tests | `tests/unit/` | Encoder/decoder/size round-trip per form incl. zero-byte immediates; >256 register/const stress; flag-compare × jump matrix; `DECL`/bitset branch cases; `.qlc` v6 round-trip; reserved-kind rejection | — |
| Docs | `docs/spec/13-bytecodes.md`, `docs/SERIALIZATION.md`, `docs/compliance/*`, this plan | Spec finalization (done in this round), v6 layout, hashes after tests | — |

Not affected: lexer, parser (beyond the optional `$$"literal"` form), jump backpatching,
register/constant pooling model, value semantics, `ObjectOps`.

---

## 11. Verification gate

```bash
cmake -B build && cmake --build build
ctest --test-dir build
build/qlang_run --asm-level 5 --profile tests/scripts/hello.ql
build/qlang_run --output /tmp/t.qlc tests/scripts/hello.ql   # then round-trip /tmp/t.qlc
```

1. **Full corpus:** every `tests/scripts/*.ql` compiles, runs, and produces byte-identical
   stdout to the pre-change build.
2. **Round-trip:** compile → write `.qlc` (v6, **v7 since Round 60**) → read → execute → stdout
   matches direct run. Committed as `codec_check`'s `qlc-exec` suite in Round 60: 23 scripts ×
   (image declares the version / image survives the round-trip / executes with identical stdout).
3. **Size/decode agreement:** a generated stream exercising every (kind, form) pair decodes
   with `instructionSize()` == the encoder's emitted length at every `pc`.
4. **Zero-byte immediates:** `true`/`false`/`nil` cost 3 bytes and add **no** pool entry
   (assert on the pool contents).
5. **Flag matrix:** every compare (`cmp`, `eqf`, `ltf`, `gtf`, `ltef`, `gtef`, `tst`) × every
   jump (`je`, `jne`, `jg`, `jge`, `jl`, `jle`, and the `ja`/`jae`/`jb`/`jbe` aliases) ×
   less/equal/greater, including the "no bit set" state.
6. **Deprecated spellings:** a hand-built v6 stream containing `movi`/`movc`/`addi`/`subi`/
   `cmpi`/`jmpfalse`/`jmptrue` still executes (coexistence, §3). **Inverted in Round 60** — the
   guarantee is now the opposite: a v6 image is rejected by the version field and the removed
   identities are never produced or decoded. The `codec_check` legacy suite that proved the old
   form was **deleted**, not kept as a must-fail test (it would exercise identities the decoder
   no longer defines); the replacement gate is the producer grep + the corpus disassembly scan
   in `phase3-opcode-retirement-plan.md` §6.
7. **Reserved slots:** a stream with `kind = 3` or immediate form 12–15 is rejected at load.
8. **`$$`:** branch-dependent declaration (`not declared` after the untaken branch, success
   after the taken one), loop re-declaration, `const` read-only, function/global rejection,
   the six error messages, and the `$$"literal"` dominance fast path.
9. **Disassembly:** `--asm-level 0–5` well-formed for every new form.
10. **Pool shrink:** a corpus diff showing bool/nil/float entries gone from the pool.

---

## 12. Open implementation questions

1. **`DecodedInstruction` shape** — it currently carries `a, b, c, imm`; the kind field needs
   to reach the VM (register vs pool vs immediate right operand). Add a `rightKind` field, or
   resolve the operand to a value at decode time? **Default:** add `rightKind` and let each
   dispatch site read accordingly (keeps decode cheap). **Settled by M1:** `rightKind` added,
   plus a typed immediate carrier on `DecodedInstruction` (`immPayload` tag with `imm` /
   `immUnsigned` / `immFloat`) — the v5 `imm` alone cannot hold a `u64` or a `double`. Decode
   stays cheap: no operand is resolved to a value at decode time.
2. **`conv`/`cast` type-id operand** — today `c` is a 1-byte type id. It stays a plain trailing
   byte (not a kind-selected operand). Confirm no kind field is consulted for it.
   **Default:** confirmed, type id is an ordinary third operand.
3. **Bitset representation** — `std::vector<bool>` vs packed `uint64_t` words vs
   `boost`-free hand-rolled bitmap; per-frame allocation cost for small frames.
   **Default:** packed `uint64_t` words with a small-size inline buffer, allocated lazily only
   for functions that actually contain `$$` uses.
4. **Declaration table for frames without `$$`** — emit it anyway (uniformity) or omit it
   (smaller files)? **Default:** omit when the function contains no `$$` use; the map is only
   needed by the indirect path.
5. **`DECL` dominance analysis** — full CFG dominance or the conservative "same block as every
   use" rule? **Default:** conservative same-block rule in v6; real dominance later if the byte
   saving matters.
6. **`tst` emission policy** — emit `tst` + `je`/`jne` for bare truthiness (uniform, +1 byte)
   or keep `jmpfalse`/`jmptrue` there until they are removed? **Default:** keep
   `jmpfalse`/`jmptrue` for bare truthiness in v6 (§5.2). **Superseded by Phase 3 D1
   (Round 60):** removal *is* the point of that step, so the default was flipped — bare
   truthiness lowers to `tst` + `je`/`jne` at all 15 sites and the pair is gone. The +1 byte
   became +1 *instruction*, which is what `perf_stress` measures (+8.7 % in the A/B run, §5.4).

---

## 13. Summary

| Aspect | v5 (shipped) | v6 (this plan) |
|--------|--------------|----------------|
| Modifier byte | nibbles: left form (0–3) / right form (4–7) | **2/2/4**: left form (0–1) / right **kind** (2–3) / right **form** (4–7) |
| Right operand kind | fixed per opcode identity | declared per instruction (`imm` / `reg` / `const`); `3` reserved |
| `i`/`c` suffixes | separate opcodes | folded into `mov`/`add`/`sub`/`cmp`; identities deprecated, **removed in v7 (Round 60)** |
| Immediates | signed `Int` 1/2/4 B | unsigned/signed 1/2/4/8 B + `float` + `bool` + `nil` (0-byte forms) |
| Constant pool | scalars + strings + closures | strings + blobs + closure consts only |
| Comparison in control position | `eq`/`lt`/… into a temp + `jmpfalse` | `eqf`/`ltf`/`gtf`/`ltef`/`gtef` + flag jump, **no temporary** |
| Truthiness branch | `jmpfalse`/`jmptrue` | `tst` + `je`/`jne` — emitted since Round 60, pair removed |
| Unsigned jumps | n/a | `ja`/`jae`/`jb`/`jbe` assembler aliases (no encoding) |
| `$$` declaration bit | unencodable (Q6) | per-frame bitset + `DECL` marker + function declaration table — **no encoding space used** |
| Opcode count | 73 / 128 | 79 after M3 (`DECL` still pending) → **72 / 128** after the Round 60 compaction; 73 when M4 appends `DECL` |
| `.qlc` | v5 | **v6** → **v7** (Round 60: same layout, compacted identities; + declaration table in the function record when M4 lands) |
