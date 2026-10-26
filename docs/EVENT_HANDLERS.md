# Event Handler Contract (draft)

**Engine-specifics document.** How a game engine (C++) drives QLang script
as event handlers. The QLang C++ API it builds on — the object dispatch
surface, the step counter, the ownership model, the error boundary — is part
of **QLang** and is documented in `docs/EXTERNAL_TYPES.md` and `SPEC.md`.
Everything in *this* document (the multi-level handler architecture, the
level objects, the selection mechanisms, the budget/retention/re-entrancy
*policies*) is the engine's use of that API, not part of QLang.

Status: **POC-stage design.** Consumes `SPEC.md` (classes),
`docs/EXTERNAL_TYPES.md` (external types + dispatch = the QLang API), and
`docs/SERIALIZATION.md` (step counter, durability).

## 0. Boundary: QLang API vs. engine specifics

| Concern | Lives in |
|---|---|
| `ObjectOps` dispatch (`getField`/`setField`/`callMethod`/`toStr`) | **QLang** — `EXTERNAL_TYPES.md` §9 |
| `QLangContext` wrappers: `getVar`, `hasMethod`, `callMethod` | **QLang** — the C++ API |
| VM step counter (instruction budget primitive) | **QLang** — `SERIALIZATION.md` §7 |
| Ownership model (weak/shared refs, engine authoritative) | **QLang** — `EXTERNAL_TYPES.md` §3 |
| Structured `QLangError` at the boundary | **QLang** — `EXTERNAL_TYPES.md` §8 |
| Multi-level handler architecture (type / map / game) | **Engine** — this doc |
| Level objects at global scope + selection mechanisms | **Engine** — this doc |
| Type-tag → handler-object lookup | **Engine** — this doc |
| Policy: budget every dispatch / retain level objects / convert event args | **Engine** — this doc |
| Policy: queue + drain events, no re-entrancy, no suspension | **Engine** — this doc |

The engine *uses* QLang's primitives; it does not extend them. Adding a
handler level needs no QLang change — only engine wiring (§3.4).

---

## 1. Architecture: multi-level dispatch

Handlers are **not** a single flat listener object. They form a
**multi-level handler architecture**: handlers are organized by dispatch
level, and each level has its own container object(s) *and* its own selection
mechanism. One object with a flat method set cannot express this.

Known levels (extensible — "etc" is intentional):

| Level | Selection mechanism | Example handlers |
|---|---|---|
| **type-based** | by **type tag** | `TYPE_DIAMOND`, `TYPE_SWAMP` |
| **map-based** | lifecycle event **name** | `onLoad()`, `onGoalCompleted()` |
| **game-level** | game event **name** | `onStart()`, `onGameOver()` |

What unifies every level: each is a QLang object at global scope, the engine
holds the **set** of level objects, and **every dispatch — at every level —
goes through the same `ObjectOps.callMethod`**, hasMethod-guarded and
step-budgeted. The type level adds a type-tag → object lookup *before* the
method call. This is why the object dispatch lands first: it is the single
foundation under all levels.

## 2. Scope model

- **No handler exists at global scope.** There is no `on_click` global symbol
  for the engine to look up.
- Handlers are **enclosed members** — methods *on* a table or class object.
- The **container objects** live at global scope (one per level, e.g.
  `typeHandlers`, `mapHandlers`, `gameHandlers`).
- The **engine holds the container objects** and dispatches *methods on
  them*. It never resolves a handler by a global name.
- Multiple level objects coexist; each encapsulates its own methods and
  `self`/state. The global namespace gets one entry per level, not N handler
  functions.

Consequence: "registration" = the engine is **handed the set of level
objects** explicitly. There is no implicit global resolution.

## 3. The levels

### 3.1 Type-based — selection by type tag

The engine knows an object's type and dispatches to the handler *for that
type*. Selection is by **type tag**, not event name — this is the level that
does not fit the "method name = event name" mold.

Model: a **type → handler-object** mapping. Each type maps to its own handler
(a class instance, so it carries per-type `self`/state and a full method set):

```
let typeHandlers = {
   TYPE_DIAMOND = DiamondHandler()
   TYPE_SWAMP   = SwampHandler()
}
# engine: resolve typeHandlers[typeTag] -> callMethod(that, "handle", {obj})
```

- The lookup key is the **same type identity the external-type registry uses**
  (`type_info` / type tag, `docs/EXTERNAL_TYPES.md` §2). This level is keyed
  by the type system, not ad-hoc.
- After the type lookup resolves to a handler object, the engine calls a
  **conventional entry method** (e.g. `handle(obj)`) — a stable call target.
  The engine resolves *which object* by type, then calls a known method on it.
- **Lightweight fallback**: if a type is a single function with no state,
  type-constant-named methods on one dispatcher object work
  (`dispatch.TYPE_DIAMOND(obj)`). The moment a type needs state or several
  methods, it wants its own object.

### 3.2 Map-based — lifecycle events

Lifecycle events as **methods on a map handler object**. This is the level
that matches the simple "method name = event name" model:

```
class MapHandlers {
   onLoad()[] { ... }
   onGoalCompleted()[] { ... }
}
let mapHandlers = MapHandlers()
# engine: callMethod(mapHandlers, "onLoad", {})
```

### 3.3 Game-level events

Game-event methods on a game handler object:

```
let gameHandlers = {
   onStart()[] { ... }
   onGameOver()[] { ... }
}
# engine: callMethod(gameHandlers, "onStart", {})
```

### 3.4 Extensibility

New levels follow the same shape: a container object at global scope + a
selection mechanism + dispatch through `ObjectOps`. Adding a level requires
no new engine machinery — only a new level object and (if selection is by
key) a lookup.

## 4. C++ call surface

The QLang API supplies the verbs; the engine supplies the wiring.

**QLang API (the library):** `ctx.getVar`, `ctx.hasMethod`, `ctx.callMethod`
— thin wrappers over `ObjectOps`. They are generic; they know nothing about
handlers, levels, or types.

**Engine wiring (this doc):** which level objects to hold, how to select a
handler (by type tag or event name), and the policy of guarding + budgeting
each call.

```cpp
// QLang API calls, used by engine wiring:
QLangValue typeHandlers = ctx.getVar("typeHandlers");   // engine holds these
QLangValue mapHandlers  = ctx.getVar("mapHandlers");
QLangValue gameHandlers = ctx.getVar("gameHandlers");

// type event:  engine resolves by type tag, then calls the entry method
if (auto h = lookupType(typeHandlers, typeTag))         // engine logic
   budgetedCall(ctx, *h, "handle", {objValue});         // API + engine policy

// map event:   method name = event name (engine chooses the name)
if (ctx.hasMethod(mapHandlers, "onLoad"))
   budgetedCall(ctx, mapHandlers, "onLoad", {});

// game event:
if (ctx.hasMethod(gameHandlers, "onStart"))
   budgetedCall(ctx, gameHandlers, "onStart", {});
```

- `lookupType` is **engine** logic (a table read + null check) for the type
  level.
- `budgetedCall` wraps the QLang `callMethod` with the engine's step budget
  (§8.1). The budget is engine policy; the step counter is QLang's.

## 5. Table vs. class

- **Class instance is the primary idiom** — it gives `self`, per-level /
  per-type **state** (fields), and dunders. The type level in particular
  wants class instances (each type carries its own state and method set).
- **Table-of-functions** is the lightweight option for stateless callbacks.
  It depends on the dispatch binding the table as `self` on `CALLMETHOD` —
  the open question in the dot-access audit (`docs/EXTERNAL_TYPES.md` §9,
  build step 1). If the audit shows table-method dispatch works, tables are a
  valid stateless option; otherwise that support must be added.

## 6. Missing handler / lookup-miss = no-op

A normal script `CALLMETHOD` errors on an unknown method. Engine event
dispatch must **tolerate** absence — you do not want every event to force
every handler. At **every level**, a miss is a silent no-op:

- unknown **type** (no entry in `typeHandlers`) → skip
- unimplemented **lifecycle event** (`hasMethod` false) → skip
- absent **game handler** → skip

Hence the `hasMethod` / lookup guards in §4.

## 7. Ownership and retention

The **ownership model** (engine authoritative; `weak_ptr` for engine handoff,
`shared_ptr` for script construction; no copy path) is **QLang's** —
`docs/EXTERNAL_TYPES.md` §3. What follows is the **engine's retention
policy** for handlers, applied on top of that model:

- **Retain the level objects** (the listeners). Intended retention: they are
  script-owned / reference-counted, and the engine's reference keeps them
  alive while registered — freed on **unregister** or **program reload**.
- **Never cache per-event payloads.** Retain the *listener*, convert *event
  arguments* to C++ types at the boundary. A retained payload would silently
  extend a script object's lifetime.
- **Script → engine transfer** (a script passes its own object into a
  native, e.g. `engine.spawn(player)`) needs a **per-binding ownership
  policy** — `borrow` / `take_ownership` / `copy` — declared in the
  `MethodBinding` (a QLang API extension point). Without it, co-ownership
  creeps back in the reverse direction.
- **Handed-in engine objects** are held by the script as `weak_ptr`
  (QLang model, `EXTERNAL_TYPES.md` §3); access after engine destruction is
  a clean `object destroyed: <type>` error.
- **Lifetime surprise (document for script authors):** `handlers = nil` in
  script does not free a level object while the engine still holds it.

## 8. Execution safety

The QLang API is single-threaded and provides a step counter; it does **not**
police how a host drives it. The safety *policies* below are the engine's
responsibility, built on QLang's primitives.

### 8.1 Step budget (infinite-loop containment)

A looping handler **hangs the engine thread** — there is no other outcome
without a guard. The containment is a **step budget** around every
`callMethod` dispatch:

- The language has `while` / `for` with no built-in iteration guard, so an
  infinite loop is **syntactically possible**.
- It is **practically contained**: the engine records the VM step count at
  dispatch start and compares it in the loop; a `while true {}` in a handler
  exhausts the budget and is **killed with a clean, bounded error** — a
  recoverable event, not a hang.
- This reuses the VM's **existing step counter** (already tracked for `.qls`
  snapshot/resume, `docs/SERIALIZATION.md` §7). The event-handler focus makes
  the budget a **complete** containment: guard the one dispatch boundary
  instead of the whole runtime.
- **Caveat:** the budget bounds **VM instructions, not wall time.** A loop
  spinning on a *blocking native call* does not advance the VM counter, so
  the budget will not fire. Bounding blocking calls is the engine's separate
  responsibility.
- **This is a rule the engine must implement**, not an inherent property.
  Without it, the clean call surface is equally a clean hang surface. The
  handler-object pattern is the API; the budget is the containment. They land
  together.

### 8.2 Re-entrancy and threading

The VM is **single-threaded and not re-entrant**.

- Events are **queued** and **drained at safe points** between script
  invocations — **never fired inside a handler**. A handler that calls an
  engine method which synchronously fires another event would re-enter the VM
  mid-frame.
- Events are delivered **on the VM thread**. If events arrive from other
  threads, the engine **marshals** them onto the VM thread.

### 8.3 No suspension across events

A `yield` inside a handler parks the only frame stack; the next event cannot
run. **Handlers run to completion** — no suspension across events. (POC rule;
relaxing it requires multi-entry VM support.)

### 8.4 Handler error shape

A script error inside a handler crossing to C++ is a **structured status**
(error kind + message) returned from the engine's call API — never a raw
`QLangError` the engine did not ask for. The engine decides how to report or
swallow it.

## 9. Build dependencies

All **QLang-side** landings the engine depends on — none are engine work
beyond the wiring itself.

- **Object dispatch first** (`docs/EXTERNAL_TYPES.md` §9) — the shared
  `ObjectOps` layer is the foundation every level dispatches through.
- **Dot-access audit** (`docs/EXTERNAL_TYPES.md` §9, step 1) — determines
  whether table-method dispatch (table-as-`self`) works, which decides if
  tables are a valid stateless handler option (§5).
- **Step counter** — already present for snapshots; the budget wraps it
  (§8.1).
- **External type registry** — provides the type identity the type level is
  keyed by (§3.1).
