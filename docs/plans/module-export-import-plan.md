# Plan: module export/import gaps (post-C8b audit)

Status: **plan — not started**.
Baseline `HEAD = 87356aa` ("C8b: compile a discovered .ql into a module image");
`cmake -B build` clean, `ctest --test-dir build` **56/56** green including `doc_hashes`.
Findings this plan executes: [../bugs/module-export-import-audit.md](../bugs/module-export-import-audit.md)
(F1–F6, probed 2026-11-10). Parent plans:
[archive/external-modules-plan-v4.md](archive/external-modules-plan-v4.md) (Phases 1–3c, done,
Round 65) and [missing-features-plan.md](missing-features-plan.md) **C8** (C8a Round 67, C8b
Round 69) — this plan is the *after* C8 work: what `export`/`import` still cannot do.
Spec: [../spec/09-modules.md](../spec/09-modules.md) (`## import statement`, export rules),
[../spec/13-bytecodes.md](../spec/13-bytecodes.md) (`## keyword export`, `## export table (lookup)`,
`## import table (lookup)`, `## technical specifications` 1 and 5),
[../spec/07-api.md](../spec/07-api.md) §module search paths (C8b contract, unchanged here).

Sequencing is not optional: **P0 is a blocker for the others.** P1/P2 are compiler-side binding
changes and both want to assert values across a module boundary, which F1 makes unreadable.

---

## P0 — one identity for a module-level const (F1) — **defect, do first**

A module-level `const` that a `def` of the same module reads is unreachable at run time: the `def`
returns nil for it and its exported cell reads nil through an import, while the module's own `<init>`
sees it (F1's matrix in the findings doc). The observable rule to establish: **a top-level name has
one cell, and every reference — `<init>` store, in-module `def` read, export ref, importer
`getimport` — names it.**

**Phase 0.1 — pin it, do not guess.** The static tables are already self-consistent (findings doc,
"what the merged image says"), so the divergence is only visible at run time. Instrument the F1
pair for one run and print, in order: `CompiledProgram::initSequence` (`funcIndex`, `regBase`,
`width`), each module export `ref`, each merged import entry `ref`, `tag`'s `Function::captures`,
and `globalTable().slots[regBase-1 .. regBase+width]` immediately after the init sequence and again
after main's first `getimport` (`src/QLangVM.cpp:1993-2004`, `:2540-2556`). Two outcomes:

- *the slot the store named is not the slot the export/capture named* → the compiler gave the const
  two identities: the frame register from `declareVar` (`src/QLangCompiler.cpp:670-690`) versus
  `m_globalRegMap[name] = m_scope->globalSlotOf(name)` recorded when the capture was built
  (`:3800-3828`, used at `:1817-1824`). Fix there.
- *one slot, and it is nil at the reader* → the store is not reaching the table cell the init frame
  claims; fix in the `<init>` frame/`ModuleInit` path (`src/QLangVM.cpp:1993-2004`).

**Phase 0.2 — fix in the compiler, not the linker.** Preferred shape (audit's option 2, **Q39**): in
**library mode**, a module-level immutable cell reached from a `def` is named the same way `global`
already names a module var — an absolute cell operand that the linker's existing
`-1 - slot → -1 - (regBase + slot)` rebase (`src/QLangLinker.cpp:117-131`, `:300-306`) already
handles — instead of the `immutableCell` frame-relative special case
(`src/QLangCompiler.cpp:1806-1824`, whose comment "At module top level the register IS the slot" is
true only for a script's main frame at base 0). Whatever 0.1 proves, the change belongs to the side
that records the number; the linker keeps rebasing what it is given, and `GETIMPORT` is already
correct.

**Phase 0.3 — kill the silent-nil path.** A module-level name a `def` cannot reach must be a
compile error in library mode (the mutable case already reports `use of undeclared variable`,
findings F1/p14, F5). Never emit a read that folds to nil.

**Guards.** `tests/unit/linker_test.cpp` (ctest `linker`, 130 checks): a module whose `def` reads a
module-level `const` **and** exports it — assert the merged run prints the value through all three
readers (`m.CONST`, `from m import CONST`, `m.f()`), and assert `exports["CONST"].ref` equals the
slot the init frame writes. The existing `tally` case cannot catch this: its `def` uses
`global counter` and never reads a const.
Script regression `tests/scripts/ql_module_const.ql` + fixture `tests/scripts/ql_module_const_dep.ql`
(registered in `QL_SCRIPTS`), because F1 only shows through the driver: discovery → nested compile →
`writeQlc`/`loadProgram` → link → run.

## P1 — `from *` must not collide on identity (F2)

`from a import *` + `from b import *` is a guaranteed error today, because both modules export
`MODULE_NAME` as an ordinary const (`'MODULE_NAME' is already imported from module 'a'`).
**Q40** default: `MODULE_NAME` is **identity, not an entity** — the compiler skips it when expanding
`*`; it stays reachable as `m.MODULE_NAME` and in the export table (the linker and `--asm-level 5`
keep seeing it, `--import`/discovery identity checks are untouched). Then a spec sentence in
`09-modules.md` says so, since "every public name `m` exposes" currently means the opposite.
One filter in the star-expansion, one `09-modules.md` sentence, one `linker_test` case (two
star-imported modules sharing nothing else bind cleanly; an explicit
`from a import MODULE_NAME` still binds), and a script regression that star-imports two modules.

## P2 — per-name alias `from m import f as g` (F3)

Spec requirement that has never parsed (`13-bytecodes.md` `## technical specifications` 5; recorded
not fixed in `history.md` Round 68; `ImportEntry::alias` is dead). Scope, deliberately small:

- parser: `from m import a, b as c, d` — `as` binds to the **one** name it follows; the module name
  `m` is not bound by a `from` form (unchanged rule).
- compiler: the alias is the importing module's binding name only. Duplicate/unknown-name
  diagnostics report the alias and collide on it. Qualified access keeps the original name.
- **no image change** (**Q41**): a table entry keeps naming *(defining module, exported name)*;
  the alias is a compile-time binding, so `.qlc` stays **v9** and `ImportEntry::alias` stays unused
  — delete the field rather than feed it (findings F3). `09-modules.md`'s "is not parsed" sentence
  is replaced by the form + its row in the `## import statement` table.
- guard: `tests/unit/module_search_test.cpp` (ctest `module_search`, 103 checks) — the C8 import-form
  guards live there; `--asm-level 5` listing keeps showing the defining module + exported name.

## P3 — two diagnostics (F5)

- A qualified read of a module that is loaded but **not imported here** must say so
  (`module 'leaf' is not imported in this module`; the current advice, "reach what it offers as
  `leaf.<name>`", is the thing the code is already doing).
- Library-mode reachability errors report instead of folding to nil — covered by P0.3, listed here
  so the message work lands in one place.

## Explicitly deferred (with the trigger that would promote each)

| item | why not now | promote when |
|---|---|---|
| **runtime module registry** (`13-bytecodes.md` `## technical specifications` 1, second link model): `QLangContext` has no reference to `QLangLinker`, `ctx.run()` never links | `getimport`/`callimport` already execute unmerged and the merge path is proven; the registry is a *host API* decision (who owns images, when they init), not a hole in `export`/`import` | a host needs to run images without `QLangLinker` (embedder request, or `qlang_run` gains `-r` for images) |
| **export a class** (F4) | `## keyword export` lists functions/variables/constants; a type export needs a spec decision (instance construction across the boundary, `deepcopy` rules) | the spec adds it (**Q42** stays open until then) |
| **cross-module captures** — an exported `def` whose sibling `def` has *local* captures is refused | refusal is correct and diagnosed; making it work means shipping the defining module's frame with the call | someone needs a module built around closures, and it should be its own design round |
| **by-value arguments across a module boundary**, `m.x++` silent no-op | both recorded in `history.md` (Round 68) as value-semantics questions, orthogonal to what an `import` binds | the value-semantics round |
| **Q36 warn on `MODULE_NAME` ≠ file stem** | nothing warns for `.qlc` either; a warning-only feature with no diagnostic channel | a warnings channel exists (C1 owns strict-typing levels) |
| **a library's own directory in its dependency search** | documented limit (`--help`, `07-api.md` §module search paths); the search list belongs to the context, so a fix needs a resolver-side stack of current module dirs | a real project hits it (nested third-party libs outside the entry dir) |

## Open questions — defaults adopted (numbering continues `missing-features-plan.md`)

- **Q39** (P0) which layer owns the fix — compiler identity, VM init frame, or linker? **Default: the
  compiler**, per Phase 0.1's fork; the linker's contract ("rebase what the compiler recorded") and
  `GETIMPORT` are already right. Change the answer only on instrumentation, not on reading.
- **Q40** (P1) is `MODULE_NAME` a public export? **Default: no for `import *`, yes for `m.MODULE_NAME`
  and the export table.**
- **Q41** (P2) does an aliased import need image/format support? **Default: no** — compile-time
  binding, `.qlc` stays v9, `ImportEntry::alias` gets deleted.
- **Q42** (deferred) may a `.ql` module export a class? **Default: no, unanswered — needs a spec
  sentence first.**
- **Q43** (P3) should the F1 shape ever compile? **Default: no** — an unreachable module-level name
  in a `def` is an error in library mode, matching the mutable case.

## Verification (AGENTS.md, every phase)

- `cmake -B build && cmake --build build`, then `ctest --test-dir build` — **56/56** must stay green
  (P0 adds `script_ql_module_const`, so the count moves).
- Single suites while iterating: `build/linker_test`, `build/module_search_test`.
- F1's own reproduction is the acceptance test: the F1 pair must print `[tagged] / [two] / [three]`
  and `fn:tagged`; P1's pair must print both star-imported names; P2's must print through `g`.
- Docs that need a row/hash refresh after a verified change: `docs/spec/09-modules.md` (P1, P2),
  `docs/spec/13-bytecodes.md` (P2 — `## technical specifications` 5 flips to implemented),
  `docs/compliance/{status,tests,harness}.md`, `docs/compliance/history.md` (a new round entry),
  `docs/plans/todo.md` §6, then `./update-doc-hashes.sh`. Never bump a hash before the tests pass.
- P0 is a behaviour change to compiled images' *meaning*, not to the format: no `.qlc` version bump
  in any phase of this plan. If 0.1/0.2 ever forces a table change, that is a new round with its own
  bump decision.

## Conventions

- One phase per round, one commit per concern (code, then docs), commit names of the shape
  `C9a: one identity for a module-level const (F1)`.
- This plan moves to `docs/plans/archive/` only when P0–P3 all land; the deferred table then moves to
  `missing-features-plan.md` as tracked items so nothing is lost.
