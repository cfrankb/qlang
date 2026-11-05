# Plan: Missing Features — Overview

Created 2026-11-04 (post-Round 38 audit). Overview/roadmap for every
spec'd-but-unimplemented item tracked in `docs/compliance/status.md`
(Open Items + Not Yet Implemented). This is an **overview**: each phase gets
a detailed implementation plan before work starts (precedent: the movi/addi/
subi plans in `docs/plans/archive/`).

## Inventory

### A. Language features (fully specified, unimplemented)

| # | Item | Spec anchor | Scope |
|---|------|-------------|-------|
| A1 | `nop` opcode | `13-bytecodes.md` `## OpCode nop` | trivial |
| A2 | `?` ternary operator | `05-operators.md` `## operator ?` | small |
| A3 | `$$` indirect access operator | `05-operators.md` `## operator $$` | large |

### B. Foundations (block other work)

| # | Item | Spec anchor | Blocks |
|---|------|-------------|--------|
| B1 | Warning infrastructure (toggleable warnings) | `11-external-types.md` `# warning` | B2 |
| B2 | Duplicate-declaration compile warning | `03-declarations.md` (dup-decl example), `05-operators.md` `## operator $$` (declaration-state rules) | — |

### C. API surface (spec'd "needed", undefined or partial)

| # | Item | Spec anchor | Notes |
|---|------|-------------|-------|
| C1 | Strict typing (levels 0/1/2) | `16-strict_typing.md` | draft spec — C++ API "to be provided"; examples show runtime exceptions at call/return sites |
| C2 | Enum C++ injection API | `02-lexical.md` `## enum` | language-side enum implemented (`__enum_<Name>` hidden table); only the C++ API is missing ("needs discussion") |
| C3 | External-type wrappers: `hasMethod`/`callMethod` | `11-external-types.md` | event-handler contract; `ops->callMethod` has zero VM call sites in v1 |
| C4 | RTTI validation on external handoff | `11-external-types.md` | **implemented (Phase 3)** — typed `qval_external_*<T>` helpers + `dynamic_cast` check; see §D |
| C5 | Inheritance (`ObjectOps::base`) | `10-classes.md` v1 restrictions, `EXTERNAL_TYPES.md` §10.3 | reserved, returns null; base-chain walk deferred to v2 |
| C6 | Signals (`Q_SIGNALS`/`emit`/`connect`) | `17-signals.md` (tentative draft) | draft landed 2026-11-04; finalize, then implement |
| C7 | Enum-name string form for enum-valued properties | `EXTERNAL_TYPES.md` §10.5 | no dispatch point sees both field value and type context |

### D. Resolved / no work needed

- **Table key order** — `__str__` renders lexicographic (`std::map`); the spec
  explicitly says "table key order is implementation defined (depends on the
  underlying container)" (`11-external-types.md`). Not a gap.
- **Instance-call-of-static-method** — spec-silent; documented deviation
  (hidden-anchor design, `docs/compliance/classes.md`).
- **`|||` logical-or** — intentional extension, deliberately not in the spec.
- **C4 RTTI validation on external handoff** — implemented in Phase 3
  (`EXTERNAL_TYPES.md` §10.2): the typed handoff helpers
  `qval_external_owned<T>`/`qval_external_borrowed<T>` record `typeid(T)`
  and validate the resolved pointer with a `dynamic_cast` through the
  `QLObject<T>` base; a mismatch is a clean `type error: external type
  mismatch: …` at first use, not UB. Untyped `qval_external(type, …)`
  factories remain for hosts that cannot name the concrete type (no check).
  `phase3_api` tests cover the mismatch path.

## Phases

### Phase 1 — Quick wins (independent, low risk)

1. **A1 `nop`** — append `Opcode::Nop` last (`.qlc` opcode numbers unchanged),
   `Bytecode::name()` → `"nop"`, no-op VM case in all 4 exec loops, codec
   name. No compiler emission (spec: "takes no action"). Tests: `codec_check`
   (name + round-trip). Effort: ~half a day.
2. **A2 `?` ternary** — `?` lexer token, parser rule at the spec's precedence
   level, codegen to existing branch opcodes (`cmp`/`cmpi` + `jne` + jump —
   no new opcode), block form with `return`. Tests: `spec_check2` (both
   branches, nesting, block form, precedence vs `&&`/`||`). Effort: 1–2 days.

### Phase 2 — Warning infrastructure

3. **B1 warnings** — warning emission path distinct from errors (warnings do
   not throw; diagnostics route through the gated `trace()` helper per
   AGENTS.md); toggle on `QLangContext` (`setWarningsEnabled(bool)` + a QLang
   native mirroring `set_verbose`). Spec note: "all warnings defered until
   further notice" — the toggle ships with zero active warnings.
4. **B2 duplicate-declaration warning** — re-land the reverted Round 25 check
   (explicit `local`/`let` targeting a name already declared in the same
   scope → compile warning) on top of the B1 toggle. Tests: `spec_check2`
   (warning emitted, suppressed when toggled off, no warning for plain
   assignment crossing). Effort: 1–2 days total.

### Phase 3 — `$$` indirect operator (largest feature)

5. **A3 `$$`** — per the spec's implementation notes:
   - per-frame name map (name → register, binding kind `var`/`const`/
     `function`/`lambda`, declaration bit);
   - declaration bit as a flag on existing store/init opcodes (no new opcode
     for declaration state);
   - two new opcodes `OP_INDIRECT_READ`/`OP_INDIRECT_WRITE` (append last);
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

### Phase 4 — API surface

6. **C1 strict typing** — step 1: finalize `16-strict_typing.md` (define the
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
7. **C2 enum injection** — the language-side enum is fully implemented
   (parser `parseEnum`, compiler `compileEnum` → `__enum_<Name>` hidden
   const table, `m_globalEnums`); only the C++ API is missing (spec:
   "needs discussion"). Define the API (e.g. `ctx.registerEnum(name,
   baseType, members)`) that seeds the `__enum_<Name>` hidden var into the
   global table **and** registers the name with the compiler's global-enum
   set, so injected names resolve at compile time. Tests: `test_external`
   (injected enum readable, member values, redeclaration rejected). Effort:
   small once the API is defined.
8. **C3 `hasMethod`/`callMethod` context wrappers** — add
   `QLangContext::hasMethod(obj, name)` / `callMethod(obj, name, args)`
   routing through `getObjectOps` → `ops->callMethod` (present on every ops
   table; the VM does not call it in v1 — method calls are
   `TABGET` → `BoundMethod` → `CALLCLOS`). This is the event-handler
   contract (`docs/EVENT_HANDLERS.md` §0). Tests: `test_external`
   (hasMethod true/false, callMethod dispatch, unknown-method error).
   Effort: small.
9. **C5 inheritance (`ObjectOps::base`)** — `base` returns null today;
   `10-classes.md` v1 restrictions reserve the hook and
   `EXTERNAL_TYPES.md` §10.3 defers the base-chain walk to v2. Needs a spec
   first (base-clause syntax, field/method resolution order, constructor
   chaining) — treat like C1 step 1. Effort: medium–large once specified.
10. **C6 signals (`Q_SIGNALS`/`emit`/`connect`)** — tentative spec draft
    landed: `17-signals.md` (external types only; `QL_SIGNAL` meta with
    signature deduced from an unimplemented member function; `emit`
    reserved word; `connect`/`disconnect` builtins; synchronous delivery,
    step-budget-contained recursion; C++ `emitSignal`/`hasSignal`).
    Finalize the draft (review the open design points), then implement.
    Effort: ~1 week once the draft is finalized — the largest Phase 4 item
    (language-level keyword + runtime + C++ API).
11. **C7 enum-name string form** — reverse lookup for enum-valued
    properties. Known blocker (`EXTERNAL_TYPES.md` §10.5): no dispatch
    point sees both the field value and its type context —
    `str(e.options)` sees a bare `Int`. Needs a design decision (e.g. an
    enum-typed property binding that carries the enum table) before
    implementation. Effort: small–medium once designed.

### Phase 5 — Structural (only if a spec decision changes)

- Table key insertion order (would require reworking `TableValue`
  code-wide) — **not planned**: spec says implementation-defined.
- Instance-call-of-static-method anchor routing — **not planned**: spec-silent.

## Dependencies

```
B1 (warnings) ──> B2 (dup-decl warning)
spec finalization ──> C1 (strict typing)
API definition ──> C2 (enum injection)
spec writing ──> C5 (inheritance)
draft finalization ──> C6 (signals)
design decision ──> C7 (enum-name string form)
A1, A2, A3, C3: independent of each other
```

## Conventions (per AGENTS.md)

- One detailed plan per phase item before implementation; commit by concern.
- After each landing: rebuild + `ctest`, flip the Open Item to verified in
  `docs/compliance/status.md`, append the round to `docs/compliance/history.md`,
  re-verify affected spec-module hashes (never bump a hash before testing).
- New opcodes append last in the enum; `.qlc` v5 stays the only supported
  format (bump only if the on-disk encoding changes).
