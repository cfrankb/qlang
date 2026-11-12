# C++ External Types — Importing C++ Structures via the QLang API

Status: **implemented (2026-10-27)**
Related: `docs/spec/11-external-types.md` (spec module), `docs/SERIALIZATION.md` (§3 value tags), `docs/EVENT_HANDLERS.md` (handler dispatch), `include/qlang/QLangMeta.h` (macro layer)

## 1. Goal

Expose C++ structs/classes to QLang scripts — construct instances, read/write
fields, call methods, use enum/flag constants — through a public registration
API on `QLangContext` (`import<T>()` / `registerType()`), driven by a
per-class **Qt-style declarative meta** written in plain C++: no MOC, no code
generation, no class-name repetition.

## 2. The `QL_` declarative meta model

C++ RTTI **cannot do this alone**. `type_info` provides type identity and
`dynamic_cast` only; it has zero knowledge of member layout, fields, or
methods. The design is therefore **explicit declarative meta, one line per
binding** — and v1 ended up using no RTTI at all (see §11(e)).

The class names itself exactly once, as a CRTP base
(`qlang::QLObject<Point>`), which exposes a `SelfType` alias. Every entry
macro forms a **qualified member pointer** (`&SelfType::x`,
`&SelfType::translate`), so the C++ type and the binding kind are deduced by
the compiler from the member pointer:

```cpp
struct Point : qlang::QLObject<Point> {
  double x = 0, y = 0;                            // public data member -> read-write property
  double dist() const { return std::sqrt(x*x + y*y); }  // const getter -> read-only property
  void translate(double dx, double dy) { x += dx; y += dy; }
  std::string str() const { return "(" + std::to_string(x) + "," + std::to_string(y) + ")"; }
  QL_OBJECT(
    QL_PROPERTY(x),
    QL_PROPERTY(dist),
    QL_INVOKABLE(translate),
    QL_STR(str),
    QL_CTOR([](qlang::QLangContext&, const std::vector<qlang::QValue>& a) {
      return std::make_shared<Point>(a.at(0).as<double>(), a.at(1).as<double>());
    }))
};
```

The macro layer (`include/qlang/QLangMeta.h`) is the MOC replacement:

| Macro | Meaning |
|---|---|
| `QL_OBJECT(...)` | The one assembly point. Generates `static const TypeMeta& qlang_meta()`. The script-visible type name is recovered **at compile time** from the platform "current function" string (`QL_PRETTY_FUNCTION`: `__PRETTY_FUNCTION__` on GCC/Clang/MinGW/emscripten, `__FUNCSIG__` on MSVC; the identifier before `::qlang_meta`, innermost for nested scopes). `__CLASS__` is an MSVC-only extension and is not used. |
| `QL_PROPERTY(x)` | Data member `x` → **read-write property** (getter + setter, deduced from `&SelfType::x`). |
| `QL_PROPERTY(x)` (x a `const` getter) | **Read-only property** (bare non-const getters match no overload — list via `QL_INVOKABLE`, make `const`, or use `READ`). |
| `QL_PROPERTY(name, READ g)` | Computed read-only property via const getter `g`. |
| `QL_PROPERTY(name, READ g, WRITE s)` | Computed read-write property; the setter is `void (T::*)(M)` (Qt-style void return), so `M` is the converted script-argument type. |
| `QL_INVOKABLE(name)` / `QL_SLOTS(name)` | Method binding (slots and invokables are the same thing to the VM); const and non-const member functions both bind. |
| `QL_ENUM(E, a, b, ...)` / `QL_FLAGS(F, ...)` | **Whole-group** enum/flag table (up to 16 enumerators): the value is the compile-time constant `E::a`, the name the stringified `#a` — one line per group. |
| `QL_STR(name)` | `__str__` binding — const member function `const T* → std::string`. |
| `QL_CTOR(expr)` | Constructor: a lambda `(QLangContext&, const std::vector<QValue>&) → shared_ptr<void>`. |

A `QL_PROPERTY` on a member whose type has no `qval_of`/`as<T>` coverage is a
**compile error at the `QL_OBJECT` line**, not a runtime surprise.

Portability: everything is `QL_`-prefixed (or in `namespace qlang`), so the
header can be included alongside Qt headers without macro collisions
(`QL_OBJECT` vs `Q_OBJECT`, …). The GNU `##__VA_ARGS__` extension is avoided,
so the header compiles under g++/Clang/MinGW/MSVC/emscripten. `READ`/`WRITE`
are the only un-prefixed macros (trivial token transformers that collide with
nothing in Qt).

Memory-safety invariant: script access goes only through the registered
bindings; there is no raw-pointer escape.

## 3. Value representation and ownership

Three new `QLangValue::Type` values, appended **after `Set`** so existing
`.qlc` tags stay stable: `External` (20), `BoundMethod` (21), `TypeRef` (22).
All three are **not durable** — the `.qlc` codec emits tag 0 (Nil) for them
(`docs/SERIALIZATION.md` §3).

`External` is a **view** over exactly one C++ object:

```cpp
struct ExternalRef {
  std::shared_ptr<TypeDescriptor> type; // shared handle (Phase 3): outlives clear()
  std::shared_ptr<void> owned;          // set when the script constructed the object
  std::weak_ptr<void> borrowed;         // set when the engine handed the object over
  const std::type_info* expected;       // concrete C++ type (typed handoff; null if unknown)
  std::function<bool(const void*)> check; // validates the resolved pointer (null if unknown)
};
```

| Source | Lifetime | Reference held by the view |
|---|---|---|
| C++ engine handoff (`qval_external_borrowed<T>(type, weak)`) | engine-controlled | `weak_ptr` |
| Script construction (`T(args)` via `QL_CTOR`) | script-controlled | `shared_ptr` |

There is **no copy path**: every `External` value names one C++ object, and a
script setter write is visible to the engine (single-instance semantics). An
expired borrow throws `runtime error: object destroyed: <type>` on next
access.

**Type-validated handoff (Phase 3).** The typed helpers
`qval_external_owned<T>(type, shared)` / `qval_external_borrowed<T>(type,
weak)` record the concrete C++ type `T` (`expected` = `&typeid(T)`, `check` =
a `dynamic_cast` through the polymorphic `QLObject<T>` base). `self()` /
`selfMut()` validate the resolved pointer against it: a mismatch is a clean
`type error: external type mismatch: expected <T> for type '<name>'` at first
use, **not UB** (the old "a wrong `weak_ptr` is UB" is closed — see §10.2).
The untyped `qval_external(type, …)` factories carry no check (both fields
null) and are for host code that cannot name the concrete type.

## 4. Field access — the v1 dispatch paths

Dot syntax is per spec; no syntactic fork. The VM routes member get/set
through `getObjectOps(value)` (`src/QLangObjects.cpp`), which now returns ops
for Table, Object (class), External, and TypeRef:

| Script operation | v1 path |
|---|---|
| `p.x` (property read) | `TABGET` → `external_ops::getField` — resolves the descriptor's member list; kind 0/1 → `get(self)` (kind 0 = read-write, kind 1 = read-only) |
| `p.x = v` | `TABPUT` → `external_ops::setField` — kind 0 only; kind 1 → `"... is read-only"`, kind 2 → `"... is a method"` |
| `p.m(args)` (method call) | compiles to `TABGET` + `CALLCLOS` — the kind-2 member read returns a **`BoundMethod`** value, and `CALLCLOS` dispatches it. `ops->callMethod` is **not** called by the VM in v1 (see §9) |
| `f"{p}"` / `str(p)` / `print(p)` | `valueToString` (context-threaded) → `ops->toStr` → the `QL_STR` binding; a type with no `QL_STR` gets a type error |
| `Point.AnimatedDocks` | `TABGET` on the seeded `TypeRef` variable → `typeref_ops::getField` → the type's enum/flag tables → `Int` |

Method extraction is a first-class value: `m = p.translate; m(4, 5)` works —
a `BoundMethod` is exactly what `CALLCLOS` dispatches, not a special case.

## 5. Methods — the `BoundMethod` value type

A method read of an `External` value produces a **`BoundMethod`** value (tag
21, not durable): `BoundMethodRef { const Member* member; ExternalRef target }`
— the bound member plus the target view. This **replaces the draft's
`NativeMethod` function-pointer proposal**: `CALLCLOS` gained a `BoundMethod`
branch (alongside its `Function`/`Generator` branches) that invokes
`member->invoke(self, ctx, args)`.

- `Member::invoke` is
  `std::function<QLangValue(void*, QLangContext&, const std::vector<QLangValue>&)>`.
  The binding converts each script argument via `as<A>()` and throws a `Type`
  error on an arity mismatch (`"method expects N argument(s), got M"`).
- C++ exceptions escaping a binding are rethrown as
  `QLangError(Runtime, e.what())`; `QLangError`s pass through untouched.
- No method overloading in v1 — one binding per name; duplicate member names
  are rejected at registration (§11(b)).
- Dunders: `__str__` via `QL_STR`; **no `__destroy__`/`destroy` hook in v1**
  (plain shared/weak view — §11(d)).
- Lifetime: `BoundMethodRef::member` points into the `TypeDescriptor` named by
  the target's **shared handle** (Phase 3) — the descriptor outlives
  `clear()` and any value that names it, so a `BoundMethod` (or `External`)
  value held by the host stays valid as long as the host (or a script value)
  holds the handle. A `BoundMethod` whose handle has been dropped everywhere
  is a host bug (dangling `member`), the same class as a dangling
  `ExternalRef`.

## 6. Construction and type-level values

- **`T(args)`** — when `QL_CTOR` is present, `registerType` registers a
  **native function** under the type name that wraps the construct result in
  an owned `External` value. `let p = Point(1, 2)`. The constructor native
  captures the type's **shared handle** (Phase 3), so a copied `NativeFunc`
  stays valid across `clear()`.
- **`Type.Constant`** — `registerType` also **seeds the type name as a const
  script variable** holding a `TypeRef` (tag 22). `Point.AnimatedDocks`
  compiles as a const-variable load + `TABGET` and resolves through
  `kTypeRefOps` into the type's enum tables (→ `Int`). Flag combination is a
  native int bit-op: `DockArea.AnimatedDocks | DockArea.AllowTabbedDocks` →
  `5`. `type(Point)` → `"Point"`; `Point = 1` → type error (the seeded
  variable is const).
- Import shadowing: a same-name script variable is silently overwritten at
  import; a name already taken by a native function is rejected **only** when
  the type also registers a constructor native under that name.
- `clear()` drops the context's own references — the seeded `TypeRef`
  variables and the constructor natives are removed, and the type registry
  releases its handle — but a **shared handle the host (or a script value)
  still holds stays valid** (Phase 3): the descriptor is reference-counted,
  so `registerType`/`import<T>()` return a `std::shared_ptr<TypeDescriptor>`
  that outlives `clear()`. Host code that cached a handle needs no
  re-import; only the script-visible bindings (seeded vars, ctor natives)
  must be re-established by re-running `import<T>()`.

## 7. Boundary conversion

`qval_of<T>()` (value → `QValue`) covers `bool`, `int32_t`, `uint32_t`,
`uint8_t`, `uint16_t`, `int16_t`, `int64_t`, `uint64_t`, `double`, `float`,
`std::string`; `qval_vec<T>` converts `std::vector<T>`; `qval_opt`/
`qval_ptr` convert optionals/pointers (null → Nil). The reverse direction is
`QValue::as<T>()` — the setter/invoke bindings call it per member type.
Member types without an `as<T>` specialization are a compile error at the
`QL_OBJECT` line (a diagnostic, not a crash).

## 8. Exception boundary

C++ exceptions escaping a binding are converted at the ops layer
(`src/QLangObjects.cpp`, `QL_EXTERNAL_CALL`): `QLangError` passes through;
any other `std::exception` becomes `QLangError(Runtime, e.what())`. The VM
never sees a raw C++ exception from an external binding.

## 9. Object dispatch substrate (implemented)

`ObjectOps` (`kindName`, `getField`, `setField`, `callMethod`, `toStr`,
`base`) is the neutral dispatch surface; `getObjectOps` routes:

| Value kind | Ops |
|---|---|
| Table | `table_ops::kTableOps` |
| Object (QLang class) | the per-class ops from its `ObjectDescriptor` |
| External | `external_ops::kExternalOps` |
| TypeRef | `typeref_ops::kTypeRefOps` |
| BoundMethod | none — it is callable (`CALLCLOS`), not an object |

`callMethod` exists on every ops table but the VM does **not** call it in v1
(method calls are `TABGET` → `BoundMethod` → `CALLCLOS`). It is the host-side
event-handler entry point: the context-level wrappers `QLangContext::hasMethod`
/ `QLangContext::callMethod` are **implemented** (2026-11-06, `c4ee131`) and
reach it through `getObjectOps` — `hasMethod` via `ops->getField`,
`callMethod` via `ops->callMethod` after `bindThread()` + `processFinalizers()`;
a missing method or a non-object throws `QLangError` (see
`docs/EVENT_HANDLERS.md` §4). **Callable is not the same test on every kind:**
a table/object member is callable when it is a `Type::Function` (the self
anchor is added by `table_ops::callMethod`), an external member when
`findMember` reports `kind == 2`. `hasMethod` currently accepts
`Function` **or** `BoundMethod` on every kind, so a table field holding a
`BoundMethod` passes the guard and then throws `member 'x' is not a function`
— open defect, per-kind callable test pending. `base` is reserved for
inheritance (returns null in v1).

## 10. Open questions

1. **`hasMethod`/`callMethod` context wrappers** for the event-handler
   contract (`docs/EVENT_HANDLERS.md`) — ✓ **implemented** (2026-11-06,
   `c4ee131`; `test_external` §19–§20). One open detail before it can be called
   verified: the callable test must be per kind (§9) — `hasMethod` today also
   accepts a table field holding a `BoundMethod`, which `callMethod` then
   rejects.
2. **RTTI validation on handoff** — ✓ **implemented (Phase 3).** The typed
   handoff helpers (`qval_external_owned<T>` / `qval_external_borrowed<T>`)
   record the concrete type and `self()`/`selfMut()` validate the resolved
   pointer with a `dynamic_cast` through the `QLObject<T>` base; a mismatch
   is a clean `type error: external type mismatch: …` at first use, not UB.
   The untyped `qval_external(type, …)` factories remain for hosts that cannot
   name the concrete type (no check).
3. **Inheritance** — `ObjectOps::base` is reserved; base-chain walk deferred
   to v2.
4. **Signals** (`Q_SIGNALS`/`emit`/`connect`) — revisit with
   `docs/EVENT_HANDLERS.md`; tentative spec draft in
   `docs/spec/17-signals.md`.
5. **Enum-name string form** (reverse lookup for enum-valued properties) —
   deferred: no dispatch point sees both the field value and its type context
   (`str(e.options)` sees a bare `Int`).
6. **Copy vs. view** (`to_table`/`from_table` APIs) — the view model is v1.

## 11. Implementation status

**Implemented** (2026-10-27); **host-API hardening landed in Phase 3**
(2026-11-03, `docs/plans/architecture-remediation-plan.md`). Files:
`include/qlang/QLangMeta.h` (macro layer), `QLangTypes.h` (`TypeDescriptor`),
`QLangValue.h`/`QLangVariant.h` (the three value types +
`ExternalRef`/`BoundMethodRef`/`TypeRefRef`), `QLangContext.h`/
`src/QLangContext.cpp` (`registerType`/`import<T>()` returning a
`std::shared_ptr<TypeDescriptor>`, validation, TypeRef seeding, constructor
native, the `TypeRegistry` in the Pimpl, the finalizer queue +
`processFinalizers()`), `src/QLangObjects.cpp` (`external_ops`,
`typeref_ops`, `getObjectOps` routing), `src/QLangVM.cpp` (`CALLCLOS`
`BoundMethod` dispatch in the single executor; context-threaded
`valueToString` → `toStr`; the `release()` builtin; the finalizer-deleter
enqueue), `src/QLangBytecodeIO.cpp` (tags 20–22 emit tag 0). The **compiler is
unchanged** — `T.Constant` compiles as an ordinary const-variable load.
Tests: `tests/unit/test_external.cpp` (ctest `external`, 23 checks) and
`tests/unit/phase3_api.cpp` (ctest `phase3_api`, 23 checks — stable handles,
type-mismatch/expired-borrow clean errors, finalizer queue, `release()`).

Deviations from the draft:

- **(a) Compile-time integrity via member pointers** — identity and layout
  come from the `QL_OBJECT` member-pointer list, not from RTTI; every binding
  is type-checked at the `QL_OBJECT` line.
- **(b) Registration-time validation** — `registerType` rejects empty names,
  duplicate member names, and type-name/native-function clashes at
  `import<T>()` time, not at script-execution time.
- **(c) `Object` landed at tag 16 before external** — the draft proposed
  `External` at tag 17; the final layout is 16=Object, 17=Eof, 18=Long,
  19=Set, 20=External, 21=BoundMethod, 22=TypeRef.
- **(d) No `__destroy__`/`destroy` hook in v1** — a plain shared/weak view;
  the resource-teardown hook is deferred. (QLang classes have `__destroy__`;
  external types do not.)
- **(e) `type(x)` returns the declared name string, not `type_info::name`** —
  the type *name* is still the declared string. RTTI entered in Phase 3, but
  only for **handoff validation** (`typeid(T)` + `dynamic_cast` in the typed
  `qval_external_*<T>` helpers, §3/§10.2) — it is not used for `type(x)` or
  for binding identity/layout (those remain the `QL_OBJECT` member-pointer
  list, (a)).

## 12. Self-registration and manifest dumping (portable-module-plan P3)

`QL_OBJECT` now also declares one line of bookkeeping beside
`qlang_meta()`:

```cpp
inline static const bool qlang_meta_registered_ =
    ::qlang::meta_detail::registry_add(&qlang_meta);
```

The C++17 `inline static` member has exactly one instance per program and
its initializer runs at dynamic init, storing only the *accessor pointer*
(`const TypeMeta& (*)()`) into `qlang::metaRegistry()` — a
construct-on-first-use function-local static, so no static-init order can
be observed and nothing a host registers can throw at static-init time
(`qlang_meta()` assembles lazily on first call). `metaRegistry()` is
public: observers (the dumper, P4's checks) walk it; two entries sharing a
type **name** are kept, and `qlang::writeTypeManifest(std::ostream&)`
refuses such a dump with a `QLangError` naming the duplicate rather than
silently dropping one.

`writeTypeManifest` is the host one-liner for extracting the process's
type interface as a `QLM1` manifest (SERIALIZATION.md §11); `qlang_run
--qlmeta-dump <file.qlm>` is the same call as a standalone mode for
binaries shaped like it (report on stdout, exit 0; no script mode combines
with it).

**Caveats:**

- The registrar is *emitted in a translation unit only when that TU
  references the class* (inline static members get their dynamic
  initializer where the class is used). A type declared in a header but
  never referenced by the dumping binary is not in its registry — same
  reach rule as the meta accessor itself.
- A **local class** (declared at function or block scope) cannot hold the
  registrar: local classes may not have such static data members. Declare
  `QL_OBJECT` types at namespace scope (test fixtures that used local
  declarations moved accordingly).
- Registration is by *declared name*: two classes that extract the same
  innermost name (e.g. `ns1::Thing` and `ns2::Thing`) collide, and the
  dump refuses until the host resolves the clash — by design, since the
  manifest vocabulary (and scripts) address types by that name.

## 13. Compile-time validation from a manifest (portable-module-plan P4)

`qlang_run --qlmeta <file.qlm>` loads a `QLM1` manifest (SERIALIZATION.md
§11) and feeds it to `QLangCompiler::addExternalTypes(...)` — shape checks
on `.ql` sources with **no engine linked**: type names resolve, `T(...)`
constructor arity/tags, field reads and writes (read-only included),
method and signal membership, arity and literal argument tags, and
manifest-enum folding through the type name (`Point.Right`), all with
messages mirroring the runtime wording. Without `--qlmeta` the compiler
behaves exactly as before, byte for byte.

The checks are strict where the compiler is *sure* and silent everywhere
else, mirroring the lenient runtime:

- A variable is a **known receiver** of `T` only if the module contains
  exactly one write to its plain name and that write initializes it with
  `T(...)`; any second write anywhere in the module — even inside a `def`
  — makes it dynamic again, and receiver checks run at top level only
  (a function body could shadow the name). Type-name uses (`T.field`,
  `T.method(...)`, `T.CONST`) cannot be shadowed and are checked
  everywhere.
- Arguments whose value is not a literal, and any position whose manifest
  tag is `Unknown`/`Void`/`External`, stay silent — tags describe, the
  runtime still decides.
- `connect`/`disconnect` validate the signal name when the first argument
  is a plain known receiver; `T::M` and `T.M` are the same checked path.
- If the host *also* injects an enum the manifest carries (via
  `registerEnum`/`addExternalEnums`), a value mismatch is a hard error
  (`enum 'E' member 'M' disagrees with external type 'T'`), so a stale
  manifest is caught instead of silently folded.

Constructor and method calls on manifest types compile to **named** negative
native slots, re-resolved when a `.qlc` loads. Only the calls the source actually
makes are written into the image: a manifest type it never constructs puts no name
in the file, so a host without that constructor still loads the image. A host that
cannot serve a name the image *does* carry is refused **at load**, which names
every such slot (`missing native function(s): …`) and runs nothing — the failure is
not deferred to the call. Loose files carry no integrity link by design (§12, and
the packaged handshake in §14).

## 14. Portable modules (portable-module-plan P5)

A `QPM1` package (§13's manifest + the `.qlc` images that need it) is the unit
you ship when the script side is compiled ahead of time and the C++ side is
already built: `docs/SERIALIZATION.md` §12 defines the container, this section
the host's obligations.

```sh
qlang_run --qlmeta host.qlmeta -c app.qlc app.ql      # compile against the shape
qlang_run --qlmeta host.qlmeta --package app.qpm app.ql lib.qlc
qlang_run app.qpm                                     # or embed it:
```

```cpp
QLangContext ctx;
ctx.import<Point>();                       // the host's QL_OBJECT types
auto entry = ctx.loadPortableModuleFile("app.qpm");
std::map<std::string, int> nativeIds;      // the driver's slot re-resolution
for (auto& [slot, name] : entry->nativeSlots) nativeIds[name] = -1 - slot;
QLangLinker linker(ctx.loadedModules());   // merges the embedded libraries
QLangVM vm(linker.link(entry->program, nativeIds), ctx);
```

**What the handshake guarantees.** Before any image is loaded, every type in
the package's manifest must be registered by the host, and its canonical entry
bytes must equal the manifest's (`manifestTypeFrom` on both sides, so the
comparison is the same one the dumper wrote). A missing type is
`missing type 'T' required by portable module`; a drifted one — a property
whose type changed, a method that disappeared, an enum value that moved — is
`external type 'T' does not match the portable module`. Either way nothing is
registered and the context is unchanged: the failure is a load error, not a
wrong result at some later call.

**After the handshake, the values are the host's.** A manifest type with a
`QL_CTOR` compiles (through §13) to a *named* native slot, so the constructor is
resolved against the loading context, never carried in the package: `Point(3.5,
2.0)` in a packaged script builds that host's `Point`, hands it across module
boundaries like any other value, and every member read goes through the host's
descriptor — which is where §10.2's typed `qval_external_*` handoff (and its
`dynamic_cast`) applies. A package therefore cannot smuggle an object of the
building host's `Point` into a host that registered another one: nothing but the
name crosses. Pinned by the `portable_module` typed-payload gate (library reads
members of a Point the entry constructed, one package, one fresh context).

**What it does not.** The manifest carries *shape*, not C++ identity: two
`Point`s with the same members but different `sizeof`, layout or method bodies
compare equal, and each builds its own fingerprint-independent binding. That is
deliberate (§11, "tags, not mangled names") — the package is portable across
compilers and build configurations, which a mangled-name check would break. A
host that wants layout equality must assert it in its own C++, not in the
package.

**Where that leaves the guard.** Because the handshake cannot see C++ identity,
the last check is the one the handoff itself carries: `ExternalRef::check`, wired
from the registered descriptor by the constructor native and applied by
`resolveExternal`, turns a pointer that is not the registered type into
`external type mismatch: … for type 'Point'` at the **first member read**. A host
whose `Point` drifted — same registered name, same describable shape, another
build — passes the handshake byte for byte and still cannot read garbage out of
its own objects. Pinned by the `portable_module` drifted-host gate, which loads
the *same package bytes* the honest host ran above into a context whose
registered constructor hands out the other build: the fault arrives before one
member is read, and the context still reads an honestly-built value afterwards.

**A host that registers no types** needs no handshake: a package whose manifest
is empty (what `qlang_run --package` writes without `--qlmeta`) loads anywhere
`qlang_run` runs.
