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
exactly the constructs it is known to reject; a `cl /std:c++20`
compile on a Windows box would close the last cell.

## Compatibility matrix

| Compiler | Version tested | Status | Evidence |
|---|---|---|---|
| **g++** (GCC) | 16.2.1 | ✅ verified | full build + `ctest` 5/5 + `spec_check2` 421/0 + smoke test runs |
| **Clang** | 22.1.8 | ✅ verified | compiles **and runs** the smoke test ("all checks passed") |
| **MinGW** (`x86_64-w64-mingw32-g++`) | 16.2.0 (GCC) | ✅ verified | compiles to object |
| **emscripten / WASM** (`em++`) | 6.0.9 | ✅ verified | compiles to object |
| **MSVC** | via CI: VS 2022 on `windows-latest` (19.51.36260) | ⚠️ run 3 **build** failure in three new test targets (POSIX-only harness, fixed; mingw reproduces it locally) | runs 1–2: every library target **compiled and linked**, 42/43 ctest passed (the one failure was CRLF checkout, not a compiler defect); run 3 stopped at `manifest_compile.cpp: C1083 sys/wait.h` |

Everything in the header is **standard C++20** (CRTP, qualified
pointers-to-member, `std::variant`, templates, lambdas, `std::function`,
`std::pair` brace-init) except for **three** constructs, each of which a
conforming front-end (in practice: MSVC) rejects or mis-handles. All three are
now gone from the functional code path.

## The three non-standard constructs (and the fix for each)

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

### 3. odr-use of the declared-only signal function → `decltype` (MSVC `LNK2019`)

Found by the **first MSVC CI run**, not by reasoning: every target compiled, and
only `signal_test` failed to link.

`QL_SIGNAL` originally formed a member pointer like every other entry macro:

```cpp
#define QL_SIGNAL(name) qlang::meta_detail::signal(#name, &SelfType::name)
```

A signal's member function is **declared, never defined**
([spec/17-signals.md](spec/17-signals.md)) — only its signature is needed, to
type-check `emit` arguments. But forming `&SelfType::levelChanged` **odr-uses**
the function, which requires a definition to exist. GCC and Clang discard the
unused address before the link step; MSVC keeps the reference:

```
signal_test.obj : error LNK2019: unresolved external symbol
  "public: void __cdecl Sensor::levelChanged(int)" (?levelChanged@Sensor@@QEAAXH@Z)
  referenced in function "... QLangContext::import<Sensor>(void)"
```

**Fix:** name the *type* instead of the value — `decltype(&SelfType::name)` is an
unevaluated context and therefore does not odr-use the function:

```cpp
#define QL_SIGNAL(name) \
  qlang::meta_detail::signal<decltype(&SelfType::name)>(#name)
```

`meta_detail::SignalArgs<Sig>` strips the member-pointer type back to its
parameter pack (non-const and `const` specializations; the primary template
carries a `static_assert`, so a data member or a non-member name fails to
compile with a readable message instead of a template wall), and `signal_args`
builds the same one-`arg_check<A>`-per-parameter list as before. Behaviour is
unchanged on every compiler; only the mechanism of obtaining the signature
changed.

Reproducing it **without a Windows box** — the reference is visible in the object
file when the optimiser does not discard it:

```sh
g++ -std=c++20 -Iinclude -O0 -c tests/unit/signal_test.cpp -o /tmp/st.o
nm -C /tmp/st.o | grep levelChanged
# old header:  U Sensor::levelChanged(int)   U DupSig::levelChanged(int)
# new header:  (no output) — same check with clang++ and the mingw cross
```

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

`QL_SIGNAL` is the one binding line that does **not** form a member pointer: it
needs only the signature, and `decltype(&SelfType::name)` yields the
member-function *type* without the odr-use that `&SelfType::name` would impose
(construct 3 above).

## How to re-verify

```sh
# native (build + run the smoke test, which exercises every macro):
g++     -std=c++20 -Iinclude smoke.cpp build/libqlang.a -o smoke && ./smoke
clang++ -std=c++20 -Iinclude smoke.cpp build/libqlang.a -o smoke && ./smoke
# cross (compile to object only — no runnable target on this host):
x86_64-w64-mingw32-g++ -std=c++20 -Iinclude -c smoke.cpp -o smoke.o
em++    -std=c++20 -fexceptions -Iinclude -c smoke.cpp -o smoke.o
# (-fexceptions is required for any *runnable* emscripten build: emscripten 6
#  disables C++ exception catching by default and the library throws QLangError)
# MSVC (on Windows):
cl /std:c++20 /Iinclude smoke.cpp qlang.lib
```

The smoke test defines a CRTP `Point` with a data property, a computed
read-only property, a method, a 3-value `QL_ENUM`, a `QL_STR`, and a `QL_CTOR`,
then `import<Point>()`s it and asserts: the type name round-trips, property
get/set closures work, the method invokes, enum values are correct, and a
duplicate type name is rejected by `registerType`.

`QL_SIGNAL` is **not** in the smoke test — it is exercised by
`tests/unit/signal_test.cpp`, which every CI job builds and links (the MSVC job
is what caught construct 3, a *link*-level defect a compile-only smoke test
cannot see).

## CI matrix (GitHub Actions)

Since MSVC (and the wider toolchain matrix) cannot be covered in the local
build environment, the matrix cells are gated by three workflows, triggered
**only on push** (placeholder — the repo branch is renamed from `master` before
CI goes live; no PR, tag, or other-branch triggers by design): `ci.yml` and
`ci-wasm.yml` on `main`, `ci-msvc.yml` still on `master` (Round 48):

- `.github/workflows/ci.yml` — native matrix, all on `ubuntu-latest`:
  **gxx** (system g++), **clang** (clang++), **mingw** (mingw-w64 cross,
  build-only — Windows `.exe` cannot run on a Linux runner).
- `.github/workflows/ci-msvc.yml` — MSVC x64 (`windows-latest`).
- `.github/workflows/ci-wasm.yml` — emscripten (emsdk `latest` on
  `ubuntu-latest`): builds `qlang` + `qlang_example` with
  `-sALLOW_MEMORY_GROWTH -fexceptions` and runs the self-contained smoke
  via `node build/qlang_example.js`. The full ctest suite is not
  applicable on wasm (no filesystem for the script-level regression
  suite).

MSVC job details (the cell this document tracks):

- **Runner:** `windows-latest` → Visual Studio 2022 Community with the
  most recent hosted MSVC toolset (14.51 / compiler 19.51.36260).
- **Configure:** `cmake -B build -A x64 -DCMAKE_BUILD_TYPE=Release`.
- **Build:** `cmake --build build --config Release --parallel` — *all*
  targets in the root `CMakeLists.txt`, so a new target is covered
  automatically.
- **Test:** `ctest --test-dir build -C Release -E perf --output-on-failure`
  (43 of the 45 entries — `-E perf` is a regex, so it drops `perf` **and**
  `script_perf_stress`; the perf gate is calibrated for g++/Linux and runs only
  in the gxx job).
- **Checkout line endings:** the runner image sets `core.autocrlf=true` in git's
  system config, so `actions/checkout` would rewrite every text file to CRLF.
  `.gitattributes` (`* text=auto eol=lf`, binaries declared `binary`) keeps the
  checkout byte-identical to the committed blobs — without it the `doc_hashes`
  entry fails with all 24 module hashes "stale" (Round 48).
- **Test-harness portability:** the CLI smokes (`manifest_check`,
  `manifest_compile`, `portable_module`) drive `qlcc` through
  `tests/unit/test_cli.h` — `run()` (popen/_popen, exit code + combined
  output), `arg()` (shell quoting), `scratch()` (a per-run temp directory) —
  instead of `std::system` + `WEXITSTATUS`. Neither Windows compiler has
  `<sys/wait.h>`, and the shell half of the old checks (`2>/dev/null`,
  `grep -q`) is replaced by matching the captured output. Both Windows gates
  are reproducible on Linux with the MinGW cross, which breaks on the same
  constructs: `cmake -B <tmp> -G "MinGW Makefiles"
  -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++ && cmake --build <tmp>`.
  It also caught two non-header traps: `EnumEntry::value` is a `long`, so the
  manifest fixture uses `std::numeric_limits<long>::max()` rather than
  `5000000000L` (32-bit `long` on Windows), and `std::filesystem::path` does
  not convert to `std::string` implicitly under MSVC
  (`examples/portable_module.cpp`).
- **What the MinGW proxy cannot see:** it *is* GCC, so GCC/Clang-only
  predefined macros compile fine there — run 3's next error was
  `__FILE_NAME__` in `examples/integration/logger.h` (now `LOG_FILE_NAME`,
  `__FILE__` on MSVC, with `Logger::log` trimming to a basename so the output
  is unchanged). Local proxies: compile with `-U__FILE_NAME__` to force the
  fallback branch, and sweep the whole set with `-Wall` for
  `no return statement` — MSVC makes that **error C4716**, and in
  `CActor::tileAt` the empty body had also been letting GCC delete the only
  reference to `tileDefs`, whose generated definition was never vendored
  (`examples/integration/tilesdefs.cpp` now supplies the one entry the demo
  reaches).
- **Version capture:** the *Show compiler version* step prints `cl /?`
  and records the exact "C/C++ Optimizing Compiler Version" in the step
  output. That number is the one to fill into the matrix row above.

## Open item

- **MSVC — three CI runs done; awaiting the fourth.** Run 1
  (`.github/workflows/ci-msvc.yml`): **every target compiled** with the hosted
  VS 2022 toolset and the only failure was a **link** error in `signal_test` —
  construct 3 above, invisible to a compile-only check and to GCC/Clang, which
  discard the unused member pointer. It is removed; the local `nm` check above
  reproduces it on Linux. Run 2: everything compiled **and** linked and the
  suite ran — 42/43 passed, the single failure being `doc_hashes`, i.e. **not a
  compiler defect** but the CRLF checkout described above (fixed by
  `.gitattributes` + the LF-normalized hash). Run 3: the **build** failed — the
  three portable-module-era targets had landed since run 2 with a POSIX-only
  harness (`<sys/wait.h>`, `popen`, `::mkdir`, a hardcoded `/tmp` scratch
  path); see the test-harness bullet above — the mingw job breaks on the same
  code. The rest is standard C++20, but
  MSVC is not present in the local environment, so the cell stays reasoned
  until CI is green: on the **first green run**, flip the MSVC matrix row to ✅
  verified and record the exact compiler version from the *Show compiler
  version* step log.
