# Compiler portability of the declarative-meta header (`QLangMeta.h`)

Date: 2026-11-01
Scope: `include/qlang/QLangMeta.h` — the Qt-style declarative macros that expose
C++ types to QLang (External C++ Types). Companion to
[EXTERNAL_TYPES.md](EXTERNAL_TYPES.md) and [spec/11-external-types.md](spec/11-external-types.md).
Commits: `ce31e3c` (value types + boundaries), `a93e58e` (declarative meta +
`import<T>`), `ab1afa5` (MSVC portability).

**Verdict: the macro system is portable across g++, Clang, MinGW, MSVC, and
emscripten/WASM.** Four of the five were verified by actually compiling (and,
for the two native ones, running) a smoke test that exercises every macro.
MSVC is not installed in the build environment, so it is addressed by removing
exactly the two non-standard constructs it is known to reject; a `cl /std:c++17`
compile on a Windows box would close the last cell.

## Compatibility matrix

| Compiler | Version tested | Status | Evidence |
|---|---|---|---|
| **g++** (GCC) | 16.2.1 | ✅ verified | full build + `ctest` 5/5 + `spec_check2` 421/0 + smoke test runs |
| **Clang** | 22.1.8 | ✅ verified | compiles **and runs** the smoke test ("all checks passed") |
| **MinGW** (`x86_64-w64-mingw32-g++`) | 16.2.0 (GCC) | ✅ verified | compiles to object |
| **emscripten / WASM** (`em++`) | 6.0.9 | ✅ verified | compiles to object |
| **MSVC** | n/a (not installed) | ⚠️ reasoned | both incompatibilities removed; not compile-tested here |

Everything in the header is **standard C++17** (CRTP, qualified
pointers-to-member, `std::variant`, templates, lambdas, `std::function`,
`std::pair` brace-init) except for **two** constructs, both of which MSVC's
conforming preprocessor / front-end lacks. Both are now gone from the
functional code path.

## The two non-standard constructs (and the fix for each)

### 1. `__PRETTY_FUNCTION__` → `QL_PRETTY_FUNCTION`

`__PRETTY_FUNCTION__` is provided by GCC, Clang, MinGW (GCC-based), and
emscripten (Clang-based). **MSVC does not define it** — it provides
`__FUNCSIG__` instead.

```cpp
#if defined(_MSC_VER)
#define QL_PRETTY_FUNCTION __FUNCSIG__
#else
#define QL_PRETTY_FUNCTION __PRETTY_FUNCTION__
#endif
```

`QL_OBJECT` calls `meta_detail::className(QL_PRETTY_FUNCTION)`. `className()`
recovers the class name by locating the substring `::qlang_meta` and walking
backward over identifier characters, so it does **not** depend on the exact
format. This matters because the formats differ:

- GCC:    `static const qlang::TypeMeta& Point::qlang_meta()`
- Clang:  `static const qlang::TypeMeta & Point::qlang_meta()`  (space before `&`)
- MSVC:   `static const class qlang::TypeMeta & __cdecl Point::qlang_meta(void)`

All three contain `::qlang_meta`, so the same parser works for every compiler.

### 2. `##__VA_ARGS__` (GNU comma-swallow) → arity dispatch / plain pack

`##__VA_ARGS__` is a GNU extension that drops a preceding comma when
`__VA_ARGS__` is empty. MSVC's conforming preprocessor does not implement it.
It was used in two places:

- **`QL_PROPERTY`'s optional tail.** Bare `QL_PROPERTY(x)` (no `READ`/`WRITE`)
  is the *common* case, so this one is non-trivial: you cannot portably drop a
  trailing comma. Replaced with an **arity dispatch** that counts the *total*
  args (the name included), so the zero-tail case needs no comma-swallow:

  ```cpp
  #define QL_PROPERTY(...) \
    QL_PROPERTY_DISPATCH(QL_PROP_NARG(__VA_ARGS__), __VA_ARGS__)
  #define QL_PROP_NARG(...) QL_PROP_NARG_IMPL(__VA_ARGS__, 3, 2, 1, 0)
  #define QL_PROP_NARG_IMPL(_1, _2, _3, N, ...) N
  #define QL_PROPERTY_DISPATCH(N, ...) QL_PROPERTY_SEL(N)(__VA_ARGS__)
  #define QL_PROPERTY_SEL(N) QL_PROPERTY_SEL_IMPL(N)
  #define QL_PROPERTY_SEL_IMPL(N) QL_PROPERTY_CASE##N
  #define QL_PROPERTY_CASE1(name)   qlang::meta_detail::prop(#name, &SelfType::name)
  #define QL_PROPERTY_CASE2(name, a) qlang::meta_detail::prop(#name, &SelfType::name, a)
  #define QL_PROPERTY_CASE3(name, a, b) qlang::meta_detail::prop(#name, &SelfType::name, a, b)
  ```

  Counting the total (name + tail) sidesteps the "count 0 args" problem that
  plagues `__VA_OPT__`-less C++17 counting macros. `READ(a)`/`WRITE(a)` expand
  to `acc(...)` calls whose internal commas are *not* top-level, so each counts
  as a single argument — `QL_PROPERTY(x, READ(g), WRITE(s))` counts as 3 total
  and dispatches to `CASE3`.

- **`QL_ENUM_COUNT`.** Dropped `##`. The only behavioral change is the
  degenerate **0-enumerator** case (it now miscounts as 1); 1–16 enumerators
  count identically on every compiler, and a 0-enum `QL_ENUM` is meaningless.

The `QL_ENUM`/`QL_FLAGS` fold selection also uses a portable two-level paste
(`QL_ENUM_SELECT` → `QL_ENUM_PASTE` → `QL_FE2_##N`); the extra indirection level
is required because `##` suppresses argument expansion, so the count must be
expanded by a normal (non-pasting) argument pass before it is pasted.

A dead, GCC-format-specific helper (`meta_detail::enumName()`, never called)
was removed as part of this work.

## Key design finding: the plan's member-pointer mechanism was not implementable

This is the most important non-obvious finding from building the header, and it
is the reason the macros look the way they do.

The original plan called for binding lines with **no class name**, forming
*unqualified* member pointers (`&x`, `&translate`) inside the static
`qlang_meta()` accessor. That is **not valid C++**:

- In a **static** context, `&x` (a data member) is ill-formed — *"invalid use
  of member"* — and `&f` (a member function) is *"forbids taking the address of
  an unqualified … member function."*
- In a **non-static** context, `&x` yields a *bound* `int*` (wrong type), and
  `&f` is still rejected.
- Only the **qualified** form `&T::member` produces a pointer-to-member, and no
  macro can produce `T` as a *type* (a macro can only yield the *string* `T`
  via `#T`).

**Resolution:** a trivial CRTP base so the class is named exactly **once**:

```cpp
struct Point : qlang::QLObject<Point> {   // named once, here
  double x = 0, y = 0;
  QL_OBJECT(
    QL_PROPERTY(x),          // binding lines stay free of the class name
    QL_INVOKABLE(translate),
    QL_ENUM(DockOption, A, B, C),
    QL_STR(str)
  );
};
ctx.import<Point>();
```

`QLObject<Self>` exposes only a `using SelfType = Self;` alias; the entry macros
form `&SelfType::member`. This preserves the plan's "no class name in the
binding lines" UX with one small addition (the base). The type *name* for the
script is still recovered from the function string, not from the base.

Related: the plan assumed `__CLASS__` was the portable way to get the type name;
in fact `__CLASS__` is an **MSVC-only** extension. The function-string approach
above is the portable one.

## How to re-verify

```sh
# native (build + run the smoke test, which exercises every macro):
g++     -std=c++17 -Iinclude smoke.cpp build/libqlang.a -o smoke && ./smoke
clang++ -std=c++17 -Iinclude smoke.cpp build/libqlang.a -o smoke && ./smoke
# cross (compile to object only — no runnable target on this host):
x86_64-w64-mingw32-g++ -std=c++17 -Iinclude -c smoke.cpp -o smoke.o
em++    -std=c++17 -Iinclude -c smoke.cpp -o smoke.o
# MSVC (on Windows):
cl /std:c++17 /Iinclude smoke.cpp qlang.lib
```

The smoke test defines a CRTP `Point` with a data property, a computed
read-only property, a method, a 3-value `QL_ENUM`, a `QL_STR`, and a `QL_CTOR`,
then `import<Point>()`s it and asserts: the type name round-trips, property
get/set closures work, the method invokes, enum values are correct, and a
duplicate type name is rejected by `registerType`.

## Open item

- **MSVC compile-test.** The two known MSVC incompatibilities are removed and
  the rest is standard C++17, but MSVC is not present in this environment.
  Confirm with `cl /std:c++17` on a Windows host (or CI) before claiming a
  green MSVC cell.
