# Signals — implementation plan

Status: **draft** (2026-11-05) — pending approval.
Spec: `docs/spec/17-signals.md` (tentative draft, SHA256 `1db5c6b6…`, recorded
in `docs/spec/README.md`).
Supersedes: the C6 inventory entry in `docs/plans/missing-features-plan.md`
(Phase 4, item 10).

Signals let an external (C++) type notify script code of state changes. v1
scope: **external types only** — user-defined QLang classes have no signal
declaration syntax.

## What signals do

- **Declaration (C++ meta)** — `QL_SIGNAL(name)` in the `QL_OBJECT` list; the
  argument signature is deduced from a **declared, unimplemented** member
  function (Qt-style). A signal is a name + typed argument list; it has no
  body and is **not a method** (`sensor.levelChanged(1)` is a type error).
- **Emit** — `emit obj.signal(args)`; `emit` is a reserved word. The signal
  name is resolved against the object's type descriptor at runtime; argument
  count and types are checked against the declared signature. Delivery is
  **synchronous** (each connected handler runs to completion, in connection
  order, before `emit` returns). Zero connections → silent no-op. Re-entrant
  emits recurse and are contained by the step budget.
- **Connect / Disconnect** — `connect(sender, "signal", handler)` (function
  value or `BoundMethod`) and `connect(sender, "signal", target, "method")`
  (string-slot form) return an int connection handle; `disconnect(handle)` and
  `disconnect(sender, "signal")` drop connections. A connection retains its
  handler (strong ref) but **not** its sender (weak ref). When the sender is
  destroyed, its connections are dropped at the next finalizer drain.
- **C++ API** — `ctx.hasSignal(obj, name)` and `ctx.emitSignal(obj, name,
  args)`; same checks and delivery rules as script `emit`.

## Design decisions

1. **Signal metadata** — a new `SignalMeta` struct (name + one per-argument
   convertibility checker) deduced from the member function pointer by a
   `signal()` template in `meta_detail`. Each checker reuses
   `QLangValue::as<A>()`, so the argument conversion rules match method calls
   exactly (the spec's "same conversion rules as method calls").
2. **No new value type** — a signal is not a value; it is a name on the type
   descriptor. Reading `obj.signal` is a type error (signals are not methods
   and produce no `BoundMethod`).
3. **Connection registry on the context** — `std::unordered_map<int,
   SignalConnection>`; the handle is a monotonically increasing int. A
   connection stores a **weak** reference to the sender (does not keep it
   alive) plus a **strong** reference to the handler (and, for the string-slot
   form, the target).
4. **Delivery lives in the VM** — the `EMIT` opcode calls a new
   `QLangVM::deliverSignal()` member; the C++ `emitSignal` creates a VM and
   delegates to the same method. One home for the delivery logic (resolve
   signal → check args → iterate connections → call handlers).
5. **Auto-disconnect** — connections hold a `weak_ptr` to the sender; expired
   connections are skipped at emit time and dropped at the finalizer drain
   (`processFinalizers()`), matching the spec's "dropped via the finalizer
   queue".
6. **Opcode** — `EMIT` is appended **last** in the `Opcode` enum. The `.qlc`
   format version stays v5 (the escape-byte stream already carries arbitrary
   opcode numbers; only `kOpcodeUsedCount` changes).

## Impact assessment

| Area | File(s) | Change | Risk |
|------|---------|--------|------|
| Signal meta | `include/qlang/QLangMeta.h` | New `SignalMeta` struct; `signal()` template (deduces param types from the member function pointer, one `as<A>()`-based checker per arg); `QL_SIGNAL(name)` macro; `add(TypeMeta&, SignalMeta)` overload; `TypeMeta::signals` | Low. Pure addition; the member-pointer deduction mirrors the existing `method()` template. |
| Type descriptor | `include/qlang/QLangTypes.h` | `TypeDescriptor` gains `std::vector<SignalMeta> signals` | Low. |
| Registration | `src/QLangContext.cpp` | `registerType()` copies `meta.signals` into the descriptor and validates signal names (clash with a property/method of the same type, duplicate signal) → `QLangError` at import | Low. Mirrors the existing member-name validation. |
| Lexer | `include/qlang/QLangLexer.h`, `src/QLangLexer.cpp` | `TokenType::Emit`; `keywordType()` maps `"emit"` | Low. |
| Parser | `include/qlang/QLangParser.h`, `src/QLangParser.cpp` | `StmtKind::Emit`; `parseEmit()` (`emit obj.signal(args)`); `Stmt` fields `emitSender`/`emitSignal`/`emitArgs` | Low. Mirrors `parseAssert` (a keyword that parses a call-shaped body). |
| Opcode | `include/qlang/QLangBytecode.h`, `src/QLangBytecode.cpp` | `EMIT` appended last; `name()` → `"emit"`; `kOpcodeUsedCount` 69 → 70 | **Enum position is load-bearing**: appending keeps every existing `.qlc` valid. |
| Compiler | `src/QLangCompiler.cpp` | `compileEmit()` — evaluate the sender to a register, load the signal name as a string constant, evaluate the args into r[a+1..a+c], emit `EMIT a, b, c` | Low. Mirrors the `CALL`/`CALLCLOS` codegen (same arg-register convention). |
| VM | `include/qlang/QLangVM.h`, `src/QLangVM.cpp` | `EMIT` case in `runFrames()`; `deliverSignal(sender, signal, args)` member (resolve signal, check arity/types, iterate the context's connections, call each handler); `callHandler(value, args)` helper (Function via `callClosure`, `BoundMethod` invoked inline like the `CALLCLOS` case) | Medium: the delivery logic is new; re-entrancy is contained by the step budget, not a guard. |
| Context API | `include/qlang/QLangContext.h`, `src/QLangContext.cpp` | `SignalConnection` struct; the connection registry + next-handle counter; public `hasSignal`/`emitSignal`/`connectSignal`/`connectSignalSlot`/`disconnectSignal`/`disconnectSignalAll`/`dropExpiredConnections`; `connect`/`disconnect` native builtins (arity-dispatched); `dropExpiredConnections()` called from `processFinalizers()` | Medium: the registry lifecycle (weak sender, strong handler, drain-time cleanup) must be correct. |
| Tests | `tests/unit/signal_test.cpp`, `CMakeLists.txt` | New `signal_test` suite (precedent: `class_static_test`); `add_executable` + `add_test` | — |
| Docs | `docs/spec/17-signals.md`, `docs/spec/README.md`, `docs/compliance/status.md` (+ `history.md`) | Finalize the spec (drop "tentative draft"), record the new hash, add a compliance round entry, flip the C6 open item to verified | Per AGENTS.md: **test first, then bump hashes**. |

Not affected: register allocation (EMIT uses a dest + arg registers like
CALL), jump backpatching (EMIT is not a jump), `valueKey`/constant-pool dedup,
the `.qlc` format version (v5 stays; EMIT serializes as a new opcode number in
the existing escape-byte stream), the disassembler (a new `emit` mnemonic is
added in `examples/qlang_run.cpp` for completeness).

## Implementation steps

1. **`QLangMeta.h`** — `SignalMeta` struct; `signal()` template (one
   `as<A>()`-based checker per deduced parameter); `QL_SIGNAL(name)` macro;
   `add(TypeMeta&, SignalMeta)`; `TypeMeta::signals`.
2. **`QLangTypes.h`** — `TypeDescriptor::signals`.
3. **`QLangContext.cpp`** — `registerType()` copies `meta.signals` into the
   descriptor; validates signal names (property/method clash, duplicate) →
   `QLangError`.
4. **`QLangLexer.h/.cpp`** — `TokenType::Emit`; `keywordType()` maps `"emit"`.
5. **`QLangParser.h/.cpp`** — `StmtKind::Emit`; `parseEmit()`; `Stmt` fields
   `emitSender`/`emitSignal`/`emitArgs`.
6. **`QLangBytecode.h/.cpp`** — `EMIT` opcode (append last); `name()` case;
   `kOpcodeUsedCount` 69 → 70.
7. **`QLangCompiler.cpp`** — `compileEmit()` (sender reg, signal-name
   constant, args in r[a+1..a+c], `EMIT a, b, c`).
8. **`QLangVM.h/.cpp`** — `EMIT` case in `runFrames()`; `deliverSignal()`;
   `callHandler()`.
9. **`QLangContext.h/.cpp`** — `SignalConnection`; the registry + handle
   counter; public API (`hasSignal`/`emitSignal`/`connectSignal`/
   `connectSignalSlot`/`disconnectSignal`/`disconnectSignalAll`/
   `dropExpiredConnections`); `connect`/`disconnect` builtins;
   `dropExpiredConnections()` from `processFinalizers()`.
10. **`examples/qlang_run.cpp`** — `emit` disassembler mnemonic (completeness).
11. **Tests** — `tests/unit/signal_test.cpp`; CMake `add_executable` +
    `add_test`. Build `cmake -B build && cmake --build build`, run
    `ctest --test-dir build`.
12. **Docs** — finalize the spec section, record the hash, add the compliance
    round entry, flip the C6 open item.

## Test cases

Dedicated suite `signal_test` (precedent: `class_static_test`):

- **emit no-op** — `emit sensor.levelChanged(3)` with zero connections →
  silent no-op (no throw, no side effect).
- **connect + emit (function value)** — `connect(sensor, "levelChanged",
  onLevel)` then `emit` → the handler runs with the emitted args.
- **connect + emit (string-slot)** — `connect(sensor, "levelChanged", ui,
  "show")` then `emit` → `ui.show(args)` runs; a missing method is a silent
  no-op.
- **argument conversion + arity error** — `emit sensor.levelChanged()` (0
  args, expects 1) → type error; `emit sensor.levelChanged("x")` (string for
  an int param) → type error.
- **unknown signal** — `emit sensor.nope(1)` → `type error: unknown signal
  'nope' on type 'Sensor'`.
- **disconnect by handle** — `disconnect(c)` → a subsequent emit does not call
  the handler; `disconnect` with a stale handle is a silent no-op.
- **disconnect by signal** — `disconnect(sensor, "levelChanged")` → all
  connections for that signal are dropped.
- **auto-disconnect on sender destruction** — drop the sender's last reference
  → the next finalizer drain drops its connections; a later emit does not call
  the stale handler.
- **re-entrant emit + step-limit containment** — a handler that emits the same
  signal → recursion is contained by the step budget (`runtime error: infinite
  loop (step limit)`), not a re-entrancy guard.
- **C++-side `emitSignal`** — `ctx.emitSignal(sensor, "levelChanged", {3})` →
  the connected handler runs.
- **`hasSignal`** — `ctx.hasSignal(sensor, "levelChanged")` → true;
  `ctx.hasSignal(sensor, "nope")` → false.
- **emit on a non-external object** — `emit table.signal(1)` → type error.
- **signal name clash at registration** — a type whose signal name matches a
  property/method → `QLangError` at `import`.
- **duplicate signal name at registration** → `QLangError` at `import`.

## Risks / open questions

1. **Delivery re-entrancy** — a handler that emits (the same or another
   signal) recurses `deliverSignal` → `callHandler` → `callClosure` → a new
   frame. The step budget contains runaway recursion; there is no re-entrancy
   guard (per spec).
2. **Connection handle space** — the handle is a monotonically increasing int;
   it does not wrap in practice. A stale handle is a silent no-op (per spec).
3. **Weak sender identity** — a connection matches its sender by the locked
   `weak_ptr`'s object pointer. Two distinct objects of the same type never
   collide (pointer identity); a borrowed sender the engine drops expires
   cleanly.
4. **String-slot method resolution** — reuses `ObjectOps::callMethod`; a
   missing method is a silent no-op (the event-handler miss rule), but a
   method that throws propagates (the handler's error becomes the emit's
   error).
5. **Spec finalization** — the draft is "tentative". Open design points to
   confirm before the docs step: whether `emit` is a statement (this plan
   assumes so, like `assert`) or an expression, and the exact error-message
   wording. The language-side `emit` keyword also lands in `02-lexical.md`
   (reserved words) on finalization.
