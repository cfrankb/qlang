# CALL arg-slot clobber: low `dest` re-home (return / yield / reassignment)

Status: **implemented — Round 98 (2026-11-15).** Date 2026-11-15. Found while verifying
Round 97 (`return f"{x}"` printed `%s`); the f-string is one symptom of a
general CALL-lowering hazard. Reference spec: `docs/spec/13-bytecodes.md`
(CALL convention: args at `dest+1..dest+argCount`, result at `dest`).

---

## 1. Symptom

Any call-shaped expression whose lowering gets an *old, low* destination
register corrupts values that live inside the slot window `dest+1..dest+N`.
Reproduced on HEAD (Round 97 tree), all via `qlcc --run`:

| # | Context | Source (essence) | Expected | Actual |
|---|---------|------------------|----------|--------|
| f1 | `return` + f-string | `local n = 7` · `return f"x{n}"` | `x7` | `xx%s` |
| h3 | `return` + 2-arg call | `local a=5; local b=6` · `return h(b, a)` | 605 | **606** |
| k1 | reassignment + call | `local x=0; local n=5; local m=6` · `x = h(m, n)` | 605 | **606** |
| k4 | reassignment + f-string | `local x=0; local n=7` · `x = f"a{n}"` | `a7` | `aa%s` |
| k5 | `yield` + call | gen body: `local a=5; local b=6` · `yield h(b, a)` | 605 | **606** |
| k6 | `yield` + f-string | gen body: `local n=7` · `yield f"v{n}"` | `v7` | `vv%s` |

Safe controls (invariant already honored — must stay green):

- `local s = f"x{n}"` (declaration: dest is the freshly allocated home) → `x7` ✓
- top-level `println(f"…")` (statement dest = `tmpRegister()` above the frame) ✓
- `emit obj.sig(args)` — `compileEmit` puts the sender in a fresh temp
  (`QLangCompiler.cpp:3223`) → slots fresh ✓
- `local a=1` + `return h(a)` — single arg whose slot *is* its own home
  (self-MOV, no clobber) → 50 ✓

Disassembly of f1 (function `g`, `registers: 3`):

```
0000  mov  01, 7            ; local n = 7  (home r1)
0004  mov  01, pool[00]     ; template "x%s" -> dest+1 = r1 — n's home!
0008  mov  02, 01           ; arg {n} copied AFTER n was overwritten
000b  call dest=00, Format, 2
```

`Format("x%s", "x%s")` → `xx%s`. The h3/k1 shape is the same window with the
*args* clobbering each other: arg0's slot MOV lands on arg1's source register
before arg1's MOV reads it.

## 2. Root cause

The CALL convention requires `dest+1..dest+N` to be registers **nothing else
is using**. `beginArgSlots` (`QLangCompiler.cpp:782`) grows the frame and
quarantines *free-pool* registers in the window, so later `tmpRegister()`
calls can't hand a slot back — but it cannot protect registers that were
**already allocated to variables** before the call lowering ran. The whole
lowering (template MOV first, then `compileArgsToSlots` simple-operand
`mov slot, src` staging, callee hoists) relies on the unstated invariant:

> **INV:** `dest` is freshly allocated at the frame high-water, so
> `dest+1..dest+N` lies above every live value.

`tmpRegister()`-dest sites (statements, declarations, nested calls) honor
INV trivially. Three expression-compile sites violate it:

1. `compileReturn` / `compileYield` compile the value **directly into r0**
   (SPEC §bytecodes encoding guideline — "always copy directly to the final
   register"). r0+1.. is the frame's local-variable area.
2. Plain assignment `x = <call>` compiles the value into `x`'s home register;
   when `x` was declared before other still-live variables, the window lands
   on them (k1/k4).

F-strings are the loudest symptom because their lowering always writes the
template into `dest+1` **first** and always needs N+1 slots
(`QLangCompiler.cpp:5595`); ordinary multi-arg calls break by intra-arg
aliasing instead (h3).

## 3. Design — restore INV once, in the two lowerings

Guard at the two `compileExpr` sites that own a CALL lowering, before any
slot is reserved — `QLangCompiler.cpp:4620` (`reserveArgSlots(dest,
argCount + 1)` in `case ExprKind::Call`) and `:5595` (`beginArgSlots(dest,
totalArgs)` in `case ExprKind::FString`):

```cpp
// dest+1.. must be fresh (SPEC §bytecodes CALL layout): an old dest
// (return/yield's r0, a reassigned variable's home) leaves live
// registers inside the slot window. Re-home the lowering to a fresh
// temp; the result moves back after the call.
int home = dest;
if (dest + 1 < m_nextReg) dest = tmpRegister();
```

…and at the end of each case, after the `CALL` emit (and its `endArgSlots`):

```cpp
if (dest != home) { emit(Opcode::MOV, home, dest); freeRegister(dest); }
```

Notes:

- Condition `dest + 1 < m_nextReg` is *conservative*: a freed temp inside the
  window forces a re-home the quarantine would actually have covered. That
  costs one MOV on rare patterns; correctness first. Refinement (check the
  window against `m_freeRegs`/`m_quarantined` for true live occupancy) is
  recorded here, deferred until `codegen_size` shows it matters.
- Everything downstream works on the re-homed `dest` unchanged: `argBase`
  derives from `dest`, `emitImportCall`/method shifts/callee hoists use the
  locals. The existing `destIsSource` capture-alias check in the Call case
  becomes unreachable for re-homed dests (a fresh temp is nobody's home);
  leave it — non-re-homed dests still take the old path.
- The fixup `MOV home, dest` runs **after** the call returns, so the
  r0-capture-cell aliasing the callee-hoist comment (4622–4633) worries
  about is untouched.
- Alternative fixes rejected:
  - *alias-aware ordering inside `compileArgsToSlots`* — re-derives liveness
    per arg at every call shape (methods, imports, closures, format); many
    more moving parts than restoring the invariant.
  - *make `compileReturn`/`compileYield` use a temp always* — fixes only
    sites 1, misses reassignment, and pays a MOV on every
    `return foo(...)`/`yield foo(...)` even where r0 is provably safe
    (single-arg `return h(a)`), against the "copy directly to the final
    register" guideline.
  - *a VM that copies args itself* — changes the CALL convention; out.
- No VM, opcode, encoding or `.qlc` change; the spec CALL layout text stays
  true (the compiler, not the contract, was violating it). Optional: one
  sentence in `13-bytecodes.md` noting the compiler requires the slot window
  to be fresh registers.

## 4. Steps

1. Add the guard + fixup to the two lowerings; `ponytail:` comment on the
   conservative condition if kept.
2. New checks in `tests/unit/spec_check2.cpp` (pins 1–4 of the matrix):
   `return` + 2-arg call → 605; `return` + f-string → `"x7"`; reassign +
   call → 605; reassign + f-string → `"a7"`; plus a 3-arg return variant and
   the existing-safe controls (declaration form, `return h(a)`) to pin that
   the guard doesn't regress the direct-landing guideline where it is safe.
3. New checks in `tests/unit/gen_test.cpp` (pins 5–6): `yield h(b, a)` →
   605, `yield f"v{n}"` → `"v7"`.
4. `cmake -B build && cmake --build build && ctest --test-dir build`.
   Investigate every failure: the suite was green over corrupt behavior, so
   a failure is either a budget/golden shift (re-record with the frame's
   `registers:`/instruction count change explained in history) or a test
   that pinned a corrupted value (re-pin with a comment — the old number was
   wrong).
5. `codegen_size` budgets: any script whose corpus contains
   return/yield/reassign call shapes moves by the fixup MOV (+ register
   count). Re-record per the AGENTS discipline, never before testing.
6. Docs: compliance history (the round that lands this), `tests.md` tallies,
   `todo.md` §6 entry flip, `update-doc-hashes.sh` re-record if any
   hashed doc moves.

## 5. Risks

- **Budget churn** — expected, bounded, one MOV per affected call shape.
- **Tests pinning corrupt outputs** — by definition already wrong; re-pin
  transparently.
- **Conservative over-homing** (freed temp in window) — extra MOV, no
  semantic effect; refinement path recorded in §3.

## 6. Out of scope

- `compileEmit` (already fresh-sender safe).
- Member/subscript `++` targets and by-value module arguments — remain with
  the value-semantics round (`missing-features-plan.md`).
- `return f"{x}"` at top-level *directly inside `println`* was never broken
  (statement temp dest) — this plan is not an f-string feature round; the
  f-string appears only as the invariant's loudest witness.
