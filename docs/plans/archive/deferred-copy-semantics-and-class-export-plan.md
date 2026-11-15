# Plan: shallowcopy, `copy` parameter specifier, class exports (F4 / Q42)

Adopted: 2026-11-14 (post-Round 94). Status: **plan fully landed — archived (Round 112,
2026-11-15)**. Phase 0 landed (Round 104, Q42 + the class-export
spec sentences, plus the `copy` spec sentence in `docs/spec/06-functions.md` `### copy
parameters` with Phase 2); Phase 1 landed (Round 105, `shallowcopy`); Phase 3 superseded by
[class-export-plan.md](class-export-plan.md) and landed with it (Rounds 104–107, F4
closed); Phase 2 (`copy` specifier) **landed Round 112** — contextual keyword in
`parseParamList`, in-place `DEEPCOPY p, p` prologue in `buildFunction` (no new opcode, no
`.qlc` change), spec `### copy parameters`, guards `spec_check2` +14 + `copy_param.ql`.

Provenance of the four items (all *explicitly deferred by decision*, never by omission):

| item | deferred by | promotion trigger that fired |
|---|---|---|
| `shallowcopy` | `deepcopy-plan.md` open decision #5 — "cheap and useful; still deferred" | the copy-semantics round (this plan) |
| `copy` param specifier (Option D) | `deepcopy-plan.md` #6 — "natural later sugar" over the shipped `deepcopy` builtin | same; also the module plan's deferred row "**by-value arguments across a module boundary** — promote when: the value-semantics round" (`module-export-import-plan.md`) |
| class exports (audit **F4**) | `docs/bugs/module-export-import-audit.md` F4, `module-export-import-plan.md` deferred table | this plan (Phase 3) |
| **Q42** (may a `.ql` module export a class? "Default: no, unanswered — needs a spec sentence first") | same records; `todo.md` §"owned Q39–Q43 (all resolved except Q42)" | Q42 **is** F4's spec decision — Phase 0 answers it, Phase 3 lands it |

## Baseline facts (verified Round 94, 2026-11-14)

- `.qlc` is **v10** (`QLangBytecodeIO.h:38`); `kOpcodeUsedCount = 74` (defined identities 0..73),
  so a new opcode is identity **74**. `Builtin::Deepcopy` is the last builtin — `Shallowcopy`
  gets the next id.
- `deepcopy(x)` lowers **directly** to `Opcode::DEEPCOPY` (no `CALL`; the `len`→`LEN`
  precedent), arity is a compile error, VM `QLangVM::deepCopyValue` (`src/QLangVM.cpp:1181`)
  recurses with a payload-address identity map and dispatches `__deepcopy__` on the original.
- Value semantics (`01-types.md` `## value semantics`): `array`/`set`/`table`/`blob`/instance
  **share** by handle since Phase 2 (Round 43); `string` is the immutable copy exception;
  `deepcopy()` is the only independent copy.
- Appending an opcode does **not** bump `.qlc` (Round 51 `indexget`/`indexput` shipped on v5;
  `deepcopy`/`nop`/`EMIT` likewise) — versions move for layout/breaking changes only.
- Classes are a **compile-time-only** concept per module: registry `m_classes`
  (`QLangCompiler.cpp:4138`), construction is `m_classes.find(e->callee)`
  (`QLangCompiler.cpp:4781`), the prototype is a synthetic `Name_prototype` function +
  `__proto_Name` global; instances share the class meta keys and carry the type-name string
  in `__class__`. External types already have the asymmetric shape F4 needs: **compile-time**
  arity checks for source classes, **binding-table/runtime** arity for external types
  (`10-classes.md` `## construction`).
- `export` parses only before `def` / `const` / assignment (guard at `QLangCompiler.cpp:1699`);
  the export table is `ExportEntry {kind ∈ {Var, Const, Func}, name, ref}` (`SERIALIZATION.md`
  §4.6, `.qlc` v9+).
- `copy` is **not** a reserved word (`02-lexical.md`); the import-alias `as` is the contextual
  keyword precedent.
- New AGENTS rule: `editors/vscode/` config must track the latest reserved words/keywords —
  every phase below ends with a check of `qlang.tmLanguage.json` / `language-configuration.json`.

## Phase 0 — spec decisions (docs-only gate, own commit)

F4 is blocked on Q42, and the `copy` specifier is language syntax; fix the spec text before
code. Three spec-module edits (re-hash after green):

1. `01-types.md` `## value semantics` + `06-functions.md`: add `shallowcopy(x)` —
   top-level copy, elements/fields **shared by handle**; for `blob` (bytes are not shared
   handles) shallow ≡ deep; `__deepcopy__` is **not** consulted (the hook is the deep-copy
   contract); identity/cycle sharing falls out of non-recursion.
2. `06-functions.md`: `def f(copy x)` — the argument is deep-copied into the parameter at
   call entry; `copy` is a **contextual** keyword in parameter position only (precedent `as`;
   no `02-lexical.md` reserved-words change); not valid on `self`, on captures (`[x]` cells),
   or on arrow-closure params.
3. `09-modules.md` + `13-bytecodes.md` `## keyword export` / `## export table (lookup)`:
   **Q42 resolved: yes** — `export class Name` is legal; the export table gains kind `Class`.
   The one sentence that was missing: a class export exposes the **constructor value**;
   construction across the boundary is the same shape external types already have —
   local classes keep compile-time arity errors, **imported** classes get runtime-checked
   construction (same asymmetry as `import<T>()` TypeRefs, `10-classes.md` `## construction`).
   `deepcopy` of an instance needs **no** cross-boundary rule: an instance is a table whose
   class meta keys are shared handles — already spec'd behavior. Imported-class **statics**
   (`m.Point::counter`) stay out of scope.

## Phase 1 — `shallowcopy` (no language change; one round) — **LANDED Round 105, 2026-11-15**

- `Opcode::SHALLOWCOPY` appended last (identity 74); `Builtin::Shallowcopy`; lower
  `shallowcopy(x)` directly to the opcode mirroring the `deepcopy` lowering, arity compile
  error (`shallowcopy expects exactly one argument, got N`). `.qlc` stays **v10**.
- `QLangVM::shallowCopyValue`, a ~20-line non-recursive sibling of `deepCopyValue`:
  nil/primitives/string/blob/primitives per Phase 0 wording; array/set/table build a new top
  container and insert elements/values **as-is** (handles shared, scalars already independent);
  instance → new instance table, meta shared, fields as-is, `attachFinalizer` applied;
  `Function`/closure/generator/`Object`/external/bound-method/TypeRef share the handle
  (identical to `deepCopyValue`'s share-list); **no** `seen` map, **no** hook dispatch.
- Guards: `value_semantics` +~10 checks (top independence for array/table; element aliasing
  visible through the copy; instance field aliased; hook present-but-ignored; arity compile
  errors), `codec_check` identity/round-trip/direct-lowering cases (count grows from the
  Round 94 baseline).

## Phase 2 — `copy` parameter specifier (one round)

- Parser: in the parameter list, an identifier `copy` followed by an identifier is consumed
  as a specifier (contextual, like `as`); typed params read `copy x:type`.
- Codegen: function prologue emits `DEEPCOPY p, p` (in-place, the parameter register) right
  after argument binding. **No new opcode, no `.qlc` change** — this is sugar over shipped
  `deepcopy`, exactly as decision #6 framed it. Call sites untouched, so imported defs get it
  free: `export def f(copy x)` is the module-boundary by-value answer the module plan deferred.
- Errors: `copy` on `self`, on a capture, or on an arrow-closure param → parse error
  (`copy is not allowed here`); the specifier never appears in the export table or signatures.
- Guards: `spec_check2` +~8 cases (caller's array mutation-invisible; rebind-invisible;
  primitive/string param behaves identically with/without; method param; negative parses),
  one script regression `copy_param.ql`.
- Editors check: contextual word, `as`-class — verify the tmLanguage grammar needs nothing
  beyond confirming `as` itself is also unlisted (stay consistent).

## Phase 3 — class exports (F4; two rounds, lands Phase 0's Q42 sentence)

**P3a (export side).** Allow `export class` at the guard (`QLangCompiler.cpp:1699`); the export
pre-pass adds `ExportEntry {Class, name, ref = prototype fn index}`; the class meta the
importer needs (name, field list, `__init__` arity) travels as the existing prototype-record
reachability — resolve during P3a whether `ExportEntry` grows a small fixed meta block or the
linker reads it off the merged program. Either way the kind field gains a fourth value and its
load-time range check must reject it on old readers → **`.qlc` v10 → v11**, breaking bump,
clean-break precedent (Round 60): `SERIALIZATION.md` §4.6 + version matrix updated same round.

**P3b (import side).** `from leaf import Point` / `m.Point(args)` seed a *reduced* local
`m_classes` record (name, arity, fields — marked **imported**): compile-time arity checking
is skipped for imported classes, construction lowers to the **runtime-checked dynamic shape**
external types already use (`10-classes.md` `## construction` already distinguishes the two).
`type(p)` keeps naming the class (`__class__` already holds the name string). Imported-class
statics, inheritance-like patterns, and identity (`is-a` beyond `type()`) stay deferred —
record in this plan's table below.

- Guards: `test_module` / `module_search` cases (export table kind `Class`, qualified and
  `from`-import construction, runtime arity error message names the class, readonly rules
  unchanged), `.qlc` v11 matrix in `codec_check` (v10 rejected at the version field), new
  script regression `class_export.ql`.

## Phase 4 — tracking (with the phase that closes each)

Flip: `missing-features-plan.md` F4 row + Q42; `todo.md` Q42 clause ("all resolved except
Q42"); `deepcopy-plan.md` stays archived (its #5/#6 pointer now leads to a landed
plan — note it here, not there); `status.md` value-semantics bullet gains the follow-ups;
`history.md` entries; `tests.md`/`harness.md` tallies re-recorded **as measured**;
`update-doc-hashes.sh` after green, never before. Editors config check per AGENTS rule.

## Explicitly deferred (promote on the stated trigger)

| item | why not now | promote when |
|---|---|---|
| `copy` on captures / arrow-closure params | captures are cells shared by design; a copy-per-call of a capture is a different feature | someone designs module-built closures (the module plan's cross-module-captures row) |
| imported-class **statics** (`m.Point::x`) | needs the statics reachable without the full compile-time record | a real library ships a class with counters |
| `m.x++` silent no-op on module consts | orthogonal (assignment-to-module-var), tracked in the module plan's deferred table | its own diagnostic round |
| `frozen`/immutability | `deepcopy-plan.md` out-of-scope list, unchanged | nobody has asked yet |

## Verification (AGENTS.md, every phase)

`cmake -B build && cmake --build build`, `ctest --test-dir build` (67/67 at the Round 94
baseline; P3 adds script/codec entries); Release `perf` gate expected untouched (no stream
change for existing scripts; v11 changes files, not execution); doc-hash gate green after
`bash update-doc-hashes.sh` on spec-touching rounds. Commits: one per phase, docs sync in
the phase's commit or its own docs commit — stage by concern.
