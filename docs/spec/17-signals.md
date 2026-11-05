# signals

**Status: final (2026-11-05) — implemented and conforming.**
Resolves the open question in `docs/EXTERNAL_TYPES.md` §10.4 ("Signals —
revisit with `docs/EVENT_HANDLERS.md`"). Supersedes the C6 inventory entry in
`docs/plans/missing-features-plan.md`.

Signals let an external (C++) type notify script code of state changes.
v1 scope: **external types only** — user-defined QLang classes have no
signal declaration syntax.

## declaration (C++ meta)

Signals are declared in the `QL_OBJECT` meta with `QL_SIGNAL(name)`. The
argument signature is deduced from a **declared, unimplemented** member
function (Qt-style):

```cpp
struct Sensor : qlang::QLObject<Sensor> {
  int level = 0;
  void levelChanged(int newLevel);   // declared only — never defined
  QL_OBJECT(
    QL_PROPERTY(level),
    QL_SIGNAL(levelChanged)
  );
};
```

- a signal is a name + typed argument list; it has no body and is **not a
  method** — `sensor.levelChanged(1)` is a type error.
- `QL_SIGNAL` takes the member function pointer `&SelfType::levelChanged`;
  parameter types are deduced exactly as for `QL_INVOKABLE`.
- no `Q_SIGNALS` section marker: every `QL_` line is self-describing
  (deviation from Qt; consistent with the no-MOC meta model).
- a signal name clashing with a property/method name of the same type, or a
  duplicate signal name, is rejected at registration time (like duplicate
  members).

## emit

```
emit sensor.levelChanged(3)
```

- `emit` is a reserved word (`02-lexical.md`).
- the target is a postfix chain (member/subscript access, no calls); the
  **last** dot separates the target from the signal name, so
  `emit a.b.c(1)` emits signal `c` on object `a.b`.
- the signal name is resolved against the object's type descriptor at
  runtime: unknown name → `type error: unknown signal 'x' on type 'T'`.
- argument count and types are checked against the declared signature (same
  conversion rules as method calls).
- `emit` with zero connections is a silent no-op.
- delivery is **synchronous**: each connected handler runs to completion, in
  connection order, before `emit` returns.
- re-entrant emits (a handler emits the same or another signal) are allowed
  and recurse; runaway recursion is contained by the step budget
  (`runtime error: infinite loop (step limit)`), not by a re-entrancy guard.
- `emit` runs only on the VM thread. A host emitting from another thread
  must marshal onto the VM thread (same rule as event dispatch,
  `docs/EVENT_HANDLERS.md` §8.2).

## connect / disconnect

```
let c  = connect(sensor, "levelChanged", onLevel)     # function value
let c2 = connect(sensor, "levelChanged", ui, "show")  # target + method name
disconnect(c)                                          # by handle
disconnect(sensor, "levelChanged")                     # all for that signal
```

- `connect(sender, "signal", handler)` — `handler` is a QLang function
  (def/lambda) or a `BoundMethod`; returns a connection handle (int).
- `connect(sender, "signal", target, "method")` — string-slot form; the
  method is resolved on `target` at emit time. A missing method is a silent
  no-op (the event-handler miss rule, `docs/EVENT_HANDLERS.md` §6).
- `connect` on a non-external object, or with an unknown signal name, is a
  type error.
- a connection retains its handler (the closure's reference count is raised
  while connected).
- when the sender is destroyed, all its connections are dropped via the
  finalizer queue (drain points: `ctx.processFinalizers()` et al.).
- `disconnect` with a stale handle is a silent no-op.

## C++ API

```cpp
bool QLangContext::hasSignal(const QLangValue& obj, const std::string& name) const;
void QLangContext::emitSignal(const QLangValue& obj, const std::string& name,
                              const std::vector<QLangValue>& args);
```

- `emitSignal` is the C++-side emit (a C++ object notifying script of a
  state change). Same checks and delivery rules as script `emit`.
- `hasSignal` mirrors `hasMethod` (C3) for the event-handler contract.

## restrictions (v1)

- external types only — no signal declaration on QLang classes
- no queued/async delivery (synchronous direct only)
- no signal-to-signal connections
- no connection modifiers (Qt's `UniqueConnection` etc.)
- `emit` on a non-external object → type error

## tests

Dedicated suite `signal_test` (precedent: `class_static_test`): emit
no-op, connect + emit (both handler forms), argument conversion + arity
error, unknown signal, disconnect by handle / by signal, auto-disconnect on
sender destruction, re-entrant emit + step-limit containment, C++-side
`emitSignal`, `hasSignal`.
