# C++ External Types — Importing C++ Structures via the QLang API

Status: **draft — design findings, decisions open**
Related: `SPEC.md` (language), `docs/SERIALIZATION.md` (§3 value tags), `docs/EVENT_HANDLERS.md` (handler dispatch)

## 1. Goal

Expose C++ structs/classes to QLang scripts: create instances, read/write
fields, call methods — through a public registration API on
`QLangContext` (not yet defined).

## 2. The RTTI reality check

C++ RTTI **cannot do this alone**. `type_info` provides type identity and
`dynamic_cast` only; it has zero knowledge of member layout, fields, or
methods. A struct's fields cannot be enumerated from `type_info`.

The design is therefore: **explicit registration tables, with RTTI as the
identity/safety layer** (the pybind11 model).

RTTI's actual contributions:

1. `std::type_index` as the registry key — compile-time-safe lookup, no
   stringly-typed keys.
2. `dynamic_cast` validation when C++ hands an object across the boundary
   (and inheritance support, if added later).
3. `type_info::name` for a `type()` builtin and diagnostics.

Everything else comes from a host-written registration table:

```cpp
struct FieldBinding {
  const char* name;
  QLangValue (*get)(const T&);
  void (*set)(T&, const QLangValue&);   // nullptr = read-only
};

struct MethodBinding {
  const char* name;
  QLangValue (*invoke)(T*, QLangContext&, const std::vector<QLangValue>&);
};

struct TypeDescriptor {
  std::type_index id;        // RTTI identity
  const char* name;          // script-visible name
  std::shared_ptr<void> (*construct)(QLangContext&,
                                     const std::vector<QLangValue>&);
  std::vector<FieldBinding> fields;
  std::vector<MethodBinding> methods;
  QLangValue (*str)(const T&) = nullptr;   // __str__, optional
  void (*destroy)(T*) = nullptr;           // __destroy__, optional
};
```

A `registerType<T>(ctx, ...)` template fills the descriptor with minimal
boilerplate.

**Memory-safety invariant:** script access goes *only* through registered
bindings — never raw offsets, never `void*` escape hatches. No
memory-safety hole by construction.

## 3. Value representation and ownership

New `QLangValue::Type::External` — **tag 17** in the serialization spec
(amendment to `docs/SERIALIZATION.md` §3):

```
payload: const TypeDescriptor* + ownership handle (see below)
```

**The engine (C++) is the authoritative owner.** QLang is a consumer of
engine state, not an owner of it. Ownership follows the object's source:

| Source | Handle held by script | Lifetime authority |
|---|---|---|
| engine handoff (event handler args, native return values) | `weak_ptr<void>` | **engine** — the script cannot keep it alive |
| script construction (`let p = Point(1,2)`) | `shared_ptr<void>` | script (the engine never held a reference) |

- Engine-handoff objects: every access `lock()`s the weak reference. If the
  engine has destroyed the object, access is a clean runtime error
  (`object destroyed: <type>`) — never a dangling pointer, never a silent
  extension of the engine's lifetime.
- Script-constructed objects die when the script drops its last reference.
- **No copy path** (decision): there is no `to_table` / `from_table`
  flatten API. Engine objects are **long-lived** and continuously evolving;
  a QLang deep copy is a **short-lived snapshot** — stale by the next event,
  diverged from the authoritative state. The script must operate on the
  *live* object.
- **Not durable**: External values cannot be written to `.qlc`. The writer
  emits tag 0 (Nil), same rule as Generator (SERIALIZATION.md §3 R2 extends
  to tag 17).

## 4. Field access — resolved: dot syntax per SPEC.md

SPEC.md already specifies the object model ("user defined classes" section):
dot field access (`person.name`, `self.total`), constructor calls
(`let money = Money(125)`), dot method calls (`money.receive(25)`), and
f-string interpolation that resolves fields and methods
(`f"balance: {money.balance()}"`). Dot access on table literals is existing
sugar over the key-resolution pipeline.

**There is no syntax fork.** External C++ values must participate in the
*same* object dispatch as QLang classes, at four points:

| Dispatch point | Table/Class behavior | External behavior |
|---|---|---|
| field get (`p.x`) | map lookup / instance field | registered `FieldBinding::get` |
| field set (`p.x = v`) | map insert / field store | registered `FieldBinding::set` (nullptr → read-only error) |
| method call (`p.m(a)`) | class method with `self` | `MethodBinding::invoke` with `self` |
| string conversion (`f"{p}"`, `str(p)`) | `__str__` | registered `__str__` binding (else → type name + error) |

The VM's object-dispatch path is shared between QLang classes and External
types; a script cannot tell the difference. Note: the SPEC.md class section
is spec-only — not yet implemented in the VM. The object-dispatch path is
the shared foundation both features build on (see §9).

## 5. Methods

New `NativeMethod` signature — the existing `NativeFunc` has no `this`:

```cpp
using NativeMethod =
    QLangValue (*)(void* self, QLangContext&, const std::vector<QLangValue>&);
```

- `self` first — matches the SPEC.md class-method convention
  (`receive(self, amount)`).
- No overloading in v1: method names are unique per type.
- Invocation dispatches through the type's `MethodBinding` table.

### Dunder bindings

The class model's dunders map directly onto optional descriptor bindings:

| Dunder | Descriptor binding | Behavior |
|---|---|---|
| `__str__` | `QLangValue (*str)(const T&)` | used by `str(p)` and `f"{p}"`; if absent, string conversion is a `Type` error naming the type |
| `__destroy__` | `void (*destroy)(T*)` | cleanup hook run on the **final** `shared_ptr` release; `shared_ptr` still owns lifetime — this is for resource teardown (handles, sockets), not memory |

`__init__` is the constructor binding (§6).

## 6. Construction

Registered constructors make the type name callable:

```
let p = Point(1, 2)
```

Exactly the SPEC.md form (`let money = Money(125)`). The compiler's name
resolution already has a native-function path; type names join that table.
Small compiler/VM change, large ergonomics win.

## 7. Boundary conversion table

Applied at every field get/set and method argument/return:

| C++ | QLang |
|---|---|
| bool | Bool |
| char | Char |
| int8 / uint16 / int16 / int32 / uint32 | Byte / Word / Short / Int / Unsigned |
| int64 / uint64 | Qword |
| float / double | Float |
| std::string | String |
| enum | Int |
| std::vector\<T\> | Array |
| nested registered type | External |
| std::optional\<T\> / pointer | value / Nil |

Mismatch → `QLangError(Type)` naming the field.

## 8. Exception boundary

C++ exceptions thrown inside a binding MUST be caught at the boundary and
converted to `QLangError`. C++ unwinding must never tear through the VM's
frame stack.

## 9. Object dispatch foundation (shared runtime infrastructure)

Both consumer features — External C++ types and QLang classes — need the
exact same four runtime operations on an object value, and none of those
operations contains feature-specific semantics:

```
GETFIELD(obj, name)          -> value
SETFIELD(obj, name, value)
CALLMETHOD(obj, name, args)  -> value
TOSTRING(obj)                -> string        (f-strings, str())
```

A class instance and a C++ external object differ in *what* those operations
do — not *that* they exist. The shared foundation is a dispatch layer: a
"kind" vtable per object value.

```cpp
// qlang/QLangObjects.h — the entire neutral surface
struct ObjectOps {
  const char* kindName;
  QLangValue (*getField)(const QLangValue&, const std::string&, QLangContext&);
  void       (*setField)(QLangValue&, const std::string&, const QLangValue&, QLangContext&);
  QLangValue (*callMethod)(const QLangValue&, const std::string&,
                           const std::vector<QLangValue>&, QLangContext&);
  QLangValue (*toStr)(const QLangValue&, QLangContext&);
  const ObjectOps* (*base)(const QLangValue&);  // reserved, null in v1 — inheritance later
};

const ObjectOps* getObjectOps(const QLangValue&);  // table kind today; null otherwise
```

### Why it is "neutral"

Zero class semantics (no `self` allocation, no instance storage, no
`__init__` protocol, no inheritance) and zero C++-RTTI (no `type_info`, no
`dynamic_cast`, no ownership rules). Four function pointers and a lookup.
The only existing consumer is the **table kind**: today's table
key-lookup/insert semantics ported into the pointers — a pure refactor. The
existing table test corpus must pass unmodified; that is what makes the
layer trustworthy before either real feature lands on it.

`base()` is reserved in v1 with a null implementation so that inheritance
later fills it in without changing the struct layout.

### Build order

1. **Audit** — what does the parser/VM already do with `x.name` on tables?
   SPEC.md specifies it. If the VM desugars dot to table-index today, step 2
   is a refactor. If not, the dispatch landing includes implementing dot
   access for tables — still neutral, just bigger. This audit determines the
   size of the work.
2. **`QLangObjects`** — `ObjectOps` + table kind + registry.
3. **VM** — route field access/call through `getObjectOps()` (one
   indirection); add `GETFIELD`/`SETFIELD`/`CALLMETHOD` where missing.
   Opcodes are cheap: the enum grows, and old `.qlc` files stay valid
   because the loader checks `op < kOpcodeCount` (SERIALIZATION.md §5.1).
4. **Compiler** — `x.name`, `x.name = v`, `x.name(args)` emit the object
   opcodes instead of inline table desugaring. Mechanical.
5. **f-string / `str()` path** — `TOSTRING` on an object value calls
   `toStr`. One branch, serves both features permanently.

### Why dispatch-first (option a) beats External-types-first (option b)

If External types land first, the dispatch gets written *against*
`TypeDescriptor` — field lookup hardcoded to the binding table, `toStr`
hardcoded to `__str__`. When classes arrive with needs the dispatch wasn't
shaped for (instance field storage, `self` binding, prototype lookup,
`__init__` at construction), the same code gets reworked — and SPEC.md's
still-open class decisions (how is `self` represented? are instance fields a
map? inheritance?) have already leaked into the C++ API's shape, coupling
the two features' release timelines.

With dispatch-first, the coupling points at the tiny stable `ObjectOps`
surface: both features depend on it, neither on the other. External types
register one `ObjectOps` that delegates to `TypeDescriptor` — no class
concepts touched. Classes, whenever their semantics are settled, register
another. Each feature is independently shippable.

**Caveat:** "small" is conditional on the audit (step 1). The dispatch
itself is a day or two of work; if dot access is unimplemented in the
parser/VM, the parser/lexer/opcode work for table dot sugar joins the same
landing — same neutral layer, more surface.

### Consumers of `ObjectOps`

Two features land on the dispatch — External C++ types and QLang classes.
A third is the **event handler contract** (`docs/EVENT_HANDLERS.md`): the
engine drives multi-level handler dispatch (type / map / game levels) through
`callMethod`, guarded by `hasMethod` and a **step budget** around each
dispatch. It consumes the same `ObjectOps` surface and adds no new dispatch
machinery — only the level objects and the budget.

## 10. Open questions (decisions pending)

1. **Copy vs. view** — is a cheap `to_table(T)` / `from_table<T>(table)`
   flatten path wanted for identity-less POD data, as a complement to the
   view model? (Leaning yes, as a separate API.)
2. **Inheritance** — registered base type with `dynamic_cast` downcast on
   access. Complicates the descriptor (base-chain walk). Leaning defer to
   v2. The `ObjectOps::base` hook is already reserved for it (§9).
3. **Class runtime dependency** — resolved in principle: land the
   object-dispatch path first as neutral runtime infrastructure (§9), then
   External types and QLang classes build on it independently. Remaining
   unknown: the audit result (step 1 of §9) — how much dot access the
   parser/VM already implements.

## 11. Implementation status

Nothing implemented. No API defined on `QLangContext` yet. The
object-dispatch layer (§9) is the prerequisite landing for this feature.
This document is the design baseline for when the API is written.
