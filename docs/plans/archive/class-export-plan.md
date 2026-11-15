# Plan: F4 — class exports (`export class`, Q42)

Status: **fully landed — Phase 0 (Round 104), clone round (Round 105, D2 `shallowcopy`),
P3a (Round 106, `.qlc` v11), P3b (Round 107, 2026-11-15, importer-side construction)**;
archived per AGENTS (fully implemented). Drafted in Plan mode
2026-11-15; baseline HEAD `cc4744a`,
Round 98). Supersedes the class-export sections (Phase 0 item 3, Phase 3) of
`docs/plans/deferred-copy-semantics-and-class-export-plan.md`. Reference spec:
`docs/spec/09-modules.md` (`## keyword export`), `docs/spec/13-bytecodes.md`
(`## export table (lookup)`), `docs/spec/10-classes.md` (`## construction`),
`docs/SERIALIZATION.md` §4.6.

## Baseline facts (verified 2026-11-15 against HEAD `cc4744a`)

- `.qlc` is **v10** (`QLangBytecodeIO.h:38`); `kOpcodeUsedCount = 74` — next
  opcode identity is **74**.
- Export machinery: `ExportKind {Var=0, Const=1, Func=2}` +
  `ExportEntry{kind, name, ref}` (`QLangBytecode.h:57-70`); `ref` = function
  index (Func) or **global-slot register** (Var/Const). Collected by
  `collectExports` + `exportableDecl` (`QLangCompiler.cpp:1600-1775`), resolved
  in `finishExports`, kind range-checked at load (`QLangBytecodeIO.cpp:733,761`
  — both reject `kind > Func`), rebased in the linker (`QLangLinker.cpp:385,447`:
  Func → `funcBase`, everything else → `regBase` **cell**). Module `<init>`s run
  before consumers (Q35), so exported cells are populated before reads.
- Classes are compile-time-only per module: `m_classes` → `ClassRecord`
  (`QLangCompiler.h:683`: prototype fn index, `methodNames/methodArities/
  fieldNames/staticFields/constFields/fieldTypes`); the prototype table is
  cached in the ordinary module global **`__proto_<Name>`**
  (`QLangCompiler.cpp:4209`). Construction (`QLangCompiler.cpp:5337-5490`) =
  `TABCREATE` + per-name `TABGET/TABPUT` method + field-default copies +
  `__statics__` / `__const_fields__` / `__field_types__` / `__fields__` copied
  **by reference** + `__class__` name stamp + `CALLFN` to `__init__` with
  compile-time arity check (Phase 4.3/4.4: no `__init__` ⇒ zero-arg ctor).
- Parser guard: `QLangParser.cpp:958-964` accepts prefix `export` only before
  Def/Const/Declaration/Assignment/Arrow; `export can only prefix def, const or
  a variable assignment`.
- Key structural fact: the prototype table's *content* is exactly what an
  instance needs (methods, field defaults, meta tables — all shared by handle by
  the local construction path anyway), so **a shallow clone of the prototype +
  a `__class__` stamp reproduces a locally-constructed instance layout for any
  class, without the importer knowing any method/field names.** That is what
  makes F4 cheap.

## Phase 0 — spec decisions first (docs-only, own commit)

1. `09-modules.md` + `13-bytecodes.md` (`## keyword export`,
   `## export table (lookup)`): **Q42 resolved — `export class Name` is legal**;
   export table gains kind `Class`; a class export exposes the **constructor**;
   construction across the boundary uses the class meta carried by the export
   table (decision D1); imported-class **statics** (`m.Point::counter`)
   explicitly out of scope.
2. `10-classes.md` `## construction`: imported classes construct like local
   ones — field defaults **shared by handle** (identical to local instances,
   *not* deep-copied); `type()` keeps naming the class (`__class__` already
   holds the name string).
3. `bash update-doc-hashes.sh` after green, never before.

## Design decisions (settle before coding)

- **D1 — the `Class` export entry carries a small fixed meta block:
  `{initArity}`** (one int; −1 = no `__init__`). Recommended. Unlike external
  types, the meta is present in the `.qlc` at import-compile time, so the
  importer keeps the **compile-time** constructor-arity error
  (`Point(1) → 'Point' expects 2 argument(s)`); runtime is only a defensive
  fallback. This supersedes the deferred draft's "runtime-checked like external
  types" asymmetry, which only held for a no-meta design.
  Lazy alternative (rejected default): no meta, runtime-only arity from the VM
  call path.
- **D2 — construction primitive: reuse the deferred plan's Phase 1
  `shallowcopy`** (opcode identity 74, `Builtin::Shallowcopy`, ~20-line
  `shallowCopyValue` sibling of `deepCopyValue`) as its **own round first**;
  then imported-class construction lowers to `SHALLOWCOPY instance, proto` and
  clone semantics get pinned independently by the `value_semantics` suite.
  Alternative if F4 must be self-contained: a dedicated `NEWCLASSINSTANCE`
  opcode doing clone+stamp in the VM (more VM surface, nothing reusable).
- **D3 — `.qlc` v10 → v11 (breaking bump).** With D1 the `ExportEntry` record
  layout genuinely grows → clean-break precedent (Round 60):
  `SERIALIZATION.md` §4.6 + `codec_check` version matrix (v10 rejected at the
  version field) land in the same round. A no-bump (kind-domain extension only)
  is only coherent if D1 is rejected; default here is the bump.

## Phase P3a — export side (one round)

1. Parser: add `StmtKind::Class` to the prefix-export guard
   (`QLangParser.cpp:958`); bare `export Point` (an `ExportRef`) resolves via
   `exportableDecl` gaining `case StmtKind::Class → kind Class,
   name = s->className`; `rejectNestedExports` already walks any top-level stmt.
2. `ExportKind::Class = 3`; `ExportEntry` gains `initArity` (D1);
   serialization writes/reads it, widening the kind range checks to 3
   (`QLangBytecodeIO.cpp:733,761`); v11 bump + matrix (D3).
3. `finishExports`: `kind == Class` → `ref = m_regs["__proto_" + name]` (a
   register/cell — the linker's non-Func branch (`QLangLinker.cpp:447`) rebases
   it as a cell for free).
4. Negative guards: writes/rebinding a class name stay rejected
   (`QLangCompiler.cpp:5703` already treats classes as non-values);
   `guardNotExportedFunc` untouched (a class is not a func).
5. Checks: `test_module` export-table kind-`Class` round-trip; `codec_check`
   v11 matrix, kind 3 round-trip, kind 4 rejected.

## Phase P3b — import side (one round; after P3a and after D2's clone round)

1. `from leaf import Point` and `import m; m.Point(…)`: the import site
   (`moduleMember`, the `m_imports` `ImportEntry`) accepts kind `Class`,
   carrying ref(cell) + `initArity`. Audit every `kind == Func`-vs-else
   dispatch so Class flows through the **cell** path with its own explicit
   branches: `QLangCompiler.cpp:3384, 3505, 3517, 4463, 4760`,
   `QLangLinker.cpp:385`, `QLangVM.cpp:2686, 2702`.
2. **Non-construction uses are compile errors**: `x = Point`, passing `Point`
   as a value, `m.Point` outside a call position →
   `class 'Point' must be constructed: Point(args)` (Variable-load and
   qualified-member sites). GETIMPORT on a Class import stays
   compiler-unreachable; VM unchanged.
3. Construction lowering (Call case, next to `:5337` and the `moduleQualified`
   path): `GETIMPORT protoR` → `SHALLOWCOPY instR, protoR` → `TABPUT __class__
   = "<Name>"` (pool string) → compile-time arity check from meta → `TABGET
   __init__` + dynamic call with self+args (no `__init__` ⇒ zero-arg rule,
   matching Phase 4.4). **Checkpoint:** confirm the post-clone `TABPUT
   __class__` is not gated by closed-field enforcement (the local path stamps
   via raw `TABPUT`s; enforcement lives at SETFIELD level) — if it is gated,
   stamp inside the clone helper.
4. Method calls on imported instances (`p.translate()`) work unchanged (method
   values live in the cloned table); their **arities become runtime-checked**
   (VM call path) — compile-time would need full `methodArities` in the export
   table → deferred.
5. Checks: `test_module` + `module_search` (both spellings construct, `type(p)`,
   field-handle sharing between two imported instances, cross-module class whose
   `__init__` reads a module const — F1's shape again); new script
   `tests/scripts/class_export.ql`; compile-time arity errors name the class.

## Explicitly deferred (recorded, not silent)

| item | promote when |
|---|---|
| imported-class **statics** (`m.Point::counter`) | a real library ships a class with counters |
| compile-time **method** arities for imported classes | cross-module dispatch bugs demand it |
| identity / `is-a` beyond `type()`; inheritance | the inheritance round (**Q21**, C5) |

## Verification (AGENTS.md, every round)

`cmake -B build && cmake --build build`; `ctest --test-dir build` (67/67
baseline; P3 adds script + codec entries); Release `perf` gate expected
untouched (v11 changes files, not execution); `bash update-doc-hashes.sh` after
green; editors-config check per AGENTS (no new reserved words — `class`/`export`
already keyworded); `history.md` entry + `tests.md`/`harness.md` tallies
re-recorded **as measured**.

## Tracking flips (with the round that closes each) — **all done, Round 107**

- `docs/bugs/module-export-import-audit.md`: F4 status line → fixed.
- `docs/plans/missing-features-plan.md`: F4 row + Q42 → landed.
- `docs/plans/todo.md`: Q42 clause ("all resolved except Q42") → all resolved;
  §5/§6 pointer to this plan closed.
- `docs/plans/deferred-copy-semantics-and-class-export-plan.md`: its Phase 0
  item 3 / Phase 3 sections replaced by this plan (note in place; plan moves to
  `docs/plans/archive/` when its own remaining phases land).
- `docs/compliance/status.md` / `history.md` / `tests.md` per round.

## Suggested commit sequence

1. `docs(spec): resolve Q42 — export class Name is legal (F4 Phase 0)` — **landed Round 104**
2. `feat(vm): shallowcopy builtin — top-level copy, elements shared (clone round)` *(D2)* — **landed Round 105**
3. `feat(compiler): export class — export table kind Class + initArity meta, .qlc v11 (F4 P3a)` — **landed Round 106**
4. `feat(compiler): construct imported classes from the exported prototype (F4 P3b)`
