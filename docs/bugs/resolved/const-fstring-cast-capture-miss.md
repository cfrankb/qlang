# Top-level `const` read through an f-string or `cast` inside a function body evaluates to nil/0

**Date:** 2026-11-14
**Severity:** high (silent wrong value — no error, `nil`/`0` where a const is read)
**Status:** open — fix implemented in working tree (uncommitted), verified 2026-11-14
**Cited code at commit:** `5b15ba7` (`QLangCompiler.cpp` line numbers at that commit)

## Symptom

```ql
const G = "hello"
def plain(n)  return G        end   // plain read:   hello  ✓
def interp(n) return f"{G}"   end   // inside f-string: nil  ✗
println("plain ["  + plain(1)  + "]")   // plain [hello]
println("interp [" + interp(1) + "]")   // interp [nil]   ← BUG
println("direct [" + f"{G}"    + "]")   // direct [hello] (top level: fine)
```

And the cast sibling (`/tmp/opencode/defects/d2b.ql`):

```ql
const N = 7
def cast_read(n) return cast<float>(N) end   // → 0   ✗
def fstr_read(n) return f"{N}!"        end   // → "nil!" ✗
```

At HEAD: `interp [nil]`, `cast [0]`, `fstr [nil!]` while the plain read and the
top-level f-string print correctly. With the fix: `interp [hello]`,
`cast [7]`, `fstr [7!]`. Same family as the module-const capture work (F1,
Round 70) — that fix widened *which* consts get capture cells; this bug is
*which expressions* trigger the capture scan at all.

## Root cause

Capture analysis for script `def`s happens in `buildFunction`
(`src/QLangCompiler.cpp:1877`): `updatedCaptures` (`:1987`) starts empty
(the AST `captures` field is unpopulated for script defs) and is built by a
free-variable scan, `collectIds`/`collectStmtIds`
(`:1798-1840` / `:1842+`). Consts found by the scan are captured at
`:2014-2016` (`if (m_constVars.count(id))` → `updatedCaptures.push_back`).

`collectIds` recurses through `Variable, Binary, Unary, Call, Index, Member,
Array, Table, Closure, Ternary` — **but has no case for `ExprKind::FString`
(field `exprParts`, `include/qlang/QLangParser.h:92`) or `ExprKind::Cast`
(field `castOperand`, `:97`)**. Both fall through to the end of the function
silently, so a const referenced only inside an interpolated f-string or a
cast operand is never a "free variable" and is never captured.

The hole then surfaces at the compile-time variable resolution in
`compileExpr`'s Variable case: no capture register, and the final fallback
for a known const (`:4421-4424`):

```cpp
if (m_constVars.count(e->name)) {
  r = declareVar(e->name);   // fresh FUNCTION-FRAME register
  emit(Opcode::MOV, dest, r); // MOV from itself — never loaded
```

Inside a function frame `declareVar` allocates a new local slot rather than
referring to the global const's register, so the MOV reads an uninitialized
slot → `nil` (string const) / `0` (numeric const in a cast).

`plain [hello]` works because the `Call` argument case (`:1811-1818`) does
collect the Variable; `direct [hello]` works because top-level code compiles
against the global frame, where the fallback's `declareVar` resolves to the
const's real register.

## Fix (implemented in working tree, verified)

Two recursion cases at the top of `collectIds` — nothing else. The capture
machinery, the capture cells, and the fallback are all correct; only the
scan was blind:

```cpp
if (e->kind == ExprKind::FString) {
  for (auto &part : e->exprParts) collectIds(part, ids, skip);
  return;
}
if (e->kind == ExprKind::Cast) {
  collectIds(e->castOperand, ids, skip);
  return;
}
```

After this, `collectIds` covers every composite `ExprKind`.

**Verified (measured, same binaries, stash-switched):**

| probe | HEAD | fixed |
|---|---|---|
| `interp [f"{G}"]` in def | `nil` | `hello` |
| `cast<float>(N)` in def | `0` | `7` |
| `f"{N}!"` in def | `nil!` | `7!` |
| full `ctest --test-dir build` | 67/67 | **67/67** (incl. `codegen_size`, `doc_hashes`) |

Capture-shape changes (extra capture register + cell) only occur for
functions that capture consts through these expressions; no existing
register-budget test moved.

**Deliberately not done:** hardening the `:4421` fallback (it silently
emits a self-MOV of an uninitialized slot for any const that slipped the
scan). With the scan complete, a second line of defense is speculative —
add an internal error there only if a future composite `ExprKind` gets
added and `collectIds` rots again.

## Test guard (added with the fix, `tests/unit/spec_check2.cpp` `-- const --` block)

- `const` read via `f"{G}"` inside a def → `expectStr … "[hello]"`;
- `cast<int>(N) + 1` inside a def → `expectVal … 8`;
- nested `f"v={cast<int>(N)}"` inside a def → `expectStr … "v=7"` (recurses
  through both new cases at once).

`spec_check2` **529 → 532**, all green.

## Suggested commit

`fix(compiler): scan f-string parts and cast operands when collecting captures`

## Related records

- `docs/plans/todo.md` §6/§7: the "f-string const capture" open item in the
  F1 capture family — flip on commit; move this file to `docs/bugs/resolved`.
- `docs/plans/m4-indirect-plan.md:352`: the `$$` pre-scan must recurse into
  f-string parts — same hazard class in a different pass; reuse this
  collectIds shape there.
