# External C++ Types — Implementation Plan v2

**Date:** 2026-10-27 (supersedes `2026-10-26_163424-external-cpp-types.md` v4; see "What changed vs v4")
**Repo:** `/home/cfrankb/toolkit/gcc/qlang-sandbox` (QLang: language → register-VM bytecode, C++17, GCC)
**Design baseline:** `docs/EXTERNAL_TYPES.md` (draft)
**Spec side:** `SPEC.md` "conformance with external (C++) types" table (L1376)

## Goal

Expose C++ structs/classes to QLang scripts by **declaring their interface inside the class with individual Qt-style macro lines** — `QL_OBJECT`, `QL_PROPERTY`, `QL_INVOKABLE`, `QL_SLOTS`, `QL_ENUM`, `QL_FLAGS`, `QL_STR`, `QL_CTOR` — in plain C++17, **no MOC, no code generation, no class-name repetition**, and **no macro collisions when compiled alongside Qt** (everything `QL_`-prefixed / in `namespace qlang`):

- a **public data member** listed → **read-write property** (script gets both getter and setter);
- a **private member** → exposed only via meta-declared accessors (`QL_PROPERTY(name, READ g)` = read-only; `QL_PROPERTY(name, READ g, WRITE s)` = read-write);
- a **const getter** listed bare (`QL_PROPERTY(dist)`) → **read-only property**;
- the **existing C++ interface** (methods) → one `QL_INVOKABLE(name)` / `QL_SLOTS(name)` line each, binding deduced from the member pointer;
- an **entire enum/flag group** → one `QL_ENUM(E, enumerator, ...)` line: values are compile-time constants, names recovered at compile time from `__PRETTY_FUNCTION__` (GCC).

Script side: `DockArea.AnimatedDocks` → `1`; flag combination is native QLang int bit-op: `DockArea.AnimatedDocks | DockArea.AllowTabbedDocks` → `5`. `DockArea` is a **type-level value** (`TypeRef`) seeded as a script variable at import; `DockArea.A` resolves through `kTypeRefOps.getField` into the type's enum tables. `p.translate(1, 1)` works because a method read returns a **callable `BoundMethod` value** that the VM's `CALLCLOS` dispatches — not because `ops->callMethod` is called (it is not, and is not in v1).

## What changed vs v4 (review findings this plan fixes)

The v4 plan's central claim — "the VM already routes member get/set/**call/str** through `getObjectOps()`, so one ops table needs no VM work" — was verified **half true**. Get/set: yes (`TABGET`/`TABPUT` route through `getObjectOps`, `QLangVM.cpp` L1076/L1099). Call and str: **no** —

1. **Method calls** compile to `TABGET` + `CALLCLOS` (`QLangCompiler.cpp` L1705-1753); `CALLCLOS` (`QLangVM.cpp` L2189-2204) only accepts `Function`/`Generator` values, and a C++ member method cannot be a `Closure` (a `funcIndex` into the compiled program). `ops->callMethod` has **zero call sites** in the VM. → v2 adds a **`BoundMethod` value type** + `CALLCLOS` dispatch (Task 4.5).
2. **`Point.A` does not compile**: `Point` is a native-function *name*, not a script variable; the compiler's variable-load path (`QLangCompiler.cpp` L1621-1649) throws `use of undeclared variable` — verified empirically. → v2 adds a **`TypeRef` value type** + `kTypeRefOps`, and `registerType` **seeds the type name as a script variable** (Task 3), keeping the zero-compiler-changes property.
3. **`str()`/f-strings** go through `valueToString()` (`QLangVM.cpp` L39-65) — a plain type switch; `ops->toStr` has **zero call sites**. → v2 threads `QLangContext&` through `valueToString` and dispatches `toStr` for `External` (Task 4.4).

Also fixed from the review: missing `prop` overload for bare const getters (Task 3.1); `QL_INVOKABLE` on const methods (Task 3.1); test check 9's `QL_PROPERTY("a", …)` stringification bug (Task 5); the `setFunc` wrapper's missing `QValue::External` conversion (Task 3.4); the whole Task 5 test rewritten against the real throwing `run()` API; `test_dollar` → `test_dollar_operator` in the gate script; CMake entry modeled on `range_check` (not `test_inject`, which is not a CMake target); `clear()`/import interaction specified; SPEC deviation list extended (no `__destroy__` hook, no RTTI `type_info` name).

## The two mechanisms everything hangs on

1. **Unqualified member pointers inside a class.** `&x` / `&translate` written in a static member context are valid `T::x`-form pointers, so `QL_PROPERTY(x)` → `prop("x", &x)` deduces the C++ type and kind (data member → property, function → method). No `&Point::x`, no repeated class name (`__CLASS__` supplies it). This is the no-MOC replacement for what Qt's MOC generates.
2. **Compile-time enum name recovery.** A template non-type parameter carries the enumerator *value*; `__PRETTY_FUNCTION__` (GCC) carries its *name* (`[with E = DockOption; v = DockOption::AnimatedDocks]`). Parse → name. One `QL_ENUM` line declares the whole group.

## The value model (what the VM actually dispatches)

Three new `QLangValue::Type` values, inserted **before `Eof`** (Eof stays last, parser sentinel, never serialized):

| Type | Payload | Meaning | Durable |
|---|---|---|---|
| `External` (17) | `ExternalRef {const TypeDescriptor* type; shared_ptr<void> owned; weak_ptr<void> borrowed}` | a **view** over exactly one C++ object (owned = script-constructed; borrowed = engine handoff) | no — emit tag 0 |
| `BoundMethod` (18) | `BoundMethodRef {const Member* member; ExternalRef target}` | a **callable** method of an `External` object; what `TABGET` returns for a kind-2 member | no — emit tag 0 |
| `TypeRef` (19) | `TypeRefRef {const TypeDescriptor* type}` | the **type itself** as a value: enum constants + construction; seeded as a script variable at import | no — emit tag 0 |
| `Eof` (20) | — | unchanged | — |

`QLangVariant` gains the three new alternatives **at the end** (indices 15, 16, 17). The `asX()` accessors use type-based `std::get<StorageType>` (not fixed indices), so appending is safe.

Dispatch table (`getObjectOps()`):

| Value type | Ops | getField | setField | callMethod | toStr |
|---|---|---|---|---|---|
| `Table` | `kTableOps` (unchanged) | map/instance | map/instance | (unused by VM) | `<table>` |
| `Object` | `desc->ops` (unchanged) | as today | as today | (unused by VM) | as today |
| `External` | `kExternalOps` | kind 0/1 → `get(self)`; **kind 2 → `BoundMethod` value**; unknown → `Type` error | kind 0 → `set`; kind 1 → read-only error; kind 2 → "is a method"; unknown → no field | kind 2 → `invoke` (kept for the future event contract; **not** the v1 call path) | `toString` if set, else `Type` error |
| `TypeRef` | `kTypeRefOps` | **enum-table lookup → `Int`**; unknown → `Type` error | always `Type` error ("type is not settable") | `construct(args)` → owned `External` | `<type Name>` |
| `BoundMethod` | `nullptr` | n/a — **callable via `CALLCLOS` only** | n/a | n/a | n/a |

Method call path (the part v4 got wrong): `e.translate(1, 1)` → compiler emits `TABGET func, e, "translate"` + `CALLCLOS func, n, n` (non-class path, no self prepend). `kExternalOps.getField` returns `QLangValue::BoundMethod(&member, ref)`; the **extended `CALLCLOS`** recognizes the type, resolves `target.self()` (expired borrow → `QLangError(Runtime, "object destroyed: <type>")`), gathers args from `regs[i.a+1 .. i.a+i.c]` (the same convention `CALL` uses), and calls `member->invoke(self, ctx, args)`. **No new opcode** — `CALLCLOS` already carries the callee value.

Type-level path: `Point(3, 4)` keeps compiling via the native-function table (`registerType` calls `setFunc(name, …)` — zero compiler changes). `Point.A` compiles because `registerType` also seeds `ctx.set(name, QValue::TypeRef(desc))`; the compiler sees `Point` as a seeded variable → `MOV` + `TABGET` → `kTypeRefOps.getField` → enum value.

## The macro set (what goes in `include/qlang/QLangMeta.h`)

```cpp
// === Entry macros — pure token transformers, one declarative line each ===

// Data member -> read-write property (public member: script gets a setter).
// Computed: QL_PROPERTY(name, READ getter)              -> read-only
//           QL_PROPERTY(name, READ getter, WRITE setter) -> read-write
// Const getter listed bare -> read-only property.
#define QL_PROPERTY(name, ...) \
  qlang::meta_detail::prop(#name, &name, ##__VA_ARGS__)
#define READ(a)   qlang::meta_detail::acc(&a, qlang::meta_detail::Acc::read)
#define WRITE(a)  qlang::meta_detail::acc(&a, qlang::meta_detail::Acc::write)

// Method (non-const and const member functions). Slots and invokables are
// the same thing to the VM.
#define QL_INVOKABLE(name)  qlang::meta_detail::method(#name, &name)
#define QL_SLOTS(name)      qlang::meta_detail::method(#name, &name)

// Whole enum/flag group in one line: bare enumerator names.
// Values are NTTPs; names from __PRETTY_FUNCTION__ (GCC). Portable fallback:
//   QL_ENUM(E, ("Name", E::Name), ...)                  (explicit-pair overload)
#define QL_ENUM(E, ...)    qlang::meta_detail::enum_table(#E, __VA_ARGS__)
#define QL_FLAGS(F, ...)   qlang::meta_detail::enum_table(#F, __VA_ARGS__)

// str()/f-string conversion, and script-side construction.
#define QL_STR(name)   qlang::meta_detail::str_clause(&name)
#define QL_CTOR(expr)  qlang::meta_detail::ctor_clause(expr)

// === The one assembly point (what Qt's MOC generates) ===
#define QL_OBJECT(...) \
  public: \
  static const qlang::TypeMeta& qlang_meta() { \
    static const qlang::TypeMeta m_{ qlang::meta_detail::assemble(__CLASS__, __VA_ARGS__) }; \
    return m_; \
  }
```

```cpp
namespace qlang::meta_detail {

enum class Acc { read, write };
template <class P> struct AccPtr { Acc k; P p; };
template <class P> AccPtr<P> acc(P p, Acc k) { return {k, p}; }

// QL_PROPERTY(x) — data member -> read-write.
template <class T, class M> Member prop(const char* n, M T::*m) {
  return {0, n,
    [m](const void* s){ return qval_of(static_cast<const T*>(s)->*m); },
    [m](void* s, const QLangValue& v){ static_cast<T*>(s)->*m = v.as<M>(); }};
}
// QL_PROPERTY(x) — const getter -> read-only.   (v4 was MISSING this overload;
// without it a bare QL_PROPERTY(dist) on `double (T::*)() const` matched no
// candidate and the plan's own usage example failed to compile.)
template <class T, class R> Member prop(const char* n, R (T::*)() const g) {
  return {1, n, [g](const void* s){ return qval_of((static_cast<const T*>(s)->*g)()); },
          nullptr, nullptr};
}
// QL_PROPERTY(x, READ g) — computed read-only.
template <class T, class R> Member prop(const char* n, AccPtr<R (T::*)() const> g) {
  return {1, n, [g](const void* s){ return qval_of((static_cast<const T*>(s)->*g.p)()); },
          nullptr, nullptr};
}
// QL_PROPERTY(x, READ g, WRITE s) — computed read-write (private-member path).
template <class T, class R, class M> Member prop(const char* n,
    AccPtr<R (T::*)() const> g, AccPtr<M (T::*)(M)> s) {
  return {0, n, [g](const void* o){ return qval_of((static_cast<const T*>(o)->*g.p)()); },
          [s](void* o, const QLangValue& v){ (static_cast<T*>(o)->*s.p)(v.as<M>()); }};
}
// QL_INVOKABLE / QL_SLOTS — non-const function member -> method.
template <class T, class R, class... A> Member method(const char* n, R (T::*f)(A...)) {
  return {2, n, nullptr, nullptr, [f](void* s, QLangContext& c, const std::vector<QLangValue>& a){
    return call(f, static_cast<T*>(s), a, std::index_sequence_for<A...>{}); }};
}
// QL_INVOKABLE / QL_SLOTS — const function member -> method.   (new in v2)
template <class T, class R, class... A> Member method(const char* n, R (T::*f)(A...) const) {
  return {2, n, nullptr, nullptr, [f](void* s, QLangContext& c, const std::vector<QLangValue>& a){
    return call(f, static_cast<const T*>(s), a, std::index_sequence_for<A...>{}); }};
}

// Whole-group enum table from bare enumerator names (GCC name recovery).
template <class E, E... Vs>
EnumMeta enum_table(const char* tn, Vs... vs) {
  EnumMeta m{tn, {}};
  ((m.entries.push_back({enumName(vs), (long)vs})), ...);
  return m;
}
// Portable fallback: explicit (name, value) pairs.
template <class E, class... Pairs>
EnumMeta enum_table(const char* tn, std::pair<const char*, E>... ps) {
  return {tn, { {ps.first, (long)ps.second}... }};
}
// GCC: "... [with E = DockOption; v = DockOption::AnimatedDocks]" -> "AnimatedDocks"
template <class E, E v>
std::string enumName() {
  const char* s = __PRETTY_FUNCTION__;
  const char* p = std::strstr(s, "v = ");  p = p ? p + 4 : s;
  const char* c = std::strrchr(p, ':');    p = c ? c + 1 : p;  // scoped vs unscoped
  const char* e = std::strchr(p, ']');
  return std::string(p, e ? (size_t)(e - p) : std::strlen(p));
}

struct StrClause  { std::function<QLangValue(const void*)> f; };
struct CtorClause { std::function<std::shared_ptr<void>(QLangContext&, const std::vector<QLangValue>&)> f; };
template <class T, class R> StrClause  str_clause(R (T::*f)() const) {
  return { [f](const void* s){ return qval_of((static_cast<const T*>(s)->*f)()); } };
}
template <class F> CtorClause ctor_clause(F f) { return {std::move(f)}; }

// Assembly: fold the heterogeneous entry list into a TypeMeta.
inline void add(TypeMeta& m, Member e)       { m.members.push_back(std::move(e)); }
inline void add(TypeMeta& m, EnumMeta e)      { m.enums.push_back(std::move(e)); }
inline void add(TypeMeta& m, StrClause e)     { m.toString = std::move(e.f); }
inline void add(TypeMeta& m, CtorClause e)    { m.construct = std::move(e.f); }
template <class... Ts> TypeMeta assemble(const char* name, Ts... es) {
  TypeMeta m{name};
  ((add(m, es), ...));
  return m;
}
}  // namespace qlang::meta_detail
```

Data structures (same header): `Member` {`int kind` (0 read-write property, 1 read-only property, 2 method), `std::string name`, `get`/`set`/`invoke` closures}; `TypeMeta` {`name`, `members`, `toString`, `construct`, `enums`}; `EnumEntry` {`name`, `value`}; `EnumMeta` {`typeName`, `entries`}. `meta_detail::call` is the index-sequence arg-convertor (converts `QLangValue` args via `as<Args>()`, throws `Type` on arity mismatch).

**Overload rules (compile-time, by construction):** a bare non-const getter (`R (T::*)()`) matches *no* `prop` overload — deliberate: list it via `QL_INVOKABLE`, make it `const`, or expose it through `READ`. A const getter matches exactly the new read-only `prop` overload. A data member matches exactly the first `prop` overload.

Usage, as the user writes it:

```cpp
enum DockOption {
  AnimatedDocks = 0x01, AllowNestedDocks = 0x02, AllowTabbedDocks = 0x04,
  ForceTabbedDocks = 0x08, VerticalTabs = 0x10, GroupedDragging = 0x20
};

struct DockArea {
  double x = 0, y = 0;                                  // public -> read-write
  int options = 0;                                      // public -> read-write
  double dist() const { return std::sqrt(x*x + y*y); }  // const getter -> read-only
  void translate(double dx, double dy) { x += dx; y += dy; }
  std::string str() const { return "Point(...)"; }

  QL_OBJECT(
    QL_PROPERTY(x),
    QL_PROPERTY(options),
    QL_PROPERTY(dist),
    QL_INVOKABLE(translate),
    QL_ENUM(DockOption, AnimatedDocks, AllowNestedDocks, AllowTabbedDocks,
            ForceTabbedDocks, VerticalTabs, GroupedDragging),
    QL_STR(str),
    QL_CTOR([](qlang::QLangContext& c, const std::vector<qlang::QValue>& a) {
      return std::make_shared<DockArea>(a[0].as<double>(), a[1].as<double>()); })
  );
};
```

## Qt concept mapping (what we replicate, what we skip)

| Qt | QLang equivalent | Verdict |
|---|---|---|
| `Q_OBJECT` | `QL_OBJECT(...)` — the single assembly point, no class name | **Yes** |
| `Q_PROPERTY(x, READ x, WRITE x)` | `QL_PROPERTY(x)` / `QL_PROPERTY(x, READ g)` / `QL_PROPERTY(x, READ g, WRITE s)` | **Yes** — individual lines, exact Qt shapes |
| `Q_INVOKABLE` / `Q_SLOTS` | `QL_INVOKABLE(name)` / `QL_SLOTS(name)` — identical (VM has one kind of method) | **Yes** |
| `Q_ENUM(E)` | `QL_ENUM(E, a, b, c, ...)` — whole group, one line | **Yes** (enumerator list required — no MOC to scan the declaration) |
| `Q_FLAGS(F)` | `QL_FLAGS(F, ...)` — same table, marked; script ORs ints natively | **Yes** |
| `Q_SIGNALS` / `emit` / `connect` / slots-as-callbacks | — | **Skip** — no signal/slot event system in the VM (`docs/EVENT_HANDLERS.md` unimplemented). Revisit when events land. |
| `metaObject()` / `qt_metacall` | `qlang_meta()` | **Yes** — the accessor |

Skipped for v1 (one line each): `Q_SIGNALS`/signals, method overloading (one name per type), inheritance (`ObjectOps::base` hook already reserved), `__destroy__` resource-teardown hook (deviation (d) below).

## Current context / assumptions (re-verified 2026-10-27)

- **Build/run harness:**
  ```sh
  cd /home/cfrankb/toolkit/gcc/qlang-sandbox
  cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
  g++ -std=c++17 -Iinclude examples/<file>.cpp build/libqlang.a -o /tmp/<name>
  stdbuf -oL -eL /tmp/<name> 2>/dev/null   # stderr=debug logging; stdout block-buffered
  ```
- **Value model:** `QLangValue` (alias `QValue`, `include/qlang/QLangValue.h`) — a `Type m_type` tag + `QLangVariant m_storage` (`std::variant`, 15 alternatives today, `ObjectValue` last). `Type` enum `Nil=0 … Function=14, Generator=15, Object=16, Eof=17`. New types go **before `Eof`**; new variant alternatives go **at the end**. `asX()` uses type-based `std::get<StorageType>`, so appending is index-safe.
- **Object dispatch (built, and its real shape):** `struct ObjectOps { kindName, getField, setField, callMethod, toStr, base }` in `include/qlang/QLangObjects.h`; `getObjectOps()` (`src/QLangObjects.cpp` L196) routes `Type::Table` and `Type::Object`. **The VM routes member get/set through `getObjectOps()`** (`TABGET`/`TABPUT`). **Method calls do NOT** — they compile to `TABGET` + `CALLCLOS`, and `CALLCLOS` accepts only `Function`/`Generator`. **String conversion does NOT** — `str()`/f-strings go through the file-local `valueToString()` type switch (`src/QLangVM.cpp` L39-65); `ops->toStr` has zero call sites. Both gaps are closed by Tasks 4.4/4.5.
- **Native functions:** `QLangContext::setFunc(const std::string&, NativeFunc)`, `NativeFunc = std::function<QLangValue(QLangContext&, const std::vector<QLangValue>&)>`. A registered type's name joins this table for **construction** via `setFunc` — zero compiler changes, no trampoline. (Reading the name as a *value* is a different compiler path — that is what `TypeRef` seeding solves.)
- **Context API (the real one):** `void run(const std::string&)` — **throws** `QLangError` on lex/parse/compile/run failure, writes final variable state back into the context scope; there is **no** `bool run(src, QLangValue&)` out-param form. Values come back via `get(name)`. `set(name, value, isConst=false)` seeds variables visible to later `run()` compiles. `clear()` wipes `m_vars`, `m_nativeFuncs`, `m_modules`, compiled programs — v2 extends it to `m_types` (see Task 3.4).
- **Errors:** `QLangError : std::runtime_error` (kinds `Lex, Parse, Type, Runtime, Syntax, Io`); throw from bindings, VM catches at top level, `run()` rethrows.
- **Serialization:** `.qlc` codec declared but not implemented — `docs/SERIALIZATION.md` tag table is stale (shows `16=Eof`, missing `Object`; the real enum already has `Object=16`).
- Green baseline to preserve (re-verified 2026-10-27): `spec_check2` 252/252, `spec_check` 33/33, `spec_cast_test` 20/20, `gen_test` 5/5, `test_module`, ctest 2/2 (`smoke`, `range`), plus `test_inject`/`test_events`/`test_arr`/`test_call`/`test_dollar_operator` (manual `g++` builds — **not** CMake targets).

## Architecture

`registerType(const TypeMeta&)` validates the meta, stores an erased `TypeDescriptor` in `std::vector<std::unique_ptr<TypeDescriptor>> m_types` (pointers stable for the context's lifetime), joins the type name to the native-function table (construction), and **seeds the type name as a script variable holding a `TypeRef` value** (constants + `type()`). `ctx.import<T>()` is `registerType(T::qlang_meta())`. `kExternalOps` dispatches instance get/set/toStr and turns kind-2 reads into `BoundMethod` values; `kTypeRefOps` dispatches type-level get (enums) and call (construct); the extended `CALLCLOS` invokes `BoundMethod` values. A **data-member property has a real setter** (writes back through the member pointer); a **const/computed property is read-only**. No new opcodes, no parser/compiler changes; three new value types; one `CALLCLOS` extension; one `valueToString` signature change.

## Step-by-step tasks

### Task 1 — Value types: `External`, `BoundMethod`, `TypeRef`

**Files:** `include/qlang/QLangValue.h`, `include/qlang/QLangVariant.h`, `src/QLangValue.cpp`, `include/qlang/QValue.h`, `include/qlang/QLangTypes.h` (stub)

1. Create `include/qlang/QLangTypes.h` **first** (Task 3 fills the rest):
```cpp
#pragma once
namespace qlang {
struct Member;      // defined in qlang/QLangMeta.h
struct TypeDescriptor;  // full definition in Task 3
}
```
2. In `include/qlang/QLangValue.h` (after `ObjectValue`):
```cpp
#include "QLangTypes.h"   // forward decls only

// A handle to one C++ object — a view, never a copy. Exactly one of:
//  owned    = script-constructed (script shares lifetime)
//  borrowed = engine handoff (engine owns; script holds a weak ref)
struct ExternalRef {
  const TypeDescriptor* type = nullptr;
  std::shared_ptr<void> owned;
  std::weak_ptr<void> borrowed;
  const void* self() const;  // throws QLangError(Runtime,"object destroyed:<type>") on expired borrow
  void* selfMut() const;     // same, non-const (for setters); throws on expired/null type
};
// A callable method of an External object (what TABGET returns for kind 2).
struct BoundMethodRef {
  const Member* member = nullptr;   // stable: lives in the context-owned TypeDescriptor
  ExternalRef target;
};
// The type itself as a value: enum constants + construction.
struct TypeRefRef {
  const TypeDescriptor* type = nullptr;
};
```
3. Add `External`, `BoundMethod`, `TypeRef` to the `Type` enum **between `Object` and `Eof`**; add `ExternalRef`, `BoundMethodRef`, `TypeRefRef` as the **last three** `QLangVariant` alternatives (in `QLangVariant.h`).
4. Accessors/factories (mirror the `Object` pattern): `static QValue External(const TypeDescriptor*, const std::shared_ptr<void>&)`, `static QValue External(const TypeDescriptor*, const std::weak_ptr<void>&)`, `static QValue BoundMethod(const Member*, const ExternalRef&)`, `static QValue TypeRef(const TypeDescriptor*)`; `asExternal()`, `asBoundMethod()`, `asTypeRef()` (+ non-const where the `Object` pattern has them).
5. In `src/QLangValue.cpp`: implement both `External` factories, `BoundMethod`, `TypeRef`, the accessors (`std::get<ExternalRef>` etc.), `ExternalRef::self()`/`selfMut()` (`owned.get()` else `borrowed.lock()`, throw `Runtime` on expiry), and add the three names to `typeName()` (`"external"`, `"bound method"`, `"type"`). (There is no `toString` method on `QLangValue` — stringification is the ops/`valueToString` story, Task 4.4.)
6. In `include/qlang/QValue.h`: `qval_external(const TypeDescriptor*, const std::shared_ptr<void>&)` + `weak_ptr` overload, `qval_bound_method(const Member*, const ExternalRef&)`, `qval_type_ref(const TypeDescriptor*)`.

**Verify:** `cmake --build build -j` exit 0; `stdbuf -oL -eL /tmp/sc 2>/dev/null | tail -1` → `252 passed, 0 failed`; `ctest --test-dir build` → 100%. **Commit:** `git commit -am "Add External/BoundMethod/TypeRef value types (handles to one C++ object, its methods, and the type)"`

### Task 2 — Boundary conversions

**Files:** `include/qlang/QValue.h`, `include/qlang/QLangValue.h`, `src/QLangValue.cpp`

What makes the meta bindings work: property getters convert C++→`QValue` via `qval_of`; setters/method-args convert `QValue`→C++ via `as<T>()`.

1. `QLangValue::as<T>()` template declaration; specializations in `src/QLangValue.cpp` for `bool, int32_t, int64_t, double, float, std::string` (wrong script type → `QLangError::Type`). `as<double>()` also accepts `Int` (1 → 1.0); `as<int32_t>()` accepts `Int`/`Bool`/`Char` storage per the existing `asInt` rules. Member types outside this set are a **compile error at the host's `QL_OBJECT` line** until a specialization is added — expected, and diagnostic.
2. In `QValue.h`:
```cpp
// Generic C++ -> QValue for scalars (used by meta bindings).
inline QValue qval_of(bool v); inline QValue qval_of(int64_t v);
inline QValue qval_of(uint64_t v); inline QValue qval_of(double v);
inline QValue qval_of(float v); inline QValue qval_of(const std::string& v);
inline QValue qval_of(const char* v);
// (+ the small int/uint width overloads that map to qval_int)

template <class T> QValue qval_vec(const std::vector<T>& v);  // -> QValue::Array
template <class T> QValue qval_opt(const std::optional<T>& o); // -> value or Nil
```
Match the existing `qval_bool/qval_int/...` helper names in that header (read it first). No QValue→C++ container inverse — script data enters C++ one argument at a time via `as<T>`.

**Verify:** build exit 0; suites green. **Commit:** `git commit -am "Add qval_of/as<T>/qval_vec/qval_opt boundary conversions for external types"`

### Task 3 — Declarative meta: `QLangMeta.h` + `TypeDescriptor` + `import<T>()`

**Files:** new `include/qlang/QLangMeta.h`, `include/qlang/QLangTypes.h` (full), `include/qlang/QLangContext.h`, `src/QLangContext.cpp`

1. Create `include/qlang/QLangMeta.h` **exactly as shown in "The macro set" above** — the 8 `QL_`/`READ`/`WRITE` macros, `AccPtr`/`acc`, the **four** `prop` overloads (data member / **bare const getter** / `READ` / `READ`+`WRITE`), the **two** `method` overloads (non-const / const), `call`, `enum_table` (whole-group + pair overloads), `enumName` (`__PRETTY_FUNCTION__` parse), `StrClause`/`CtorClause`, `str_clause`/`ctor_clause`, `add`/`assemble`, and the `Member`/`TypeMeta`/`EnumMeta`/`EnumEntry` structs. Includes: `qlang/QLangValue.h` (for `QLangValue`/`QValue` in the closures).
2. Complete `include/qlang/QLangTypes.h`:
```cpp
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "qlang/QLangMeta.h"
#include "qlang/QLangValue.h"
namespace qlang {
class QLangContext;
// Runtime form of a registered type, owned by the context. The pointer from
// import<T>() is stable for the context's lifetime (invalidated by clear()).
struct TypeDescriptor {
  std::string name;
  std::vector<Member> members;          // deduced from the QL_OBJECT list
  std::function<QLangValue(const void*)> toString;
  std::function<std::shared_ptr<void>(QLangContext&, const std::vector<QLangValue>&)> construct;
  std::vector<EnumMeta> enums;          // QL_ENUM / QL_FLAGS
};
}
```
3. In `include/qlang/QLangContext.h`: `#include "qlang/QLangTypes.h"`; add
```cpp
  const TypeDescriptor* registerType(const TypeMeta& meta);  // validates (step 4)
  template <class T> const TypeDescriptor* import() { return registerType(T::qlang_meta()); }
```
and private `std::vector<std::unique_ptr<TypeDescriptor>> m_types;`.
4. In `src/QLangContext.cpp` (`#include <set>`): `registerType` cross-validates and throws `QLangError::Type` on: empty type name; empty member name; **duplicate member names**; a name used as both property and method; **duplicate type name** (already-registered `meta.name` — re-import after `clear()` is the supported path, since `clear()` also clears `m_types` and the constructor native); type name clashing with an existing native function **when `construct` is set**. Then copies `members`/`toString`/`construct`/`enums` into the descriptor and, if `construct` is set:
```cpp
setFunc(meta.name, [d = desc.get()](QLangContext& c, const std::vector<QLangValue>& a) {
  return QValue::External(d, d->construct(c, a));   // wrap: NativeFunc returns QValue
});
```
(the v4 snippet returned the bare `shared_ptr<void>` — a type error; `d->construct` yields the `shared_ptr`, `External` wraps it). Then **seeds the type-level value**: `set(meta.name, QValue::TypeRef(desc.get()))` — this is what makes `Point.A` compile (the name becomes a seeded variable; the native table keeps `Point(3,4)` working). Finally `m_types.push_back(std::move(desc)); return m_types.back().get();` (push_back moves the `unique_ptr`, not the pointee — the captures stay valid).
5. In `src/QLangContext.cpp` `clear()`: add `m_types.clear();` — descriptors, constructor natives, and the `TypeRef` variable all die together; host code must re-run `import<T>()` after `clear()`. Document this in the `registerType` comment.

**Verify:** build exit 0; suites green. **Commit:** `git commit -am "Add QL_OBJECT/QL_PROPERTY/QL_ENUM declarative meta (member-pointer deduction) + import<T>() with validation and TypeRef seeding"`

### Task 4 — `kExternalOps` + `kTypeRefOps` dispatch + `type()` builtin

**Files:** `src/QLangObjects.cpp`, `include/qlang/QLangObjects.h` (comment table), `src/QLangVM.cpp`

1. In `src/QLangObjects.cpp`, two namespace blocks mirroring `table_ops`:
   - `selfOf(v)` — resolve `owned`/`borrowed`; expired borrow → `QLangError(Runtime, "object destroyed: <type>")`.
   - **`kExternalOps`:**
     - `getField` — find member by name; `kind 0/1` → `get(self)`; **`kind 2` → `QValue::BoundMethod(&member, externalRefOf(v))`** (the callable form — this is how `e.translate(1,1)` works); no member match → `Type` error "no field 'x' on type 'T'"; C++ exceptions → `QLangError(Runtime, e.what())`. (No enum fallback here — enum constants live on the *type* value, Task 4.2.)
     - `setField` — find member by name; `kind 0` (data member) → `set(selfMut, value)`; `kind 1` (const/computed) → `Type` error "read-only"; `kind 2` → `Type` error "is a method"; unknown → `Type` error "no field 'x' on type 'T'".
     - `callMethod` — `kind 2` → `invoke(self, ctx, args)`; else `Type` error. (Not called by the VM in v1 — kept for the future event-handler contract.)
     - `toStr` — `toString` if set, else `Type` error naming the type.
     - `kExternalOps = { .kindName="external", .getField=…, .setField=…, .callMethod=…, .toStr=…, .base=nullptr };` **Match the exact `ObjectOps` signatures** (read `QLangObjects.h` first; `table_ops` is the reference).
   - **`kTypeRefOps`:**
     - `getField` — search the type's `enums` tables for a constant name → `QValue::Int(value)` (this is how `DockArea.AnimatedDocks` resolves); no match → `Type` error "type 'T' has no constant 'x'".
     - `setField` — always `Type` error "type 'T' is not settable".
     - `callMethod` — `descriptor->construct` if set → `QValue::External(desc, …)` (owned); else `Type` error "type 'T' is not constructible".
     - `toStr` — `"<type T>"`.
2. In `getObjectOps()` (L196): `if (obj.type() == QLangValue::Type::External) return &kExternalOps;` and `if (obj.type() == QLangValue::Type::TypeRef) return &kTypeRefOps;` (`BoundMethod` → `nullptr` — it is callable, not an object).
3. In `src/QLangVM.cpp` `Builtin::Type` (L261), after the `Object` branch: `if (args[0].type()==Type::External || args[0].type()==Type::TypeRef) return QValue::String(<desc>.name);` (both name the same type — `type(Point)` and `type(p)` → `"Point"`).
4. **`valueToString` → `toStr` wiring** (closes the v4 str gap): change the file-local `std::string valueToString(const QLangValue&)` to `std::string valueToString(const QLangValue&, QLangContext&)`; at the top: `const ObjectOps* ops = getObjectOps(v); if (ops && (v.type() == Type::External || v.type() == Type::TypeRef)) return ops->toStr(v, ctx).asString();` — the type switch stays as the fallback (and gains the three new static cases: `External` → `"<external: " + type->name + ">"`, `BoundMethod` → `"<bound method>"`, `TypeRef` → `"<type " + name + ">"`, so `print()` is sane even without a `QL_STR`). Update all ~20 call sites in `QLangVM.cpp` — every one has `m_scope` in hand (builtins at L247/L282/L286, `printf`/`sprintf` at L318/L386, f-string `Format` at L471, cast at L735, `$`/`EVALKEY` at L772, concat at L787, table-key stringify at L895, `TABGET`/`TABPUT` key paths at L1078/L1101/L1410/L1433/L1787/L1807/L2129/L2151). This also fixes the pre-existing gap where `str(instance)` of a QLang class returns `"<table>"` only for tables — external and type values now stringify through their bindings.
5. Update the dispatch comment table in `include/qlang/QLangObjects.h` to list the external and typeref kinds (and note `callMethod`/`toStr` are the event-contract / `valueToString` consumers).

**Verify:** build exit 0 (watch for `-Wswitch` — the `valueToString` switch now handles all enum values); `stdbuf -oL -eL /tmp/sc 2>/dev/null | tail -1` → `252 passed, 0 failed`. **Commit:** `git commit -am "Add kExternalOps/kTypeRefOps dispatch (data-member setters, read-only computed props, BoundMethod reads, enum constants) + toStr wiring in valueToString"`

### Task 4.5 — `CALLCLOS` dispatch for `BoundMethod` (closes the v4 method-call gap)

**Files:** `src/QLangVM.cpp`

In the `CALLCLOS` handler (L2189), **before** the `Generator`/`Function` checks:
```cpp
if (cv.type() == QLangValue::Type::BoundMethod) {
  frame.pc += 1;
  const auto& bm = cv.asBoundMethod();
  const void* self = bm.target.self();   // throws "object destroyed: <type>" on expired borrow
  std::vector<QLangValue> args;
  for (int k = 0; k < i.c; k++)
    args.push_back(frame.regs[i.a + 1 + k]);   // same arg convention as CALL (dest+1..dest+n)
  frame.regs[i.a] = bm.member->invoke(self, m_scope, args);
  break;
}
```
Notes: `Member::invoke` is `std::function<QLangValue(void*, QLangContext&, const std::vector<QLangValue>&)>` — pass `const_cast`-free: `self()` returns `const void*`; for methods that mutate, the binding's `invoke` takes `void*` — store `invoke` as `std::function<QLangValue(void*, QLangContext&, const std::vector<QLangValue>&)>` and use `bm.target.selfMut()` here (the method binding was compiled from a non-const member pointer for mutable methods; const methods go through the const `method` overload and only need `const void*` — `selfMut()` is the uniform choice and throws on expiry like `self()`). No new opcode: `CALLCLOS` already carries the callee value, and the compiler's non-class method path (`QLangCompiler.cpp` L1748-1753) already emits exactly `MOV dest, funcReg; CALLCLOS dest, n, n`.

**Verify:** build exit 0; suites green. **Commit:** `git commit -am "Dispatch BoundMethod values in CALLCLOS (external method calls, no new opcode)"`

### Task 5 — Tests (TDD: the failing tests for Tasks 3–4.5)

**Files:** new `examples/test_external.cpp`, `CMakeLists.txt`

Written against the **real** throwing API (`void run(const std::string&)` + `get(name)`; the v4 draft used a nonexistent `bool run(src, out)`):

```cpp
// examples/test_external.cpp
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include "qlang/QLangContext.h"
#include "qlang/QLangMeta.h"
#include "qlang/QLangErrors.h"

using namespace qlang;

enum class Kind { A = 1, B = 2 };

struct Point {
  double x = 0, y = 0;                                  // public -> read-write
  double dist() const { return std::sqrt(x*x + y*y); }  // const getter -> read-only
  void translate(double dx, double dy) { x += dx; y += dy; }
  std::string str() const { return "Point(" + std::to_string(x) + "," + std::to_string(y) + ")"; }

  QL_OBJECT(
    QL_PROPERTY(x),
    QL_PROPERTY(dist),
    QL_INVOKABLE(translate),
    QL_ENUM(Kind, A, B),
    QL_STR(str),
    QL_CTOR([](QLangContext& c, const std::vector<QValue>& a) {
      return std::make_shared<Point>(a[0].as<double>(), a[1].as<double>()); })
  );
};

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; printf("FAIL %d: %s\n", __LINE__, #cond); } } while (0)

// run() throws QLangError on failure; results come back via get().
static QValue runAndGet(QLangContext& ctx, const std::string& expr) {
  ctx.run("r = " + expr);
  return ctx.get("r");
}
static bool runThrows(QLangContext& ctx, const std::string& src) {
  try { ctx.run(src); return false; } catch (const QLangError&) { return true; }
}

int main() {
  QLangContext ctx;
  const TypeDescriptor* pt = ctx.import<Point>();

  // 1. Public data member = read-write: script SETTER works and is visible to C++.
  auto e = std::make_shared<Point>(1, 2);
  ctx.set("e", QValue::External(pt, std::weak_ptr<void>(e)));
  CHECK(!runThrows(ctx, "e.x = 10"));        // setter on public member
  CHECK(e->x == 10);                         // referential integrity: same object
  CHECK(runAndGet(ctx, "e.y + e.x").asFloat() == 12);

  // 2. Computed/const property is read-only.
  CHECK(runThrows(ctx, "e.dist = 9"));       // read-only -> error
  CHECK(runAndGet(ctx, "e.dist").asFloat() > 0);

  // 3. Method mutation (TABGET -> BoundMethod -> CALLCLOS).
  CHECK(!runThrows(ctx, "e.translate(1, 1)"));
  CHECK(e->x == 11 && e->y == 3);

  // 4. str / type (valueToString -> toStr wiring).
  CHECK(!runThrows(ctx, "s = f\"{e}\"; t = type(e)"));
  CHECK(ctx.get("s").asString() == "Point(11,3)");
  CHECK(ctx.get("t").asString() == "Point");

  // 5. Script construction via the type name in the native table.
  CHECK(runAndGet(ctx, "Point(3, 4).dist").asFloat() == 5);

  // 6. Enum group: named constants via the TypeRef value + flag combination.
  CHECK(runAndGet(ctx, "Point.A | Point.B").asInt() == 3);

  // 7. Unknown member / method.
  CHECK(runThrows(ctx, "e.nope"));
  CHECK(runThrows(ctx, "e.warp()"));

  // 8. Engine destroys the object -> clean "object destroyed".
  e.reset();
  CHECK(runThrows(ctx, "e.x"));

  // 9. Validation: duplicate member name rejected.
  struct Dup {
    int a = 0, b = 0;
    int getB() const { return b; }
    void setB(int v) { b = v; }
    QL_OBJECT(QL_PROPERTY(a), QL_PROPERTY(a, READ getB, WRITE setB));  // name "a" twice
  };
  bool rejected = false;
  try { QLangContext c2; c2.import<Dup>(); } catch (const QLangError&) { rejected = true; }
  CHECK(rejected);

  // 10. Method extraction: a kind-2 read is a first-class callable value.
  auto e2 = std::make_shared<Point>(0, 0);
  ctx.set("e2", QValue::External(pt, std::weak_ptr<void>(e2)));
  CHECK(!runThrows(ctx, "m = e2.translate\nm(4, 5)"));
  CHECK(e2->x == 4 && e2->y == 5);
  CHECK(ctx.get("type(e2)").asString() == "Point");  // type() on the instance

  // 11. type-level value: type(Point) names the type; Point is not settable.
  CHECK(runAndGet(ctx, "type(Point)").asString() == "Point");
  CHECK(runThrows(ctx, "Point = 1"));

  if (failures) { printf("%d FAILURES\n", failures); return 1; }
  printf("test_external: all checks passed\n");
  return 0;
}
```

Register in `CMakeLists.txt` — copy the **`range_check`** shape (L53-57; `test_inject` is not a CMake target):
```cmake
add_executable(test_external examples/test_external.cpp)
target_link_libraries(test_external PRIVATE qlang)
add_test(NAME external COMMAND test_external)
```

**TDD sequence:** land Tasks 1–4.5 (green) → write `test_external.cpp` + CMake → `ctest --test-dir build -R external --output-on-failure` (fix dispatch, not the contract) → full `ctest` green.

**Commit:** `git commit -am "Add test_external: public-member setters, read-only computed props, methods (incl. extraction), enum constants, str, handoff integrity, validation"`

### Task 6 — `QL_FLAGS` verification (small, after the core is green)

**Files:** `include/qlang/QLangMeta.h` (only if the pair overload needs a tweak), `src/QLangObjects.cpp`, `examples/test_external.cpp`

- `QL_FLAGS` is already the same `enum_table` (marked); verify a flag-style table (bitmask values) resolves and ORs in script — add a `DockOption`-style struct (6 bitmask enumerators) with 2 checks: named constant resolves, `F.A | F.C` == expected.
- **Deferred (named, not in v1):** `str`/`f"..."` of an enum-valued *property* rendering the enumerator **name** (reverse lookup) — there is no dispatch point that sees both the field value and its type context (`str(e.options)` sees a bare `Int`); it would need a tagged field value or a compiler change. Revisit with the event-handler work.

**Verify:** `ctest -R external` green, full suite green. **Commit:** `git commit -am "Add QL_FLAGS verification (bitmask table resolves and ORs in script)"`

### Task 7 — Docs

**Files:** `docs/SERIALIZATION.md`, `docs/EXTERNAL_TYPES.md`, `SPEC_COMPLIANCE.md`

1. `docs/SERIALIZATION.md` §3 tag table: `16=Object`, `17=External`, `18=BoundMethod`, `19=TypeRef` (all **not durable** → emit tag 0), `20=Eof`; extend R2 to "MUST NOT emit tags 15, 16, 17, 18, 19".
2. `docs/EXTERNAL_TYPES.md`: status `draft`→`implemented (2026-10-27)`; rewrite §2/§6 to the **Qt-style `QL_` macro model** (no MOC; individual `QL_PROPERTY`/`QL_INVOKABLE`/`QL_ENUM`/`QL_FLAGS` lines; `QL_OBJECT` assembly; member-pointer deduction; public data member = read-write, const getter = read-only, function = method; whole-group `QL_ENUM` with `__PRETTY_FUNCTION__` name recovery; `QL_` prefix avoids collisions when compiled alongside Qt); §3 ownership = read view over one object (unchanged); §4 dispatch table updated with the **real** v1 paths (method call = `TABGET`→`BoundMethod`→`CALLCLOS`, not `callMethod`; string = `valueToString`→`toStr`); §5 methods: `BoundMethod` value type replaces the `NativeMethod` pointer proposal; §6 construction: native-table path **plus** `TypeRef` seeding for `Type.Constant` reads; §11 status + deviations: (a) compile-time integrity via member pointers, (b) registration-time validation, (c) `Object` landed at tag 16 before external, (d) **no `__destroy__`/`destroy` hook in v1** (plain shared/weak view; resource-teardown hook deferred), (e) **`type(x)` returns the declared name string, not `type_info::name`** (no RTTI).
3. `SPEC_COMPLIANCE.md`: external C++ types implemented (`<commit>`); update the conformance-table status per deviations (a)–(e); open: `hasMethod`/`callMethod` context wrappers (event contract), RTTI validation on handoff, inheritance (`ObjectOps::base` reserved), signals, enum-name string form. Per AGENTS.md: rebuild + re-run the affected suites **first**, then flip Open Items and update the SHA256/tally/checkpoint rows.

**Verify:** `git diff --stat` shows only the three docs. **Commit:** `git commit -am "Docs: external types implemented (QL_ declarative meta, BoundMethod/TypeRef dispatch); SERIALIZATION tag table corrected"`

## Tests / validation

Per-task TDD embedded above. Final gate after Task 6:
```sh
cmake --build build -j
ctest --test-dir build --output-on-failure   # ALL pass, incl. new `external` (3 tests total)
for t in spec_check spec_cast_test gen_test test_module test_inject test_events test_arr test_call test_dollar_operator; do
  g++ -std=c++17 -Iinclude examples/$t.cpp build/libqlang.a -o /tmp/$t && stdbuf -oL -eL /tmp/$t 2>/dev/null | tail -1
done
```
Expected: every line ends `passed, 0 failed` / `ALL PASS` / equivalent (spec_check2: 252, spec_check: 33, spec_cast_test: 20, gen_test: 5).

## Risks, tradeoffs, open questions

- **`QL_` prefix** — chosen specifically so a translation unit can include this header *and* Qt headers without macro collision (`QL_OBJECT` vs `Q_OBJECT`, …). `READ`/`WRITE` are the only un-prefixed macros; they are trivial token transformers and collide with nothing in Qt (Qt uses them only *inside* `Q_PROPERTY`). If a host ever needs them gone, they become `QL_READ`/`QL_WRITE` in a one-line change.
- **`__CLASS__` is a GCC/Clang extension** — fine (GCC sandbox); not standard C++17. Portable escape hatch: `QL_OBJECT_NAME(Point, ...)` variant.
- **`__PRETTY_FUNCTION__` parse for enumerator names is GCC-format-specific.** Shortcut: if GCC ever reformats it, switch call sites to the explicit-pair overload `QL_ENUM(E, ("Name", E::Name), ...)` — the overload already exists, no header change.
- **`QL_PROPERTY(x)` needs the member named `x` to exist and be accessible** — inside the class all members are reachable, so publicity is *by listing*: a listed data member is exposed read-write (public member = automatic setter; private member listed = the class explicitly opts it in). A private member with only `QL_PROPERTY(name, READ g)` = read-only. Bare non-const getters deliberately match no overload — list via `QL_INVOKABLE`, make `const`, or use `READ`.
- **`BoundMethod` lifetime:** `BoundMethodRef::member` points into the context-owned `TypeDescriptor` — stable exactly as long as the `TypeDescriptor*` in the `ExternalRef` (both die in `clear()`). A `BoundMethod` value outliving its context is a host bug, same class as a dangling `ExternalRef`.
- **`CALLCLOS` arg convention:** the `BoundMethod` branch reads `regs[i.a+1 .. i.a+i.c]` — must stay in lockstep with the `CALL` opcode's `dest+1..dest+argCount` convention and the compiler's emission (`QLangCompiler.cpp` L1748-1753). Pinned by test checks 3 and 10.
- **`TypeRef` as a script variable:** importing a type binds its name in the script scope (shadowing is the user's choice; `delete Point` breaks `Point.A` until re-import — acceptable). `registerType` rejects a name already taken by a *native function* only when `construct` is set; a same-name script variable is silently overwritten at import (documented).
- **`valueToString` signature change:** file-local to `QLangVM.cpp`, all ~20 sites have `m_scope`; the full suite (252 + 33 + 20 + 5) exercises print/printf/f-string/concat/table-key paths — the existing corpus is the regression net.
- **`Type` enum ↔ variant index coupling:** `asX()` uses type-based `std::get<StorageType>`; appending three alternatives and three enum values before `Eof` is safe because Eof is a parser sentinel (no codec writes it).
- **`as<T>()` coverage (v1):** `bool, int32_t, int64_t, double, float, std::string` (+ `as<double>` accepts `Int`). Other member types (`unsigned`, `uint64_t`, …) are a compile error at the `QL_OBJECT` line until a specialization is added — a diagnostic, not a crash.
- **Single-instance semantics are structural:** the wrapper is a pointer to one C++ object; no copy path exists in the API. Test check 1 (engine sees the script's setter write) pins it.
- **No `dynamic_cast` on handoff:** a wrong `weak_ptr` is UB; accepted for v1 (the meta is built from the type's own pointers, so it's hard to get wrong). RTTI validation stays open.
- **Skipped (named):** `Q_SIGNALS`/`emit`/`connect` (no event system — revisit with `docs/EVENT_HANDLERS.md`), method overloading, inheritance, `qt_metacall`, `__destroy__` resource teardown, enum-name string form for enum-valued properties.
