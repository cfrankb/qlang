# Plan: module export/import gaps (post-C8b audit)

Status: **P0 landed 2026-11-10 (Round 70); P1–P3 open**.
Baseline `HEAD = 87356aa` ("C8b: compile a discovered .ql into a module image");
`cmake -B build` clean, `ctest --test-dir build` **56/56** green including `doc_hashes` at that
baseline (P0 landed on top of it: **57/57**, Round 70).
Findings this plan executes: [../bugs/module-export-import-audit.md](../bugs/module-export-import-audit.md)
(F1–F6, probed 2026-11-10). Parent plans:
[archive/external-modules-plan-v4.md](archive/external-modules-plan-v4.md) (Phases 1–3c, done,
Round 65) and [missing-features-plan.md](missing-features-plan.md) **C8** (C8a Round 67, C8b
Round 69) — this plan is the *after* C8 work: what `export`/`import` still cannot do.
Spec: [../spec/09-modules.md](../spec/09-modules.md) (`## import statement`, export rules),
[../spec/13-bytecodes.md](../spec/13-bytecodes.md) (`## keyword export`, `## export table (lookup)`,
`## import table (lookup)`, `## technical specifications` 1 and 5),
[../spec/07-api.md](../spec/07-api.md) §module search paths (C8b contract, unchanged here).

Sequencing is not optional: **P0 was a blocker for the others.** P1/P2 are compiler-side binding
changes and both want to assert values across a module boundary, which F1 made unreadable; P0
landed first, as planned.

---

## P0 — one identity for a module-level const (F1) — **landed Round 70**

A module-level `const` that a `def` of the same module reads was unreachable at run time: the `def`
returned nil for it and its exported cell read nil through an import, while the module's own `<init>`
saw it (F1's matrix in the findings doc). The observable rule established: **a top-level name has
one cell, and every reference — `<init>` store, in-module `def` read, export ref, importer
`getimport` — names it.**

**Phase 0.1 pinned it, and both predicted forks were wrong.** The instrumentation showed one slot,
written correctly and then *emptied*: the linker's `Function::captures` keeps the absolute sentinel
`-1 - slot` for an immutable const, but the `MAKEFUNC` spec the def itself built captured
`regOf(V, slot)` — the frame register the def's flat body reads. `bindCaptures` honoured that spec
with a module-owned `Env` fed from the init frame, and at init-frame pop `releaseFrame` moved the
value out of the merged slot into that `Env` (trace: `MOV2` write-back, then `RELEASE fn=1 move
regs[1] -> env cell 0`), nilling it for every other reader. Neither "declareVar vs `globalSlotOf`
divergence" nor "the store never reached the cell" — the audit's excluded table lost the release row.

**Phase 0.2 — fixed in the compiler, not the linker (Q39 confirmed).** The def's capture-spec loop
reuses the negative sentinel when the cell is an immutable module const, so store, def read, export
ref and `GETIMPORT` name the one cell; the linker's rebase handles it unchanged, and `GETIMPORT` was
already right. Mutable top-level `let` keeps the frame-relative copy — spec §1's copy rule. The
linker needed no change.

**Phase 0.3 — the silent-nil path is dead in library mode (Q43 confirmed).** A `def` capture that
resolves to no cell is a compile error — `def '<fn>' captures '<name>', which has no cell in module
'<mod>'` — covering the non-exported module-local const/let and the missing-symbol case. Script mode
is unchanged.

**Guards — landed.** `tests/unit/linker_test.cpp` (ctest `linker`, 130→138 checks):
`testModuleConstOneIdentity` runs the F1 shape with three readers printing the value and asserts the
export and capture refs *are* the sentinel cell. Script regression
`tests/scripts/ql_module_const.ql` + fixture `ql_module_const_dep.ql` (in `QL_SCRIPTS`): importer +
same-module `def`, a duplicate `init`, and a live importer after the init frame popped. One
`qlc_compile` capture-folding assertion flipped (`specs == 2 → 1`): the second spec named a frame
register and was never a second module cell — it had encoded the defect.

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
- Library-mode reachability errors report instead of folding to nil — **landed with P0.3**
  (Round 70); the first bullet is what remains of P3.

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

- **Q39** (P0) which layer owns the fix — compiler identity, VM init frame, or linker? **Resolved
  Round 70: the compiler**, as the default predicted — the def's capture spec now reuses the
  sentinel cell; the linker and `GETIMPORT` needed no change.
- **Q40** (P1) is `MODULE_NAME` a public export? **Default: no for `import *`, yes for `m.MODULE_NAME`
  and the export table.**
- **Q41** (P2) does an aliased import need image/format support? **Default: no** — compile-time
  binding, `.qlc` stays v9, `ImportEntry::alias` gets deleted.
- **Q42** (deferred) may a `.ql` module export a class? **Default: no, unanswered — needs a spec
  sentence first.**
- **Q43** (P3) should the F1 shape ever compile? **Resolved Round 70: no** — in library mode an
  unreachable module-level name in a `def` is a compile error, as the default adopted.

## Verification (AGENTS.md, every phase)

- `cmake -B build && cmake --build build`, then `ctest --test-dir build` — 56/56 at the baseline,
  **57/57 green since P0** added `script_ql_module_const`.
- Single suites while iterating: `build/linker_test`, `build/module_search_test`.
- F1's own reproduction is the acceptance test: the F1 pair must print `[tagged] / [two] / [three]`
  and `fn:tagged` — **passes since Round 70**; P1's pair must print both star-imported names;
  P2's must print through `g`.
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
