# Expression-form `++`/`--` increments its own result register — nil error or silent garbage

**Date:** 2026-11-15
**Severity:** high (correctness — every value-context `++`/`--` produces a wrong value or a
bogus error, and a dangling statement-level `++` silently corrupts the next statement)
**Status:** open — fix implemented in working tree (uncommitted), verified 2026-11-15
**Cited code at commit:** `ae0b2ea`

## Symptom

```ql
local a = 1
let z = a++
```

`build/qlcc --run` → `type error at line 2,1: operator '+' operand must be numeric, got nil`
(repro: `/tmp/opencode/inc/e1.ql`). Prefix (`let z = ++a`) and both `--` forms are identical.
When the result register happens to hold a leftover number (register reuse), the same code
compiles, runs, and yields a **wrong value with no diagnostic at all**.

Two statement-level shapes inherit the same broken path whenever the inc/dec operand is not a
bare variable: `++t.n` is a silent no-op (the step lands in the statement's throwaway temp),
and `t.n++` — whose `++` the parser refuses to bind (postfix binds bare variables only,
`QLangParser.cpp` `parsePostfixChain`) — has its stray `++` parsed as a **prefix on the
following statement**: `t.n++` newline `println(…)` compiles as `++println(…)`, prints, steps
a garbage register, exit 0 (`/tmp/opencode/inc/sw1.ql`).

## Root cause

`QLangCompiler::compileExpr`'s Unary branch emits `emit(unOp("++"), dest, operandReg)`, but
the VM's `INC`/`DEC` step `r[a]` **in place** and store one operand
(`docs/spec/13-bytecodes.md` line 64: INC takes a "dest reg"). The expression form therefore
never read `a` — it stepped `z` itself: nil → the bogus type error above (the arithmetic error
helper labels INC as `'+'`), or leftover garbage → a silent wrong number. The statement path
(`compileExprStmt`, Round 95) is what makes bare `a++` work, by emitting `INC r, r`.

The AST carries no prefix/postfix distinction — both shapes parse to the same `Unary` node
(`QLangParser.cpp` `parseUnary` / `parsePostfixChain`) — so C-style old-value copy semantics
were never implementable on this path, and the spec never shows `++`/`--` inside a value
context (spec `04-control-flow.md`: `yield i; ++i` — statements only).

## Fix (Round 96)

`compileExpr`, Unary branch: `++`/`--` throw at compile time —

> `operator '++' is a statement: it has no value (use it on its own line, or 'x + 1' as an expression)`

Every value context becomes honest instead of corrupt. Side effects in the good direction:
`++t.n` reports instead of no-op'ing, and the dangling `t.n++` swallow reports at the stray
`++` token instead of silently stepping a garbage register. Whether `t.n++` / `a[i]++` should
*legally* mutate stays with the value-semantics round (Round 68 deferral, unchanged).

## Verification

- `spec_check2` 538 → 542: expression-form `a++`, `++a`, `--a` and `++t.n` each refused.
- Repros against `build/qlcc`: `let z = a++` / `let z = --a` → parse error with correct
  line:col; `t.n++` → parse error at the `++` (3:4), nothing executes; statement-form
  `--a` while-loop still counts to 0; `a + 1` unaffected.
- `ctest --test-dir build` 67/67 (perf, doc_hashes skipped).
