# Plan: Missing Features — Overview

Created 2026-11-04 (post-Round 38 audit). **Re-audited 2026-11-05 (post-Round
52)** — Rounds 39–52 landed A1 `nop`, A2 `?`, C2 enum injection, C6 signals and
the whole of A4 `deepcopy`/value semantics; those items moved to §D, the phases
were renumbered, and C8 (module search paths) was added. Overview/roadmap for
every spec'd-but-unimplemented item tracked in `docs/compliance/status.md`
(Open Items + Not Yet Implemented). This is an **overview**: each phase gets
a detailed implementation plan before work starts (precedent: the movi/addi/
subi plans in `docs/plans/archive/`).

## Inventory

### A. Language features (fully specified, unimplemented)

| # | Item | Spec anchor | Scope |
|---|------|-------------|-------|
| A3 | `$$` indirect access operator | `05-operators.md` `## operator $$` | large |

A1 `nop` (Round 39), A2 `?` (Round 39) and A4 `deepcopy` + share-by-default
value semantics (Rounds 42–45, plan closed Round 45) are **implemented** — see
§D. The numbering is kept so the round records and `status.md` cross-references
stay readable.

### B. Foundations (block other work)

| # | Item | Spec anchor | Blocks |
|---|------|-------------|--------|
| B1 | Warning infrastructure (toggleable warnings) | `11-external-types.md` `# warning` | B2 |
| B2 | Duplicate-declaration compile warning | `03-declarations.md` (dup-decl example), `05-operators.md` `## operator $$` (declaration-state rules) | — |

### C. API surface (spec'd "needed", undefined or partial)

| # | Item | Spec anchor | Notes |
|---|------|-------------|-------|
| C1 | Strict typing (levels 0/1/2) | `16-strict_typing.md` | draft spec — C++ API "to be provided"; examples show runtime exceptions at call/return sites |
| C3 | External-type wrappers: `hasMethod`/`callMethod` | `11-external-types.md` | event-handler contract; `ops->callMethod` has zero VM call sites in v1 |
| C5 | Inheritance (`ObjectOps::base`) | `10-classes.md` v1 restrictions, `EXTERNAL_TYPES.md` §10.3 | reserved, returns null; base-chain walk deferred to v2 |
| C7 | Enum-name string form for enum-valued properties | `EXTERNAL_TYPES.md` §10.5 | no dispatch point sees both field value and type context |
| C8 | Module search paths + external search callback | `07-api.md` `## module search paths`, `## external search callback` | **new in this audit** — spec text only, no C++ surface (`searchPath`/`searchCallback` appear nowhere in `include/` or `src/`); prerequisite/front end for `external-modules-plan-v3.md` |

C2 enum injection (Round 41), C4 RTTI handoff validation (Phase 3) and C6
signals (Round 40) are **implemented** — see §D.

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

## Phases

### Phase 1 — Warning infrastructure

1. **B1 warnings** — warning emission path distinct from errors (warnings do
   not throw; diagnostics route through the gated `trace()` helper per
   AGENTS.md); toggle on `QLangContext` (`setWarningsEnabled(bool)` + a QLang
   native mirroring `set_verbose`). Spec note: "all warnings defered until
   further notice" — the toggle ships with zero active warnings.
2. **B2 duplicate-declaration warning** — re-land the reverted Round 25 check
   (explicit `local`/`let` targeting a name already declared in the same
   scope → compile warning) on top of the B1 toggle. Tests: `spec_check2`
   (warning emitted, suppressed when toggled off, no warning for plain
   assignment crossing). Effort: 1–2 days total.

### Phase 2 — `$$` indirect operator (largest feature)

3. **A3 `$$`** — per the spec's implementation notes:
   - per-frame name map (name → register, binding kind `var`/`const`/
     `function`/`lambda`, declaration bit) — the map lives on `Frame`, so the
     read/write handlers sit in the single `runFrames()` dispatch loop;
   - declaration bit as a flag on existing store/init opcodes (no new opcode
     for declaration state);
   - **reuse the reserved `Opcode::INDGET` / `Opcode::INDPUT` placeholders**
     (`QLangBytecode.h:123-124`) rather than adding opcodes: they already carry
     fixed identities inside the defined range (`kOpcodeUsedCount` is 73,
     identities 0..72), the `indget`/`indput` mnemonics
     (`QLangBytecode.cpp:796-797`), the 3-operand default arity and the
     `qlang_run` pseudo-text — so **no identity is appended and
     `kOpcodeUsedCount` does not change**. Round 51 deliberately disambiguated
     `INDEXGET`/`INDEXPUT` from these placeholders. The spec's
     `OP_INDIRECT_READ`/`OP_INDIRECT_WRITE` names are the same pair; the spec
     wording should be aligned to the implemented mnemonics.
   - six-stage check pipeline with canonical `indirect variable error:`
     messages (string → identifier lexeme → reserved word → declared/visible
     → binding kind → local-only writes);
   - resolution order: writes frame locals only; reads locals +
     `global`-exposed (global const visible everywhere);
   - no variable creation; functions/lambdas never targetable; const
     read-only; branch-dependent declarations can be `not declared`;
   - optional compile-time fast path for `$$"literal"`.
   Tests: dedicated suite (precedent: `class_static_test`) covering all six
   pipeline stages, resolution order, declaration state, const/function
   rejection. Effort: 1–2 weeks.

### Phase 3 — API surface

4. **C1 strict typing** — step 1: finalize `16-strict_typing.md` (define the
   `QLangContext` API, e.g. `setStrictTypingLevel(0/1/2)`). The spec's
   examples show runtime exceptions at call and return sites, so
   enforcement is **runtime**: the compiler records declared parameter/
   return types, the VM checks them. Open questions to resolve in the spec:
   the coercion table's "smaller storage size" must read "same or smaller"
   (the examples accept `int` for an `int` parameter); level 2's "throw an
   error if type is not declared" — compile-time or runtime; whether a
   declared type accepts `nil`; whether return checks use the same coercion
   rules; interaction with explicit `cast<>`. Step 2: implement — level 1
   checks declared parameter types at call sites, level 2 enforces
   declarations; nil implicit; coercion rules (signed/unsigned integer
   narrowing only, no cross-sign, float/array/set/table/class exact).
   Tests: `spec_check2` (mismatch per type family, narrowing accepted,
   cross-sign rejected, return-type check, level 0/1/2 matrix). Effort:
   1–2 days spec + ~1 week implementation.
5. **C3 `hasMethod`/`callMethod` context wrappers** — add
   `QLangContext::hasMethod(obj, name)` / `callMethod(obj, name, args)`
   routing through `getObjectOps` → `ops->callMethod` (present on every ops
   table; the VM does not call it in v1 — method calls are
   `TABGET` → `BoundMethod` → `CALLCLOS`). This is the event-handler
   contract (`docs/EVENT_HANDLERS.md` §0); `hasSignal`/`emitSignal` (Round 40)
   already mirror the shape and cite `hasMethod` in a comment, so the pair
   should land together with the comment made accurate. Tests: `test_external`
   (hasMethod true/false, callMethod dispatch, unknown-method error).
   Effort: small.
6. **C5 inheritance (`ObjectOps::base`)** — `base` returns null today;
   `10-classes.md` v1 restrictions reserve the hook and
   `EXTERNAL_TYPES.md` §10.3 defers the base-chain walk to v2. Needs a spec
   first (base-clause syntax, field/method resolution order, constructor
   chaining) — treat like C1 step 1. Effort: medium–large once specified.
7. **C7 enum-name string form** — reverse lookup for enum-valued
   properties. Known blocker (`EXTERNAL_TYPES.md` §10.5): no dispatch
   point sees both the field value and its type context —
   `str(e.options)` sees a bare `Int`. Needs a design decision (e.g. an
   enum-typed property binding that carries the enum table) before
   implementation. Effort: small–medium once designed.
8. **C8 module search paths + external search callback** — **new in the
   Round 52 re-audit.** Spec `07-api.md` `## module search paths` ("API C++ will
   provide a means to set the search paths to find modules"; POC modules are
   single files, so resolution is `[modulename].ql` tried in each directory in
   order) and `## external search callback` (a host-assigned
   `string searchCallback(string modulename)` for non-standard filesystems such
   as Qt `:/...`). Today modules are C++-registered (`ctx.registerModule()`) and
   resolved at compile time only — no filesystem lookup, no path list, no
   callback hook (`searchPath`/`searchCallback` appear nowhere in `include/` or
   `src/`). Work: the `QLangContext` API (`setModuleSearchPaths(...)`,
   `setSearchCallback(...)`) plus the `import` resolution path; no lexer, parser
   or VM change is implied. Prerequisite/front end for
   `docs/plans/external-modules-plan-v3.md` (import/export tables assume a module
   can be *found* before it can be linked). Tests: `test_module` (+ a
   `tests/scripts/` regression once `.ql` file loading exists). Effort:
   small–medium.

C2 enum injection (Round 41) and C6 signals (Round 40) are **implemented** — see
§D and the detailed sections below.

### Phase 4 — Structural (only if a spec decision changes)

- Table key insertion order (would require reworking `TableValue`
  code-wide) — **not planned**: spec says implementation-defined.
- Instance-call-of-static-method anchor routing — **not planned**: spec-silent.

## Detailed implementation plans

Per the overview convention, each phase item gets a detailed plan before work
starts. The two entries below are the detailed plans of items that have since
landed (C2 Round 41, C6 Round 40) and are kept as the design record; C6's full
plan lives in `docs/plans/archive/signals-implementation-plan.md`. The open
items (B1/B2, A3, C1, C3, C5, C7, C8) still need theirs.

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

**Risks / open questions** (resolved at landing)

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
B1 (warnings) ──> B2 (dup-decl warning)
spec finalization ──> C1 (strict typing)
spec writing ──> C5 (inheritance)
design decision ──> C7 (enum-name string form)
C8 (module search paths) ──> external-modules-plan-v3.md (import/export tables)
A3, C3, C8: independent of each other and of the above

landed since this plan was written: A1 + A2 (Round 39) · C6 (Round 40) ·
C2 (Round 41) · A4 Phases 1–4 (Rounds 42–45) · C4 (Phase 3 host-API hardening)
```

## Conventions (per AGENTS.md)

- One detailed plan per phase item before implementation; commit by concern.
- After each landing: rebuild + `ctest`, flip the Open Item to verified in
  `docs/compliance/status.md`, append the round to `docs/compliance/history.md`,
  re-verify affected spec-module hashes (never bump a hash before testing).
- New opcodes append last in the enum; `.qlc` v5 stays the only supported
  format (bump only if the on-disk encoding changes). Before appending, check
  for an existing reserved placeholder — `kOpcodeUsedCount` is 73 (identities
  0..72 after Round 51) and `$$` already owns the reserved `INDGET`/`INDPUT`.
