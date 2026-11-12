# Plan: portable modules, follow-on — the external-type handoff reached from a package

Status: **PROPOSED** (2026-11-11). Baseline: Round 79 HEAD, `.qlc` **v9**,
`QLM1`/`QPM1` v1. Closes the two gaps named when P5 shipped
(`docs/plans/archive/portable-module-plan.md` P5) — neither is a container
change.

**No `.qlc` version bump, no `QLM1`/`QPM1` layout change anywhere in this plan**,
and no new public API unless Phase 1 cannot be written without one (it should
not need one). If a container format does change, the untrusted-input matrix in
`portable_module_test.cpp` grows in the same commit.

## Goal

`QPM1` proves a package's *shapes* match the host (`loadPortableModuleFile`,
`docs/SERIALIZATION.md` §12) and, since Round 79, that a manifest type is
**usable** from a loaded image: the entry builds `Point` through the *named*
ctor slot P4 leaves, the embedded library reads that host object's members, and
the typed handoff is reached (`portable_module` typed-payload gate,
`docs/EXTERNAL_TYPES.md` §14 "After the handshake, the values are the host's").

Two things are still unproven, in order of value:

1. The **refusal** half of that handoff has never been reached from a package.
   `phase3_api` T2 (`tests/unit/phase3_api.cpp`) pins `external type mismatch`
   for a host-built `qval_external_*` value; nothing pins it for a value that
   arrives through a package-loaded module. The manifest deliberately carries
   shape and not C++ identity (§11 "tags, not mangled names", §14 "What it does
   not"), so two different C++ types registered under one name pass the
   handshake **by design** — the `dynamic_cast` in `qval_external_owned<T>` /
   `qval_external_borrowed<T>` (`include/qlang/QLangVariant.h`) is the last
   line, and it is the line a hostile or merely stale package would hit.
2. There is no runnable **demo** of the host workflow. The only executable
   proof is the test suite plus `qlang_run --package`, and `qlang_run`
   registers no `QL_OBJECT` types, so the typed path is invisible from the CLI.

## Phase 1 — the mismatch path, through a package (`tests/unit/portable_module_test.cpp`)

One new gate, `testHandoffMismatch`, in the existing suite (one ctest entry, no
CMake change):

- A second fixture type beside `Point`: `struct PointB : QLObject<PointB>` with
  the **same describable shape** — two `double` properties, `QL_STR`, a typed
  `QL_CTOR` of two doubles. Its `serializeTypeEntry` bytes are identical to
  `Point`'s, so the handshake cannot tell them apart. Assert that as the first
  check: `serializeTypeEntry(manifestTypeFrom(*A)) ==
  serializeTypeEntry(manifestTypeFrom(*B))` — the premise of the whole gate, and
  the reason it must not be "fixed" by a name-only or hash-only handshake.
- The loading context registers **`PointB`** under the name `Point` and also a
  native `stale_point()` returning `qval_external_owned<PointA>(...)` — the
  shape of the real failure: a host whose own module was built against another
  build of the same type name.
- The packaged entry calls `stale_point()` and reads `.x`. Expected: a clean
  `QLangError` of kind `Type` whose message says `external type mismatch`, not
  a garbage number, not a crash. Then one liveness check — a `PointB` the same
  context built still reads fine afterwards (the refusal must not poison the
  value or the context).
- Reuse `pack()`, `qlcImage()`, `captureStdout()` and the scratch area already in
  the file; a fixture that cannot write is already an assertion failure.

**Verify:** the new checks fail when the guard is removed — delete the
`dynamic_cast` check in `QLangVariant.h` locally and confirm the suite reports a
wrong value rather than nothing (record the result in the commit message, do not
commit the removal). Then full `ctest`.

## Phase 2 — the host demo (`examples/portable_module.cpp`, optional)

Only if a human is going to run it; the suite is otherwise the proof. Register
`add_executable(qlang_portable_module examples/portable_module.cpp)` beside the
other example target. Script of the demo, in one process:

1. `writeTypeManifest` the host's own types to a scratch `.qlm`,
2. compile one library + one entry that constructs the type (the same in-process
   compile `portable_module_test.cpp` already does),
3. `writeQpm` → a **fresh** `QLangContext` → `loadPortableModuleFile` →
   `QLangLinker` → `QLangVM` → print the value,
4. one deliberate mismatch (a context registering a same-named, same-shaped
   different type and a native that hands out the other one) printing the clean
   refusal, so the demo shows both outcomes.

Deterministic stdout, scratch under `/tmp/opencode/` (never the source tree),
exit 0 only if the run printed the expected value.

## Explicitly skipped (add when the trigger fires)

- **C++ identity / layout in the container** (mangled names, `sizeof`, a
  compiler stamp): breaks portability across builds, which is the container's
  purpose — decided in §11 and repeated in §14. A host that needs layout
  equality asserts it in its own C++.
- **Per-image integrity hash** in `QPM1`: the readers already refuse a malformed
  payload, and a hash that nothing signs is theatre. Add when packages cross an
  untrusted boundary where provenance matters.
- **A package with several entries**, `--qlmeta-dump` *from* a `.qpm`, and
  `--package` writing a manifest that merges several binaries' registries: no
  consumer today.
- **Runtime module registry** and the `QLS1` snapshot — separate items
  (`docs/compliance/status.md`), unrelated to the container.

## Docs round (after each phase)

`docs/compliance/history.md` round entry, `tests.md` row + tally for the new
checks (harness.md defers numbers to tests.md), `status.md` external-types Open
Item; `EXTERNAL_TYPES.md` §14 gains the refusal sentence in Phase 1.
`bash update-doc-hashes.sh` **after** the suites are green — never before.
Move this plan to `docs/plans/archive/` when both phases land.
