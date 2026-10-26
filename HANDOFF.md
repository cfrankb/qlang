# Session Handoff — QLang Serialization & External Types Design

Date: 2026-10-25
Stage: **POC** (user-stated). Design docs are the deliverable of this session;
implementation is explicitly deferred.

## Project

- Location: `/home/cfrankb/toolkit/gcc/qlang-sandbox`
- Build/test harness: see `AGENTS.md` (g++ C++17, `libqlang.a`,
  `stdbuf -oL -eL` when running — stderr is debug logging, stdout is
  block-buffered).
- Public API: `include/qlang/` — QLangValue, QLangContext, QLangCompiler
  (CompiledProgram), QLangVM, QLangErrors.

## What exists now (this session's work)

### 1. `qlang_run` app — POC bytecode output (`examples/qlang_run.cpp`)

- `--profile` — tracks bytecode compilation time and execution time
  separately, displayed in ms.
- `--output <file.qlc>` — writes the compiled program to a `.qlc` file via a
  `QlcWriter` class in the app. **This writer is a POC prototype** of the
  library codec; it will be deleted when the library codec lands.
- Known gap: the writer emits the v1 layout **minus the native name table**
  (SERIALIZATION.md §4.2). Without it the file is not loadable. POC stage —
  fix when the library writer is built, not before.
- A sample `test.qlc` (~8 KB) sits in the project root.

### 2. `docs/SERIALIZATION.md` — .qlc/.qls format spec (draft)

- **§3** value encoding: fixed tag table = `QLangValue::Type` enum order;
  tag 15 (Generator) never emitted — non-durable values round-trip as Nil.
- **§4** program image `QLC1` v1: header, functions, **native name table**
  (slot + name — the fix the POC writer lacks), vars/constVars (order
  *unspecified* — `unordered_map`/`unordered_set` backing), registerCount.
- **§5** load contract: bounds-checked reads, structural invariants
  (`functions[0] == "main"`, `op < kOpcodeCount`, closure index range,
  unique native slots), native binding = file read + context bind in two
  steps, all missing natives reported in one error.
- New API proposed: `QLangErrorKind::Io`, `QLangBytecodeIO.h`
  (serializeValue/deserializeValue/writeQlc/readQlc),
  `QLangContext::loadProgram(CompiledProgram&&)` + `execute()`.
- **§7** snapshot `QLS1` (v2, future): program + SHA-256 hash + steps/
  deleted/frames/generators/context-var values; frame-slot indirection for
  capture pointers; codec lives in `QLangVM` (`Frame` is private there).
- **§8–9** untrusted-input limits (depth 64, 64 Mi values/instructions) and
  the test matrix (round-trip, malformed suite, native mismatch, resume).

### 3. `docs/EXTERNAL_TYPES.md` — C++ structure import design (draft)

- **Core finding**: C++ RTTI cannot reflect member layout. Design = explicit
  registration tables (`TypeDescriptor`/`FieldBinding`/`MethodBinding`) with
  RTTI as identity/safety layer only (`type_index` registry key,
  `dynamic_cast` validation, `type_info::name` for `type()`).
- `QLangValue::Type::External` = **tag 17**: `{TypeDescriptor*,
  shared_ptr<void>}`, shared ownership, **not durable** in .qlc (emits Nil).
- Dunders: `__str__` (optional; absent → Type error on string conversion),
  `__destroy__` (cleanup hook on final release — resources, not memory).
- Boundary conversion table (C++ scalar ↔ QLangValue) in §7; C++ exceptions
  MUST be caught at the binding boundary → `QLangError` (§8).
- **§9 Object dispatch foundation** — the key architectural decision (below).

### 4. `SPEC.md` — "user defined classes" section completed

Was: one example, no semantics. Now specifies: class body rules (global
scope, field defaults, **`const` fields** for read-only conformance, unique
method names, dunders), construction (fixed arity, zero-args without
`__init__`), **closed field set** (no open key insertion, `$` pipeline N/A),
method calls (`self` implicit), string conversion (`__str__`; class default
= class name — documented asymmetry vs. external types), **reference
counting** with `__destroy__` on final release (the `shared_ptr` analog),
`type(x)`, v1 restrictions (no inheritance/overloading/statics), and the
full conformance mapping table to external-type bindings.

Fixed latent bug in the spec example: `spend` did `+= amount`, now `-=`.

### 5. `docs/EVENT_HANDLERS.md` — event handler contract (draft)

**Engine-specifics doc** — how the engine drives QLang as event handlers,
built *on* the QLang C++ API (not part of it). Key findings: **multi-level**
dispatch (type-based by type tag, map-based lifecycle, game-level events —
extensible), not a single flat object; no handler at global scope (handlers
are enclosed members of level objects the engine holds); all levels dispatch
through `ObjectOps.callMethod` (hasMethod-guarded, no-op on miss); type level
keyed by the external-type identity; table-vs-class (class primary);
ownership/retention (retain listeners, never cache payloads, per-binding
transfer policy); and execution safety — **step budget** (infinite-loop
containment), no re-entrancy, no suspension across events, structured handler
errors. **§0 boundary table** splits the QLang API (dispatch, step counter,
ownership model, error boundary — all in EXTERNAL_TYPES/SERIALIZATION/SPEC)
from engine specifics (the handler architecture + its policies); adding a
level needs no QLang change.

## Key decisions on record

1. **Dispatch-first build order** (EXTERNAL_TYPES §9): land the neutral
   `ObjectOps` layer (GETFIELD/SETFIELD/CALLMETHOD/TOSTRING + kind registry,
   table kind as first consumer) *before* both External types and QLang
   classes. Rationale: prevents the dispatch from being shaped against
   `TypeDescriptor` and reworked when class semantics land; keeps the two
   features independently shippable. `base()` hook reserved in the v1 struct
   for inheritance.
2. **Dot syntax is settled** — SPEC.md already specifies it (class section +
   f-strings); there is no syntax fork. External types join the existing
   object dispatch; scripts cannot distinguish object kinds.
3. **Native name table is mandatory** in the .qlc format — bytecode
   references natives by slot index; pointers can't be stored; names are
   what make the image loadable.
4. **Engine is the authoritative owner** — C++ owns external objects; the
   script holds a `weak_ptr` to engine-handoff objects (access after
   destruction → clean `object destroyed: <type>` error) and a `shared_ptr`
   only to script-constructed ones. Class instances remain script-owned /
   reference-counted. `__destroy__` = final-release hook in both models.
5. **No copy API** — view-only. `to_table`/`from_table` rejected: engine
   objects are long-lived; a qlang deep copy is a short-lived snapshot that
   is stale by the next event.
6. **Codec treats input as untrusted** — bounds checks, depth/count limits,
   one structured `QLangError(Io)` per failure, no partial out-params.
7. **Multi-level handler architecture** — handlers are organized by dispatch
   level (type / map / game, extensible), each level its own container object
   at global scope with its own selection mechanism; no handler is a global
   symbol; the engine holds the level objects and dispatches through
   `ObjectOps`. See `docs/EVENT_HANDLERS.md`.
8. **Step budget is mandatory** — a looping handler hangs the engine thread;
   every handler dispatch is wrapped in a VM step budget (reuses the snapshot
   step counter) so runaway loops are killed, not hung. Bounds instructions,
   not wall time.
9. **QLang API vs. engine specifics** — the C++ API (object dispatch, step
   counter, ownership model, error boundary) is part of **QLang**; the
   handler architecture and its budget/retention/re-entrancy *policies* are
   **engine-specifics** built on that API (`docs/EVENT_HANDLERS.md` §0).
   Adding a handler level needs no QLang change — only engine wiring.

## Next steps (in dependency order)

1. **Dot-access audit** (EXTERNAL_TYPES §9, step 1) — determine what the
   parser/lexer/VM already implement for `x.name` / `x.name = v` /
   `x.name(args)` on tables. This sizes the dispatch landing. Nothing in the
   VM implements classes today; table dot sugar status is the open unknown.
2. **Object dispatch layer** (`qlang/QLangObjects.{h,cpp}`) per EXTERNAL
   TYPES §9 build order — table kind port, VM indirection, compiler opcode
   emission, f-string `toStr` branch. Existing table test corpus must pass
   unmodified.
3. **Serialization library codec** per SERIALIZATION.md — `QLangErrorKind::Io`,
   `QLangBytecodeIO.h`, reader with §5 validation, `QLangContext::
   loadProgram/execute`. Then delete the app's `QlcWriter`.
4. **External types API** — `registerType<T>` on `QLangContext`, External
   value tag 17, `ObjectOps` registration delegating to `TypeDescriptor`,
   engine→script callback support (event handlers pass live objects as
   arguments; script holds them weakly).

## Open questions (need user decisions)

1. **Copy vs. view** — resolved: **view only** (see decision 5 above).
2. **Inheritance** — deferred to v2 (both class and external side); `base`
   hook reserved.
3. **Snapshot (v2 / `QLS1`)** — timing: only after the program-image loader
   works end-to-end.

## File map

| File | Role |
|---|---|
| `SPEC.md` | language spec; class section now complete |
| `docs/SERIALIZATION.md` | .qlc/.qls format spec (draft) |
| `docs/EXTERNAL_TYPES.md` | C++ import design + dispatch foundation (draft) |
| `docs/EVENT_HANDLERS.md` | event handler contract: multi-level dispatch, C++ surface, safety (draft) |
| `AGENTS.md` | build/test harness for agents |
| `examples/qlang_run.cpp` | app with `--profile`, `--output`, POC `QlcWriter` |
| `test.qlc` | sample POC bytecode output |
