# Class `static` + `__str__` implementation plan

Spec source: SPEC.md (uncommitted 2026-10-27 waves 1–3) — `static` reserved word,
`::` scope-resolution operator, `## static` section (new extension beyond v1),
Money example. Status: **not implemented** (SPEC_COMPLIANCE.md, Round 11).

Two work items, one test phase:

1. **Fix `__str__`** — `print`/`println`/`printf %s`/f-string/`join` bypass the
   `__str__` dispatch that `str()` already has.
2. **Implement class `static`** — static fields, static methods, `::` access,
   instance-level static access, `static const`.
3. **New test suites** — `class_str_test` + `class_static_test` (ctest-registered).

Guiding constraint: **no new opcodes.** Everything reuses TABGET/TABPUT/TABCREATE/
CALLCLOS and the existing closure-capture machinery.

---

## Part 1 — Fix `__str__`

### Root cause (verified 2026-10-27)

`str()` is correct: `Builtin::ToStr` (`src/QLangVM.cpp:232-247`) checks `__str__`
on tables, calls it via `vm->callClosure`, and falls back to the `__class__`
name for instances without `__str__`.

Every other display path calls the context-free `valueToString()`
(`src/QLangVM.cpp:39-65`), which hard-codes `Table → "<table>"` (line 58):

| Site | Line | Behavior today |
|---|---|---|
| `Builtin::Print` | 282 | `<table>` |
| `Builtin::Println` | 286 | `<table>` |
| `Builtin::Printf` `%s` | 318 | `<table>` |
| `Builtin::Format` (f-string) fallback | 471 | `<table>` |
| `Builtin::Join` element stringify | 555-567 | `<table>` |

Probe results: `str(money)` → `balance: 125` ✓; `println(money)` → `<table>` ✗;
`f"{money}"` → `<table>` ✗; class without `__str__` → `<table>` ✗ (spec: class
name).

Also dead: `table_ops::toStr` (`src/QLangObjects.cpp:173-177`) — the
dispatch-layer TOSTRING stub always returns `"<table>"`.

### Changes

1. **`src/QLangVM.h`** — add a VM member (needs `this` for `callClosure`):
   ```cpp
   std::string displayString(const QLangValue &v);
   ```
2. **`src/QLangVM.cpp`** — implement `QLangVM::displayString(v)`:
   - `v` is a Table with a `Function` value under `__str__` →
     `QLangValue r = callClosure(strFn, {v});` return `r.asString()` if String,
     else `valueToString(r)`.
   - Table with `__class__` string → return the class name.
   - Everything else → existing `valueToString(v)`.
   - **Re-entrancy guard:** `bool m_inDisplayString = false;` on `QLangVM` —
     if already set, skip the `__str__` call (fall back to class name /
     `<table>`) so an `__str__` that prints `self` terminates.
3. Replace the five `valueToString(...)` call sites above with
   `vm->displayString(...)` (all inside `callBuiltin`, where `vm` is in scope;
   `ToStr` can keep its inline logic or delegate to the same helper).
4. **`src/QLangObjects.cpp`** — make `table_ops::toStr` do the same dispatch
   via `ctx.callClosure` (it already takes `QLangContext&`), keeping the
   EXTERNAL_TYPES.md §9 dispatch layer honest.

### Verification

`class_str_test` (Part 3) green + full ctest green. The 42 existing
class-conformance cases (spec_check2 L449–L615) must stay green — several
assert `str()` behavior that must not change.

Suggested commit: `fix(class): __str__ dispatch in print/println/printf/f-string/join`

---

## Part 2 — Class `static`

### Design (no new opcodes)

Static storage is a **`__statics__` table embedded in the prototype**.
`QLangValue::Table` is a `shared_ptr<map>`; instances already copy table-valued
entries from the prototype by reference (construction: `src/QLangCompiler.cpp:1762-1786`),
so the prototype and every instance point at the **same** `__statics__` map —
instance writes mutate class-level state for free. No VM opcodes, no new value
type.

```
prototype table:  { total: 0, receive: fn, __fields__: {...}, __statics__: &shared{banner: "GreatAccount", getBanner: fn, setBanner: fn, __static_const__: {...}} }
instance:         { total: 125, receive: fn, __fields__: {...}, __statics__: &shared (same map) }
```

### 2.1 Lexer — `src/QLangLexer.cpp`, `include/qlang/QLangLexer.h`

- `keywordType()`: `if (text == "static") return TokenType::Static;`
- New `TokenType::Static` + `TokenType::ScopeRes` in the enum.
- `operatorToken()` two-char section (line ~346):
  `if (c == ':' && n == ':') return make(TokenType::ScopeRes, "::", 2);`
  (before the single-char `:` case at line 385).

### 2.2 Parser — `src/QLangParser.cpp`, `include/qlang/QLangParser.h`

**`Stmt`** gains (QLangParser.h):
```cpp
bool isStatic = false;                 // on Assignment/Const/Arrow in class bodies
std::unordered_set<std::string> staticFields;   // on Class stmt
std::unordered_set<std::string> staticMethods;  // on Class stmt
```
**`Expr`** (`ExprKind::Member`): add `bool scopeStatic = false;`

**`parseClass()`** (line 743): at the top of the body loop,
`bool isStatic = match(TokenType::Static);`
- `isStatic` + `local`/`let` → **compile error** `static cannot be combined with local or let`.
- `isStatic` + `const name = value` → Const stmt, `isStatic = true`,
  `stmt->staticFields.insert(name)`, `stmt->constFields.insert(name)`.
- `isStatic` + `name = value` → Assignment stmt, `isStatic = true`,
  `stmt->staticFields.insert(name)`.
- `isStatic` + `name(params)[] { }` (Arrow) → `isStatic = true`,
  `stmt->staticMethods.insert(name)`; duplicate-name check shared with the
  existing `methodLines` map (a static and an instance member with the same
  name is a compile error).
- `isStatic` + anything else → parse error.

**`ClassName::name` expression:** in the member-access chain (where `Dot` is
handled for `ExprKind::Member`), accept `ScopeRes` the same way:
base `Ident` + `::` + `Ident` → `ExprKind::Member` with `scopeStatic = true`.
This makes `Money::banner` (read), `f"{Money::banner}"` (f-string expr part),
and `Money::getBanner()` (via the existing `calleeMember` call path,
`src/QLangCompiler.cpp:1706-1754`) all work with no call-site changes.

### 2.3 Compiler — `src/QLangCompiler.cpp`, `include/qlang/QLangCompiler.h`

**`ClassRecord`** (QLangCompiler.h:148) gains:
```cpp
bool hasStatics = false;
std::unordered_set<std::string> staticFields;
std::unordered_set<std::string> staticConstFields;
std::unordered_map<std::string, int> staticMethodFuncIndices;
std::unordered_map<std::string, int> staticMethodArities; // params, NO self
```

**Prototype build** (class compile, `src/QLangCompiler.cpp:~1316-1435`):
- Static fields/methods are **not** added to `fieldNames`/`methodNames` (they
  stay out of the instance's `__fields__` schema and out of the per-instance
  method copies).
- After the field/method loop: if any statics exist,
  `TABCREATE staticsReg`; for each static field emit
  `CONST key / compileExpr(value) / TABPUT(staticsReg, key, val)`; for each
  static method emit `MAKEFUNC` + `TABPUT(staticsReg, key, fn)`.
  Then `TABPUT(protoReg, "__statics__", staticsReg)` — emitted **before**
  `__fields__` (same ordering constraint as `__const_fields__`, line 1375-1393:
  metadata goes in while the prototype is not yet a "class instance").
- `static const` names also go into a `__static_const__` set table inside
  `__statics__` (mirrors `__const_fields__`).
- **Hidden capture for method bodies:** declare `__statics__` as a local in
  the class-body compile context (register holding the statics table, in
  `m_regs`/`m_captureRegs`). When compiling **every** method of a class with
  statics (instance methods and static methods alike), append `"__statics__"`
  to its capture list — mirroring the existing global-capture pattern
  (line 1481-1494). `MAKEFUNC` then binds it.
- **Static-name resolution inside method bodies:** while compiling a method of
  class C, a bare identifier in C's `staticFields`/`staticMethods` compiles to
  `TABGET(captureReg, name)` (read) / `TABPUT(captureReg, name, val)`
  (assignment). This is what makes `__str__`'s `f"...{banner}..."` and
  `setBanner`'s `banner = newBanner` work. Track the current class's static
  name set on the compiler during method compilation.

**`ClassName::name` codegen** (`ExprKind::Member` with `scopeStatic`,
line 2019-2026):
```
compileExpr(base → baseR)                     // class name -> prototype table
TABGET tmp, baseR, "__statics__"
TABGET dest, tmp, name
```
Compile-time checks: base must be a known class (`m_classes`); name must be in
`staticFields` ∪ `staticMethods`, else **compile error**
`'C' has no static member 'x'`.

**Static method calls** (`Money::getBanner()`): the existing `calleeMember`
path compiles the `::` expression into `funcReg` (the two-hop TABGET above)
and CALLCLOSes it — **without** `self` (plain-closure convention, line 1750-1752).
Arity check: expected = `staticMethodArities[name]` (no `self` offset);
`Money::getBanner(1)` → compile error.

### 2.4 VM — `src/QLangObjects.cpp`

New key constant: `kStaticMetaKey = "__statics__"`.

- **`getField`** (line 71): after the direct map miss, **before** the
  closed-field-set error: if the instance has a `__statics__` table containing
  `name` → return it. Set an out-param `bool &fromStatics`.
- **`setField`** (line 99): if `name` is not a declared field but is in
  `__statics__` → write into the statics map. Reject if the name is in the
  `__static_const__` set: `cannot assign to const static field 'x'`.
  (No `__field_types__` constraint applies to statics — lazy: type-checking
  static writes is a v2 nicety; noted as a ceiling.)
- **`callMethod`** (line 146): use the `fromStatics` flag — methods resolved
  from `__statics__` are called **without** prepending `self` (static
  convention); instance methods keep `self`.

### 2.5 Spec semantics → mechanism map

| Spec behavior | Mechanism |
|---|---|
| `Money::banner` | `::` Member → two-hop TABGET via `__statics__` |
| `money.banner` | VM `getField` statics fallback |
| `money.banner = "Turtle"` | VM `setField` statics fallback (shared map) |
| `Money::banner` after instance write | same shared map — reads "Turtle" |
| `Money::setBanner("X")` / `Money::getBanner()` | `::` call path, no self, arity-checked |
| `f"{Money::banner}"`, `f"{money.banner}"` | f-string expr parts are normal expressions |
| `{banner}` inside `__str__`/methods | hidden `__statics__` capture + TABGET |
| `static const x = 5` | `__static_const__` set; write → runtime error |
| `static local x` / `static let x` | parse error |
| `local static = 1` | `static` is a keyword → parse error |

### Verification

`class_static_test` (Part 3) + full ctest. Existing 42 class cases unchanged.

Suggested commits:
- `feat(lexer,parser): static keyword, :: scope-resolution token, static class members`
- `feat(class): static fields/methods via shared __statics__ table (no new opcodes)`

---

## Part 3 — Test suites

Both follow the `examples/range_check.cpp` pattern (self-contained main,
`N passed, M failed`, exit code) and are registered in `CMakeLists.txt`
(exactly like lines 55-57).

### `examples/class_str_test.cpp` → ctest `class_str` (~10 cases)

Assert via value-level paths (sprintf/f-string/str) to avoid stdout scraping;
one stdout-capture case for `println` itself.

1. `str(m)` with `__str__` → `"balance: 125"` (regression guard)
2. `sprintf("%s", m)` → `__str__` output (printf path)
3. `f"obj={m}"` → `__str__` output (f-string path)
4. `join([m, m2], ", ")` → both `__str__` outputs (join path)
5. `println(m)` stdout capture → `__str__` output + newline
6. `print(m)` stdout capture → no trailing newline
7. class **without** `__str__` → class name (`"Plain"`) via str/println/f-string
8. plain table → `"<table>"` (unchanged)
9. `__str__` returning a non-string (e.g. `return 42`) → `"42"`
10. recursive `__str__` (`return str(self)`) → terminates, falls back to class name

### `examples/class_static_test.cpp` → ctest `class_static` (~16 cases)

1. `static` field read via class: `Money::banner` → `"GreatAccount"`
2. `static` field read via instance: `money.banner` → `"GreatAccount"`
3. instance write mutates class: `money.banner = "Turtle"` then `Money::banner` → `"Turtle"`
4. class write via instance visible across instances: new `money2.banner` → `"Turtle"`
5. static method via class: `Money::setBanner("X")` then `Money::getBanner()` → `"X"`
6. static method arity: `Money::getBanner(1)` → compile error
7. f-string static via class: `f"{Money::banner}"`
8. f-string static via instance: `f"{money.banner}"`
9. `__str__` referencing a static (`f"balance: {self.total} at {banner}"`) → contains banner
10. `static const` read via class + instance
11. `static const` write via instance → runtime error `cannot assign to const static field`
12. `static local x = 1` in class body → compile error
13. `static let x = 1` in class body → compile error
14. `local static = 1` at top level → compile error (reserved word)
15. `C::missing` on a class with statics → compile error `no static member`
16. instance closed-field error intact: `money.notAField` → runtime error (statics must not leak into the instance schema)

### CMakeLists.txt

```cmake
add_executable(class_str_test examples/class_str_test.cpp)
target_link_libraries(class_str_test PRIVATE qlang)
add_test(NAME class_str COMMAND class_str_test)

add_executable(class_static_test examples/class_static_test.cpp)
target_link_libraries(class_static_test PRIVATE qlang)
add_test(NAME class_static COMMAND class_static_test)
```

---

## Phasing

| Phase | Scope | Done when |
|---|---|---|
| 1 | `__str__` fix (VM + table_ops) + `class_str_test` | `class_str` green, ctest green, 42 class cases green |
| 2 | Lexer + parser (`static`, `::`, parseClass, `::` expr) | compiles clean; `local static = 1` errors; `Money::x` parses |
| 3 | Compiler (`__statics__` table, ClassRecord, hidden capture, `::` codegen, static const, compile errors) | static fields/methods + `::` work at value level |
| 4 | VM statics dispatch (getField/setField/callMethod) + `class_static_test` | `class_static` green, full ctest green |
| 5 | SPEC_COMPLIANCE.md: re-run suites, flip `static`/`::` Open Items to implemented, new hash + Round 12 entry | hash matches `sha256sum SPEC.md` |

Build & test per AGENTS.md after every phase:
`cmake -B build && cmake --build build && ctest --test-dir build`

## Risks / known ceilings

- **Re-entrancy:** `__str__` calling `str(self)` — guarded by
  `m_inDisplayString` (Part 1). Same guard covers `__str__` that prints `self`.
- **Static write type-checking:** not enforced (`__field_types__` covers
  instance fields only). `ponytail:` ceiling — add if a real type-safety need
  appears.
- **`__statics__` in `type()`/`len()`/iteration:** metadata keys are already
  invisible to those paths (same as `__fields__`); no change needed, but case
  16 guards it.
- **Instance call of a static method** (`money.getBanner()`): resolves via the
  statics fallback and is called **without self** (callMethod flag). Spec is
  silent; this is the consistent choice. If it should be an error instead, it
  is a one-line change in `callMethod`.
- **`ClassName::name` where base is not a class** (e.g. `x::y` for a variable)
  → compile error in the `::` codegen (base must be in `m_classes`).
