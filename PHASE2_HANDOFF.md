# Phase 2 Handoff — Single Global Variable Home

**Date:** 2026-11-03
**Repo:** `/home/cfrankb/toolkit/gcc/qlang-sandbox`
**Plan:** `docs/plans/architecture-remediation-plan.md` (Phase 2: single global variable home)
**Status:** BUILD GREEN, 31/34 ctest pass, 3 failures remain (1 is a Phase 2 regression, 2 pre-existing + 1 segfault to triage).

---

## 1. Critical context: the tree was modified concurrently

When this work started, the working tree already contained an **incomplete concurrent
Phase 2 implementation** (not authored by this session) in:
`include/qlang/QLangContext.h`, `include/qlang/QLangVM.h`, `src/QLangContext.cpp`,
`src/QLangVM.cpp` (all vs git HEAD `ba4bc40`).

It implemented most of the Phase 2 design (see §2) but had stale call sites and was
missing the compiler half, so it did not compile. This session reconciled it, added
the missing compiler changes, and got the build green.

**Do not `git checkout` the modified files** — the diff vs HEAD is the whole
concurrent implementation plus this session's fixes. Review with `git diff`.

Current `git status`:
```
 M include/qlang/QLangContext.h
 M include/qlang/QLangVM.h
 M src/QLangCompiler.cpp        <- only file this session changed from scratch
 M src/QLangContext.cpp
 M src/QLangVM.cpp
?? debug_s2.cpp debug_s2b.cpp debug_s3.cpp   <- delete before finishing
```

---

## 2. What Phase 2 is (design, as now implemented in the tree)

Single global home: context's `GlobalTable` (slot vector + name→slot map + erased
set) replaces the old `m_vars` copy + `syncMainFrameOut`/seed-in.

- `QLangContext::GlobalTable` (`include/qlang/QLangContext.h`): `slots` vector,
  `names` map, `erased` set. API: `globalSlots()`, `globalSlotFor(name)`,
  `hasGlobal/getGlobal/eraseGlobalSlot/removeGlobal/clearGlobals`, `globalCount()`.
- `QLangVM::RegFile` (`include/qlang/QLangVM.h` ~L69): either `owned` vector or
  `aliased` pointer to the global slots; `takeOwned()` snapshots an aliased file.
  **The main frame aliases `m_scope.globalSlots()`** (`QLangVM::run()`, ~L1500) —
  top-level names live in the table directly; no seed-in, no sync-out, table is
  current even on mid-run throws.
- `adoptGlobals(prog)` (`QLangVM.cpp` ~L855, called from the VM constructor): for
  standalone `.qlc` programs, adopts `prog.vars` names→slots (relocating on
  conflict). No-op for programs compiled against the context (compiler already used
  table slots, see §3).
- **Global captures**: `CaptureRef{nullptr, slot, -1}` (env==null encodes "global",
  `cell` = table slot). `bindCaptures(Frame*, const Function&, spec, out)`
  (`QLangVM.cpp` ~L1395) splits on `fn.captureNames` membership in `m_prog.vars`:
  global → table alias (re-read live, so later top-level assignments stay visible —
  fixes S2/P3); local → fresh `Env` cell seeded from the creating frame,
  live-aliased via `Env::owner`/`ownerRegs`, registers moved into cells on frame
  pop (`releaseFrame`).
- `cellAt()` (~L1436): env==null → `globalTable().slots[cr.cell]`; else `env->cells[cr.cell]`.
- **DELETEx** (runFrames): main-frame delete → `m_scope.eraseGlobalSlot(reg)`
  (per-slot erased set, value cleared); nested-frame delete → local register clear
  only (fixes P2).
- `liveProgram()` returns `shared_ptr<const CompiledProgram>`; `runFinalizer`
  takes that shared_ptr; `attachFinalizer` (`QLangVM.cpp` ~L115) captures
  `scope.liveProgram()` in the deleter (replaces old program-id scheme).
- `m_deleted`, `m_noSync`, `m_registers`, `syncMainFrameOut`, `currentProgramId`
  are all gone.

---

## 3. Changes made this session (on top of the concurrent impl)

All in `src/` + `include/qlang/`:

1. **5 stale `bindCaptures` call sites** → new 4-arg form:
   - CALLFN (~L1727), MAKEFUNC (~L1797), MAKEGEN (~L1818),
   - `callFunction` generator path + normal path (~L1988, ~L2010): the old
     nil-cell fallback branches were folded into `bindCaptures`
     (`frame == nullptr` → local captures get nil cells, globals alias slots).
2. **`bindCaptures`/`pushCall` signatures**: dropped `const` (it mutates
   `frame->envs`, sets `env->owner`); param is `Frame*`; `pushCall` now takes
   `RegFile&` (header + cpp).
3. **`CompiledProgram` forward declaration** added to `QLangContext.h`
   (public API exposes `shared_ptr<const CompiledProgram>` without including
   `QLangCompiler.h`).
4. **`attachFinalizer`**: `currentProgramId()` (removed) → `scope.liveProgram()`.
5. **`runFinalizer`** (`QLangContext.cpp` ~L430): removed `vm.setNoSync(true)`
   (no longer exists; finalizer runs in a pushed frame, main frame never created).
6. **Compiler table-awareness** — the missing half of the design
   (`src/QLangCompiler.cpp`):
   - `compile()` (~L676): `m_nextReg`/`m_nextNamedReg` start at ≥
     `m_scope->globalCount()` so locals/temps never claim table slots.
   - `declareVar` top-level path (~L358): when `m_scope` is set, the register
     **is** `m_scope->globalSlotFor(name)` (existing names keep their slot, new
     names append); reclaims the slot from both free lists, pushes the counters
     past it. Fallback dense allocation kept for `m_scope == nullptr` (tests
     always pass `&ctx`, so this is defensive only).
   - Why it matters: without this, host pre-set vars (table slots 0..k-1) would
     alias script top-level names/locals (dense registers from 0) — value
     clobbering.

Earlier edits from before the concurrent impl was discovered (include, pushFrame
aliasing, sync removals, YIELD `takeOwned`, DELETEx) are consistent with the
concurrent design and are part of the current tree.

---

## 4. Build & test status

```
cd /home/cfrankb/toolkit/gcc/qlang-sandbox
cmake --build build -j$(nproc)          # exit 0 — library + all test binaries
ctest --test-dir build --output-on-failure
```

**31/34 pass. Failures:**

| Test | Symptom | Class |
|---|---|---|
| `spec_check2` | **SEGFAULT** | not yet triaged (see §5.3) |
| `phase0_scope` | 5 asserts (S2, S3, S1) | 1 regression + 2 pre-existing (see §5.1/5.2) |
| `phase0_api` | not yet triaged (P3: finalizer/program lifetime) | see §5.4 |

**Ground-truth baseline** (git worktree at HEAD, built): `/tmp/qlang-head`
(remove with `git worktree remove /tmp/qlang-head` when done). HEAD `phase0_scope`:
S2 fails (same parse error), S3 passes, P2 fails, S1 fails. So:
- **S3 is a Phase 2 regression** (must fix).
- S2 + S1 are pre-existing; S2 is explicitly in Phase 2 scope (global capture
  liveness); P2 is now fixed by Phase 2.

---

## 5. Remaining work, in order

### 5.1 S2 (in scope) — `def` with captures can't be called from another `def`

```
x = 42
def f() { global x; return x }
def g() { return f() }      # parse error: cannot call captured function 'f'
```

Root cause (verified): `collectIds` for `ExprKind::Call`
(`src/QLangCompiler.cpp` ~L796) collects only **argument** ids, never the callee
name. So `f` never enters `g`'s `allIds` → capture-analysis loop (~L933–990)
never adds it → `m_captureRegs["f"]` absent → call site (~L3075–3086) throws.

Planned fix (safe, verified against the dispatch order):
1. In `collectIds` Call case: also `ids.insert(e->callee)`.
2. In the capture-analysis loop, add `if (m_nativeFuncIds.count(id)) continue;`
   (native injected funcs would otherwise be captured harmlessly-but-wastefully).
   Builtins are already skipped (`builtinId(id) >= 0`), classes skipped,
   capture-free user defs skipped (`m_functions[fi-1].captures.empty()`).

Expected result after fix: `f` captured by `g` as a **global** capture
(name in `m_prog.vars` → table-slot alias) → REFLOAD + CALLCLOS → live value.

### 5.2 S3 (REGRESSION) — interleaved generator second resume returns nil

```
def gen(pad, base, q) { for i = 0 to 2 { yield base * 10 + i } }
local g1 = gen(0, 1, 0)
local g2 = gen(0, 2, 0)
local a = g1()   # 10 ok
local b = g2()   # 20 ok
local c = g1()   # nil (want 11)
```

Passes on HEAD, fails on the Phase 2 tree. Bisect harness written:
`debug_s3.cpp` (T1 single-gen 3 resumes / T2 two gens / T3 for-loop gen /
T4 S3 exact). Last action before this handoff was fixing its compile error
(`toInt64(v)` is a free function, not a member). Run:
```
g++ -std=c++17 -Iinclude debug_s3.cpp -o /tmp/debug_s3 build/libqlang.a && /tmp/debug_s3
g++ -std=c++17 -I/tmp/qlang-head/include debug_s3.cpp -o /tmp/debug_s3_head /tmp/qlang-head/build/libqlang.a && /tmp/debug_s3_head
```

Suspect areas (generator path, all in `src/QLangVM.cpp`):
- YIELD (~L1860–1900): `gs` is only found when `m_cur >= 1 && callDest >= 0` and
  the caller slot holds the Generator; top-level `g1()` calls use
  `resumeGenerator` (callDest == -1) so state goes to `m_suspended`, then
  `gen = std::move(*m_suspended)`.
- `resumeGenerator` (~L1476): `frame.regs.owned = std::move(gen.regs)`,
  resize to `fn.registerCount`, `frame.captures = std::move(gen.captures)`.
- MAKEGEN (~L1807) creates the instance with only the 3-arg register file.
- `releaseFrame(frame)` at YIELD moves captured regs into cells — check it
  doesn't clear something the generator needs (gen has no captures here, so
  likely not this).
- Check `QLangValue` copy semantics on the Generator value stored in the main
  frame's aliased slot (shared_ptr<GeneratorState> — should be fine).

### 5.3 `spec_check2` SEGFAULT — not yet triaged

Run `./build/spec_check2` directly (it prints PASS/FAIL lines; find the last one
before the crash), then gdb if needed. 34 other tests pass, so the library is
mostly sound; likely one specific feature (capture/finalizer/generator related —
spec_check2 exercises a lot of the changed paths).

### 5.4 `phase0_api` (P3) — not yet triaged

Run `./build/phase0_api`. P3 = instance finalizer (`__destroy__`) / program
lifetime via the new `liveProgram()` shared_ptr path. Relevant code:
`attachFinalizer` (`QLangVM.cpp` ~L115), `runFinalizer`
(`QLangContext.cpp` ~L430), `QLangContext::clear()`/destructor (check they don't
destroy `livePrograms` out from under live finalizers).

### 5.5 S1 (pre-existing, scope decision needed)

"closure outlives creator frame — wrong value" (forked child in `phase0_scope`).
Fails identically on HEAD. Check the plan's phase mapping: if S1 belongs to a
later phase, document it as known-red; if Phase 2, fix (suspect: `releaseFrame`
cell move-in timing — `Env::ownerRegs` move on pop).

### 5.6 New Phase 2 tests (plan requires)

Add a `tests/unit/phase2_scope.cpp` (or extend `phase0_scope.cpp` once red tests
go green) covering:
- top-level assignment visible to a closure defined later (live global capture);
- closure created before assignment sees the later value (S2 shape);
- `delete` of a top-level var erases only that name (P2 shape, already passes);
- host `set()` before `run()` + script top-level var of a *different* name must
  not alias the host slot (the exact case §3.6 fixes — worth a regression test:
  `ctx.set("x", 1); ctx.run("a = 5; ...")` → `ctx.get("x")` still 1);
- mid-run throw leaves the table consistent (throw after `a = 1` → `get("a")` == 1).

Register it in `CMakeLists.txt` like the other unit tests
(`add_executable` + `target_link_libraries(... qlang)` + `add_test`).

### 5.7 Cleanup before done

- Delete `debug_s2.cpp`, `debug_s2b.cpp`, `debug_s3.cpp` (workspace root).
- Remove `/tmp/qlang-head` worktree: `git worktree remove --force /tmp/qlang-head`.
- Final: `./build.sh clean && ./build.sh test` (full clean build + ctest +
  harness binaries) must be green.
- Optionally update the plan doc's Phase 2 status section.

---

## 6. Key file/line map (current tree)

| What | Where |
|---|---|
| `GlobalTable` + global API | `include/qlang/QLangContext.h` (pimpl: `m_globals`) |
| `GlobalTable` methods, `run()`, `set()`, `clear()`, `runFinalizer`, `liveProgram` | `src/QLangContext.cpp` (run ~L92, runFinalizer ~L430) |
| `RegFile` | `include/qlang/QLangVM.h` ~L69 |
| `bindCaptures` / `cellAt` / `releaseFrame` | `src/QLangVM.cpp` ~L1395 / ~L1436 / nearby |
| `adoptGlobals` + ctor | `src/QLangVM.cpp` ~L850 |
| `attachFinalizer` | `src/QLangVM.cpp` ~L115 |
| main-frame alias setup in `run()` | `src/QLangVM.cpp` ~L1500 |
| `runFrames` (DELETEx ~L1850, YIELD ~L1860, RETURN ~L1847, REFLOAD/STORE ~L1836) | `src/QLangVM.cpp` |
| `resumeGenerator` / `callFunction` (gen ~L1988, normal ~L2010) | `src/QLangVM.cpp` |
| `declareVar` (table path) / `compile()` (counter init) | `src/QLangCompiler.cpp` ~L358 / ~L676 |
| `collectIds` Call case (S2 fix site) | `src/QLangCompiler.cpp` ~L796 |
| capture-analysis loop (S2 fix site 2) | `src/QLangCompiler.cpp` ~L933 |
| call dispatch order (member→class→userFuncs→closureVars→genVars→builtin/native) | `src/QLangCompiler.cpp` ~L2728–3135 |
| `CaptureRef` / `GeneratorState` | `include/qlang/QLangVariant.h` L35 / L59 |
| `Function` (has `captureNames`) | `include/qlang/QLangBytecode.h` L260 |

## 7. Commands cheat-sheet

```
cmake --build build -j$(nproc)                    # incremental build
ctest --test-dir build --output-on-failure        # full suite (34 tests)
./build/phase0_scope && ./build/phase0_api        # the two failing harnesses
g++ -std=c++17 -Iinclude <file>.cpp -o /tmp/x build/libqlang.a   # ad-hoc harness
git worktree remove --force /tmp/qlang-head       # when baseline no longer needed
```
