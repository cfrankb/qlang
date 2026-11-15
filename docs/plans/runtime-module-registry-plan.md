# Runtime module registry + name-based module discovery — implementation plan

- **Status:** M0 + M1 done 2026-11-14 (spec section in `07-api.md`;
  `run()`/`runProgram()` link at the run boundary, resolved through
  `findModule`; `module_search` suite 117 → 128 checks, ctest 67/67).
  M2 (harness dedup + compliance round) remains.
  Prepared 2026-11-14.
- **Backlog id:** the C8 remainder (`docs/plans/missing-features-plan.md` §C8: the
  search-pair C8a/C8b landed Round 67/69; what was deferred is the *registry*
  half — running without the `qlcc` driver), listed in
  `docs/compliance/status.md` as *"Remaining for `# external modules`"*.
- **Spec anchors:** `docs/spec/13-bytecodes.md` `## technical specifications` 1
  (link model), `docs/spec/07-api.md` `## module search paths` /
  `## external search callback`, `docs/spec/09-modules.md`.
- **Estimated effort:** 2–3 working days solo (contrast: the Qt bridge plan is 4–6 weeks —
  this feature is almost finished; only the last seam is missing).

## 1. What exists today (verified against the tree, 2026-11-14)

The registry already does everything except *own the run boundary*:

| Piece | Location | State |
|-|-|-|
| Name → image discovery (cache → registry → callback → search paths, `.ql` compiled on the fly, `.qlc` loaded, deps walked transitively) | `QLangContext::findModule` (`QLangContext.h:381`, `compileModuleSource` `QLangContext.cpp:669`, `resolveModuleDependencies` `:629` called at `:826`) | ✓ C8a/C8b |
| Registry (`loadProgram`/`loadProgramFile`/`loadedModule`/`loadedModules`, native re-resolution by name, `"module '<name>' is already loaded"` guard) | `QLangContext.cpp` (`ModuleRegistry` pimpl `:190`) | ✓ |
| The merge itself (functions concat, global slices, import-table dedup, pool fold, `initSequence`) | `QLangLinker` (`QLangLinker.h:52`) | ✓ Round 64/65 |
| The unmerged opcodes (`getimport`/`callimport` runnable on a raw image) | `QLangVM.cpp` `CALLIMPORT` `:2696` — "the form is what a future runtime registry executes (plan Phase 3c)" — pinned by `linker_test.cpp:877` `testUnlinkedOpcodes` | ✓ Round 65 |
| `initSequence` execution (module `<init>`s, dependency order, once) | `QLangVM::run` `:2120` | ✓ |
| **Who runs a program with imports** | **only `qlcc`** (`examples/qlcc.cpp:2489`: `if (!compiled.imports.empty()) { QLangLinker linker(ctx.loadedModules()); … }`); `module_search_test.cpp:201` duplicates the same compile→link→run harness privately | ✗ the gap |

`QLangLinker.h:55` names the missing feature itself: *"The linker never loads
anything: finding a module by name without it being loaded is the registry's
job (plan C8)."* And spec 13 item 1: *"what is missing is the feature around
it: nothing resolves a module by name at run time and hands its image over."*

### 1.1 The hazard this plan also fixes (reproduced, 2026-11-14)

`QLangContext::run(source)` (`QLangContext.cpp:321`) compiles — the compiler
resolves and *registers* imported modules via `findModule` — then runs the
**unlinked** program in the VM. On an entry with no functions of its own the
`CALLIMPORT` guard fires (`imported function 'f' is not in the function
table`); on an entry with ≥ 1 function the import-table `ref` (a function index
in the *defining* module's table) silently lands inside the *entry's* function
table. Verified against `build/libqlang.a`:

```
def g() { return 99 }        // m.ql:  export def f() { return 7 }
from m import f
local x = f()                # x == 99 — f ran as g, no error
```

`getimport` has the same aliasing on global slots. Nothing else executes
unlinked images today (qlcc always links first), which is why it never bit.

## 2. Decisions (defaults; dissent costs nothing yet)

- **Q1 — which link model?** The spec lists two (`link-time merge`,
  `runtime module registry`) and requires they "share a reusable solution".
  **DECIDED (default): link-at-the-run-boundary** — the registry resolves import
  names with `findModule` and hands the images over, `QLangLinker` merges, the
  VM runs the merged program. The alternative (execute images unmerged: frames
  carrying their defining program's constant pool, shared export cells across
  two global tables) is a VM hot-loop redesign duplicating linker semantics for
  zero observable gain — the opcodes stay implemented and pinned as they are,
  they just are not what the context runs.
- **Q2 — API shape.** One new host method
  `void QLangContext::runProgram(const CompiledProgram &entry)` (host pairs it
  with `loadProgramFile` + `loadedModule()`), **and** `run(source)` gains the
  same auto-link. Both: if `!imports.empty()`, link via
  `QLangLinker(loadedModules())` before `QLangVM::run` — the exact 5 lines
  qlcc already runs.
- **Q3 — does running register the entry?** No. Running is not importing; the
  entry never joins the module registry (a `main.qlc` exporting names must not
  become importable by running it).
- **Q4 — link failures** propagate as the linker's own `QLangError(Io)`
  (`imported module '<x>' is not loaded`). On both entry paths discovery has
  already run (`findModule` during compile; `resolveModuleDependencies` during
  `loadProgram`), so this is a defensive tail, same as in qlcc.
- **Q5 — VM-side guard for unlinked runs?** None. The unmerged opcode forms are
  spec'd and pinned (`testUnlinkedOpcodes`); a merged program keeps a non-empty
  `imports` table (`getimport` survives the link), so `imports.empty()` cannot
  distinguish "unlinked" at the VM. The fix is routing, not a flag: the context
  never *runs* an unlinked program. A host that builds a `QLangVM` directly on
  a raw image keeps today's pinned behavior.
- **Q6 — merged programs stay non-serializable** (`.qlc` v9 has no
  `initSequence` section, `writeQlc` rejects). `runProgram` is run-only.
- **Out of scope:** C++ modules loading `.ql` modules (spec: "cannot … could be
  a feature later"); the known exported-def-capturing-sibling gap (module-cell
  capture addressing, recorded in status.md, separate problem).

## 3. Milestones

### M0 — spec (0.5 d)
- `docs/spec/07-api.md`: a short `## running programs with modules` section —
  `runProgram(const CompiledProgram&)` runs a (possibly loaded) program,
  resolving its imports through the registry/search paths exactly as `run()`
  resolves them at compile time; auto-link whenever `imports` is non-empty; the
  merged program is what the VM runs and what `call()` later binds against;
  the entry is not registered.
- Hash re-recorded after M1 tests are green (never before).

### M1 — the seam (1–2 d)
- `QLangContext::run(source)`: after natives are populated, replace
  `programs.store(std::move(compiled))` with store-**then**-link: if
  `!compiled.imports.empty()`, `QLangLinker(loadedModules()).link(compiled,
  compiler.nativeFuncIds())` and store the merged program. (~8 lines.)
- `QLangContext::runProgram(const CompiledProgram &entry)`: same link-or-run
  tail, sharing one private helper with `run()`; store the (merged) program in
  the program cache so `call()`/finalizers bind against it, then `QLangVM.run()`.
- Tests (`module_search_test.cpp`, the C8 suite — extend, don't re-home):
  1. the §1.1 misfire repro becomes a pin: `f()` returns **7**, not 99;
  2. `ctx.run(source)` with a search-path `.ql` import runs end to end (today:
     `imported function 'f' is not in the function table`);
  3. `runProgram` on a `loadProgramFile`d image with a transitively-discovered
     dep runs;
  4. C++-module-only programs (empty `imports`) bypass the link untouched;
  5. `call()` after `runProgram` reaches a module function (merged table).

### M2 — paperwork + dedup (0.5 d)
- `module_search_test.cpp:201`'s private compile→link→run harness may collapse
  onto the new API where it fits (keep the pins, shrink the harness).
- `qlcc` **unchanged**: it needs the pre-link snapshot for `-c`/package saving
  (`qlcc.cpp:2484`), which `run()` never had to care about. Note it in the plan
  archive; delegate later if the duplication ever bites.
- Compliance round: status.md "Remaining for `# external modules`" paragraph →
  resolved (the capture gap sentence stays — different problem); spec 13 item 1
  ◐ → ✓; `history.md` round entry; `tests.md` `module_search` tally bump; all
  hash rows re-recorded via `check_doc_hashes.cmake` after `ctest` is green.
- `docs/plans/todo.md`: close the entry.

## 4. Risks

- **Program-cache identity**: `programs.store` currently holds the *source*
  compile; storing merged changes what `call()` sees for import-using programs.
  That is the point (today it would misfire anyway), but any host test pinning
  function indices of an unlinked cache program must be found — grep
  `programs.compiled` users during M1; none expected outside the VM/finalizer
  plumbing which wants the runnable program.
- **`link()` cost on every `run()`**: negligible (imports are rare and the link
  is linear); no caching of merged images (YAGNI — hosts that care keep the
  program and `call()`).

## 5. Suggested commits

1. `feat(api): runtime module registry — run()/runProgram link imported modules (C8 M1)`
2. `docs(compliance): close C8 remainder — runtime module registry round`
