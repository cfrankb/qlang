## Class Implementation Details

### Syntax
```
class Name {
  field = value;
  method(self, params)[] { body }
}
```

### Compilation
- Class body compiled in fresh context; prototype function creates table and populates fields/methods
- Constructor call: `TABCREATE` instance → `CALLFN` prototype → `TABGET`/`TABPUT` copy methods → `CALLFN` `__init__(self, args...)`
- Member calls (`obj.method(args)`): prepend `self` as first arg for known class methods; plain closures in tables called without self

### Verified
- `class Money { total = 0; __init__(self, amount)[] { self.total = amount; }; receive(self, amount)[] { self.total += amount; }; balance(self)[] { return self.total; } }`
- `local money = Money(125); money.receive(25); println(money.balance());` → outputs `150`
- Closure in table still works: `local events = { greet = (who)[] { return "hi " + who } }; events.greet("world")` → `"hi world"`

## user defined classes — spec conformance (c83ebc8)

The spec section was expanded from the original Money prototype example into a
full specification. **Status: fully conforming (2026-10-26).** All 9
implementation phases from `class_action_plan.md` are DONE (`6a4c559` unique
method/dunder names, `8c013a1` fixed arity, `b242645` `__str__`, `0d7115b`
refcounting + `__destroy__`; phases 0–2 earlier). The `ObjectOps` dispatch
layer (Phase 0, `docs/EXTERNAL_TYPES.md` §9) landed first as required;
instances are tables carrying an object-ops descriptor
(`__fields__`/`__const_fields__` in `QLangObjects.cpp`) — no separate `Object`
value type (documented deviation, Phase 0.2).

| Spec requirement | Status |
|---|---|
| `class Name { ... }` in global scope | ✓ implemented (`parseClass`) |
| field declarations → instance defaults | ✓ implemented |
| `__init__(self, ...)` at construction | ✓ implemented |
| `self` implicit in method calls | ✓ implemented |
| **closed field set** — undeclared `obj.f` read/write is a runtime error | ✓ implemented (tests L454–L456); `field 'x' not found on type 'T'` |
| **`const` fields** — read-only; `x.f = 1` → runtime (Type) error | ✓ implemented (tests L463–L470); `cannot assign to const field 'x'` |
| **unique method/dunder names** — compile error | ✓ implemented (`6a4c559`, tests L475–L477); `duplicate method 'm' defined at line X and line Y` |
| **fixed arity** — `ClassName(args)` vs `__init__` signature → error; class without `__init__` → zero args only | ✓ implemented (`8c013a1`, tests L495–L497) |
| **`__str__`** — `str(obj)`/f-string whole-object; class default = class name | ✓ implemented (`b242645`, tests L577–L583) |
| **reference counting** — share, never copy | ✓ implemented (`0d7115b`, tests L589–L592); `std::shared_ptr` descriptor, assignment shares |
| **`__destroy__`** — runs exactly once at final release | ✓ implemented (`0d7115b`, tests L597–L613) |
| **`type(x)`** — returns the class name | ✓ implemented (tests L539, L585) |
| instances are not tables — the `$` dynamic-key pipeline does not apply | ✓ implemented (tests L620–L625) — **deviation:** `$` on an instance is a **runtime** type error, not a compile error (instances are typed `Table` at compile time, so the compiler cannot distinguish them; Phase 9.2 in `class_action_plan.md`) |

### `static` class members + `::` (new extension beyond v1 — implemented Round 12, 2026-10-27)

Spec additions (2026-10-27, uncommitted): `static` reserved word, `::` scope-resolution
operator, `## static` section, and the Money example extended with `static banner`,
`static getBanner()`/`setBanner()`, `Money::setBanner(...)`, and `__str__`
interpolating `{banner}`. **Status: fully conforming** (17 cases in
`examples/class_static_test.cpp`, ctest `class_static`). No new opcodes.

| Spec requirement | Status |
|---|---|
| `static` is a reserved word (`local static = 1` → parse error) | ✓ `TokenType::Static` in `keywordType()` |
| `static banner = ...` static field (class-bound) | ✓ stored in shared `__statics__` table |
| `ClassName::field` read via the class name | ✓ two-hop TABGET (`__proto_<Name>` → `__statics__` → field) |
| `ClassName::method()` call via the class name | ✓ TABGET closure + pass `__statics__` as hidden anchor |
| static field read **through an instance** (`money.banner`) | ✓ `table_ops::getField` `__statics__` fallback |
| instance write **mutates class-level storage** (`money.banner = "Turtle"` → `Money::banner` reflects it) | ✓ `table_ops::setField` writes the shared map (by reference) |
| class write via instance **visible across instances** | ✓ every instance shares one `__statics__` map |
| `::` works inside f-string interpolation (`f"{Money::banner}"`) | ✓ (format-spec splitter skips `::`) |
| `static` + `const` allowed; `static` + `local`/`let` forbidden | ✓ const → `__static_const__` set (write → runtime error); `local`/`let` → parse error |
| `__str__` referencing a static (`f"balance: {self.total} at {banner}"`) | ✓ bare static name resolves via `self.__statics__` in method bodies |
| static method arity enforced (`Money::getBanner(1)` → compile error) | ✓ `staticMethodArities` (declared user params, no anchor) |
| `C::missing` → compile error | ✓ `'C' has no static member 'missing'` |
| instance closed-field error intact (statics not in the field schema) | ✓ statics excluded from `__fields__`; `money.notAField` still errors |

**Deviations:** the plan's "hidden capture" was replaced by a **hidden anchor
parameter** for static methods (a capture by frame-register reference dangles
once the prototype frame is popped). Instance-call-of-a-static-method
(`money.getBanner()`) is not routed to pass the anchor (compiler `obj.method()`
emits TABGET+CALLCLOS, not the VM `callMethod`); spec is silent, not in the
required test set.

