# Phase 1 Handoff — Generator Redesign (qlang-sandbox)

Date: 2026-11-03
Repo: `/home/cfrankb/toolkit/gcc/qlang-sandbox`, branch `master`, base commit `973eab5` ("phase 0")

## Status at a glance

Phase 1 (per-call generator instances, `GeneratorState`, `MAKEGEN` constant-pool
closure, `resumeGenerator()` in the single `runFrames()` dispatch loop, removal
of the old `GeneratorValue::operator()` thunk) is **implemented and working**.
gen_test passes 5/5; the standalone `gen_range(1,3)` repro yields 1,2,3 and
prints `3`.

Full suite (`ctest` in `build/`, 34 tests): **30 pass, 4 fail**:

| Test | Status | Notes |
|---|---|---|
| `spec_check2` | 460 pass / 3 fail | No longer segfaults (was SIGSEGV). Failures: nested-fn call, set append x2. Root causes diagnosed, fixes designed (below). |
| `phase0_scope` | 5 fail | S2 "cannot call captured function 'f'" (parse error), P2 "top-level var survives nested delete", S1 "closure outlives creator frame — wrong value". NOT caused by Phase 1 edits; see "Open questions". |
| `phase0_api` | 3 fail | P1 stale scope after throw, P3 stale snapshot after run(), F1 writeQlc drops non-durable constants. Same category. |
| `perf` | perf_stress -27.3% vs baseline (threshold ±20%) | Need to determine pre-existing vs regression. |

## Fixes applied this session (all uncommitted, on top of the pre-existing
uncommitted Phase-0 VM rewrite in the working tree)

### 1. Generator dispatch (the Phase 1 core)
- `include/qlang/QLangVariant.h`: `GeneratorState` (funcIndex, regs, captures,
  pc, exhausted); removed old `GeneratorValue` from the `QLangValue` variant.
- `src/QLangVM.cpp`: `resumeGenerator(GeneratorState&)` moves state onto a
  live frame (`callDest = -1`), grows regs to `fn.registerCount`, runs
  `runFrames()`; on `Yielded` restores state from `m_suspended`, on `Returned`
  sets `gen.exhausted = true` and returns nil.
- `src/QLangVM.cpp` RETURN case + fall-off-end path: a generator frame
  (callDest == -1) returning with a caller on the stack stops the loop
  (`return RunResult::Returned;`) instead of running the caller past the
  resume boundary.
- `src/QLangCompiler.cpp`: `MAKEGEN` emits a `Closure` constant in the
  function's constant pool (`m_generatorFuncIndices` /
  `m_generatorConstIndices` maps); generator defs no longer use thunk emission.

### 2. Dangling-reference fixes in `runFrames()` (all in `src/QLangVM.cpp`)
Rule: never use `frame` / `cv` (references into `m_frames`) across a
`push_back` / `pushCall` / `resumeGenerator` — they can dangle after
reallocation.
- CALLCLOS generator branch: was `break` (trailing pc increment double-advanced
  pc, breaking consecutive `gen()` calls) → `continue`; state pointer taken
  before resume: `GeneratorState *gs = const_cast<GeneratorState *>(&cv.asGenerator());`
  (cv is const); result written via re-fetched `m_frames[m_cur].regs[i.a]`.
- CALLCLOS Function branch: `const Closure &clos = cv.asFunction();` extracted
  **before** `pushCall`; `m_frames[m_cur].captures = clos.captures;` after.
  (The Closure object is heap-owned via shared_ptr, so the reference survives.)

### 3. `captureNames` never populated (spec_check2 segfault #1)
`src/QLangCompiler.cpp` buildFunction: added `fn.captureNames = updatedCaptures;`
The Def-statement closure-building path reads `built.captureNames` to build the
MAKEFUNC closure constant; the field was declared (QLangBytecode.h:273) and
read (QLangCompiler.cpp:1806) but **never written**. Result: def-functions with
free variables got 0-capture closure constants → REFLOAD into an empty
`frame.captures` → assertion/segfault. Verified: HEAD passes this test (463/463
in `/tmp/qlang-head-build`), so it was a working-tree regression.

### 4. Finalizer re-entrancy (spec_check2 segfault #2)
`QLangContext::clear()` clears `m_vars` first; destroying a table value fires
its `__destroy__` finalizer (`attachFinalizer` deleter → `runFinalizer` →
`callClosure` → `runFrames` → `syncMainFrameOut` → `ctx.set("b", ...)`),
mutating the very `unordered_map` that is mid-clear → heap corruption →
SIGSEGV in hash bucket traversal.
Fix: `QLangVM::setNoSync(bool)` + `m_noSync` flag (include/qlang/QLangVM.h);
both `syncMainFrameOut` call sites in `runFrames()` (RETURN case and
fall-off-end) are guarded with `if (m_cur == 0 && !m_noSync)`.
`QLangContext::runFinalizer` (src/QLangContext.cpp) sets `vm.setNoSync(true)`.
This is also semantically correct for live finalizers: a finalizer's transient
frame must not overwrite the host scope.

## Remaining spec_check2 failures (3) — root causes diagnosed

### A. "nested fn" (L211): argument slots clobber the callee register
```
def outer(a)
  def inner(b)
    return a + b
  end
  return inner(10)
end
local r = outer(5);
```
Bytecode of `outer`: `MAKEFUNC 1,0` (r1 = closure) then `MOVI 1,10` — the arg
`10` is compiled straight into slot `dest+1 == 1`, **overwriting the closure**
before the CALLCLOS branch loads the callee from r1. Then `MOV 0,1; CALLCLOS 0`
tries to call the Int 10 → "attempt to call a non-function value".

Design of the fix (not yet applied): in the `ExprKind::Call` case of
`src/QLangCompiler.cpp` (~line 2694), **before** `compileArgsToSlots(e->args,
argBase)`, hoist the callee load when the callee value's register lies in the
arg-slot range `[argBase, argBase+argCount-1]` (dest itself is never an arg
slot, so `dest` is a safe home):
```cpp
bool calleePreloaded = false;
auto preloadCallee = [&](int cr) {
  if (cr >= 0 && cr >= argBase && cr <= argBase + argCount - 1) {
    emit(Opcode::MOV, dest, cr);
    calleePreloaded = true;
  }
};
{
  auto uit = m_userFuncs.find(e->callee);
  if (uit != m_userFuncs.end() && !m_generatorFuncIndices.count(uit->second)) {
    int fi = uit->second;
    if (!m_functions[static_cast<size_t>(fi) - 1].captures.empty()
        && m_captureRegs.find(e->callee) == m_captureRegs.end())
      preloadCallee(regOf(e->callee));
  } else if (m_closureVars.count(e->callee)) {
    preloadCallee(regOf(e->callee));        // arrow-closure vars: same bug
  } else if (m_generatorVars.count(e->callee)) {
    preloadCallee(regOf(e->callee));        // generator vars: same bug
  }
}
```
Then guard the three later dispatch branches (user-def CALLCLOS,
m_closureVars, m_generatorVars) with `if (!calleePreloaded)` around their
callee loads. REFLOAD-based callee loads (capture cells) are immune — cells
can't be clobbered by register writes.

### B. "set append" (L800, L801): ARRAPP rejects Set
`local s = set(1,5,7); s[] = 11; local n = len(s);` → "cannot append to
non-array value". `case Opcode::ARRAPP` (src/QLangVM.cpp:1595) only accepts
`Type::Array`; a set compiles to ARRAPP on a Set value. Decide semantics:
either ARRAPP accepts Set (insert, dedup — test expects len 4 after adding 11
and len 3 after adding duplicate 5) or the compiler routes set-append
elsewhere. The test expectations imply set-insert semantics (dedup).

## Open questions (verify before/while fixing)

1. **Are the phase0_scope / phase0_api failures pre-existing in the working
   tree** (i.e., in the uncommitted Phase-0 rewrite, before this session's
   edits)? They are in parser/scope-sync/QLC-IO paths not touched by Phase 1:
   S2 is a *parse* error ("cannot call captured function 'f'"), P1/P3 are
   scope-snapshot semantics, F1 is writeQlc constant durability. Check by
   stashing Phase-1-only edits or inspecting those code paths' git diffs
   (the whole VM rewrite is uncommitted, so a clean A/B against HEAD is not
   possible — HEAD has a completely different VM).
2. **perf_stress -27.3%**: pre-existing (the uncommitted rewrite added
   per-instruction `trace(TRACE_VM_LOOP, ...)` calls and a loop-top
   `const Function &fn` re-lookup) or caused by Phase 1? Baseline comparison
   is via the `perf` ctest target.

## Build & test environment

- Main build: `build/` (default flags, no line info). `cd build && ctest
  --output-on-failure`; test binaries live at the build-dir root
  (`./spec_check2`, `./gen_test`, ...). Tests source is `tests/` (not `test/`).
- Debug build: `/tmp/qlang-dbg` (`-DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-g -O0"`).
  Relink repros against `/tmp/qlang-dbg/libqlang.a` for assert backtraces:
  `g++ -g -O0 -I include <src> -o <bin> /tmp/qlang-dbg/libqlang.a` (run from
  repo root; `QLangValue` has `asString()`, no `toString`).
- HEAD baseline: worktree `/tmp/qlang-head` + build `/tmp/qlang-head-build`
  (spec_check2 there: 463/463 pass — proves the working tree regressed it).
- Repro binaries in /tmp (stale until relinked against the fresh lib):
  `genrepro` (gen_range → prints 3), `refload_repro` (global const/var in
  def-fn), `nested_repro` (nested-fn clobber), `dumpbc2/3/4` (bytecode +
  constant-pool dumper; pipeline: `ctx.parse(src)` →
  `QLangCompiler(std::move(program), &ctx)` → `c.compile()`).
- Opcode numbers (enum order in QLangBytecode.h): 0 MOVC, 1 MOV, 27 CALL,
  32 ARRAPP, 49 CALLFN, 50 CALLCLOS, 51 RETURN, 52 MAKEFUNC, 53 MAKEGEN,
  54 REFLOAD, 55 REFSTORE, 56 YIELD, 57 MOVI.

## Conventions / gotchas learned

- `edit_file` rejects edits after out-of-band modification (python scripts,
  sed) — re-`read_file` first.
- Stale statically-linked /tmp binaries give false "still failing" results —
  always relink after rebuilding `libqlang.a`.
- `grep` via bash with `\[` patterns exits 1 on no match — use the `grep`
  tool for content search.
- `QLangContext::parse` is a member; `ctx.get(name)` throws on missing var.
- Dangling-reference rule (see fix #2) applies to any future opcode added to
  `runFrames()`.

## Suggested next steps

1. Apply the nested-fn preload fix (design above) + set-append semantics;
   re-run `spec_check2` → target 463/463.
2. Full `ctest`; for the 4 non-spec failures, determine pre-existing vs
   regression and either fix or document as Phase-0 gaps in the remediation
   plan doc.
3. Clean up /tmp artifacts and remove the worktree
   (`git worktree remove /tmp/qlang-head --force`) when done.
