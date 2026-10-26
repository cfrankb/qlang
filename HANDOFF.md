# Handoff — QLang: Phase 7 finalizer fix (COMPLETE)

Date: 2026-10-26. Supersedes the 2026-10-25 design handoff (that session's
docs remain valid: `docs/SERIALIZATION.md`, `docs/EXTERNAL_TYPES.md`,
`docs/EVENT_HANDLERS.md`, `SPEC.md`).

## Project
- `/home/cfrankb/toolkit/gcc/qlang-sandbox` — QLang → register-VM bytecode, C++17.
- Build: `for f in src/*.cpp; do g++ -std=c++17 -O1 -Iinclude "$f" -c -o "build/$(basename ${f%.cpp}).o"; done && ar rcs build/libqlang.a build/*.o`
- Test: `g++ -std=c++17 -O1 -Iinclude examples/spec_check2.cpp build/libqlang.a -o /tmp/sc && stdbuf -oL -eL /tmp/sc 2>/dev/null`
  (stderr = debug logging; stdout block-buffered).

## This session: finalizer bug fixed, verified
**Bug**: `__destroy__` fired at frame end, not at last user-reference release.
**Root cause**: statement codegen compiles RHS into temp `t`, then
`MOV r, t`; `tmpRegister()` never reuses registers, so `t` held the
instance until frame end → `use_count` never hit 0.

**Changes (all in place, all verified)**:
1. `src/QLangCompiler.cpp` — after `MOV r, t` in the `local` declaration
   (~L460), `Const` (~L490), and `Assignment "="` (~L545) cases, emit
   `CONST t, nil`. Compound assignments untouched.
2. `src/QLangVM.cpp` — `attachFinalizer` (~L58) wraps the table deleter:
   wrapped deleter keeps the original `shared_ptr` alive, calls
   `scope.runFinalizer(map, closure, progId)`, then `orig.reset()` frees
   the map. `TABFINALIZE` opcode case calls it.
3. `src/QLangContext.cpp` — `runFinalizer` re-owns the map with a no-op
   deleter (refcount > 0 during the call → no recursive re-entry), runs
   `__destroy__` with the table as `self`, swallows exceptions (deleter
   may run during unwinding).
4. Debug instrumentation removed (`g_instanceMap`, `dbgVars()`, per-instr
   trace, all raw `fprintf(stderr)` lines). No raw stderr prints in src/.

**Verification**: 240/240 spec_check2 pass, incl. 4 finalizer tests
(runs once, not before final ref, absent-finalizer no-crash, writes self
field). Scenario: `a = D(log); b = a; a = nil; b = nil; n = log.n` →
finalizer fires at `b = nil`, `n = 1`.

## Known limits (accepted)
- Immediacy only for statement-level `local x = D(...)` / `x = D(...)`.
  Constructor results in deeper expression temps finalize at frame end.
  Correctness (runs exactly once, sees final state) holds everywhere.

## Next steps (carried over, in dependency order)
1. **Serialization library codec** per `docs/SERIALIZATION.md` —
   `QLangErrorKind::Io`, `QLangBytecodeIO.h`, reader with §5 validation,
   `QLangContext::loadProgram/execute`. Then delete the app's `QlcWriter`
   (POC in `examples/qlang_run.cpp`, emits v1 minus native name table).
2. **External types API** per `docs/EXTERNAL_TYPES.md` — `registerType<T>`,
   External tag 17, `ObjectOps` kind registration, engine→script callbacks
   (script holds engine objects via `weak_ptr`, script-constructed via
   `shared_ptr`).
3. **Snapshot `QLS1` v2** — only after the program-image loader works
   end-to-end.

## Open (need user decision)
- Inheritance: deferred to v2; `base()` hook reserved in the v1 struct.

## Stale
- The 2026-10-25 handoff's "dot-access audit" and "object dispatch layer"
  steps are done — class dispatch and `__destroy__` are implemented.
- The `qlang-sandbox` skill is stale — reload with skill_view and update
  before relying on it.
