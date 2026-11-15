# Plan: a library's own directory in its dependency search (C8 deferred row)

Status: **plan written 2026-11-15 — not started.** Promoted from
[missing-features-plan.md](missing-features-plan.md) §"Deferred from the module
export/import plan" (the "a library's own directory in its dependency search"
row, deferred Round 73 with trigger "a real project hits it (nested third-party
libs outside the entry dir)"). Completes the C8 story (C8a Round 67, C8b
Round 69, run-boundary Round 92).

## The problem, grounded

`QLangContext::findModule` (`src/QLangContext.cpp:825`) resolves a module name
through one order: registered C++ module → resolved cache → registry → search
callback → **`m_moduleSearchPaths`, in order** (`.qlc` before `.ql` within a
directory). The path list belongs to the context; `qlcc` seeds it once — entry
file's directory, working directory, then each `--module-path`
(`examples/qlcc.cpp`, the C8b entry-directory rule).

A module's *own* imports are answered by the **same** `findModule` on the
context — both through `resolveModuleDependencies` (`src/QLangContext.cpp:675`,
which walks a loaded image's import table) and through the C8b scratch compile,
which deliberately holds **no paths of its own** (`scratch.setModuleSearchPaths({})`,
`src/QLangContext.cpp:759`) and answers every import by calling back into the
outer `findModule` (`src/QLangContext.cpp:770`).

Consequence: when `main.ql` (in the entry dir) imports `vendor/lib.ql`, that
source is found because `vendor/` is *not* searched either — it is found only if
`vendor` was named in the list. Take the working, documented shape instead:

```
project/main.ql          # import util
project/vendor/util.ql   # found: --module-path project/vendor
project/vendor/helper.ql # util.ql does: import helper  -> NOT FOUND
```

`helper` is looked up in the context's paths only — the directory `util.ql` was
found in (`project/vendor/`) is never consulted. The deferred row's diagnosis is
exact: *"the search list belongs to the context, so a fix needs a stack of
current module dirs on the resolver side."* Today the user's workaround is to
list every library directory in `--module-path` (or `addModuleSearchPath`),
which does not scale past a hand-written project and leaks third-party layout
into the host's command line.

## Baseline facts (verified 2026-11-15)

- The in-flight cycle chain `m_compilingSources` (`QLangContext.h`) is already
  the exact stack discipline this fix needs: push on entry, RAII pop, and
  nested `findModule` calls run *between* push and pop. Mirror it.
- Provenance exists where the dependency walks happen: `LoadedModule::path`
  (`QLangBytecodeIO.h:79`, the file the image was read from, `""` for
  stream/callback images) and the `path` parameter of
  `compileModuleSource(path)`. `resolveModuleDependencies` already prints
  `mod.path` in its not-found message.
- Resolution is once-per-context: the resolved-name cache and the registry
  (findModule steps 2) short-circuit *before* the paths, so a directory stack
  only ever affects first-time discovery; first-discoverer-wins under the
  registry key is unchanged semantics.
- The entry's own imports go through `QLangContext::run()`'s
  resolve-then-link boundary (`src/QLangContext.cpp:329`, Round 92); the entry
  directory is already first in the context's list, so nothing here changes the
  entry's resolution.
- No `.qlc` change, no opcode, no compiler change: this is entirely
  `QLangContext` resolution order.

## Design — the resolver-side directory stack

`QLangContext` gains `std::vector<std::string> m_moduleDirStack` — directories
of the modules whose dependencies are *currently being resolved*, innermost
last. Two push sites, both RAII (same as `InFlight` in `compileModuleSource`):

1. `compileModuleSource(path)`: push `std::filesystem::path(path).parent_path()`
   for the duration. Covers a `.ql` hit — its compile's imports are answered by
   the outer `findModule` through the callback, and by then the stack already
   carries the importing library's directory. Composition is free: a nested hit
   recurses through `findModule` → `compileModuleSource` and pushes again.
2. `resolveModuleDependencies(ctx, mod)`: push `parent_path(mod.path)` for the
   duration of the walk. Covers a `.qlc` (or already-loaded) image, whose
   imports are resolved *after* its compile is long gone — this is the only
   place that knows where the image came from.

A `""` path (stream- or callback-supplied image) contributes nothing — guard,
never push an empty or nonexistent directory (the paths loop already skips
non-directories, and an empty entry means the working directory, which must not
accidentally become a module's dir).

**`findModule` step 4 search order becomes: `m_moduleDirStack` innermost-first,
then `m_moduleSearchPaths`,** with `.qlc` before `.ql` within each directory and
first-match-wins unchanged. Within-directory precedence, the `.qlc`-reuse rule
and the compile-on-hit behaviour are untouched; steps 1–3 (C++ module, cache,
registry) still short-circuit everything, so the stack cannot duplicate an
already-resolved module.

The callback (step 3) still runs **before** any directory search, so a host
callback still shadows both the stack and the context paths (deviation-free:
the spec's callback wording already says "before the paths").

Skipped on purpose (YAGNI, promote when): exporting the stack through
`moduleSearchPaths()` (it is host configuration, the stack is resolver state);
a `qlcc` flag to disable it (a project that *wants* strict paths lists its
paths and keeps library dirs uncollided — same exposure `--module-path`
already has); per-module resolved-name scoping (the registry is still
name-keyed, first discoverer wins — Q3 below).

## Open questions (defaults recommended)

- **Q44 ordering — nearest-parent before or after the context paths?**
  **Default: before.** It mirrors the entry-directory rule (C8b: the entry's own
  directory is searched first) and C's quoted-include; the importer's neighbour
  is the most specific location. After the paths would keep today's collisions
  winning and make the feature shadowable by any cwd file.
- **Q45 full ancestor stack or nearest parent only?** A `deep/nest/lib.ql`
  chain pushes each in-flight module's dir. **Default: full stack, innermost
  first** — free with the push-on-entry design, and "after its parent's dir" is
  the same rule one level up.
- **Q46 name collision across a parent dir and the context paths** — `foo.ql`
  in a parent dir and a different `foo.ql` in a `--module-path` dir. **Default:
  the Q44 order decides it (parent wins); the registry's first-discoverer rule
  is unchanged.** No new diagnostic; the existing `[MODULE] '%s' found at %s`
  trace names the winner.
- **Q47 does the callback see the stack?** No — its signature is spec-locked
  (`07-api.md` "external search callback", name + error only). A Qt-style
  host that wants parent-dir context gets it after the callback declines, from
  the regular search. **Default: unchanged callback.**

## Milestones

- **M0 — spec sentence (docs only).** `docs/spec/07-api.md`
  `## module search paths`: while a module's own imports are resolved, the
  directory it was found in is searched first (the stack order of Q44/Q45,
  `""`-path images contribute nothing). Re-record the spec hash in
  `docs/spec/README.md` and the compliance rows per AGENTS.md (only after the
  behaviour lands and tests pass — M2 — the sentence is written with the code
  round, committed ahead of it exactly as C1's M0 did).
- **M1 — the stack (the whole fix).**
  - `include/qlang/QLangContext.h`: `std::vector<std::string> m_moduleDirStack;`
    (Pimpl not required for this — it sits beside `m_compilingSources`).
  - `src/QLangContext.cpp`: RAII push/pop in `compileModuleSource` and
    `resolveModuleDependencies` (guard: parent path non-empty and an existing
    directory); `findModule` step 4 walks the stack (innermost first) then
    `m_moduleSearchPaths` — factor the single-directory probe (`.qlc` hit →
    report-not-skip, `.ql` hit → remember first) into the loop body over a
    concatenated view, do not duplicate it.
  - Failing test first (TDD): the `module_search` suite (`tests/unit/
    module_search_test.cpp`, 128 checks today) gains a group: a `main.ql` in
    dir A imports `outer`, whose source sits in dir B (B is in the context
    paths), whose `import inner` names a file that exists **only** in B — red
    before, green after. Plus pins: the `.qlc`-image variant of the same chain
    (exercises push site 2), the callback-still-shadows pin, the `""`-path
    (stream-loaded `outer`) variant asserting today's not-found error is
    unchanged, and the Q46 collision pin (parent file wins over a paths file).
- **M2 — compliance round (per AGENTS.md).** Rebuild + full `ctest`; flip the
  status.md external-modules/C8 Open-Item note and the missing-features-plan
  deferred row to point here; append the round to `history.md`; re-record
  `07-api.md` + touched compliance hashes with `./update-doc-hashes.sh`;
  `todo.md` §5 gains the plan line; archive this plan.

## Files touched (expected)

`include/qlang/QLangContext.h` · `src/QLangContext.cpp` ·
`tests/unit/module_search_test.cpp` (+ its temp-dir fixture helpers) ·
`docs/spec/07-api.md` · `docs/spec/README.md` (hash) ·
`docs/compliance/{status,history}.md` · `docs/plans/{todo,missing-features-plan}.md`.

No driver change (`--module-path` keeps its meaning; the help text's search
description gains one sentence), no `.qlc` version change, no new option.
