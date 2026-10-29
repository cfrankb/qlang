# Agent instructions

## Documentation Architecture
Documentation is modular under `docs/` (migrated 2026-10-29 from the monolithic root files, which are now stubs).
- **Specification:** `docs/spec/` — 12 domain modules; `docs/spec/README.md` is the entry point.
- **Compliance:** `docs/compliance/` — modular validation sets; `docs/compliance/README.md` is the entry point.
- **Instruction:** Do not append large blocks to the legacy root stubs. Create targeted `.md` files in `docs/` subdirectories. Consult directory `README.md` files as your entry points.

## Session start
- Reload `docs/spec/README.md` (and the modules it indexes) and compare against `docs/compliance/README.md` (and its modules).

## Keep Compliance Documentation Current
- Track the SHA256 of the specific specification file/module being described.
- On any change to a specification or implementation:
  1. Rebuild and re-run the affected test suites.
  2. Flip relevant Open Items to verified.
  3. Update the hash, tally, and checkpoint rows.
- Never bump a hash before testing. Testing proves correctness; the hash merely detects change.

## Build & test (required before claiming "verified")
- Run: `cmake -B build && cmake --build build`
- Run: `ctest --test-dir build` (or execute a single suite like `spec_check2.cpp` against `build/libqlang.a`).
- Always build out-of-source (`cmake -B build`). Never allow generated files to touch the source root.

## Diagnostic output (trace)
- Route all diagnostic/trace output through the gated `trace()` helper. Never use raw `fprintf(stderr, …)`.
- Use levels from the `QLangContext` `TRACE_*` enum:
  - `TRACE_OFF` (0)
  - `TRACE_FRAME` (1: frame/call events)
  - `TRACE_VM_LOOP` (2: VM loop, opcodes, returns)
  - `TRACE_FULL` (3: generators, table ops, compiler internals)
- VM sites call `trace()` on `QLangVM`; compiler sites call it on `QLangCompiler`.
- Keep format strings cheap; `trace()` short-circuits below the context's verbose level. Never route regular program output here.

## CMake layout
- Maintain separate, independently buildable targets:
  - Library: `src/*.cpp` → `libqlang.a`
  - Demo: e.g., `examples/hello.cpp`
  - Tests: One set per feature area (defined in the specification).

## Commits
- Suggest a commit name; do not commit automatically.
- Stage by concern. Keep unrelated doc edits out of feature commits.
- Exclude or delete patch artifacts (`*.orig`, `*.rej`).

## Edit this file
- Always ask for explicit user permission before editing AGENTS.md.
