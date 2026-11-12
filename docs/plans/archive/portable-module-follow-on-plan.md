# Plan: portable modules, follow-on — the external-type handoff reached from a package

Status: **both phases ✓ implemented (Phase 1 Round 80, Phase 2 Round 81,
2026-11-11).**
Baseline: Round 79 HEAD, `.qlc` **v9**, `QLM1`/`QPM1` v1. Closes the two gaps
named when P5 shipped (`docs/plans/archive/portable-module-plan.md` P5) — neither
is a container change.

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
- The loading context registers **`PointB`** under the name `Point`, with its
  registered `construct` handing out a `Point` (the older build) while the
  descriptor's `typeInfo`/`typeCheck` still say `PointB`.

**Delivered (Round 80), with one correction to the plan text.** The construction
this plan proposed — `qval_external_owned<PointA>(desc, ptrA)` behind a
`stale_point()` native — **does not fault**: `resolveExternal` validates the
`ExternalRef`'s *own recorded* `check`, and that helper records the handing-out
code's `T`, so a stale module's `PointA` claim matches its own object and the
member read proceeds. Nothing re-checks the descriptor at the access site. The
value that does fault is one whose recorded check comes **from the descriptor**
while the pointer is another build — which is exactly how the engine wires its own
constructor native (`ref.expected = d->typeInfo; ref.check = d->typeCheck` in
`QLangContext.cpp`). So the gate sabotages `meta.construct` on the registered type
and touches nothing else: no hand-rolled `ExternalRef`, no extra native, and the
path under test is the one a real host compiles.

- The typed package is built **once** (`typedPackage()`) and written to two files:
  the honest host runs it (`16.25` on stdout, `type(p) == Point`), the drifted host
  loads the identical bytes and faults on the first member read — `QLangError` of
  kind `Type`, `external type mismatch: … for type 'Point'`, with **nothing
  printed**. Then one liveness check: the same descriptor reading an
  honestly-built `PointB` still returns `7.5`, i.e. the guard refused that pointer
  and not the type.
- `captureStdout` became `captureRun` (returns the partial stdout **and** the fault
  it stopped on — that is what makes "nothing printed" checkable), and
  `loadPortableModuleFile` → `QLangLinker` → `QLangVM` moved into shared
  `runEntry`/`loadAndRun` helpers instead of being repeated per gate.
- Reuses `pack()`, `qlcImage()` and the scratch area; a fixture that cannot write is
  already an assertion failure.

**Verify (done):** `portable_module` **92 → 102 checks**, ctest 63/63. Forced
`resolveExternal`'s `ref.check` guard off locally (`src/QLangValue.cpp`) — the five
guard checks fail, and note *how*: `Point` and `PointB` share a layout, so with the
guard removed the run **silently succeeds**, which is the failure mode the guard
exists for. Removal reverted (`git diff src/QLangValue.cpp` empty), doc hashes
re-recorded after green.

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

**Delivered (Round 81), with four deviations.** `examples/portable_module.cpp` +
`add_executable(qlang_portable_module …)`. (a) **Two** refusals instead of one, and
neither is the Phase 1 sabotage: a context registering nothing (`missing type …`)
and a context whose registered `Point` has lost its `str` (the shape mismatch). A
host author reading the demo needs the two ways a host fails the contract; the
same-name-different-C++-type case is invisible at this altitude and `portable_module`
owns it. (b) Registered as a **build target, not a ctest entry**, following
`module_search`: the demo is self-checking (nine `[ok]` reports, non-zero exit on
any failure), the suites stay the proof, and a 64th entry would move the tallies for
no new assertion. (c) Scratch is `std::filesystem::temp_directory_path() /
"qlang_portable_module"` — same reason as (b): it is what the sibling demo does, and
it works where `/tmp/opencode` does not exist. (d) One extra print, `str(p)`, so the
`QL_STR` fact the contract records is demonstrated as well as `type(p)`.

The demo also earned its keep: the **library** compile, given the manifest, produced
an image whose native-slot table named `Point` even though the library never mentions
it — `addExternalTypes` reserves a slot for every `hasCtor` type and `writeQlc`
writes it, so that image demanded the type at load (`missing native function(s):
'Point'`). The demo now injects the manifest only where the source names the type and
says why. Narrowing that reservation is a `.qlc`-visible behaviour change with no
test asking for it; `EXTERNAL_TYPES.md` §13's "validation input only" wording is what
should catch up — carried as `docs/plans/native-slot-portability-plan.md` (2026-11-12).

**Verify (done):** target builds clean, the binary exits 0 with nine `[ok]` reports,
full `ctest --test-dir build` 63/63 — no `src/` or test change, so no counts moved.

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
