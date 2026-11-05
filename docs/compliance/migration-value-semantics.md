# Migration: `array` / `set` / `blob` now share (Round 43, 2026-11-05)

**Audience:** anyone with a `.ql` script or a C++ embedder written before
Round 43. **Spec:** [../spec/01-types.md](../spec/01-types.md)
`## value semantics`. **Plan:** `docs/plans/deepcopy-plan.md` Phase 2.

## What changed

`array`, `set` and `blob` moved from **copy-on-assign / copy-on-call** to
**share-on-assign / share-on-call**. In memory their payloads are now
reference-counted (`shared_ptr<vector<QLangValue>>` for array/set,
`shared_ptr<vector<uint8_t>>` for blob), so copying a value copies the handle:

```ql
local a = [1, 2, 3]
local b = a          // b names the SAME array as a
b[0] = 99
println(a[0])        // 99  (before Round 43: 1)

local s = set(1, 2)
local t = s
t[] = 3
println(len(s))      // 3   (before: 2)
```

The same applies to call arguments: a function that mutates an array, set or
blob parameter mutates the caller's value — exactly as it always did for
`table` and class instances.

Unchanged:

- **`string` is the deliberate exception** — immutable, so it is copied and
  never shared (`local t = s; t = "x"` leaves `s` alone).
- **primitives** (char/byte/word/short/int/unsigned/dword/long/qword/float/
  bool) are copied.
- `table`, class instances, closures, generators and external references
  already shared; they still do.
- **literals are fresh.** Every evaluation of `[1,2,3]`, `{n=0}` or `set(1)`
  yields new storage, even when two identical literals fold to one constant
  pool entry, and containers nested inside a literal are fresh too.
- `a + b` (array concat), `slice()`, `range()` and `cast<…>(…)` return
  **fresh** payloads — a conversion is not an alias.

## How to get the old behaviour

Ask for it explicitly with `deepcopy()`:

```ql
local b = deepcopy(a)   // independent recursive copy (Phase 1)
```

`deepcopy` recurses through nested arrays/sets/tables/instances, shares
closures/generators/external handles, shares a class instance's meta keys and
methods, and preserves cycles (`a[] = a` now round-trips instead of looping
forever).

## Audit checklist for existing scripts

Look for these patterns; each one silently changed meaning:

| Pattern | Old | New | Fix if you needed the old |
|---|---|---|---|
| `local b = a` (array/set/blob) | independent copy | alias | `local b = deepcopy(a)` |
| `f(a)` where `f` mutates its array/set/blob parameter | caller unaffected | caller affected | pass `deepcopy(a)` |
| helper returning a container that the caller then mutates | fresh each call | shares whatever it returned | return `deepcopy(...)` |
| accumulating a container into several collections | each slot independent | every slot the same object | append `deepcopy(x)` |

Patterns that are **safe** (no change): literals (`local r = []` inside a loop
is still a new array every iteration), `a + b`, `slice`, `range`, `cast`, and
everything involving `string` or primitives.

## C++ embedders

`QLangValue::asArray()`, `asSet()` and `asBlob()` now return the payload
**handle**, so element access dereferences it:

```cpp
const auto &v = ctx.get("rows");     // bind the value, not a member of it
const auto &rows = *v.asArray();     // was: const auto &rows = v.asArray();
for (const auto &e : rows) { /* … */ }
```

Factories gained three forms per type — `Array(payload)` **adopts/shares** an
existing `shared_ptr`, `Array(std::vector<QLangValue>{…})` builds a fresh
payload, `Array()` builds a fresh empty one (`Blob()` likewise; `Blob({})`
would be ambiguous). `QValue`/`qval_vec` and the module helpers were updated in
tree; only code that touched `asArray()/asBlob()` directly needs editing.

## Bytecode / `.qlc`

**No format change and no compatibility layer.** The durable form stores
payloads, never handles ([../SERIALIZATION.md](../SERIALIZATION.md) **R5**), so
`.qlc` stays **v5** and files written before Round 43 still load and behave the
same after decoding (a reader always materialises fresh payloads).

The pointerization is a **source-language** break, not a bytecode break. Per
the spec's compatibility rule ("a `.qlc` is only loadable by the exact same
compiler version", [../spec/13-bytecodes.md](../spec/13-bytecodes.md)), the
plan's open decision #7 was resolved as a **clean break**: no version bump, no
compat flag, no copy-semantics shim for old images.

## Verified by

`tests/unit/value_semantics.cpp` (`value_semantics` ctest, 40 checks: sharing
on assign/call, literal freshness incl. the deduped-literal and table-literal
cases, `deepcopy` independence, array cycles, `a + b` / `cast` freshness,
`a += a`, `string` still a value type) and `tests/scripts/value_share.ql`
(`script_value_share` ctest).
