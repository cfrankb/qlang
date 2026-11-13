# Brief — adding QLmeta (`QL_*`) bindings to a C++ type

**Audience:** an AI agent asked to "add qlmeta data to C++ files", "expose this C++
type to QLang", or "make this host type describable in the `QLM1` manifest".
**Task:** add `QL_OBJECT(...)` meta to host C++ types. Nothing else.
**Snapshot:** behaviour verified 2026-11-12 against
`include/qlang/QLangMeta.h` (sha256 `fe9ba4ed…c156cf`) — if the header's hash
differs, re-read it before trusting this brief.

## 1. What "adding qlmeta" is

C++ RTTI knows a type's identity, not its fields. QLang therefore asks the host
to state the interface explicitly: one declarative line per binding, inside the
class, expanded by macros into a `qlang::TypeMeta`. That meta is what scripts
see (`p.x`, `p.move(1)`, `Point.Right`, `str(p)`), what
`qlang::writeTypeManifest()` dumps as a `QLM1` manifest, and what the compiler
(`qlang_run --qlmeta`) and the portable-module handshake check against. No MOC,
no code generation, no hand-written `TypeMeta`.

Deliverable per type: the CRTP base, the `QL_OBJECT(...)` block, and a check
that the binding actually holds (test or manifest dump).

## 2. Read first

| file | why |
|---|---|
| `include/qlang/QLangMeta.h` | the macros themselves + the rules in comments; authoritative |
| `docs/EXTERNAL_TYPES.md` | the design doc (§2 macros, §3 ownership, §12 self-registration) |
| `docs/spec/11-external-types.md` | normative conformance + manifest extraction |
| `docs/spec/17-signals.md` | only if adding `QL_SIGNAL` |
| `tests/unit/test_external.cpp`, `tests/unit/manifest_check.cpp` | working fixtures for every macro shape |

## 3. The shape a binding takes

```cpp
#include "qlang/QLangMeta.h"

struct Point : qlang::QLObject<Point> {          // the type names itself once
  Point(double x_, double y_) : x(x_), y(y_) {}
  double x = 0, y = 0;                           // public data -> read-write
  double dist() const { return std::sqrt(x * x + y * y); }  // const -> read-only
  void translate(double dx, double dy) { x += dx; y += dy; }
  std::string str() const { return "Point"; }

  QL_OBJECT(
      QL_PROPERTY(x),
      QL_PROPERTY(y),
      QL_PROPERTY(dist),
      QL_INVOKABLE(translate),
      QL_STR(str),
      QL_CTOR([](double x_, double y_) {          // typed form: preferred
        return std::make_shared<Point>(x_, y_);
      }));
};
```

Host side: `ctx.import<Point>();` then hand objects over with
`qval_external_owned<Point>(desc, shared)` / `qval_external_borrowed<Point>(desc,
weak)`. The typed helpers record `typeid(T)` + a `dynamic_cast` check, so a wrong
pointer is a clean `type error: external type mismatch`, not UB. The untyped
`qval_external(...)` carries no check — avoid it unless the type is unnameable.

Conventions the tree follows: order the lines as **properties → methods →
enums → signals → `QL_STR` → `QL_CTOR`**; keep `QL_OBJECT` last in the class
body; it needs the trailing `;` (it ends in an `inline static` member).

## 4. Macro reference

| macro | C++ it expects | script-visible result |
|---|---|---|
| `QL_OBJECT(...)` | the whole list, inside the class | generates `static const qlang::TypeMeta& qlang_meta()` + self-registration into `qlang::metaRegistry()` |
| `QL_PROPERTY(x)` — data member | `T x;` | read-write property (kind 0) |
| `QL_PROPERTY(g)` — **const** getter | `R g() const` | read-only property (kind 1) |
| `QL_PROPERTY(name, READ g)` | `R g() const` | computed read-only property |
| `QL_PROPERTY(name, READ g, WRITE s)` | `void s(M)` — **void** return | computed read-write; `getTag`/`setTag` may differ |
| `QL_INVOKABLE(m)` / `QL_SLOTS(m)` | any `R m(A...)`, const or not | method (kind 2); arity is checked, args convert via `as<A>()` |
| `QL_ENUM(E, a, b, …)` | bare enumerator names, **max 16** | constants, addressed **flat** as `Type.a` (the table name `E` is metadata only); one line per group |
| `QL_FLAGS(F, …)` | same, bitmask values | same lookup; values are ints, script combines with `\|` |
| `QL_SIGNAL(s)` | `void s(A...);` **declared, never defined** | a signal: `emit obj.s(…)`, `connect(obj, "s", fn)`; host emits with `ctx.emitSignal(obj, "s", args)` |
| `QL_STR(f)` | `std::string f() const` | `str(p)`, `print(p)`, `f"{p}"`; without it those are type errors |
| `QL_CTOR(expr)` | typed lambda / function pointer returning `std::shared_ptr<T>`, **or** `(QLangContext&, const std::vector<QValue>&)` | `let p = Point(1, 2)` |

Notes that bite:

- **`QL_CTOR` typed form is the default choice**: the generated wrapper checks
  arity, applies the method-side conversions, and records `ctorArgTags` so the
  manifest can check `Point(...)` at compile time. The context-taking form's
  signature is invisible to the meta (presence-only) and **it must validate
  `a.size()` itself** — a short call is an out-of-bounds read, not an error.
  Generic (`auto`) lambdas are rejected by a `static_assert`.
- `QL_PROPERTY` on a **non-const** getter matches no overload: compile error at
  that line. Bind it with `QL_INVOKABLE`, make it `const`, or give `READ`.
- A member type with no `qval_of()` / `as<T>()` coverage is a **compile error on
  the `QL_OBJECT` line**, never a runtime surprise. Supported: `bool`, `char`,
  `int8/16/32/64_t`, `uint8/16/32/64_t`, `float`, `double`, `std::string`,
  `const char*` (`void` appears only as a method return tag). Anything pointer-
  or class-shaped is tagged `External`
  (0xFE, "not portable"); anything else is `Unknown` (0x00) and the manifest
  stays silent about it — that is legal, not an error.
- `READ` / `WRITE` are the only un-prefixed macros in the header.
- Enum constants resolve by a flat name scan over the type's tables
  (`src/QLangObjects.cpp`, `typeref_ops::getField`) and come back as a 32-bit
  `Int`: keep enumerator names unique across one type's tables and their values
  inside `int32` if scripts must read them.
- The script-visible name is `#name` verbatim: **no renaming**, and no method
  overloading (one binding per name, duplicates refused at `import<T>()`).

## 5. Procedure

1. Confirm the type is at **namespace scope** — a local (function/block-scope)
   class cannot hold the `inline static` registrar.
2. Derive from `qlang::QLObject<T>`. This adds a virtual destructor, i.e. a
   vptr: if the type is memcpy'd, shared across an FFI boundary, or assumed
   standard-layout anywhere, stop and flag it to the user before adding the base.
3. Make each member you intend to expose `public` (private members *do* bind —
   the macro expands inside the class — but a private field becoming script-
   writable is a design decision, not a freebie).
4. Add the `QL_OBJECT(...)` block, one line per binding, ordered as in §3.
   For construction prefer a typed `QL_CTOR`; `QL_CTOR(&Point::make)` against a
   `static std::shared_ptr<Point> make(...)` is the tidy form.
5. Check the name the script will get: it is the **innermost** identifier
   before `::qlang_meta`, so `ns::Outer::Inner` registers as `Inner`. Two types
   that reduce to the same name make `writeTypeManifest` refuse the whole dump.
6. Verify (§6). Then, if the type is part of a shipped contract, make the
   manifest an artifact of the target that ships it:

   ```cmake
   add_custom_command(TARGET myapp POST_BUILD
     COMMAND $<TARGET_FILE:myapp> --dump-qlm ${CMAKE_BINARY_DIR}/host.qlm)
   ```

   (`qlang_run --qlmeta-dump <file.qlm>` is that flag pre-installed.)

## 6. Verification loop (all of it, before claiming done)

```sh
cmake -B build && cmake --build build
ctest --test-dir build                       # 28 entries; nothing may regress
./build/qlang_run --qlmeta-dump /tmp/opencode/host.qlm   # expect your type's count
clang-format --dry-run -Werror <file>        # touched lines only, see below
```

- Formatting: Google base per `.clang-format` (2-space, 80 col). The tree is
  **not** format-clean; never run `clang-format -i` over a whole file, format
  the ranges you edited (`clang-format -i --lines=…`) so the diff stays reviewable.
- A binding with no check is an unproven binding. Extend the nearest suite
  (`tests/unit/test_external.cpp` for behaviour, `manifest_check.cpp` for the
  manifest shape) with the smallest `CHECK` that fails if the line is wrong.
  New suite = new `tests/unit/*.cpp` **plus** `add_test` in `CMakeLists.txt`.
- The reach rule: a `QL_OBJECT` registrar is emitted only in a TU that
  **references** the class. A type that lives in a static library and is never
  referenced by the dumping binary is simply absent from the dump — that is the
  usual cause of "I added it and it did not appear". To dump a deliberate
  subset, or to break a name clash on purpose, build the list directly:
  `writeManifest(os, {manifestTypeFrom(Point::qlang_meta())})`.

## 7. Known gaps — do not "fix" them in this task

- No `__destroy__`/teardown hook on external types (v1 view model).
- No enum-name reverse lookup: an enum-valued property reads back as a bare int.
- No inheritance: `ObjectOps::base` is reserved.
- `hasMethod` accepts a table field holding a `BoundMethod` and `callMethod`
  then rejects it — open defect, tracked in `docs/EXTERNAL_TYPES.md` §9/§10.1.
- The manifest carries *shape*, not C++ identity: same name + same members from
  a different build passes the handshake; only the typed handoff's pointer check
  catches that. Do not invent a fingerprint here.

## 8. Out of scope

Do not add new `QL_` macros, do not hand-write `TypeMeta`/`Member` structs, do
not call `registerType()` with a synthesized meta (use `import<T>()`), do not
register the same type twice under different names, and do not touch the VM or
compiler to "make a binding work" — a binding that cannot be expressed with the
macros in §4 is a question for the user, not a patch.

## 9. Acceptance checklist

- [ ] type at namespace scope, derives from `qlang::QLObject<T>`
- [ ] `QL_OBJECT(...)` last in the body, trailing `;`, lines ordered per §3
- [ ] every exposed member's type is inside the `qval_of`/`as<T>` set (or an
      intentional `External`/`Unknown`)
- [ ] typed `QL_CTOR` unless the context is genuinely needed
- [ ] no non-const getter in `QL_PROPERTY`, no duplicate script names, ≤16
      enumerators per `QL_ENUM`/`QL_FLAGS`
- [ ] `QL_SIGNAL` functions declared and never defined
- [ ] build clean, `ctest` green, one new/extended `CHECK` proves the binding
- [ ] touched ranges only, clang-format clean; no whole-file reformat
- [ ] type name does not collide with another registered type's innermost name
