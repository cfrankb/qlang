# Plan: `++`/`--` on member and subscript targets (`t.n++`, `a[i]++`)

Status: **plan written 2026-11-15 — not started.** Promoted from the
value-semantics deferrals: `docs/compliance/history.md` Round 68 (the
`m.x++` value-semantics question), Round 96 (legal `t.n++`/`a[i]++` "stays with
the value-semantics round" — the parse-error fix left it there), Round 97
(`emitIncDec` landed for variables; "legal `t.n++`/`a[i]++` stays with the
value-semantics round"), and the spec's own placeholder sentence in
`docs/spec/05-operators.md` ("not yet bound by the parser … until the
value-semantics round lands", lines 50–52).

**Why the gate has expired:** the value-semantics arc is closed —
share-by-default for `array`/`set`/`table`/`blob`/instance + `deepcopy`
(Rounds 42–45), `shallowcopy` (Round 104), the `copy` parameter specifier
(Round 112). Nothing in this lowering needs by-value arguments or any other
outstanding semantics: an in-place member step mutates the shared cell exactly
as `t.n += 1` already does. `m.x++` (a module member) **stays refused** — that
half of the Round 68 note is the module `readonly` rule (Q30), not a
value-semantics question, and `guardNotModuleMember` already refuses it for
assignment.

## Current state, grounded

- **Parser.** Postfix `++`/`--` binds **only to a bare variable**
  (`parsePostfixChain`, `src/QLangParser.cpp:1352-1368` — the loop's
  `e->kind == ExprKind::Variable` condition, deliberately there since Round 96
  so a statement-level `++` is never swallowed as a prefix). The dedicated
  statement site (`:333-344`) requires `Ident` immediately followed by `++`.
  Prefix (`parseUnary`, `:1337-1349`) already parses `++t.n` — any operand —
  and lets the compiler reject it.
- **Compiler.** Both forms throw
  `operator '++' can only be applied to a variable name` when the operand is
  not `ExprKind::Variable` (expression-form guard, `src/QLangCompiler.cpp:4956-4973`;
  statement forms reach it by falling through `compileExprStmt` `:3258-3274`).
  The shared lowering `emitIncDec` (`:3224-3256`) handles a register or a
  capture cell, with `wantOld`/`wantNew` MOVs selecting what the expression
  yields (postfix = old, prefix = new; `-1` = statement form).
- **The shape to copy already exists.** `compileAssignTarget`
  (`src/QLangCompiler.cpp:6216+`) lowers `a[i] += v` and `t.k += v` as
  read-modify-write: base register (captured base through `REFLOAD`), key
  (evaluated index / member-name const), `INDEXGET`/`TABGET` into a temp, the
  in-place step (`INC`/`DEC` are literally the `op.text == "++"`/`"--"` pick in
  both branches), `INDEXPUT`/`TABPUT` back. The Member branch also carries the
  manifest write checks (no field / const field / method) and
  `guardNotModuleMember`.
- **VM.** No change: `INC`/`DEC` step a register in place, `INDEXGET`/`INDEXPUT`
  dispatch on the runtime base type (Round 51), `TABGET`/`TABPUT` route through
  `ObjectOps` — which is where class const-field writes already fail with
  `cannot assign to const field 'x'`.
- No opcode, no `.qlc`, no codec change.

## Design

Bind `++`/`--` to a target of kind **Variable, Member, or Index with a
non-empty subscript**, base restricted to a **bare variable or captured
variable** (exactly the assignment-target grammar — `f().n++` and chained
`a.b[0]++` stay refused, see Deferred), in all four shapes the variables
support: statement and value position, prefix and postfix.

1. **Parser** (`parsePostfixChain`): after the member/subscript chain is
   built, bind a following `++`/`--` when the chain's kind is `Variable`,
   `Member`, or `Index` with a non-null `indexExpr`; otherwise leave the token
   (the Round 96 anti-swallow guard survives — `(a+b)++` still doesn't bind
   and keeps falling into the compiler's existing report). The statement-form
   site (`:333`) needs no change: `t.n++;` parses as an expression statement
   once the chain binds its postfix.
2. **Compiler.** New `emitIncDecTarget(const ExprPtr &target, Opcode step,
   int valueReg, bool isPostfix)` next to `emitIncDec`: base register (local /
   captured-`REFLOAD`, same as `compileAssignTarget`), key,
   `TABGET`/`INDEXGET` → temp, `wantOld` MOV, `INC`/`DEC` on the temp,
   `wantNew` MOV, `TABPUT`/`INDEXPUT`. Reuse the Member branch's guard set
   verbatim (`guardNotModuleMember`, manifest no-field / const-field / method
   checks). Do **not** refactor `compileAssignTarget` to share it — the two
   differ in the value expression vs the wantOld/wantNew MOVs; touching the
   shipped, tested compound path for ~10 similar lines is a bigger risk than
   the duplication (pin the equivalence with a test instead: `t.n++` leaves
   the same field value as `t.n += 1`).
3. **Sites to widen.** `compileExprStmt` (`:3258-3274`): the statement-form
   fast path accepts `Member`/`Index` operands and routes to
   `emitIncDecTarget` with `valueReg = -1`. Expression form (`:4956-4973`):
   the guard accepts the same operand kinds and routes with the real `dest`.
   Both keep the existing message for every other operand kind, so
   `(a+b)++`, `f()++` and `a[]++` (empty subscript is not a readable target)
   are still reported — Q49 decides that message's wording.
4. **Runtime semantics, inherited not invented:** string/bool/named targets
   step through `INC`/`DEC` → the binary ADD/SUB rules (type errors as today);
   class const fields and closed-field rules fail in `ObjectOps` with the
   assignment wording; external properties write back through the member's
   `set` lambda with its conversions; a missing table member steps `nil` →
   `operator '+' operand must be numeric, got nil` (same as `t.missing += 1`).

## Open questions (defaults recommended)

- **Q48 shape set.** Statement + value position, prefix + postfix, over
  Member + non-empty Index. **Default: all four** — symmetric with `+=` and
  with the already-specified variable semantics; the spec text rewrite (M0)
  covers them.
- **Q49 `a[]++` and expression bases.** Empty subscript (`a[]++`) has no
  readable cell to step; `f().n++` / `a.b[0]++` / `a[i][j]++` have a base the
  assignment machinery does not evaluate-once either. **Default: all keep the
  current report** `operator '++' can only be applied to a variable name` —
  imperfect wording for `a[]++`, but a second message is a second thing to
  spec; refine only if it confuses someone.
- **Q50 guard parity with assignment.** Same guards, same messages —
  including runtime `cannot assign to const field 'x'` for a class const
  field stepped with `++` (the message says *assign* while the source says
  `++`). **Default: reuse the runtime wording** — it is the single `ObjectOps`
  site that `t.n += 1` hits too; diverging for `++` alone would fork the
  diagnostics.
- **Q51 `m.x++` (module-qualified member).** Stays a compile error via
  `guardNotModuleMember` — the Q30 importer-side readonly rule, unchanged.
  The defining module stepping its *own* exported variable works (it is an
  ordinary global register; `guardNotConst` still refuses its consts).
  **Default: as decided here, nothing to land.**

## Milestones

- **M0 — spec rewrite (with the code, committed ahead, per the C1-M0
  precedent).** `docs/spec/05-operators.md` `## operators ++ and --`: delete
  the "not yet bound by the parser … value-semantics round" paragraph; state
  that the operand is a variable name, a table/class member (`t.n`) or a
  subscript (`a[i]`), evaluated once, stepped in place, obeying the same
  guards as assignment; postfix yields the value before the step, prefix the
  value after. Re-record the hash per AGENTS.md after tests pass (M2).
- **M1 — the binding.** Parser (step 1), `emitIncDecTarget` + the two widen
  sites (steps 2–3), no VM/codec change. Failing tests first (red on HEAD):
  - `spec_check2`: statement forms `t.n++`, `++t.n`, `a[i]++`, `++a[i]`;
    value forms `z = t.n++` (old) / `p = ++t.n` (new) and the array pair;
    captured-base forms inside a closure; the Q48 equivalence pin
    (`t.n++` ≡ `t.n += 1` field value); `a[i].n++`-style chain *as a base*
    stays refused (Q49); `(a+b)++`, `f()++`, `a[]++` keep the existing
    message; string-member and missing-member runtime errors pinned.
  - class instances (the 42-case classes block): field `++` works, `const`
    field reports `cannot assign to const field`, a method name target is
    refused.
  - `external` suite: `e.options++` steps through the property's `set`
    (round-trips the property's conversions).
  - corpus/`codec_check`: unchanged — no new opcode, no new lowering for
    existing programs.
- **M2 — compliance round (per AGENTS.md).** Rebuild + full `ctest`; flip the
  `05-operators.md` row in `docs/compliance/status.md` and the spec hash in
  `docs/spec/README.md`; append the round to `history.md`; mark the Round
  96/97 deferred lines closed in `todo.md` §6 and remove the matching
  deferred row from `missing-features-plan.md` (module deferred table: the
  "`m.x++` refused" half stays closed by Q51); archive this plan.

## Files touched (expected)

`src/QLangParser.cpp` · `src/QLangCompiler.cpp` / `include/qlang/QLangCompiler.h` ·
`tests/unit/spec_check2.cpp` · `tests/unit/test_external.cpp` ·
`docs/spec/05-operators.md` · `docs/spec/README.md` (hash) ·
`docs/compliance/{status,history}.md` · `docs/plans/{todo,missing-features-plan}.md`.

## Deferred (named, with triggers)

- **Expression and chained bases** (`f().n++`, `a.b[0]++`, `a[i][j]++`) —
  assignment doesn't evaluate-once those either; both `++` and `+=` should
  gain them in one round when the assignment target grammar widens.
- **`a[]++` append-then-step** — no readable cell exists pre-append; needs a
  spec sentence before it can mean anything (Q49 keeps it refused).
- **`m.x++`** — permanently refused by Q30 unless the spec revisits
  importer-side writes; not this round's decision to make.
