# Plan: Missing Features — Overview

Created 2026-11-04 (post-Round 38 audit). **Re-audited 2026-11-05 (post-Round
52)** — Rounds 39–52 landed A1 `nop`, A2 `?`, C2 enum injection, C6 signals and
the whole of A4 `deepcopy`/value semantics; those items moved to §D, the phases
were renumbered, and C8 (module search paths) was added. Overview/roadmap for
every spec'd-but-unimplemented item tracked in `docs/compliance/status.md`
(Open Items + Not Yet Implemented). Unresolved decisions are collected in
[Open questions](#open-questions) (Q1–Q38), each with a default to adopt.
This is an **overview**: each phase gets a detailed implementation plan before
work starts (precedent: the movi/addi/subi plans in `docs/plans/archive/`).

**Updated 2026-11-06 — warnings are now owned by strict typing (C1).** The
spec edits committed in `1db387d` delete `11-external-types.md` `# warning` (the
toggleable-warnings section B1 was anchored to, incl. "all warnings defered
until further notice") and add `16-strict_typing.md`
`## redeclared variables within a scope`: a duplicate `local`/`let` in one scope
is silent at level 0, `warning: duplicated variable "a" declaration within
scope` at level 1, `error: …` at level 2. B1/B2 are therefore folded into C1
(Phase 3 item 2); the B numbering is kept for `status.md` cross-references.
`05-operators.md` `## operator $$` still calls the duplicate an unconditional
"compile warning" — spec drift, **Q28**.

**Re-audited again 2026-11-06 (post-`1db387d`).** Three changes since the
Round-52 audit: (1) **C3 `hasMethod`/`callMethod` is implemented** (`c4ee131`,
committed mid-audit with no round of its own) — moved to §D, with one defect to
fix before it can be called verified; (2) `13-bytecodes.md` gained `## encoding extended` /
`## opCode scope` / `### extended OpCode eq, lt, gt, lte, gte` (committed in
`1db387d`) — a new instruction-encoding proposal plus opcode families that
exist nowhere in `include/` or `src/` and were tracked by neither this plan nor
`status.md` → new inventory item **A5**, and it invalidates **Q6**'s premise;
(3) the spec heading is `## redeclared variables within a scope` (plural) — the
anchor wording used throughout this plan is corrected.

**A5 spec finalized 2026-11-06; detailed plan written.** The draft sections
listed in the inventory row are now normative (`13-bytecodes.md` `## encoding
extended`, `## opCode scope`, `### bytecode encoding`,
`### extended OpCode eq, lt, gt, lte, gte`) and the detailed implementation plan
exists: **`docs/plans/encoding-v6-and-indirect-plan.md`**. It settles the
modifier byte as `bit 0-1` left register / `bit 2-3` right-operand **kind** /
`bit 4-7` right-operand **form**, adds the full immediate set (signed, unsigned,
`float`, `bool`, `nil`), reconciles `movi`/`movc`/`addi`/`subi`/`cmpi` into the
unsuffixed opcodes, specifies `ja`/`jae`/`jb`/`jbe` as **aliases** (not new
identities), rejects `addc`/`subc`, and closes **Q6** (§7). A5 is now
*specified and planned*, still unimplemented.

## Inventory

### A. Language features (fully specified, unimplemented)

| # | Item | Spec anchor | Scope |
|---|------|-------------|-------|
| A3 | `$$` indirect access operator | `05-operators.md` `## operator $$` | large |
| A5 | Extended instruction encoding + opcode families | `13-bytecodes.md` `## encoding extended`, `## opCode scope`, `### bytecode encoding`, `### extended OpCode eq, lt, gt, lte, gte`; plan `encoding-v6-and-indirect-plan.md` | **new in the 2026-11-06 audit** — spec **finalized**, plan written; large, breaking (`.qlc` v5 → v6). **M1 (additive v6 encoding layer) landed 2026-11-06**; pipeline still emits v5 |

A1 `nop` (Round 39), A2 `?` (Round 39) and A4 `deepcopy` + share-by-default
value semantics (Rounds 42–45, plan closed Round 45) are **implemented** — see
§D. The numbering is kept so the round records and `status.md` cross-references
stay readable.

**A5** was specified but unimplemented and untracked until the 2026-11-06 audit
(the same class of gap Round 50 flagged for `07-api.md`'s module-search
sections). The draft rewrites the escaped modifier byte — `bit 0-1` left operand
(always a register), `bit 2-3` right-operand **kind** (immediate / register /
const pool / `value 3 reserved`), `bit 4-7` right-operand form — where the
implementation still uses low nibble = left form, high nibble = right form
(`escapedInstructionSize`, `src/QLangBytecode.cpp:121-172`); it named opcode
families `addc`/`subc`, conditional jumps `ja`/`jb`/`jae`/`jbe` and extended
comparisons `eq`/`lt`/`gt`/`lte`/`gte` that set the "shared registers" flags
(only `EQ`/`LT`/`GT` exist today; no `ADDC`/`SUBC`/`JA`/`JB`/`JAE`/`JBE`/`LTE`/
`GTE`), and it renumbered the immediate forms (`9`/`10` = bool false/true, `11` =
nil, `12`–`15` reserved). Adopting it changes the on-disk encoding → `.qlc`
version bump.

Those ambiguities are resolved (`encoding-v6-and-indirect-plan.md`): the
modifier byte is **2/2/4** with the base form pinned to `e = 0x04` (register
right) / `0x40` (immediate right) / `0x08` (const right); the jump class keeps
the v5 width selector instead of the 2/2/4 split; a per-opcode **permitted-kind**
table makes any other combination a decode error; `addc`/`subc` are **rejected**
(`mov` with kind 1 already covers them); `ja`/`jae`/`jb`/`jbe` are **assembler
aliases** of `jg`/`jge`/`jl`/`jle` (the flags carry no signedness, so distinct
identities would be dead weight); the extended comparisons ship as
`eqf`/`ltf`/`gtf`/`ltef`/`gtef` (7 new opcodes, 73 → 80 of 128) and set the
existing **one-hot** flags — the spec's `lte`/`gte` rows are *capability masks*,
not two-bit states; `jmpfalse`/`jmptrue` are phased out behind `TST`; and the
immediates (`bool`/`nil`/`float` included) free constant-pool slots. **Q6** is
closed by §7 of the plan: the declaration bit is **not** encoded.

### B. Foundations (now owned by C1 — strict typing)

| # | Item | Spec anchor | Blocks |
|---|------|-------------|--------|
| B1 | Warning emission infrastructure | *was* `11-external-types.md` `# warning` — **section deleted (`1db387d`, 2026-11-06)**; no standalone anchor remains | B2 (as a C1 dependency: level 1 must be able to warn). **✓ implemented (C1 M1, 2026-11-14):** `QLangCompiler::warn` → `QLangContext::output(OUT_WARNING, …)`; the strict level is the sole control |
| B2 | Duplicate-declaration diagnostic (level-dependent) | `16-strict_typing.md` `## redeclared variables within a scope` (new), `03-declarations.md` dup-decl example (comment pinned to the level table 2026-11-14), `05-operators.md` `## operator $$` (declaration-state rules — **Q28** wording aligned 2026-11-14) | — . **✓ implemented (C1 M1, 2026-11-14):** `QLangCompiler::checkDupDecl`, level 0/1/2 matrix, `strict_typing` suite |

The former dependency `B1 ──> B2` (warnings must be toggleable before the
dup-decl check can re-land) is superseded: the strict-typing level is the
control, so both land inside C1 (Phase 3 item 2). Whether an independent
warning toggle still exists is **Q27**.

### C. API surface (spec'd "needed", undefined or partial)

| # | Item | Spec anchor | Notes |
|---|------|-------------|-------|
| C1 | Strict typing (levels 0/1/2) | `16-strict_typing.md` | **in progress — M0+M1 landed 2026-11-14** (spec finalized; context level api + duplicate-declaration matrix + `qlcc --strict` shipped, **closing B1/B2**) — plan `docs/plans/archive/strict-typing-plan.md`; M2–M5 (grammar, signatures, VM checks, `.qlc` v10) pending |
| C5 | Inheritance (`ObjectOps::base`) | `10-classes.md` v1 restrictions, `EXTERNAL_TYPES.md` §10.3 | reserved, returns null; base-chain walk deferred to v2 |
| C7 | Enum-name string form for enum-valued properties | `EXTERNAL_TYPES.md` §10.5 | no dispatch point sees both field value and type context |
| C8 | Module search paths + external search callback | `07-api.md` `## module search paths`, `## external search callback` | **✓ implemented (C8a Round 67, C8b Round 69, 2026-11-10)** — `moduleSearchPaths`/`setModuleSearchPaths`/`addModuleSearchPath`/`setModuleSearchCallback`/`moduleSearchCallback`/`findModule` on `QLangContext`, `import` resolves through it, `qlang_run --module-path` / `--no-module-search`, `--import` deprecated; a `[name].ql` hit is compiled into an image and taken in (Q36–Q38 answered); front end for `archive/external-modules-plan-v4.md` |

C2 enum injection (Round 41), C3 `hasMethod`/`callMethod` (`c4ee131`,
2026-11-06), C4 RTTI handoff validation (Phase 3) and C6 signals (Round 40) are
**implemented** — see §D.

### D. Resolved / no work needed

- **Table key order** — `__str__` renders lexicographic (`std::map`); the spec
  explicitly says "table key order is implementation defined (depends on the
  underlying container)" (`11-external-types.md`). Not a gap.
- **Instance-call-of-static-method** — spec-silent; documented deviation
  (hidden-anchor design, `docs/compliance/classes.md`).
- **`|||` logical-or** — intentional extension, deliberately not in the spec.
- **A1 `nop` opcode** — **implemented (Round 39, 2026-11-04, `7b56ac8`)**.
  `Opcode::Nop` appended last (identity 68), `Bytecode::name()` → `"nop"`,
  zero-operand, no-op in the `runFrames()` dispatch, codec round-trip; the
  compiler never emits it (spec: "takes no action"). 12 `codec_check` cases.
- **A2 `?` ternary operator** — **implemented (Round 39, 2026-11-04,
  `7b56ac8`)**. `TokenType::Question`; `parseTernary` at the lowest precedence,
  right-associative; expression or brace-block branches (a block's `return`
  yields the value, lowered to a zero-arg closure call). No new opcode. 16
  `spec_check2` cases.
- **A4 `deepcopy` + share-by-default value semantics** — **plan complete**
  (Rounds 42–45, 2026-11-05; `docs/plans/archive/deepcopy-plan.md`). Phase 1
  `deepcopy` builtin + `DEEPCOPY` (Round 42, `5de7d29`), Phase 2 pointer-backed
  `array`/`set`/`blob` so assignment and call arguments share (Round 43,
  `fede1a4`, breaking — migration note `docs/compliance/migration-value-semantics.md`),
  Phase 3 `__deepcopy__` override (Round 44), Phase 4 tracking closure (Round 45).
  Deferred by decision, not by omission: `shallowcopy` (open decision #5) and a
  `copy` parameter specifier (#6).
- **Table subscript `t[key]`** — **implemented (Round 51, 2026-11-05,
  `2fc570b`)**. Found as a spec gap in Round 50 (`docs/spec/03-declarations.md`
  `t["age"] = 33` threw `cannot subscript non-array value`); fixed with
  `INDEXGET`/`INDEXPUT` (identities 71/72), which dispatch on the runtime base
  type and route tables through `ObjectOps`, making `t["age"]` and `t.age` the
  same operation. 17 `spec_check2` + 19 `codec_check` cases +
  `tests/scripts/table_subscript.ql`.
- **C4 RTTI validation on external handoff** — implemented in Phase 3
  (`EXTERNAL_TYPES.md` §10.2): the typed handoff helpers
  `qval_external_owned<T>`/`qval_external_borrowed<T>` record `typeid(T)`
  and validate the resolved pointer with a `dynamic_cast` through the
  `QLObject<T>` base; a mismatch is a clean `type error: external type
  mismatch: …` at first use, not UB. Untyped `qval_external(type, …)`
  factories remain for hosts that cannot name the concrete type (no check).
  `phase3_api` tests cover the mismatch path.
- **C2 enum injection** — implemented in Round 41 (2026-11-05).
  `QLangContext::registerEnum(name, members, baseType = Int)` validates
  (identifier/reserved-word names, non-empty members, no duplicates, numeric
  base type, name free), seeds the members as the hidden const table
  `__enum_<Name>`, and registers the name with the compiler via the new
  `QLangCompiler::addExternalEnums` — so `E::M`/`E.M` fold, `E` resolves to
  the table, a script `enum E` collides ("cannot redeclare enum"), and
  `remove`/`clear` drop the enum + hidden var. The API carries a base type
  (the draft's "base type is metadata only" was wrong: `compileEnum` types
  members per the declared base and `m_enums` stores typed `QLangValue`s).
  26 `test_external` cases. See the detailed section below.
- **C3 `hasMethod`/`callMethod` context wrappers** — **implemented
  (`c4ee131`, 2026-11-06 — committed with no round of its own; recorded in
  `docs/compliance/history.md` Round 53)**.
  `QLangContext::hasMethod(obj, name)` resolves the member through
  `getObjectOps` → `ops->getField` and reports whether it is callable;
  `QLangContext::callMethod(obj, name, args)` drains finalizers (the
  `emitSignal`/`callClosure` safe point) and dispatches `ops->callMethod`,
  which supplies self / the statics anchor as the script-visible first
  argument. Host-only (**Q19** resolved as the default: the VM path stays
  `TABGET` → `BoundMethod` → `CALLCLOS`); unknown method or a value without
  `ObjectOps` throws `QLangError` (**Q20** resolved as the default, mirroring
  `emitSignal`). The `hasSignal` comment now reads "mirrors hasMethod above,
  which this pair predates". `test_external` §19–20 (49 → 66 checks).
  **Not yet verified** — one defect blocks the flip: `hasMethod` accepts
  `Function` **or** `BoundMethod`, but `table_ops::callMethod`
  (`src/QLangObjects.cpp:188`) and `QLangContext::callClosure` both require
  `Type::Function`, so a table field holding a `BoundMethod` passes the guard
  and then throws — breaking the `docs/EVENT_HANDLERS.md` §6 guard contract.
  Fix: per-kind callable test (table/object → `Function` only; external →
  `findMember(kind == 2)` instead of `getField` + a swallowed `QLangError`),
  plus a `BoundMethod`-field test. Landed **without a detailed plan**, against
  this document's own convention — recorded as an exception (small API item,
  design fully pinned by Q19/Q20 defaults).
- **C6 signals** — implemented in Round 40 (2026-11-05; spec
  `17-signals.md` finalized the same day). `QL_SIGNAL(name)` meta with the
  signature deduced from a declared, unimplemented member function; `emit`
  reserved word + `EMIT` opcode (appended last, `kOpcodeUsedCount` 69 → 70,
  `.qlc` stays v5); `connect`/`disconnect` builtins (function and
  string-slot forms) with a context-level connection registry (weak sender,
  strong handler, auto-disconnect at the finalizer drain); synchronous
  delivery in connection order, re-entrancy contained by the step budget;
  C++ `ctx.hasSignal()`/`ctx.emitSignal()`. 18 checks in the dedicated
  `signal_test` suite.

## Open questions

Decisions that are **not settled by the spec** and block (or shape) the phase that
owns them. Each carries a **default** — the choice to adopt if nobody objects, so
an item can start without a design round-trip. Resolving one means editing the
owning spec module (then re-hashing it) or recording the decision in
`docs/compliance/history.md`; this section is the single index.

### B1 / B2 — warning infrastructure (owned by C1 since 2026-11-06)

The `# warning` section that anchored B1 is deleted; the only warning the spec
now names is the level-1 duplicate-declaration diagnostic in
`16-strict_typing.md`. These questions survive as C1 design inputs.

**✓ Resolved as defaulted and implemented (C1 M1, 2026-11-14).** Q1: the
strict level is the only control. Q2: fresh contexts sit at level 0 (survives
`clear()`). Q3: **one deviation** — the destination is
`QLangContext::output(OUT_WARNING, …)` only, no `ctx.warnings()` accumulation
and no `trace()` echo (strict-typing plan Q27: the sink is the warning
infrastructure; a host collects through the output callback). Q4: compile-only.
Q5: same-scope via `declaredInCurrentScope`; parameters exempt, shadowing
silent.

- **Q1 control granularity** — the deleted `# warning` section said only
  "compiler and interpreter warning can be toggled off"; the surviving control
  is the strict-typing level (0 silent / 1 warn / 2 error). One global switch,
  per-warning ids/categories, or none beyond the level? See **Q27**.
  **Default:** the level is the only control; categories only once a second
  warning class exists.
- **Q2 default state** — what does a fresh context get? **Default:** level 0
  (the spec: "type declarations are optional. no enforcement"), so the
  duplicate-declaration diagnostic is off until the host raises the level.
- **Q3 destination** — AGENTS.md routes diagnostics through the gated `trace()`
  helper, but a warning is a host-visible diagnostic, not a debug trace, and
  `trace()` writes to stderr. `trace()` only, a host-readable list
  (`ctx.warnings()`), or both? **Default:** both — collect on the context and
  echo through `trace()`; never throw.
- **Q4 runtime warnings** — the deleted section promised "interpreter warning";
  the level table's only warning is compile-time (a re-declaration), and the
  level 1/2 type mismatches are exceptions, not warnings. **Default:** v1 is
  compile-only; runtime warnings return if a future spec names one.
- **Q5 what counts as a duplicate (B2)** — the level table says "within a
  scope"; the reverted Round 25 check targeted an explicit `local`/`let`
  naming a name already declared *in the same scope*; block scopes exist
  since Round 10 (`m_blockScopes`), so does a nested-block re-declaration
  warn? And is the `$$` spec's "a later explicit `local`/`let` for the same
  name is a compile warning" the same check or a second one? **Default:**
  same-scope only; nested shadowing stays silent; one shared check (the `$$`
  wording aligns via **Q28**).

### A3 — `$$` indirect operator

- **Q6 where the declaration bit lives** — **closed 2026-11-06**
  (`encoding-v6-and-indirect-plan.md` §7). Spec note 2 put it "on the existing
  store/init opcode", but the declaration bit is per-*event* state (**Q7**), not
  per-instruction, so no encoding bit can express it: one register can carry
  several declaration sites, and the same store opcode executes on both the
  declaring and the re-assigning path. The A5 finalization did **not** change
  this — v6 does name spare space (`bit 2-3 = 3`, immediate forms `12`–`15`),
  but that space is deliberately left **reserved** rather than spent on it.
  **Adopted:** compile-time declaration-site ids + a per-function **declaration
  table** (`site → {reg, name, kind}`) in the `.qlc` v6 function record, a
  per-frame **declaration bitset** cleared at frame creation, and a new
  `DECL siteId` marker opcode executed where the declaration executes. A3 is
  therefore **independent of A5's encoding layout** (it only shares the `.qlc` v6
  bump); `INDGET`/`INDPUT` are ordinary register-class ops.
- **Q7 declaration-bit granularity** — per register or per declaration site?
  Registers are reused across branches and loop iterations, and the spec's
  branch example (`$$a = 1` legal only if `cond` ran) is per-*event* state.
  **Default:** a per-frame bitset indexed by declaration-site id, cleared when
  the frame is created and set when the declaration instruction executes.
- **Q8 frame name map vs block scope** — spec note 1 says the map covers "every
  name directly declared in the frame" (frame-wide), while direct references
  have resolved block-scoped since Round 10 and the spec's "no block scope"
  clause was removed (`dde51e4`). Must the map mirror the scope stack?
  **Default:** frame-wide map + declaration bit, as spec note 1 reads; the
  spec's existing "asymmetry note" already documents that direct and indirect
  resolution differ.
- **Q9 opcode names** — the spec says `OP_INDIRECT_READ`/`OP_INDIRECT_WRITE`;
  the reserved placeholders are `Opcode::INDGET`/`Opcode::INDPUT` with mnemonics
  `indget`/`indput` (`include/qlang/QLangBytecode.h:123-124`,
  `src/QLangBytecode.cpp:796-797`). **Default:** keep the implemented names and
  align `05-operators.md` (spec edit → re-hash).
- **Q10 operand count** — the spec gives each opcode two operands
  (`nameReg → dstReg`, `nameReg, srcReg`), but the placeholders sit in the
  3-operand default class (`src/QLangBytecode.cpp:111-113`). **Default:** move
  them to the 2-operand class — they were never emitted, so there is no
  compatibility cost — and update the `qlang_run` pseudo-text.
- **Q11 does the literal form ship?** — spec note 4 says the compile-time fast
  path applies "if a literal form `$$"name"` is ever supported", i.e. the syntax
  is not committed; `$$` is otherwise documented as taking a string *variable*.
  **Default:** v1 supports only the dynamic form (`$$a`); the fast path is left
  out, so no parser change is needed.

### C1 — strict typing (`16-strict_typing.md` is a draft)

- **Q12 the coercion table contradicts the examples** — `## rules` allows
  same-group coercion "of smaller storage size" (`:59-60`), yet the examples
  reject `long` for an `int` parameter ("storage for age is too large for int",
  `:33`) **and** `byte` for an `int` ("age is a byte not an int", `:37-38`).
  Both cannot hold. **Default:** the examples win — exact type match — and the
  coercion table is rewritten to say so.
- **Q13 `nil` against a declared type** — "nil type is implicit and doesn't need
  to be declared" is about *declarations*, not about passing `nil` to `age:int`.
  **Default:** `nil` is accepted for any declared type.
- **Q14 level 2 "throw an error if type is not declared"** — compile-time or
  runtime? The signature is statically visible. **Default:** compile error at
  the definition; level 1/2 *mismatches* stay runtime (the examples show runtime
  exceptions at call/return sites).
- **Q15 who enforces** — native functions and `ctx.call()` entry points bypass
  the compiler. Are declared parameter types checked in the VM at every call
  boundary, or only where the compiler sees the call? **Default:** at the call
  boundary in the VM, so native and host-invoked calls are covered too.
- **Q16 return checks** — same rules as parameters? The spec's
  `sum_int2int(...) -> int` with no `return` yields "exception: function must
  return an int" — is that a compile error or a runtime error on returning
  `nil`? **Default:** same rules as parameters; runtime error when the frame
  returns without a value.
- **Q17 interaction with `cast<>`** — does an explicit cast satisfy a declared
  parameter type (i.e. is it the only sanctioned conversion)? **Default:** yes —
  `cast<>` yields the target type, so the check sees an exact match; without a
  cast nothing converts.
- **Q18 class / external declared types** — exact declared type only, or accept
  a derived value once `ObjectOps::base` (C5) exists? **Default:** exact in v1;
  base-chain acceptance lands with C5.
- **Q27 does an independent warning toggle still exist?** — the deleted
  `11-external-types.md` `# warning` section specified a toggle independent of
  the strict-typing level; the level table now gates the only spec'd warning by
  itself. Keep a separate `setWarningsEnabled(bool)` on top of the level, or is
  the level the sole control? **Default:** the level is the sole control — no
  independent toggle; re-introduce one only when a warning class exists outside
  the strict-typing levels (cf. **Q1**).
- **Q28 `$$` spec wording drift** — **RESOLVED 2026-11-14**: the sentence is
  aligned to `16-strict_typing.md` `## redeclared variables within a scope`
  (level-dependent diagnostic, one shared check per **Q5**); spec edited,
  re-hashed. — `05-operators.md` `## operator $$`
  ("declaration state") still says "a later explicit `local`/`let` for the same
  name is a compile warning", i.e. unconditional, while the level table makes
  it silent/warning/error by level. Spec edit aligning the sentence to
  `16-strict_typing.md` `## redeclared variables within a scope` (then re-hash).
  **Default:** align the wording; one shared check (**Q5**).

### C3 — `hasMethod` / `callMethod` (resolved 2026-11-06)

Both defaults were adopted as-is when the wrappers landed — recorded here rather
than deleted, since the landing cites them.

- **Q19 who may call `ops->callMethod`** — **resolved: host-only.**
  `QLangContext::callMethod` is the only caller; the VM path is unchanged
  (`TABGET` → `BoundMethod` → `CALLCLOS`).
- **Q20 unknown-method failure** — **resolved: throw `QLangError`**, mirroring
  `emitSignal`. A value without `ObjectOps` and a non-callable/missing member
  each throw; errors raised inside the method propagate.
- **New open detail (not a spec question)** — what "callable" means per kind:
  today `hasMethod` says yes to `Function` **or** `BoundMethod`, but the table
  ops only invoke `Function` (see §D, C3). The guard and the dispatch must agree
  before this item can be flipped to verified in `status.md`.

### C5 — inheritance (`ObjectOps::base`)

- **Q21 the whole design** — base-clause syntax, field/method resolution order
  and shadowing, constructor chaining, single vs multiple inheritance, and
  whether `base()` returns the immediate base or a chain head
  (`include/qlang/QLangObjects.h:60`, reserved, returns null;
  `EXTERNAL_TYPES.md` §10.3 defers the base-chain walk to v2). **Default:**
  single inheritance, derived-wins lookup, explicit base-constructor call,
  `base()` = the immediate base ops.

### C7 — enum-name string form

- **Q22 where the enum type context comes from** — the known blocker is that no
  dispatch point sees both the field value and its type
  (`EXTERNAL_TYPES.md` §10.5: `str(e.options)` sees a bare `Int`). Carry the
  enum table on the property binding, or tag the value? **Default:** property
  binding metadata — value tags are shared with plain `Int`.

### C8 — module search paths

**Decided 2026-11-09 (Q23), spec re-worded in `07-api.md` the same day.**
**✓ Implemented in two rounds: C8a Round 67, C8b Round 69 (2026-11-10).** C8a grew
the path list, the callback and `findModule` on `QLangContext`, `resolveImport`
resolving through it, and `qlang_run --module-path` / `--no-module-search` (`--import`
deprecated). C8b delivers the half C8a left refused: a `[name].ql` hit is compiled into
an image and taken in by the same path, so the spec's source search works with no host
help — `qlang_run app.ql` finds `lib.ql` beside `app.ql`. Guard `module_search` (81 →
**103** checks), script regression `script_ql_module` (+ its fixture `ql_module_dep.ql`),
demo `examples/module_search.cpp`.

- **Q23 does C8 include loading `.ql` source?** **DECIDED: yes — C8 owns
  compile-from-source, landed in two rounds.** The spec's search finds
  `[modulename].ql` (source) while the callback returns a *loaded* module, so
  the engine must turn source into an image somewhere; the alternative is
  contradicting the spec by searching only for `.qlc`. The nested compile is
  cheap because the machinery exists: `QLangContext::parse()` is public,
  `QLangCompiler` + `setLibraryMode`/`setModuleName` produce a `CompiledProgram`,
  `writeQlc` takes exactly what `nativeFuncIds()` hands back, and
  `loadProgram(istream)` re-resolves native **names** and registers the result —
  so a discovered `.ql` round-trips through the serializer into the existing
  registry with no new type and no `.qlc` version change. The cost is the three
  things *around* it: reentrancy (`compileImport` drives a second compiler while
  the first is mid-`compile()`), cycles (the existing `checkImportCycles` walks
  *loaded* images, so an A→B→A nested cycle recurses before any table exists —
  an in-flight set is needed), and error provenance (a compile error inside a
  discovered `.ql` must report that file, `12-errors-trace.md`).
  **C8a** = path list + callback + resolution order, resolving `.qlc` through
  `loadProgramFile` — no compiler change, and it retires `qlang_run --import`.
  **C8b** = `.ql` discovery + the nested compile. **✓ Round 69**: the compile runs on a
  private scratch context whose imports are answered by the parent (one resolution, one
  cycle chain), the image round-trips `writeQlc` → `loadProgram`, and every diagnostic
  names the discovered file.
- **Q24 precedence** — **RESOLVED by the spec (2026-11-09):** native module →
  already-loaded registry entry → search paths in order; the callback sits
  between the registry and the paths, so it can supply or shadow a file module
  but not a registered C++ module. Default path list = pwd at construction.
- **Q25 callback failure and payload** — **RESOLVED by the spec:** the
  prototype is `shared_ptr<LoadedModule> loadModuleCallback(const std::string
  &modulename, std::string &error)` (the earlier `string searchCallback(string)`
  is superseded); `nullptr` + empty `error` = not found (continue to the paths),
  `nullptr` + non-empty `error` = found but unusable (stop and report). Shared,
  not owned-raw, matching what the registry stores.
- **Q26 caching and cycles** — **written into the spec:** resolved once per
  context, cached under the resolved module's own name; an import cycle is an
  error.
- **Q36 filename vs declared identity** — `foo.ql` declaring
  `export const MODULE_NAME = "bar"` resolves `import bar` to a module
  registered as `bar`, not `foo`. Reject the mismatch, or accept and key on
  `MODULE_NAME`? **Answered (C8b, Round 69): accept, key on `MODULE_NAME`** — the
  same rule `.qlc` discovery has had since C8a, so `import foo` binds the module
  named `bar` through the alias the compiler already records. The default's
  *warn on mismatch* half is not implemented: nothing warns for a `.qlc` whose
  name differs either, and a name that resolves is not a problem to report.
- **Q37 a discovered `.ql` with no `MODULE_NAME`** — a library compile requires
  it, a plain script has none. Mandatory for a discovered file, or fall back to
  the filename stem? **Answered (C8b, Round 69): fall back to the stem** — and to
  make that possible the library-mode rule was re-worded from "must declare
  `MODULE_NAME`" to "must be namable" (`setModuleName` above), which is also what
  `qlang_run --import lib.ql` has always wanted. A discovered file with no name of
  its own is compiled under the stem of the file that was searched for.
- **Q38 what the callback sees** — the requested name only, or the search paths
  too (so a Qt resource can mirror the directory list)? **Answered (C8b, Round 69):
  name only.** The nested compile answers its own imports by calling the parent's
  `findModule`, so a callback sees the same name the entry did and the paths stay
  the engine's business; a host that wants to mirror a directory list already owns
  the list it set.

Scope note: C8 is *discovery*. `QLangContext::run()` still does not link —
`QLangLinker` is driven by `qlang_run` — so the **runtime module registry** stays
a separate open item in `status.md`.

## Phases

### Phase 1 — Warning infrastructure (folded into C1 — 2026-11-06)

The former items 1 (B1 warning infrastructure) and 2 (B2 duplicate-declaration
warning) no longer stand alone: the `# warning` section they were anchored to
is deleted and the diagnostic is now specified by strict-typing level
(`16-strict_typing.md` `## redeclared variables within a scope`). Both land
inside C1 — see Phase 3 item 2. The B numbering is kept for `status.md`
cross-references.

### Phase 2 — `$$` indirect operator (largest feature)

3. **A3 `$$`** — per the spec's implementation notes:
   - per-frame name map (name → register, binding kind `var`/`const`/
     `function`/`lambda`, declaration bit) — the map lives on `Frame`, so the
     read/write handlers sit in the single `runFrames()` dispatch loop; frame-wide
     vs block-scoped coverage is **Q8**;
   - declaration bit as a flag on existing store/init opcodes (spec note 2) —
     **rejected**: the bit is per-*event* state, so it cannot ride an
     instruction (**Q6** closed, **Q7**). Adopted design: compile-time
     declaration-site ids + a per-function **declaration table**
     (`site → {reg, name, kind}`) + a per-frame **bitset**, set by a new
     `DECL siteId` marker opcode (`encoding-v6-and-indirect-plan.md` §7.2–§7.3);
   - **reuse the reserved `Opcode::INDGET` / `Opcode::INDPUT` placeholders**
     (`QLangBytecode.h:123-124`) rather than adding opcodes: they already carry
     fixed identities inside the defined range (`kOpcodeUsedCount` is 73,
     identities 0..72 — A5 raises it to 80 with `eqf`…`gtef` + `TST` + `DECL`,
     which appends after them and does not move these identities), the
     `indget`/`indput` mnemonics
     (`QLangBytecode.cpp:796-797`), the 3-operand default arity (**Q10** decides
     whether to keep it) and the `qlang_run` pseudo-text — so **no identity is
     appended and `kOpcodeUsedCount` does not change**. Round 51 deliberately
     disambiguated `INDEXGET`/`INDEXPUT` from these placeholders. The spec's
     `OP_INDIRECT_READ`/`OP_INDIRECT_WRITE` names are the same pair; the spec
     wording should be aligned to the implemented mnemonics (**Q9**).
   - six-stage check pipeline with canonical `indirect variable error:`
     messages (string → identifier lexeme → reserved word → declared/visible
     → binding kind → local-only writes);
   - resolution order: writes frame locals only; reads locals +
     `global`-exposed (global const visible everywhere);
   - no variable creation; functions/lambdas never targetable; const
     read-only; branch-dependent declarations can be `not declared`;
   - optional compile-time fast path for `$$"literal"` — the spec calls the
     literal form "if … ever supported", so whether the syntax ships at all is
     open (**Q11**).
 Tests: dedicated suite (precedent: `class_static_test`) covering all six
 pipeline stages, resolution order, declaration state, const/function
 rejection. Open questions: **Q7–Q11** — **Q6 is closed** (2026-11-06), so
 nothing gates the design any more; `DECL` is the only new opcode A3 needs.
 Effort: 1–2 weeks.
4. **A5 extended instruction encoding + opcode families** — **new in the
   2026-11-06 re-audit** (spec committed in `1db387d`, untracked before that);
   spec **finalized** and planned the same day.
   **Full plan: `docs/plans/encoding-v6-and-indirect-plan.md`.**
   Rewrite the escaped modifier byte to the `bit 0-1` left register /
   `bit 2-3` right-operand kind / `bit 4-7` right-operand form layout, ship the
   full immediate set (signed/unsigned 1/2/4/8 B, `float`, `bool`, `nil`),
   reconcile `movi`/`movc`/`addi`/`subi`/`cmpi` into the unsuffixed opcodes, add
   the flag-setting `eqf`/`ltf`/`gtf`/`ltef`/`gtef` plus `TST` (which retires
   `jmpfalse`/`jmptrue`) and `DECL`, and spell `ja`/`jae`/`jb`/`jbe` as assembler
   aliases. `addc`/`subc` are dropped. Touches `QLangBytecode.{h,cpp}`
   (encode/decode/size), the VM dispatch loops, the compiler's fusion sites and
   `qlang_run`'s disassembly; **changes the on-disk encoding → `.qlc` v5 → v6**
   (v5-only reader today, so a clean break per the Round 37 precedent). The
   three draft defects are fixed (empty `## encoding extended` heading filled,
   `## opCode scope` table repaired, "shared registers" defined and its flag
   model made normative). Sequencing: **independent of A3** — A3's **Q6** is
   closed and needs no encoding space, only the same `.qlc` v6 bump, so the two
   can land in either order or together. Effort: 1–2 weeks.

### Phase 3 — API surface (absorbs the former Phase 1)

1. **C1 strict typing** — **step 1 DONE 2026-11-14**: `16-strict_typing.md`
   finalized (M0 of `docs/plans/archive/strict-typing-plan.md`), **Q12–Q18, Q27–Q28
   settled there** (Q28's `$$` sentence aligned this round). Q12's premise was
   wrong — the `## rules` table and the examples do **not** contradict: they
   encode same-sign-group widening with no cross-sign and no narrowing, and
   the spec now says so explicitly. Step 2 (implement levels 1/2) follows
   `strict-typing-plan.md` milestones M1–M5. Historical decision record:
   define the
   `QLangContext` API, e.g. `setStrictTypingLevel(0/1/2)`) and settle
   **Q12–Q18, Q27–Q28**. Q12 gates everything: the draft's `## rules` coercion table
   ("smaller storage size" within the signed/unsigned group) contradicts its own
   examples, which reject `byte` for an `int` parameter — so the choice is
   *narrowing within a group* vs *exact type match*. The examples show runtime
   exceptions at call and return sites, so mismatches are checked at **runtime**
   (the compiler records declared parameter/return types, the VM checks them;
   who covers the native and `ctx.call()` paths is Q15, level-2 undeclared types
   are Q14, return checks Q16). Step 2: implement level 1 (check declared
   parameter/return types) and level 2 (undeclared types are an error), with
   `nil` implicit (Q13), `cast<>` as the only conversion (Q17) and exact
   class/external matching (Q18), plus the duplicate-declaration diagnostic
   (item 2). Tests: `spec_check2` (mismatch per type family, the Q12 outcome
   recorded explicitly, cross-sign always rejected, return-type check, level
   0/1/2 matrix). Effort: 1–2 days spec + ~1 week implementation.
2. **B2 duplicate-declaration diagnostic (re-lands inside C1)** — re-land the
   reverted Round 25 check (explicit `local`/`let` targeting a name already
   declared in the same scope) as a compiler-side check reading the context's
   strict-typing level: level 0 silent, level 1
   `warning: duplicated variable "a" declaration within scope`, level 2
   `error: duplicated variable "a" declaration within scope`
   (`16-strict_typing.md` `## redeclared variables within a scope`). Level 1
   needs the warning emission path — distinct from errors, never throws,
   routed through the gated `trace()` helper per AGENTS.md (the former B1;
   **Q3**) — and no independent toggle (**Q27**). The `$$` spec's
   "compile warning" sentence is aligned to the level table (**Q28**, spec
   edit → re-hash). Tests: `spec_check2` (silent at level 0, warning at
   level 1, error at level 2, no diagnostic for plain assignment crossing).
   Open questions: **Q5** (what counts as a duplicate once block scopes
   exist). Effort: ~1 day on top of C1.
3. **C5 inheritance (`ObjectOps::base`)** — `base` returns null today;
   `10-classes.md` v1 restrictions reserve the hook and
   `EXTERNAL_TYPES.md` §10.3 defers the base-chain walk to v2. Needs a spec
   first — **Q21** is the whole design (base-clause syntax, resolution order,
   constructor chaining, single vs multiple inheritance) — treat like C1 step 1.
   Effort: medium–large once specified.
4. **C7 enum-name string form** — reverse lookup for enum-valued
   properties. Known blocker (`EXTERNAL_TYPES.md` §10.5): no dispatch
   point sees both the field value and its type context —
   `str(e.options)` sees a bare `Int`. **Q22** is the design decision (enum
   table on the property binding vs a value tag) and must be settled before
   implementation. Effort: small–medium once designed.
5. **C8 module search paths + external search callback** — **✓ implemented (C8a
   Round 67, C8b Round 69, 2026-11-10).** New in the Round 52 re-audit; Q23 decided
   2026-11-09, spec re-worded the same day.
   Spec `07-api.md` `## module search paths` ("API C++ will
   provide a means to set the search paths to find modules"; POC modules are
   single files, so resolution is `[modulename].ql` tried in each directory in
   order, `.qlc` first within a directory) and `## external search callback` (a host-assigned
   `shared_ptr<LoadedModule> loadModuleCallback(const std::string &modulename,
   std::string &error)` for non-standard filesystems such
   as Qt `:/...`). Today modules are C++-registered (`ctx.registerModule()`) and
   resolved at compile time only — no filesystem lookup, no path list, no
   callback hook (`searchPath`/`searchCallback` appear nowhere in `include/` or
   `src/`). C8a delivered the `QLangContext` API
   (`moduleSearchPaths`/`setModuleSearchPaths`/`addModuleSearchPath`/
   `setModuleSearchCallback`/`findModule`) plus the `import` resolution path; no
   lexer, parser or VM change was needed. Prerequisite/front end for
   `docs/plans/archive/external-modules-plan-v4.md` (import/export tables assume a module
   can be *found* before it can be linked). **C8a** (paths + callback + order,
   `.qlc` only) landed Round 67; **C8b** (`.ql` discovery + the nested compile,
   reentrancy, in-flight cycle chain, error provenance) landed Round 69. Open
   questions **Q36–Q38 are answered** (a declared `MODULE_NAME` wins and is the
   registry key, a source with none takes its file stem, the callback sees the
   requested name only). Tests: `module_search` (81 → 103 checks) and
   `script_ql_module`, which loads a sibling `.ql` through the driver.
   Effort: done.

C2 enum injection (Round 41), C6 signals (Round 40) and C3
`hasMethod`/`callMethod` (`c4ee131`, 2026-11-06, pending the `BoundMethod`
guard fix) are **implemented** — see §D and the detailed sections below.

### Phase 4 — Structural (only if a spec decision changes)

- Table key insertion order (would require reworking `TableValue`
  code-wide) — **not planned**: spec says implementation-defined.
- Instance-call-of-static-method anchor routing — **not planned**: spec-silent.

## Detailed implementation plans

Per the overview convention, each phase item gets a detailed plan before work
starts. The two entries below are the detailed plans of items that have since
landed (C2 Round 41, C6 Round 40) and are kept as the design record; C6's full
plan lives in `docs/plans/archive/signals-implementation-plan.md`. **A5 has its
plan** — `docs/plans/encoding-v6-and-indirect-plan.md` (2026-11-06, finalized
together with the spec; it also carries the A3/**Q6** design, so A3 needs only
its check-pipeline and test-plan detail). The remaining open items (A3, C1 —
incl. the folded B1/B2 dup-decl work, C5, C7, C8) still need theirs — each one
opens by
closing the **Q#**s its item points at (adopting the default is a valid answer;
it just has to be recorded). **C3 is the one exception**: it landed
(`c4ee131`, 2026-11-06) with no detailed plan of its own — acceptable for an
item this size only because Q19/Q20 had already pinned the design, and recorded
here so the exception is not read as a new precedent.

### C2 — Enum C++ injection API

Status: **implemented** (Round 41, 2026-11-05) — landed with three
refinements to this draft: (1) the API carries a base type
(`registerEnum(name, members, baseType = Int)`) because `compileEnum` types
members per the declared base and `m_enums` stores typed `QLangValue`s (the
draft's "base type is metadata only" premise was wrong); (2) the hidden name
`__enum_<Name>` is also registered in `m_injectedConsts` — that is how the
compiler loads it (injected-const `declareVar` path) and keeps it out of the
writable-external-var set; (3) `compile()` pre-binds each injected enum's
hidden var to its global slot, and member folding inside a function body
accepts injected enums via a dedicated `m_injectedEnumNames` set (using
`m_globalEnums` there would have weakened block-scoped `local enum`
invisibility — caught by `spec_check2`).
Spec: `docs/spec/02-lexical.md` `## enum` ("C++ api needed to inject enums to
the global context (needs discussion)").

The language-side enum is fully implemented (Round 28): `parseEnum` →
`compileEnum` stores the members in a hidden const table `__enum_<Name>` and
records the name in the compiler's `m_enums` (name → member values, for
compile-time `E::M`/`E.M` folding) and `m_globalEnums` (top-level visibility +
redeclaration check). Only the C++ injection API is missing.

**API**

    // QLangContext
    bool registerEnum(const std::string &name,
                      const std::vector<std::pair<std::string, int64_t>> &members,
                      QLangValue::Type baseType = QLangValue::Type::Int);

- `name` — the enum name (a valid QLang identifier, not a reserved word).
- `members` — member name → value, in declaration order. Values are
  `int64_t`; implied sequences are the caller's job (explicit values only).
- `baseType` — numeric member type (default `Int`); values are converted
  exactly as `compileEnum` types a declared enum's members.
- Returns `true` on success; throws `QLangError` on validation failure.

**Behavior**

1. **Validation** (→ `QLangError`): `name` is a valid identifier and not a
   reserved word; `members` is non-empty (spec: "valueless enum triggers an
   error"); each member name is a valid identifier; no duplicate member names;
   `name` is not already in use (a live global in `m_globals.names` or a
   previously injected enum) → "cannot inject enum 'X': name already in use".
2. **Seed the hidden table** — build a `TableValue` (name → `Int`) and
   `set("__enum_" + name, table, /*isConst=*/true)`. The main frame aliases
   the global slots directly, so the table is visible at runtime with no VM
   change; `isConst` puts it in `m_injectedConsts` so it is immutable and
   excluded from the writable-external-var set.
3. **Register with the compiler** — track `m_injectedEnums[name] = members`
   on the context; `run()` passes it to a new `compiler.addExternalEnums(...)`
   (mirrors `addExternalConsts`/`addExternalVars`), which populates `m_enums`
   and `m_globalEnums`. This is what makes `E::M`/`E.M` fold at compile time
   and `E` resolve to the hidden table.
4. **Redeclaration** — a script `enum X` colliding with an injected `X` hits
   the existing `m_globalEnums.count(name)` check in `compileEnum` → "cannot
   redeclare enum 'X'". No new code.
5. **Removal** — `remove(name)` and `clear()` also erase `m_injectedEnums` and
   the `__enum_<Name>` slot, so a removed enum is invisible to the next
   compile.

**Impact assessment**

| Area | File(s) | Change | Risk |
|------|---------|--------|------|
| Compiler | `include/qlang/QLangCompiler.h`, `src/QLangCompiler.cpp` | `addExternalEnums(...)` populating `m_enums` + `m_globalEnums` | Low. Pure addition, mirrors `addExternalConsts`. |
| Context API | `include/qlang/QLangContext.h`, `src/QLangContext.cpp` | `m_injectedEnums`; `registerEnum(...)`; wire into `run()`; `remove`/`clear` cleanup | Low–medium: validation + lifecycle (remove/clear) must be correct. |
| Tests | `tests/unit/test_external.cpp` | New enum-injection checks | — |
| Docs | `docs/spec/02-lexical.md`, `docs/compliance/status.md` (+ `history.md`) | Record the API, flip the C2 open item | Per AGENTS.md: test first, then bump hashes. |

Not affected: lexer, parser, VM (the hidden table is a plain global const; the
main frame already aliases the global slots), `.qlc` format, opcodes.

**Implementation steps**

1. `QLangCompiler.h/.cpp` — `addExternalEnums(...)`.
2. `QLangContext.h/.cpp` — `m_injectedEnums`; `registerEnum(...)` (validate,
   seed `__enum_<Name>` as const, track); wire into `run()`; `remove`/`clear`
   cleanup.
3. `tests/unit/test_external.cpp` — the cases below. Build
   `cmake -B build && cmake --build build`, run `ctest --test-dir build`.
4. Docs — record the API in the spec, flip the C2 open item.

**Test cases** (in `test_external`, the external/C++-API suite)

- **injected enum readable** — `registerEnum("Animal", {{"Dog",0},{"Cat",1},{"Horse",2}})`
  → `Animal::Dog` == 0, `Animal.Cat` == 1, `Animal::Horse` == 2 (both
  notations fold).
- **len** — `len(Animal)` == 3 (the hidden table).
- **redeclaration rejected** — after injection, `run("enum Animal { A }")` →
  compile error "cannot redeclare enum 'Animal'".
- **unknown member** — `Animal::Nope` → compile error.
- **validation: empty members** — `registerEnum("E", {})` → `QLangError`.
- **validation: duplicate member** — `registerEnum("E", {{"A",0},{"A",1}})` →
  `QLangError`.
- **validation: name clash** — `set("Animal", 1)` then
  `registerEnum("Animal", ...)` → `QLangError`.
- **removal** — `remove("Animal")` → the next compile no longer sees `Animal`
  (unknown-enum error); `clear()` likewise.

**Risks / decisions** (all resolved at landing — not part of §Open questions)

1. **Base type** — resolved: the API carries `baseType` (the draft's premise
   was wrong — `compileEnum` honors the declared base). v1 does **not**
   range-check member values against the base: `registerEnum(..., 300, Byte)`
   wraps to 44, exactly like a declared `enum E:byte { A = 300 }`
   (`static_cast<uint8_t>`). Consistent; validation is a possible future
   change for both paths.
2. **Name-clash policy** — resolved: injection is **rejected** when the name
   is a live global, a previous injection, or a declared enum (matches "enum
   cannot be redefined"). Consequence: a `local enum` inside a block cannot
   shadow an injected enum of the same name (the hidden-var collision is
   detected via the pre-bound register).
3. **`.qlc` / `call()`** — verified: `call()` reuses the cached program and
   resolves injected enums; a `.qlc` written from a program that used an
   injected enum carries `__enum_<Name>` in `prog.vars` (like any top-level
   name) — the host must `registerEnum` before executing a loaded program,
   same contract as any injected global.

### C6 — Signals

Status: **implemented** (Round 40, 2026-11-05) — spec `17-signals.md`
finalized the same day; all steps of the plan landed.
**Full plan: `docs/plans/archive/signals-implementation-plan.md`** (the linked
file carries the complete impact assessment, implementation steps, and test
cases).

Signals let an external (C++) type notify script code of state changes. v1
scope: **external types only** — user-defined QLang classes have no signal
declaration syntax. Key design points:

- **Declaration** — `QL_SIGNAL(name)` in the `QL_OBJECT` list; the argument
  signature is deduced from a declared, unimplemented member function
  (Qt-style). A signal is a name + typed argument list; it is **not a method**.
- **Emit** — `emit obj.signal(args)`; `emit` is a reserved word; synchronous
  delivery in connection order; zero connections → silent no-op; re-entrant
  emits recurse and are contained by the step budget.
- **Connect / Disconnect** — `connect(sender, "signal", handler)` and the
  string-slot form return an int connection handle; a connection retains its
  handler (strong ref) but **not** its sender (weak ref); expired connections
  drop at the finalizer drain.
- **Opcode** — `EMIT` appended last (`kOpcodeUsedCount` 69 → 70); `.qlc` stays
  v5.
- **C++ API** — `ctx.hasSignal(obj, name)` / `ctx.emitSignal(obj, name, args)`.
- **Tests** — dedicated `signal_test` suite (precedent: `class_static_test`).

## Dependencies

```
C1 (levels + warning emission) ──> B2 (dup-decl diagnostic, Phase 3 item 2)
spec finalization ──> C1 (strict typing)
spec finalization ──> A5 (extended encoding)      ✅ done 2026-11-06
A5 detailed plan                                   ✅ done 2026-11-06
                                                   (encoding-v6-and-indirect-plan.md)
A5 M1 v6 encoding layer (additive *V6 codec)       ✅ done 2026-11-06
A5 M2 pipeline switch · M3 flag flow · M4 $$       pending
A5 (encoding layout) ──✗──> A3 Q6: edge removed 2026-11-06 — Q6 is closed without
                            consuming encoding space, so A3 no longer waits on A5
                            (they share only the `.qlc` v6 bump)
spec writing ──> C5 (inheritance)
design decision ──> C7 (enum-name string form)
C8 (module search paths) ──> archive/external-modules-plan-v4.md (import/export tables)
                            ✅ done 2026-11-09 (Rounds 61–65) — the plan shipped and moved to
                            docs/plans/archive/; C8 (discovery) landed Round 69
C8a (paths + callback, .qlc) ──> C8b (.ql discovery + nested compile)
                            ✓ both landed — C8a Round 67, C8b Round 69 (Q36–Q38
                            answered with it); C8 itself is closed, the runtime
                            module registry in QLangContext::run() is not part of it
A3, A5, C8: otherwise independent of each other and of the above

open questions gate the work (see §Open questions):
  Q1–Q4  ──> C1 warning shape    Q5   ──> B2 dup-decl rule
  Q6     ──> A3 encoding design (closed 2026-11-06)  Q7–Q11 ──> A3 details
  A5: no open Q# — six implementation defaults stand (§12 of the v6 plan)
  Q12    ──> C1 step 2           Q13–Q18 ──> C1 spec finalization
  Q27    ──> C1 toggle decision  Q28  ──> $$ spec wording (spec edit → re-hash)
  Q19–Q20 ──> C3 API shape (resolved 2026-11-06, adopted as defaulted)
  Q21 ──> C5 spec, Q22 ──> C7 design
  Q23    ──> C8 scope (DECIDED 2026-11-09: C8 owns compile-from-source)
  Q24–Q26 ──> C8 resolution contract (RESOLVED by the 07-api.md spec edit)
  Q36–Q38 ──> C8b (identity vs filename, missing MODULE_NAME, callback input) —
             ANSWERED with C8b (Round 69)

landed since this plan was written: A1 + A2 (Round 39) · C6 (Round 40) ·
C2 (Round 41) · A4 Phases 1–4 (Rounds 42–45) · C4 (Phase 3 host-API hardening)
· C3 (`c4ee131`, 2026-11-06, pending the hasMethod/callMethod guard fix)
```

## Deferred from the module export/import plan (archived Round 73)

From [archive/module-export-import-plan.md](archive/module-export-import-plan.md) (audit
F1–F6, Rounds 70–73) — its P0–P3 all landed; these items were explicitly deferred there with
their promotion triggers and are tracked here so nothing is lost. **Q42** (may a `.ql` module
export a class?) stays open until the spec answers it.

| item | why not now | promote when |
|---|---|---|
| **runtime module registry** (`13-bytecodes.md` `## technical specifications` 1, second link model): `QLangContext` has no reference to `QLangLinker`, `ctx.run()` never links | `getimport`/`callimport` already execute unmerged and the merge path is proven; the registry is a *host API* decision (who owns images, when they init), not a hole in `export`/`import` | a host needs to run images without `QLangLinker` (embedder request, or `qlang_run` gains `-r` for images) |
| **export a class** (audit F4) | `## keyword export` lists functions/variables/constants; a type export needs a spec decision (instance construction across the boundary, `deepcopy` rules) | the spec adds it (**Q42** stays open until then) |
| **cross-module captures** — an exported `def` whose sibling `def` has *local* captures is refused | refusal is correct and diagnosed; making it work means shipping the defining module's frame with the call | someone needs a module built around closures, and it should be its own design round |
| **by-value arguments across a module boundary**, `m.x++` silent no-op | both recorded in `history.md` (Round 68) as value-semantics questions, orthogonal to what an `import` binds | the value-semantics round |
| **Q36 warn on `MODULE_NAME` ≠ file stem** | nothing warns for `.qlc` either; a warning-only feature with no diagnostic channel | a warnings channel exists (C1 owns strict-typing levels) |
| **a library's own directory in its dependency search** | documented limit (`--help`, `07-api.md` §module search paths); the search list belongs to the context, so a fix needs a resolver-side stack of current module dirs | a real project hits it (nested third-party libs outside the entry dir) |
| **library-mode assignment to a module const** reports `assignment to undeclared identifier 'C'` (audit F5's sibling) | P0.3 closed the *read* path; the assign site resolves before the module-const lookup, so its message is merely wrong, not silent | someone hits the misleading message (same shape as F5's first bullet, which Round 73 fixed) |

## Conventions (per AGENTS.md)

- One detailed plan per phase item before implementation; commit by concern.
- After each landing: rebuild + `ctest`, flip the Open Item to verified in
  `docs/compliance/status.md`, append the round to `docs/compliance/history.md`,
  re-verify affected spec-module hashes (never bump a hash before testing).
- New opcodes append last in the enum; `.qlc` v5 stays the only supported
  format (bump only if the on-disk encoding changes). Before appending, check
  for an existing reserved placeholder — `kOpcodeUsedCount` is 73 (identities
  0..72 after Round 51) and `$$` already owns the reserved `INDGET`/`INDPUT`.
  A5 is the pending bump: v6 appends `eqf`/`ltf`/`gtf`/`ltef`/`gtef` + `TST` +
  `DECL` (73 → 80 of 128) and retires the `i`/`c` suffix identities in a later
  bump, keeping them decodable in v6 (`encoding-v6-and-indirect-plan.md` §3, §6).
