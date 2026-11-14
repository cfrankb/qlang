# Plan: C1 — Strict Typing (levels 0/1/2)

Created 2026-11-14. Detailed implementation plan for
`docs/spec/16-strict_typing.md` (compliance Open Item "strict typing ✗ NOT
implemented"; overview item **C1**, folded items **B1** warning emission and
**B2** duplicate-declaration diagnostic — `docs/plans/missing-features-plan.md`
Phase 3 items 1–2). Per the overview convention this plan opens by closing the
**Q#**s C1 points at (§Decisions); implementation follows the milestones.

Status: **planned, unimplemented.** The `qlcc --strict <0-2>` flag already
parses and deliberately rejects (`examples/qlcc.cpp:1949-1969`, its comment
waits for exactly this API: "Wire ctx here when the engine grows
setStrictTypingLevel()").

## Scope

Enforcement lives at the `QLangContext` level (spec line 3): a C++-settable
level the compiler and VM consult.

| Level | Meaning (spec) | Compile-time | Runtime |
|-------|----------------|--------------|---------|
| 0 | no type checking | nothing | nothing (today's engine) |
| 1 | check type if declared | dup-decl → **warning** | call/return checks where a type is declared |
| 2 | error if type not declared | dup-decl → **error**; undeclared param/var types → compile error | same as level 1 |

Out of scope (spec-silent or owned elsewhere): type enforcement on plain
assignments to typed locals (the `:type` declaration modifier stays the lenient
coercion it has always been — SPEC §declarations calls it a *coerce modifier*,
and the spec's enforcement examples are all call/return sites), `yield` values,
external `BoundMethod` calls and native `NativeFunc` calls (no declared
signature exists to check — host contract), derived-class acceptance (waits for
C5 `ObjectOps::base`, **Q18**), insert-order and `$`/`$$` interactions.

## Decisions (closing Q1–Q5, Q12–Q18, Q27–Q28)

Adopted answers; spec edits land with M0 (then re-hash). Where a default from
`missing-features-plan.md` §Open questions is overridden, it says so.

| Q | Decision |
|---|----------|
| **Q12 coercion vs exact** | **The default's premise is wrong — the table and the examples do not contradict.** The rules table allows, for a signed target, same-group sources *of smaller storage* (implicit widening); `byte→int` fails the group test and `long→int` fails the size test — exactly the two examples that reject. **Adopted rule (`typeMatches`): exact `QLangValue::Type` equality, OR same signed group `{Char,Short,Int,Long}` with strictly smaller source storage, OR same unsigned group `{Byte,Word,Unsigned,Qword}` likewise, OR the Nil rule below.** Cross-sign is always rejected. Float accepts only Float. String/Array/Set/Table/Blob/Function accept only their own type. `cast<>` is the only other conversion (**Q17**, adopted as defaulted): it yields the target type, so the check sees an exact match. Spec edit re-words the table unambiguously (one table per the target-type reading). |
| **Q13 nil** | A `Nil` value satisfies **any declared parameter type** ("nil type is implicit"). |
| **Q16 returns** | A declared return type **rejects nil** — this is what makes the spec's `sum_int2int` with no `return` throw ("function must return an int": the implicit fall-through returns nil, `QLangCompiler.cpp:2010-2014` already forces reg0 to nil + `RETURN`). Same for a bare `return`. Recorded as the one asymmetry against Q13, driven by the spec's own example. |
| **Q14 level-2 undeclared = compile error, and what "undeclared" means** | Compile error at the definition site (the signature/initializer is statically visible). Requires a type: function/method parameters, and `local`/`let`/`const` **whose initializer is not a closure literal or generator call** (the spec's own lambda example is a bare `let f = (...)[]->str{}` — there is no `:function` annotation, so those initializers are exempt). **Exempt: valueless `local z`** — it declares an implicit nil ("nil type is implicit and doesn't need to be declared", spec line 14; also keeps the `## redeclared variables` example writable at level 2). Also exempt: loop variables, capture-list names, `global` statements, class fields (they have inferred types, Phase 5). A **return type** is enforced only where `-> type` is present; no `-> nil` syntax exists, so procedures stay legal at every level. |
| **Q15 who enforces** | The **VM call boundary** — adopted as defaulted. One shared helper called from `pushCall` (`QLangVM.cpp:1809`, covers `CALLFN` + `CALLCLOS` + script method calls incl. signal handlers), `callFunction` (`QLangVM.cpp:2634`, host `ctx.call()`), the `MAKEGEN`/generator-creation arg copy, and the `RETURN` handler for the return value. The level is read **live** from `m_scope` (same pattern as `maxSteps`), so declared-type checks apply regardless of when the host set the level. |
| **Q18 class / external** | Exact identity in v1: a class annotation `p:Person` accepts a table instance whose `__class__` string equals `"Person"` (script instances are `__class__`-tagged tables, `QLangVM.cpp:280-285`), or an `External` value whose `TypeDescriptor` **name** equals it. Base-chain acceptance lands with C5. |
| **Q1/Q27 warning control** | The strict-typing level is the sole control; **no independent toggle** (adopted as defaulted). |
| **Q2 default level** | **0** — "type declarations are optional. no enforcement". A fresh context is today's engine. |
| **Q3 warning destination** | **Simplified from the default:** warnings go through the universal output sink only — `QLangContext::output(OUT_WARNING, …)`, the severity the sink comment already reserves for "future engine warnings" (`QLangContext.h:45-59`). No parallel `ctx.warnings()` list until a host asks for one. Never throws. |
| **Q4 runtime warnings** | None. The level-1 dup-decl diagnostic is compile-time; level 1/2 mismatches are exceptions (adopted as defaulted). |
| **Q5 what counts as a duplicate** | Same-scope only, as the reverted Round-25 check did: an explicit `local`/`let`/`const` naming a name already bound **in the current scope** (innermost block map; at top level, the global area). Nested shadowing stays silent; one shared check (aligns `$$` wording via **Q28**, spec edit → re-hash). |
| **Bool** | New rules-table row (spec edit): `bool` accepts only Bool (and nil for parameters). Bool is a distinct `QLangValue::Type`; it never satisfies `:int`. Hosts that want it pass `cast<int>(…)`. Flagged in §Risks. |
| **B2 message text** | Spec-verbatim, diagnostic kind included: `warning: duplicated variable "a" declaration within scope` at level 1 (via `output(OUT_WARNING, …, module, line)`), `error: duplicated variable "a" declaration within scope` at level 2 (a thrown `QLangError` of the same kind existing semantic compile errors use, cf. the `cannot redeclare enum` throw). |

**API constants** (spec line 80: "provide constant values … to prevent magic
numbers") — `QLangContext.h`, mirroring the `TRACE_*` anonymous-enum pattern:

```cpp
// Strict typing enforcement levels (SPEC §strict typing).
enum { STRICT_OFF, STRICT_DECLARED, STRICT_ALL };
```

## Design

### 1. Context API (`QLangContext.h/.cpp`)

```cpp
// Strict typing level (SPEC §strict typing): STRICT_OFF (default),
// STRICT_DECLARED (check where declared), STRICT_ALL (+ undeclared = error).
// Host configuration: clear() keeps it (like the output callback).
void setStrictTypingLevel(int level) { m_strictTypingLevel = level; }
int strictTypingLevel() const { return m_strictTypingLevel; }
```

`int m_strictTypingLevel = STRICT_OFF;`. The compiler already holds `m_scope`
(null-guarded) and the VM holds `m_scope` — no plumbing beyond these two
accessors. Compiler semantic errors and the dup-decl warning consult
`m_scope->strictTypingLevel()`; the compiler treats a null scope as
`STRICT_OFF` (lenient), matching its other scope-gated behaviors.

### 2. Shared type-annotation model (`QLangValue.h`)

The AST needs typed parameters and return annotations; `Function` needs to
carry them to the VM; the codec needs to serialize them. One struct, defined in
`QLangValue.h` (so parser, compiler, bytecode, codec all see it without
including each other):

```cpp
// A declared type annotation: builtin `type` (Nil = none) plus `className`
// when the annotation names a user class or external type (`p:Person`).
struct TypeAnnotation {
  QLangValue::Type type = QLangValue::Type::Nil;
  std::string className;
  bool declared() const { return type != QLangValue::Type::Nil || !className.empty(); }
};
```

- `Stmt` (QLangParser.h): `std::vector<TypeAnnotation> paramTypes; TypeAnnotation returnType;`
  (next to `params`; `declaredType` already covers declarations).
- `Expr` Closure node: same two fields.
- `Function` (QLangBytecode.h): `std::vector<TypeAnnotation> paramTypes; TypeAnnotation returnType;`
  (indices align with params 0..paramCount-1; a class method's entry 0 — the
  hidden self — stays undeclared).

### 3. Lexer / parser (`QLangLexer.*`, `QLangParser.*`)

- New punctuation token `TokenType::RArrow` for `->` (nothing exists today;
  `def f(a) -> int`, `(a:int)[]->str{…}`).
- One shared `static bool typeFromToken(TokenType, QLangValue::Type &)` —
  the duplicated `tmap` arrays in `parseDeclaration`/`parseConst`
  (`QLangParser.cpp:448-475, 507-535`) collapse into it, and the new sites use
  it too (ladder rung 2: it exists four times already — fix it while adding a
  fifth caller).
- Typed parameters `name:type` **and** class-name annotations `name:Person`
  (an `Ident` in type position → `TypeAnnotation::className`) at the four
  signature sites: `parseFunction` (`:1132`), `parseArrowClosure` (`:1187`),
  the class-body method form (`:276`), and the `ExprKind::Closure` primary
  (`:1608`). Return annotation after the `)` (def) or after the capture list
  `]` (closures/methods): `if (match(RArrow)) <annotation>`.
- Annotated assignment `age:byte = 24` (the spec's re-type example, spec
  lines 38-39 — **not supported today**): in the Ident statement branch, `Ident
  ':' <type> '=' expr` → a `StmtKind::Declaration` with `declaredType` set
  against the existing name (`declareVar` returns the live register; this is a
  re-type, **not** a duplicate-decl diagnostic — the dup rule keys on explicit
  `local`/`let`/`const`, Q5).
- Level-0/1 parse behavior is unchanged for untyped signatures: annotations
  stay optional everywhere; nothing is required at parse time even at level 2
  (the requirement is a compiler semantic check, so the parser stays
  level-agnostic).

### 4. Compiler (`QLangCompiler.*`)

- **Dup-decl diagnostic (B2)** — new `bool declaredInCurrentScope(name)` (the
  block map's `back()` when inside block scopes, else `m_regs`; parameters and
  outer-scope names are deliberately not matches — Q5). Called at the top of
  `compileDeclaration`/`compileConst` (`:2196`, `:2239`) before `declareVar`;
  on a hit at level 1 emit `warning: …` through `m_scope->output(OUT_WARNING,
  msg, moduleName, line)`, at level 2 throw `error: …`. New one-line
  `QLangCompiler::warn(line, msg)` helper wraps the output call (the B1
  infrastructure is that plus the sink — nothing more, **Q3/Q27**).
- **Level-2 semantic checks** at the definition sites (`compileDef`, the
  Arrow/Closure compile paths, class methods, `compileDeclaration`/
  `compileConst`): parameter annotations missing →
  `parameter 'a' has no declared type`; variable declaration with an initializer
  and no annotation → `variable 'x' has no declared type` — both throws, gated
  by `strictTypingLevel() >= STRICT_ALL` and the Q14 exemption list.
- **Signature plumbing**: `buildFunction` gains `const std::vector<TypeAnnotation>
  &paramTypes` + a `TypeAnnotation returnType` (callers: `compileDef`, Arrow,
  Closure, class methods — they already pass `params`/`captures`); stored on
  `Function` verbatim. No encoding or emitter change.
- Untyped signatures at level 1 remain exactly today's codegen.

### 5. VM (`QLangVM.*`) — the call-boundary checks (Q15)

One file-local matcher shared by every site:

```cpp
// True when `v` satisfies annotation `a` under the strict-typing rules
// (SPEC §strict typing). `params == true` admits nil for any declared type
// (Q13); return checks pass false (Q16).
static bool typeMatches(const QLangValue &v, const TypeAnnotation &a, bool params);
```

- Nil admits (params). Exact type equality; signed/unsigned same-group
  widening per Q12; `className` cases per Q18 (`__class__` string compare on
  the instance table; `asExternal().type->name` compare). Bool: Bool only.
- `checkArgTypes(const Function &fn, const C &regs, int argc)` — iterates
  `fn.paramTypes` (only entries `declared()`), throws
  `QLangError(Type, "argument <k> of '<fn.name>' expects '<want>', got
  '<got.typeName()>'")` via `vThrow` (line/col through the CALL-site `fn.loc`
  convention where available). Called from `pushCall`, `callFunction` (both the
  generator and normal branches, before the frames are built) and the `MAKEGEN`
  arg copy. All gated by `m_scope.strictTypingLevel() >= STRICT_DECLARED` and
  by the function having annotations — an unannotated function is invisible at
  level 1, an error at level 2 (caught at compile time; loaded v9 images without
  signatures simply stay unchecked, see §Risks).
- **Return check**: in `Opcode::RETURN` (`:2432`), look up
  `m_prog.functions[frame.funcIndex].returnType`; when declared and
  `!typeMatches(m_returnValue, rt, /*params=*/false)`, throw
  `"function '<name>' must return '<want>', got '<got>'"` — the spec's
  must-return-an-int example lands here with `got 'nil'`.
- Nothing changes in `CONV` (declaration coercion stays lenient at all levels —
  it is the explicit `:type` coerce modifier, §Scope) or in `CAST`
  (`strict_convert` already produces the target type, Q17).

### 6. `.qlc` codec v10 (`QLangBytecodeIO.*`, `docs/SERIALIZATION.md`)

Without this, `import`ed precompiled modules enforce nothing and `qlcc
--strict` is half a feature. Append to each function record (after
`registerCount`): `paramCount` entries of `[u8 tag, name?×3]` + one return
annotation, where tag `0` = undeclared, `1..19` = the existing
`QLangValue::Type` tags (class annotations never use them), and tag `255` =
class annotation followed by the length-prefixed class name. `kQlcVersion` 9 →
10; strict version gate as usual (v1-9 rejected), reader range-checks nothing
new beyond the name strings. `CompiledProgram::functions` carries the fields, so
`QLangLinker` merging and `writeQlc` reject-linked-programs behavior are
unaffected. SERIALIZATION.md §4 gains the section; `docs/SERIALIZATION.md` hash
gates (if any) and `codec_check` round-trip cases cover it.

### 7. Driver (`examples/qlcc.cpp`, `cmake/check_qlcc_cli.cmake`)

Replace the rejection block (`:1949-1969`) with
`ctx.setStrictTypingLevel(level - '0')` on the driver's context (its comment
pre-authorized exactly this). Update the `--strict` help text and the `README.md`
row (drop "rejected as not implemented yet"). The cmake guard that asserts the
rejection (`check_qlcc_cli.cmake:~185`) flips to asserting behavior:
`--strict 1` on a mismatching script exits nonzero with the type error, the same
script exits 0 at `--strict 0`, and `--strict 3` still fails argument validation.

## Impact assessment

| Area | File(s) | Change | Risk |
|------|---------|--------|------|
| Context API | `include/qlang/QLangContext.h`, `src/QLangContext.cpp` | level member + accessors + `STRICT_*` enum | Low. Two ints. |
| Annotation model | `include/qlang/QLangValue.h`, `QLangParser.h`, `QLangBytecode.h` | `TypeAnnotation`; AST + `Function` fields | Low–medium: shared struct threads parser→compiler→codec. |
| Lexer/Parser | `QLangLexer.*`, `src/QLangParser.cpp` | `->` token; typed params ×4 sites + class names; `->` return ×4; annotated assignment; `typeFromToken` dedup | Medium: signature grammar; keep untyped forms byte-identical in behavior. |
| Compiler | `QLangCompiler.*` | dup-decl diagnostic + `warn()`; level-2 checks; `buildFunction` + `Function` signature plumbing | Medium: `buildFunction` has several callers (def/arrow/closure/methods incl. statics). |
| VM | `src/QLangVM.cpp` | `typeMatches` + arg checks at 4 arg-placement sites + return check | Medium: hot path — everything is gated off at level 0 with one int compare. |
| Codec | `QLangBytecodeIO.*`, `docs/SERIALIZATION.md` | `.qlc` v10 function-record signatures | Medium: version bump is a clean break per precedent (v1-v8 already rejected). |
| Driver | `examples/qlcc.cpp`, `cmake/check_qlcc_cli.cmake`, `README.md` | wire `--strict`; flip guard | Low. |
| Tests | `tests/unit/strict_typing_test.cpp` (new), `codec_check`, `CMakeLists.txt`, `tests/scripts/strict_typing.ql` | see §Tests | — |
| Docs | `docs/spec/16-strict_typing.md` (+ rules table, bool row, `## rules` re-word, API note), `05-operators.md` Q28 sentence, `docs/spec/README.md` hashes, `docs/compliance/status.md` + `history.md`, `docs/plans/missing-features-plan.md` (C1/B1/B2 → planned/landed) | M0 + landing flips | Per AGENTS.md: test first, then bump hashes. |

Not affected: opcode set, instruction encoding, `.qlc` instruction section,
`CONV`/`CAST` semantics, generators' resume protocol, signals delivery order,
existing unannotated programs at any level.

## Milestones

1. **M0 — spec finalization (0.5–1 d).** ✅ **DONE 2026-11-14.** Adopt §Decisions into
   `16-strict_typing.md` (API name `setStrictTypingLevel`, constants, nil
   param/return asymmetry, bool row, level-2 exemptions, class-annotation
   identity); re-word `## rules` per Q12; align `05-operators.md` `$$`
   duplicate wording (Q28); update both `docs/spec/README.md` hash rows; update
   `missing-features-plan.md` C1 → "specified, planned (this file)".
2. **M1 — context API + warnings + dup-decl (B1/B2) + driver wiring (1 d).**
   ✅ **DONE 2026-11-14.** §1, §4 (dup-decl + `warn`), §7 shipped:
   `STRICT_*`/`setStrictTypingLevel` on `QLangContext` (clamp + warn, survives
   `clear()`), `QLangCompiler::warn` → `OUT_WARNING`, `checkDupDecl` level
   matrix in `compileDeclaration`/`compileConst`, `qlcc --strict` wired.
   `strict_typing` suite (15 checks) + `qlcc_cli` 83 → 88; compliance rows
   flipped (Round 86).
3. **M2 — grammar (1 d).** ✅ **DONE 2026-11-14.** §3 shipped: `RArrow` token,
   `TypeAnnotation` (`QLangValue.h`), shared `typeFromToken` (the two decl/
   const `tmap` blocks collapsed), `parseParamList` + `parseTypeAnnotation`
   at all four signature sites, the annotated-assignment branch (gated on
   `<builtin type> '='` so `Ident`-then-label statements still parse),
   `compileDeclaration` re-type resolution (`regOf` else bare-assign
   auto-declare, no dup-decl diagnostic — Q5). `strict_typing` 15 → 27 checks;
   the whole legacy suite is untouched (Round 87).
4. **M3 — signature plumbing + level-2 compile checks (0.5 d).** §4.
5. **M4 — VM runtime checks (1 d).** §5. The spec's §example blocks run
   verbatim as tests at this milestone.
6. **M5 — codec v10 + SERIALIZATION + `codec_check` (1 d).** §6.

Each milestone: `cmake -B build && cmake --build build && ctest --test-dir
build` green (out-of-source build; clang-format per `.clang-format`), suggested
conventional commit per milestone (`feat(strict): …`, `feat(parser): typed
parameters and return annotations`, `feat(qlc): v10 function signatures`, …) —
suggested, not committed, per AGENTS.md.

## Test plan — `tests/unit/strict_typing_test.cpp` (`add_test(NAME strict_typing …)`)

Spec examples verbatim first, then the matrix (host-driven `QLangContext` with
an output-callback capture, precedent: `output_callback_test`):

- **hello_world** — `def hello_world(name:str, age:int)`: `"24"` for int →
  throws at level 1; `112` for str → throws; `let age:long = 24` passed for int
  → throws ("too large"); retyped `age:byte = 24` passed → throws (cross-sign);
  correct call → clean. All silent at level 0.
- **sum_int2int `-> int` with no return** → throws `must return` at level 1;
  with `return a+b` (widening ok) → clean; `return nil` → throws (Q16).
- **lambda** — the `(name:str, age:int)[]->str{…}` example: call through the
  closure value (CALLCLOS) checks like a def.
- **widening matrix** — int→long ok, short→long ok, word→qword ok, long→int
  rejected, byte→int rejected, int→byte rejected, int→float rejected,
  string→int rejected, `cast<int>` of a long ok (Q17).
- **nil** — nil argument to `age:int` ok (Q13); nil return rejected (§Q16);
  valueless `local z` legal at level 2; `local a = 1` at level 2 →
  `has no declared type`.
- **dup-decl matrix** — `local a; local a`: silent / exact warning text (captured
  at `OUT_WARNING`) / exact error text, per level; `let a=1` then `a = 2` (plain
  assignment) → never a diagnostic; nested-block shadow → silent at 2.
- **level 2 signatures** — `def f(a)` → `parameter 'a' has no declared type`;
  `def f(a:int)` without `->` → legal (checked only where `->` present).
- **class annotation** — `def greet(p:Person)` with instance of `Person` ok,
  instance of `Robot` throws, non-instance throws; External value of type
  `Person` ok (only if a test meta type is cheap here — reuse a `test_external`
  style `QL_OBJECT`).
- **callFunction/callClosure** — host-invoked `ctx.call("hello_world", {"Tommy",
  "24"})` throws (Q15 coverage of the host path).
- **generator args** — a generator function with declared params checks at
  creation.

Plus: `codec_check` signature round-trip (typed def survives writeQlc/readQlc →
enforcement on the reloaded image), `check_qlcc_cli.cmake` behavior flips
(§7), and `tests/scripts/strict_typing.ql` (+ a `--strict 1` guard invocation)
as the script-level regression.

## Compliance flips at landing (AGENTS.md order: build → test → flip → re-hash)

- `docs/compliance/status.md`: strict-typing Open Item → verified with the
  checkpoint row; the deferred duplicate-declaration item (B2) closes as landed
  here; tally/hash rows updated only after `ctest` is green.
- `docs/compliance/history.md`: round entry (incl. the Q12 re-read — the
  overview's stated contradiction does not exist).
- `docs/spec/README.md` rows for `16` (and `05` if the Q28 edit landed):
  re-hash after the modules pass review.
- `docs/plans/missing-features-plan.md`: C1/B1/B2 rows → landed; this plan moves
  to `docs/plans/archive/` when closed.

## Risks / notes

1. **Bool proliferation** — comparisons yield `Bool`; strict mode will reject
   them for `:int` parameters. Accepted (the spec's type list has no bool
   coercion row); the escape hatch is `cast<int>`. Revisit only with a spec
   edit.
2. **Loaded `.qlc` before M5** — enforcement is silently absent for precompiled
   modules until v10 lands. M5 is inside this plan precisely so the feature
   never ships with that gap.
3. **Pre-existing f-string defect (found at M2, unchanged)** — f-strings with
   ≥2 interpolations inside a function body compile garbled (template lands in
   slot 2; byte-identical at HEAD). The spec's lambda example only *declares*
   the closure, so M4's verbatim call-boundary checks never execute its body;
   do not assert interpolated text in strict-typing tests until this is fixed.
3. **`CONV` stays lenient** — typed declarations coerce rather than check at
   every level (§Scope). If the spec later wants `local x:int = "abc"` to fail
   at level 1+, gate the `CONV` handler on the level and reuse `typeMatches`.
4. **Class-name annotations are strings** — a class renamed or shadowed is
   checked by name at call time; identity-by-`__class__` string is exactly how
   the engine already decides class membership.
5. **Hot path** — every added check sits behind `strictTypingLevel() >=
   STRICT_DECLARED` (default 0): one predictable branch on an int the VM keeps
   resident; no cost for unannotated programs at level 0.
