# C3 guard defect: `hasMethod` accepts a `BoundMethod` member that `callMethod` then rejects

**Date:** 2026-11-14
**Severity:** medium-high (host-facing API contract break: the guard does not
protect the call it guards; documented contract in `docs/EVENT_HANDLERS.md` §4
is false for one shape)
**Status:** **fixed — Option A implemented in working tree (uncommitted),
verified 2026-11-14** (`table_ops::callMethod` gained the `BoundMethod`
branch; `hasMethod` unchanged; guard test added to `test_external` §19).
Option B (the `status.md:354` prescription) was rejected for the reasons
below; that record is amended with the fix commit.
**Cited code at commit:** `5b15ba7`

## Symptom (host C++, `/tmp/opencode/defects/d3.cpp`)

```cpp
ctx.run("const t = { }\nt.cb = e.translate\n");   // e: external Point, t: table
QLangValue cb = ctx.get("t").asTable()->at("cb");
// cb.typeName() == "bound method"
assert(ctx.hasMethod(t, "cb") == true);           // guard says YES
ctx.callMethod(t, "cb", {3, 4});                  // → throws:
// type error at line 0,0: member 'cb' is not a function
```

While the same member called from script works: `t.cb(3, 4)` →
`TABGET → BoundMethod → CALLCLOS`, handled at `QLangVM.cpp:2437-2459`.
`e.translate` called *on the external object* also works both ways
(`external_ops::callMethod`, `QLangObjects.cpp:326+`). The hole is exactly:
**a table (or QLang-object) field holding a `BoundMethod`, called through the
host `callMethod`.**

## Recorded status

- `docs/plans/todo.md` §6 (lines 192-197): "C3 guard defect (small, unblocks
  'verified')" + "C3 tally drift".
- `docs/compliance/status.md`:269 and :354 — C3 marked "✓ implemented …
  **not yet verified**" solely because of this defect; :354 prescribes a fix
  ("per-kind callable test — table/object → `Function` only, external →
  `findMember` with `kind == 2`").
- `docs/EVENT_HANDLERS.md` §4 (lines 145-151) documents the caveat and says
  the §6 "miss is a silent no-op" contract currently **fails** for this one
  shape — a handler-table field bound to another object's method passes the
  guard and then throws.
- `docs/plans/missing-features-plan.md` §C3 "New open detail": "The guard and
  the dispatch must agree before this item can be flipped to verified."

## Root cause

Two independent callable-ness definitions:

- `QLangContext::hasMethod` (`src/QLangContext.cpp:1323-1343`): resolves
  `ops->getField` and returns
  `type == Function || type == BoundMethod` (`:1341-1342`) — **kind-blind**,
  same rule for tables, class instances and externals.
- `table_ops::callMethod` (`src/QLangObjects.cpp:183-190`): after `getField`,
  `if (method.type() != Type::Function) throw "member '<name>' is not a
  function"` — **Function-only**. (VM never reaches this — per
  `QLangObjects.cpp:328`, `ops->callMethod` is host-only in v1, Q19.)

`BoundMethod` values legitimately land in plain table fields: reading an
external method (`external_ops::getField`, `QLangObjects.cpp:285-291`)
produces one, and script stores it like any value. The VM treats a
`BoundMethod` callee as callable everywhere it appears
(`CALLCLOS` `:2439`, signal `callHandler` `:1362-1366`, `callSlotHandler`
`:1407`) — only `QLangVM::callClosure` (`:1161-1162`) still rejects it, and
the host `callMethod` does not route through either; it routes through
`ObjectOps::callMethod`, where the table branch is Function-only.

## Two fix options (decision required)

**Option A (recommended): teach `table_ops::callMethod` to invoke
`BoundMethod`.** Mirror the VM's `CALLCLOS` handler in
`src/QLangObjects.cpp:183-191`:

```cpp
if (method.type() == QLangValue::Type::BoundMethod) {
  const auto &bm = method.asBoundMethod();
  ExternalRef target = bm.target;          // copy: selfMut() is non-const
  std::vector<QLangValue> callArgs = args; // NO self prepended: invoke's
                                           // target is the external object
  try {
    return bm.member->invoke(target.selfMut(), ctx, callArgs);
  } catch (const QLangError &) { throw; }
  catch (const std::exception &e) {
    throw QLangError(QLangErrorKind::Runtime, 0, 0, e.what());
  }
}
```

`hasMethod` stays as-is. ~12 lines, one site.
- Pros: guard and dispatch agree; matches the VM's semantics (`t.cb(3,4)`
  works, so the host API should too) and EVENT_HANDLERS §4's sentence "the
  former asks whether the member resolves to a `Function` **or
  `BoundMethod`**, the latter **invokes it**" becomes literally true;
  zero contract wording changes needed beyond deleting the caveat.
- Cons: diverges from the fix prescribed in `status.md:354` (that text must
  then be amended, not just flipped).

**Option B (status.md:354's prescription): tighten `hasMethod` per kind** —
table/object → `Function` only; external → `findMember` `kind == 2`.
- Pros: `hasMethod` "says no" exactly when `callMethod` would throw — guard
  honest; `callMethod` behavior unchanged (nothing newly succeeds).
- Cons: needs kind routing inside `hasMethod` (an `ops` identity test against
  the external ops table, or an `ObjectOps` hook) — bigger diff; and it
  makes the **host API disagree with the VM**: `ctx.callMethod(t, "cb")`
  refuses what `t.cb(3,4)` executes — reintroducing the contract wrinkle
  §4 documents away; and `EVENT_HANDLERS.md` §4's "Function or BoundMethod …
  the latter invokes it" sentence would have to be rewritten per-kind.

Recommendation: **A**. The script path already defines "callable member" as
Function-or-BoundMethod uniformly; the outlier today is the table ops, not
`hasMethod`. If B is chosen instead, the §4 wording and this report must be
re-written to match.

## Test guard (either option)

`tests/unit/test_external.cpp` — extend §19 (the `hasMethod`/`callMethod`
section, ~`:277-313`) with the d3 shape:

```cpp
// §19 extension: a table field holding a BoundMethod is callable
ctx.run("t = { }\nt.cb = e.translate\n");
QValue t = ctx.get("t");
CHECK(ctx.hasMethod(t, "cb"));
ctx.callMethod(t, "cb", {QValue::Int(3), QValue::Int(4)});
CHECK(ctx.get("e"). /* or read back */ e.x == 3 && e.y == 4);  // target mutated
```

(Exact readback idiom per the file's existing §18 `translate` checks; the
Point fixture and `QL_INVOKABLE(translate)` are already there, `:31`/`:40`.)
Under Option B this pair inverts to `CHECK(!ctx.hasMethod(t, "cb"))` +
callMethod throws `no method`, plus an external-object pair
(`hasMethod(ext, "translate")` still true) proving the per-kind split.

## Verification + doc-sync steps (the lazy fix is not done until these are)

1. `cmake -B build && cmake --build build && ctest --test-dir build` green.
2. Re-record the `external` tally **from measurement, do not copy the stale
   claims**: the recorded **49** (`docs/compliance/tests.md:76`,
   `docs/compliance/harness.md:16`, the `status.md:12` tally pinned "until C3
   lands") predates `c4ee131`; `docs/plans/todo.md:196-197`'s "HEAD has 66"
   is itself stale — the docs' counting convention (raw
   `grep -c "CHECK(" tests/unit/test_external.cpp`, per the status.md
   66→81 chain) measures **81** at HEAD. Add the new §19 checks, re-grep,
   update all four tally records + `status.md` :269/:354 (flip C3 to
   verified) and the `tests.md:90` counted-total row.
3. Contract wording: delete the §4 guard-caveat paragraph
   (`docs/EVENT_HANDLERS.md:145-151`); close the "New open detail" in
   `docs/plans/missing-features-plan.md` §C3; amend the `status.md:354`
   "Fix (own commit)" sentence if Option A is taken; flip the three
   `docs/plans/todo.md` §6 rows (guard defect, tally drift); run
   `./update-doc-hashes.sh` for the touched spec/compliance rows.
4. Move this file to `docs/bugs/resolved/`.

## Suggested commits

- `fix(api): invoke BoundMethod members in table_ops::callMethod` (Option A)
  — or — `fix(api): make hasMethod's callable test per object kind` (Option B)
- `docs(compliance): re-record external tally, flip C3 to verified`

## Repro assets

`/tmp/opencode/defects/d3.cpp` — built with
`g++ -std=c++20 -Iinclude d3.cpp build/libqlang.a -o d3`; prints
`hasMethod(t, cb) = true`, then `callMethod THREW: … not a function`, then
confirms the script-side `t.cb(3, 4)` call works. Move it into
`tests/unit/test_external.cpp` §19 as part of the fix; the standalone binary
is throwaway.
