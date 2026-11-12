# Plan: the `.qlc` native-slot table names only the natives the program calls

Status: **not started.**
Baseline: Round 81 HEAD (`9de5aa9` + the Round 81 demo), `.qlc` **v9**, `QLM1`/`QPM1`
**v1**. **No format change and no version bump anywhere in this plan** — the work is
in what the compiler puts into the table, not in how the table is written.

Found by `examples/portable_module.cpp` (Round 81) and verified in that session.

## Why

1. `addExternalFuncs` assigns a negative id the moment a name is injected
   (`src/QLangCompiler.cpp:584-596`), `nativeFuncIds()` returns the **whole** map
   (`include/qlang/QLangCompiler.h:279-283`, documented as "the map the `.qlc`
   native-slot table is written from"), and `writeQlc` writes every entry it is
   given (`src/QLangBytecodeIO.cpp:436-445`).
2. `QLangContext::loadProgram` resolves **every** table entry and throws
   `missing native function(s): 'a', 'b'` if any single one is absent
   (`src/QLangContext.cpp:442-461`) — before the program runs.
3. So an image inherits its **build host's entire native table**. Both Round 81
   demo images carry `set_verbose`, `set_max_steps`, `get_max_steps`, `connect` and
   `disconnect` — everything a default context registers — although neither source
   calls one, and `addExternalTypes` adds an entry per manifest type with `hasCtor`
   whether or not the source names it (`src/QLangCompiler.cpp:505-514`). The demo's
   `shape` library, compiled with the manifest it never uses, failed to load with
   `missing native function(s): 'Point'`; the demo works around it by not passing
   the manifest to that compile.
4. Two documents promise the opposite. `docs/EXTERNAL_TYPES.md` §13: a manifest ctor
   is a named slot and "running it on a host without the bindings fails **at that
   call only**". `docs/SERIALIZATION.md` §5.2: "A context lacking a native the
   program **calls** is a load error". Neither describes a loader that refuses on
   names nothing calls.

The portability claim is therefore only true host-to-identical-host: a `.qlc` built
by an embedder that registers 30 natives will not load on a host that registers 3,
whatever the script does. Phase 1 makes the table mean what §5.2 says it means.

## Phase 1 — the slot table is the set of calls (`src/QLangCompiler.{h,cpp}`)

- **Keep `m_nativeFuncIds` as the resolve-time truth** (see the experiment in Phase
  2 — it is what makes `Point(...)` and `math.min` recognizable as calls). Make the
  *accessor* return only ids that were actually emitted into bytecode: mark them in
  a `std::unordered_set<int> m_usedNativeIds` at the two sites that produce a
  negative `CALL` operand — `bindNative` (`src/QLangCompiler.cpp:3266-3276`) and the
  one direct `emit(Opcode::CALL, dest, nit->second, …)`
  (`src/QLangCompiler.cpp:4948-4950`) — and filter in `nativeFuncIds()`. About six
  lines; no new concept, no new table.
- **Drivers need no change, and that is part of the check.** Every consumer either
  `find`s with a guard or writes the table: `src/QLangContext.cpp:357,718`,
  `examples/qlang_run.cpp:1689,1708,1736,2396,2413,2419`,
  `examples/module_search.cpp:109,124`, `examples/portable_module.cpp:134`.
- **Format is untouched**: the table is a length-prefixed list of `(slot, name)`, so
  a shorter one is valid v9; slots derive from ids (`slot = -1 - id`), so nothing
  shifts. Images written before this plan still load (longer tables are legal);
  images written after it load on older engines. `QPM1` is unaffected.

**Red first, in `tests/unit/manifest_compile.cpp`:** a context with one extra native
(`setFunc("host_only", …)`) compiles a script that never calls it → the written
image's `nativeSlots` name no `host_only`, and `loadProgram` **succeeds** on a
context without it (today it throws `missing native function(s): 'host_only'`).
**The paired gate, same suite:** a script that *does* call that native still names it
in the table and runs — without this second gate Phase 1 could pass by pruning
everything.

**Mutation:** remove the `insert` from `bindNative` → the second gate fails (a native
the program calls is missing from its own image); remove both marks → the first gate
fails. Both new checks are load-bearing; the Round 79/80 named-`Point`-slot gates pin
the pruning boundary from the other side.

## Phase 2 — a manifest type reserves its slot on use, not on injection

Harmless once Phase 1 lands (an unused entry is simply not written), so this is
cleanup, not a fix — do it only because the reservation is then provably dead weight
and the demo can lose its workaround.

- **Verified premise, do not re-litigate:** deleting the eager
  `addExternalFuncs({t.name})` in `addExternalTypes` (`src/QLangCompiler.cpp:505-514`)
  is **not** the fix. Tried in this session: `manifest_compile` and
  `qlmeta_compile_cli` fail (gates 157–210) with `unexpected compile error: unknown
  function 'Point'`, i.e. the ctor's recognizability as a callable lives in
  `m_nativeFuncIds`, and the resolve site (`src/QLangCompiler.cpp:4948`, throw at
  `4957`) has no `m_extTypes` fallback. Experiment reverted; suites 63/63 again.
- So the change belongs at the resolve site: when `m_nativeFuncIds` misses a callee,
  consult `m_extTypes` for a `hasCtor` type of that name and route through
  `bindNative`, which already reserves on demand. The injection becomes pure
  declaration, and the header comment's "Validation input only — codegen is
  unchanged" (`include/qlang/QLangCompiler.h:262-268`) becomes literally true.
- **The demo is the check:** `examples/portable_module.cpp` passes the manifest to
  **both** compiles, the explanatory workaround comment goes away, and the demo still
  exits 0 — it fails if the library image stops loading or stops being contract-free.
- If this proves fiddly, stop: leave the reservation with a `ponytail:` comment
  naming this phase as its removal path. Phase 1 already removed the symptom.

## Phase 3 — docs catch up (after the code, never before)

- `docs/EXTERNAL_TYPES.md` §13's closing sentence: a host that cannot serve a slot
  the program calls is refused **at load**, naming every such slot
  (`missing native function(s): …`), not "at that call only"; and after Phase 1,
  the table names exactly the calls.
- `docs/SERIALIZATION.md` §4.3 and §5.2: the table is the natives the program calls;
  a name nothing calls is no longer written; v9 is unchanged, and a longer table
  from an older writer stays legal.
- `docs/spec/11-external-types.md` § *compile-time validation*: one clause — a
  manifest constructor becomes a named slot **when the source names it**.
- `docs/compliance/{history,status,tests}.md`: round entry, the `manifest_compile`
  and `portable_module` rows, and the counted-unit-cases tally, which moves.
- `bash update-doc-hashes.sh` **after** the suites are green — the spec edit above
  re-records a `doc_hashes` row (AGENTS: testing proves correctness, the hash merely
  detects change).

## Explicitly skipped (add when the trigger fires)

- **Pruning in `readQlc` or `loadProgram`.** The reader stays faithful to the bytes;
  strictness about what the table *does* name is the documented contract (§5.2).
- **Lazy binding** — a missing native failing at the call instead of at load. It
  trades one clean, complete error for a late partial one; no consumer asked for it.
- **C++ identity/layout in the container**, a per-image integrity hash, multi-entry
  packages, `--qlmeta-dump` *from* a `.qpm`, the `QLS1` runtime-module snapshot: all
  carried over from `docs/plans/archive/portable-module-follow-on-plan.md`, all still
  trigger-less.
- **Pruning the constant pool / var map the same way**: different tables, different
  readers, no reported symptom.

## Adjacent, not in this plan

- `QLangContext::hasMethod` accepts a table field holding a `BoundMethod` while
  `callMethod` requires `Type::Function`, so the guard does not protect the call it
  guards — the open defect recorded in `docs/compliance/status.md` (external types,
  `docs/EVENT_HANDLERS.md` §4/§6). Unrelated to the slot table; own commit, own plan
  if it grows.

## Verify (each phase)

`cmake -B build && cmake --build build`; `ctest --test-dir build` (63 entries today,
`perf` skips in Debug); `./build/qlang_portable_module` exits 0; every new gate
mutation-checked; `bash update-doc-hashes.sh` last. Both phases land ⇒ move this plan
to `docs/plans/archive/`.
