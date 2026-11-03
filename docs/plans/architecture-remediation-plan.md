# Plan: architecture remediation — substrate rewrite (frames, closures, scope, executor)

Status: **in progress** (drafted 2026-11-02). Phase 0 done; Phase 1 done as
of 2026-11-03; Phase 2 done as of 2026-11-03 (`7568ed8`); Phase 3 done as of
2026-11-03 (uncommitted). Phases 4–5 pending.
Scope: implementation substrate only. **No spec changes, no opcode changes, no
value-tag changes, no value-representation rewrite, no GC, no JIT, no
threading model.** The language semantics are sound; the machine under them
is not.

Spec anchors: `docs/spec/06-functions.md` (call/closure/generator semantics),
`docs/spec/14-scope.md` (scope model), `docs/spec/13-bytecodes.md` (opcode
semantics — unchanged by Phases 0–3), `docs/SERIALIZATION.md` (`.qlc` format —
changed only by Phase 4), `docs/EXTERNAL_TYPES.md` (ownership model),
`docs/EVENT_HANDLERS.md` (budget story).

---

## 1. Summary

QLang's POC core works and is test-covered, but the VM substrate has a small
number of structural defects that will compound as features land. This plan
rebuilds the frame/closure/scope substrate once, correctly, and deletes the
scaffolding that existed to paper over it:

1. **Cells, not raw pointers** — closure captures become shared cells
   (upvalue model), fixing dangling captures and the `CALLFN`/`CALLCLOS`
   disagreement.
2. **One executor** — the four duplicated dispatch loops collapse into a
   single loop with a return/yield/limit result protocol.
3. **Generators as instances** — each generator value owns its saved frame;
   the function-index-keyed map is deleted.
4. **One home for variables** — a context-owned global table replaces the
   start/end scope sync.
5. **Host API hardening** — stable type handles, `type_info` validation,
   finalizer queue instead of deleter-dispatch, context split.
6. **`.qlc` + compiler hygiene** — explicit on-wire encoding, parser depth
   limit, compiler statement split, removal of auto-capture.

The test suite (unit + script + perf baseline) is the safety net that makes
this safe. Every phase ends with a green `ctest` and a shippable state.

**Key invariant: Phases 0–3 change no opcodes, no value tags, and no `.qlc`
format. The codec version is untouched until Phase 4, which is the only phase
that bumps it.**

---

## 2. Defects driving this plan (evidence)

**As-of 2026-11-03 (after Phase 3):** all Phase 0 regression tests are
green (`phase0_scope`, `phase0_api`). S1–S3 were fixed by the Phase 1
substrate (cell captures, single executor, per-call generator instances);
P1–P4 by the Phase 2 global table (the main frame aliases the table's slots;
`m_vars`, the start/end sync, and the VM-wide `m_deleted` set are deleted;
programs are held by `shared_ptr<const CompiledProgram>`); H1–H4 by Phase 3
(shared type handles + `type_info` validation, finalizer queue, `release()`
builtin, context split). F1 remains red and is Phase 4 scope (`.qlc`
encoding); F2–F8 remain and are Phase 4/5 scope.

### 2.1 Substrate (VM core) — unsound

| # | Defect | Evidence | Severity |
|---|--------|----------|----------|
| S1 | Captures are raw `Frame*` into a `deque` that is popped on `RETURN`; a closure outliving its creating frame dangles (UB). No copy-on-pop, no weak handle. | `QLangVariant.h:21-24` (`CaptureRef`); `QLangVM.cpp:1635, 1987, 2793` (bind), `:1660-1672, 2818-2829` (deref), `:1692, 2833-2838` (pop) | High — UB |
| S2 | `CALLFN` rebinds all captures to the *immediate caller* frame; `CALLCLOS` keeps creation-time bindings. The two call paths disagree about what a closure means; a function defined in a deeper scope reads/writes the caller's registers by index. | `QLangVM.cpp:1574-1577, 1939-1942, 2738-2741` vs `:1616` | High — correctness |
| S3 | Generators are keyed by **function index**, not instance (`unordered_map<int, Frame>`). Two instances of the same generator clobber each other; nested calls of the same generator destroy the outer one. `MAKEGEN`'s lambda captures raw `this` — a generator outliving the VM dangles. Generator captures are re-pointed at the current caller on every resume. | `QLangVM.h:143`; `QLangVM.cpp:1706, 2143-2148, 2161-2162, 2437, 2454` | High — correctness |
| S4 | Every yield/resume deep-copies the entire register file 2–3× (fat `std::variant` values). | `QLangVM.cpp:2452` (save), `:2145` (copy out of map), `:2175` (restore) | High — performance |
| S5 | Four duplicated ~250-line opcode dispatch loops (`run`, `runFrames`, generator loop in `callFunction`, normal loop). Already divergent: `runFrames`' `CALLCLOS` lacks the Generator/BoundMethod arms the other three have. Any opcode change = 4 edits. | `QLangVM.cpp:1437-1782, 1789-2126, 2227-2554, 2609-2907`; divergence at `:1969-1971` vs `:1580-1623` | High — maintainability |

### 2.2 Scope model

| # | Defect | Evidence | Severity |
|---|--------|----------|----------|
| P1 | Scope sync is start/end-only. A `QLangError` mid-run skips sync-out (no RAII); the fall-off-end path never syncs. Host scope is silently stale after any failed or unterminated run. Every successful run pays O(n) fat-value copies in and out. | `QLangVM.cpp:1370-1374` (in), `:1680-1691` (out, main-frame `RETURN` only), `:1405-1411` (fall-off) | High |
| P2 | `m_deleted` is a VM-wide set of register **indices**, but registers are per-frame. A `delete` in a nested function at reg 3 erases the unrelated top-level var occupying reg 3. The set is never cleared. | `QLangVM.cpp:1454-1455, 2626-2628` (insert), `:1684-1685` (check against main-frame regs) | High — correctness |
| P3 | `run()` computes `externalVars` from `m_vars` at compile time; `call()` reuses the cached compilation with no recompile — vars `set()` after `run()` are invisible to script functions. | `QLangContext.cpp:97-101, 301-319` | High — correctness |
| P4 | Every `run()` appends to `livePrograms`; nothing releases them except `clear()`. Unbounded growth for a long-lived context. Generators/finalizers reference programs by `int` index (`currentProgramId()`). | `QLangContext.cpp:131`; `QLangContext.h:73-79` | Medium |

### 2.3 Host API

| # | Defect | Evidence | Severity |
|---|--------|----------|----------|
| H1 | `QLangContext` is a god object: scope, consts, natives, modules, types, verbose level, step limit, program cache, finalizer dispatch. The Pimpl hides only two members (include-cycle break). Every future feature lands in the same class. | `QLangContext.h:49-160`; `QLangContext.cpp:61-69` | High — maintainability |
| H2 | `ExternalRef` is `shared_ptr<void>` erasure with owned/borrowed dual; the docs admit "a wrong `weak_ptr` is UB in v1". No release path for script-constructed (`owned`) objects — structural retention leak. | `QLangValue.h:152-164`; `EXTERNAL_TYPES.md` §10.2, §11(d) | High |
| H3 | `__destroy__` runs **from a `shared_ptr` custom deleter** with `catch (...)` swallowing everything: arbitrary script execution on any thread, during stack unwinding, all errors invisible. | `QLangContext.cpp:343-369` | High |
| H4 | `registerType` returns a raw `const TypeDescriptor*` invalidated by `clear()`; ctor lambdas capture `desc.get()`. Hosts caching the pointer or a copied `NativeFunc` dangle after `clear()`. | `QLangContext.cpp:243-246, 253-254, 189`; `QLangContext.h:138-141` | Medium |
| H5 | No thread story: nothing prevents sharing a context across threads; the finalizer deleter can fire VM work on any thread. | whole library; `EVENT_HANDLERS.md` §8.2 | Medium |

### 2.4 Format, compiler, tooling

| # | Defect | Evidence | Severity |
|---|--------|----------|----------|
| F1 | `.qlc` writer emits **raw `Instruction` struct bytes** (memcpy) while the reader rebuilds field-by-field — two divergent encodings for one format. `putU32` is a memcpy of host bytes: "little-endian" is true only on LE hosts. Writer emits version **2**; docs say `version <= 1`. Non-durable types (External/BoundMethod/TypeRef/Generator/Object) silently serialize as `Nil` with no way to detect the loss; closures with captures are only loadable if the host rebinds captures (unstated constraint). | `QLangBytecodeIO.cpp:39-43, 231-233, 324, 339-342, 426-437`; `SERIALIZATION.md` §2, §5.1, R2, R4 | High |
| F2 | `compileStmt` is a 1,456-line switch (24 arms; the `Class` arm alone is 356 lines); `compileExpr` is 783 lines. Adding an opcode touches ≥5 sites. | `QLangCompiler.cpp:1038-2494, 2494-3277` | High — maintainability |
| F3 | No scope analysis: free variables are found by scanning **all identifiers in the body** and subtracting locals — a flat set, not a scope-aware pass. It silently *augments* the parser's mandatory explicit capture list, so two competing capture mechanisms exist. `global` falls back to **register 0 (the return slot)** when the name is missing. | `QLangCompiler.cpp:775-922`; `QLangParser.cpp:1412-1419`; `QLangCompiler.cpp:2372` | High — correctness |
| F4 | Parser: recursive descent, zero error recovery, **no recursion-depth limit** — deeply nested input overflows the C stack (DoS surface for an embeddable language). | `QLangParser.cpp:41-51, 138-144, 1066-1101` | Medium |
| F5 | Constant-pool dedup serializes every value to a string key (recursively for tables/arrays) — allocation + O(size) hash per constant, key map roughly doubles pool memory, large table literal is O(n²) to insert. | `QLangBytecode.cpp:9-125, 158-174` | Medium — performance |
| F6 | Step guard is not the safety primitive `EVENT_HANDLERS.md` claims: `m_steps` is private with no accessor; every `callClosure`/`callMethod` constructs a fresh VM so the counter resets per dispatch; blocking natives can't be interrupted. The documented budget is unreachable from C++. | `QLangVM.h:136`; `QLangContext.cpp:318, 334`; `EVENT_HANDLERS.md` §0, §8.1, §8.3 | High |
| F7 | CMake: static-only, **no `install()`/export rules** (embedding requires a source build); `file(GLOB)` for test scripts; MSVC `/Zc:preprocessor`. | `CMakeLists.txt:16-18, 133` | Low |
| F8 | Value representation is fat: 19-alternative `std::variant`, ~48–64 B per value, deep-copied on every register write/arg copy; tables are `std::map` (O(log n), per-access key string build); sets are `vector` (O(n) membership/dedup). | `QLangVariant.h:99-104`; `QLangVM.cpp:1356-1357, 262-334` | Medium — performance (deferred, see §5) |

---

## 3. Fix principles

1. **The test suite is the contract.** Phase 0 writes failing regression
   tests for the known bugs; every later phase is validated against them plus
   the existing suite (`ctest --test-dir build`) and the perf baseline
   (`tests/perf/baseline.json`).
2. **Fix the substrate once.** Cells + single executor + instance generators
   are one coordinated change (Phase 1), not three independent patches — they
   share the same frame model.
3. **One home for variables.** The global table (Phase 2) makes the context
   scope and the VM the *same* state; sync scaffolding is deleted, not
   patched.
4. **No format churn until it's needed.** Phases 0–3 are internal; the `.qlc`
   version is bumped exactly once, in Phase 4, alongside the explicit
   encoding.
5. **Each phase ends green and shippable.** No phase leaves the tree
   mid-migration.

---

## 4. Phases

### Phase 0 — Pin the mess down

Goal: failing regression tests that *prove* the defects, before any fix.
These tests are red now and green after the relevant phase; they are the
acceptance criteria.

**Status: DONE (tests written, all red for the right reasons).**

Delivered:
- `tests/unit/phase0_scope.cpp` — S1, S2, S3, P2.
- `tests/unit/phase0_api.cpp` — P1, P3, F1.
- Both registered in `CMakeLists.txt` (`phase0_scope`, `phase0_api`).
- Full suite verified: 32/34 pass; the only failures are the two new
  Phase 0 executables (the intended reds).

Probe-confirmed pre-fix behavior (exact RED values):
- S2: `r1 = 42`, `r2 = nil`, `r3 = 42` (nested `CALLFN` reads caller reg).
- S3: `a = 10`, `b = 11`, `c = 12` (expected `10 / 20 / 11`; shared
  saved frame). Note: `YIELD` clobbers reg 0, so the per-instance identity
  parameter must sit in slot ≥ 1 — the test uses `gen(pad, base, q)`.
- P2: `a` is *undefined* after `f()` (shared `m_deleted` set; `delete t`
  in `f` poisons main-frame reg 0).
- S1: dangling read — segfault or garbage (test forks so a crash cannot
  kill the other checks).
- P1: `get("v") == 1` after `run("v = 99; assert(false)")` (stale scope).
- P3: **segfault** in `call()` — `ctx.call()` has no caller frame, so the
  capture binds to `m_frames[-1]` (confirmed by backtrace + disassembly:
  `framePtr->regs[cr.reg]` with a dangling `framePtr`). Test forks.
- F1: `writeQlc` returns `true`; all four non-durable constants
  (Generator/External/TypeRef/Object) round-trip as `Nil`.

Test-unblocking fix (the one source change Phase 0 makes, documented here
because "no source changes" would otherwise make S1 unexpressible):
`collectLocalVars` (`QLangCompiler.cpp`) did not handle `StmtKind::Arrow`,
so a lambda declared inside a `def` function was auto-captured as a free
variable, shifting every capture index (including `global` captures) and
turning references to the lambda into `REFLOAD`s of the wrong capture slot.
Symptom: any closure stored in a table/returned from a `def` function came
back as `nil` or an unrelated value (e.g. `h = f` inside `m` stored the
value of the *first global*). The fix adds the Arrow name to the local set
(3 lines, no recursion into the lambda body — its ids are not collected by
`collectStmtIds` either). Without it, S1 cannot be written: the closure is
corrupted before it ever escapes its frame.

New tests (under `tests/unit/`, one executable per area per the CMake
convention):

| Test | Defect covered | Expected failure today |
|------|----------------|------------------------|
| closure outlives creating frame (call it after the defining scope's function returns) | S1 | UB / crash / garbage |
| `CALLFN` path vs `CALLCLOS` path on a closure defined in a nested scope | S2 | divergent results |
| two concurrent instances of the same generator function | S3 | clobbered state |
| `delete` of a name inside a nested function, then read the unrelated top-level var at the same register index | P2 | top-level var erased |
| run a script that assigns then throws; inspect `ctx.get()` | P1 | stale pre-run value |
| `set()` a var after `run()`, then `call()` a script function reading it | P3 | stale compile-time snapshot |
| `.qlc` round-trip of a program with external/generator constants | F1 | silent `Nil` (assert it *fails loudly* after Phase 4) |

Also: verify the perf baseline is current and the suite is green before
starting Phase 1.

**Exit criteria:** all new tests exist; the suite is green on the unpatched
tree for everything *except* the listed defect tests (which are red/skipped
with a marker).

### Phase 1 — Substrate rewrite (the big one)

**Status: DONE (2026-11-03).** Single `runFrames()` executor, cell-based
captures, and per-call generator instances are in place; `m_generatorFrames`
and the old `GeneratorValue` thunk path are deleted. Phase 0 generator test
S3 is green. S1/S2 remain red and are carried to Phase 2 (see §2 as-of note
— S2 now fails at compile time, which is the safer manifestation of the same
defect). `spec_check2` 463/463; full ctest 32/34 with only
`phase0_scope`/`phase0_api` red (both verified pre-existing at base commit
`973eab5`). `perf_stress` improved ~28% vs the pre-Phase-1 baseline.

One coordinated change to the `QLangVM` core. Target: `QLangVM.cpp` 2,918
lines → ~1,500–1,800.

**1.1 Single executor.**

One `runFrames()` loop, one opcode switch. The loop runs until it hits
`RETURN`, `YIELD`, or the step limit, and returns a result enum
(`returned / yielded / limit`). `run()` and `callFunction()` become thin
wrappers; generator resume is "swap the saved frame in, run the same loop."
`runFrames`' missing `CALLCLOS` arms (S5 divergence) disappear because there
is only one switch.

No opcode changes. `YIELD` keeps its encoding; its *implementation* moves
from "a second copy of the loop" to "a result the loop returns."

**1.2 Cells, not pointers (upvalue model).**

Replace `CaptureRef{void* frame, int reg}` with
`CaptureRef{std::shared_ptr<Env>, int cell}` where
`Env = struct { std::vector<QLangValue> cells; }`.

- `MAKEFUNC`: bind each capture to a fresh cell seeded with the current
  register value (or, better, alias the frame register until first pop —
  implementation detail, decide during the rewrite; aliasing keeps
  write-through to the live frame free).
- Frame pop: captured registers are **moved** into the owning `Env` (O(1)
  per value — fat values move as shared pointers/scalars).
- `REFLOAD`/`REFSTORE`: `*env->cells[i]` / `= r[a]`.

Both `CALLFN` and `CALLCLOS` now use creation-time cell bindings — the S2
rebinding code is deleted, and S1's dangling pointer cannot exist (the cell
outlives the frame by construction).

**1.3 Generators as instances.**

Delete `m_generatorFrames`. A generator value owns its state:

```cpp
struct GeneratorState {
  int funcIndex;
  std::shared_ptr<Env> env;      // captures (from 1.2)
  std::vector<QLangValue> regs;  // saved register file (moved, not copied)
  size_t pc;
  bool exhausted;
};
using GeneratorValuePtr = std::shared_ptr<GeneratorState>;
```

`MAKEGEN` allocates a fresh instance. `next(g)` / `CALLCLOS` on a generator:
move `regs` onto the live frame, run the single loop (1.1) until
`RETURN`/`YIELD`/limit, move the frame back. S3 (instance clobbering) and S4
(2–3× deep copy per resume) are both gone; the raw-`this` lambda capture is
gone with it.

**Exit criteria:** Phase 0 tests S1/S2/S3 green; full suite green; perf
baseline at or above previous (generator-heavy scripts should *improve*).

### Phase 2 — One home for variables (global table)

The context owns the global table:

```cpp
struct GlobalTable {
  std::vector<QLangValue> slots;            // by name-index
  std::unordered_map<std::string, size_t> names;
  std::unordered_set<size_t> erased;        // per-slot, not per-register-index
};
```

- The main frame's registers **alias** the table (the VM reads/writes top-level
  names through it directly; `MOV`/`CONV`/`DELETEx` on global slots go
  through the table).
- `QLangContext::get/set/isset/remove` operate on the table. `m_vars`
  is deleted.
- **Delete** the start/end sync (`QLangVM.cpp:1370-1374, 1680-1691`) — P1's
  staleness class and the O(n) copy disappear.
- `m_deleted` (P2) is replaced by the per-slot `erased` set.
- `call()` no longer needs a compile-time `externalVars` snapshot (P3):
  script functions read the live table at runtime.
- `livePrograms` (P4): generators and finalizers hold a
  `std::shared_ptr<const ProgramImage>` instead of an `int` id;
  `currentProgramId()` and the int-index protocol are deleted; the
  unbounded-growth question becomes "does anything hold a stray reference" —
  answerable, and bounded by live script state.

**Exit criteria:** Phase 0 tests P1/P2/P3 green; full suite green; no
behavior change visible to scripts.

### Phase 3 — Host API hardening

**3.1 Type registry with stable handles.**
`registerType` returns a `std::shared_ptr<TypeDescriptor>` (or a registry
index with a stable vector). `clear()` no longer invalidates handles the
host legitimately holds. `ExternalRef` gains `const std::type_info*`;
`self()`/`selfMut()` check it and throw a clean `QLangError` on mismatch —
EXTERNAL_TYPES §10.2's "wrong weak_ptr is UB" becomes a runtime error. (H2, H4)

**3.2 Finalizer queue.**
The `shared_ptr` custom deleter no longer dispatches script. It enqueues
`(table, closure, program)`; the queue is drained at a safe point — start of
the next `run()`, or an explicit `ctx.processFinalizers()` the engine calls
per frame/tick. Re-entrancy guard moves to the drain. (H3)

**3.3 Release path for owned externals.**
A `release` builtin (or `__release__` member convention) drops the `owned`
shared_ptr, letting scripts free script-constructed objects. (H2 retention
leak)

**3.4 Context split.**
`QLangContext` (thin public API: scope, `setFunc`, `registerType`, `run`,
`call`) / program cache / type registry. Three classes, not a framework; the
public header surface stays source-compatible for existing embedders. (H1)

**Exit criteria:** finalizer tests (script finalizer runs exactly once, at a
drain point, never from a destructor on a foreign thread); type-mismatch
test throws the clean error; full suite green.

### Phase 4 — `.qlc` format + compiler hygiene

**4.1 Explicit on-wire encoding (F1).**
One shared encode/decode function per field, explicit little-endian byte
order on both writer and reader (no struct memcpy, no host-endian `putU32`).
Version field written and validated from a single `constexpr` shared by
codec and docs. **Bump the format version here** (the one and only bump in
this plan) and align `SERIALIZATION.md` with the code. Non-durable types get a
marker tag the reader **rejects with a clear error** instead of silently
becoming `Nil`; the capture-rebinding constraint (R4) is written into the
spec as an explicit load-time rule.

**4.2 Compiler statement split (F2).**
Mechanical: one private method per `StmtKind` arm, dispatch switch at the
top of `compileStmt`. No behavior change. Do this first within the phase —
it makes 4.3 and future features cheaper.

**4.3 Remove auto-capture (F3).**
The spec mandates explicit capture lists; the name-scanning augmentation
(`QLangCompiler.cpp:775-922`) is deleted. `global` resolves by name against
the global table (Phase 2), killing the register-0 fallback
(`:2372`). This is a **compile-time tightening**: scripts that relied on
implicit capture will now fail to compile — audit the test scripts and
examples; update any that need it (expected: few, since the parser already
required the explicit list).

**4.4 Parser depth limit (F4).**
A depth counter in the recursive-descent entry points; past ~256, throw a
clean `QLangError` ("nesting too deep"). ~20 lines; closes the
stack-overflow DoS.

**4.5 Constant pool (F5).**
Drop the string-key dedup (it's an optimization, not a requirement) — pool
index per literal. If dedup is wanted later, hash to a fixed 64-bit digest.

**Exit criteria:** `.qlc` round-trip tests green including the
reject-non-durable case; cross-encoding test (write with old reader rejected
by version); full suite green; `SERIALIZATION.md` updated and compliance
tracker re-hashed *after* tests pass (per AGENTS.md: never bump a hash
before testing).

### Phase 5 — Paper cuts

- **Step budget (F6):** the context owns the budget; VMs decrement it (not
  reset it) per instruction; expose `stepsUsed()`/`budget()` on the
  context. Makes `EVENT_HANDLERS.md` §8.1's budget story reachable from C++.
  Update the doc accordingly.
- **Threading contract (H5):** document "one context, one thread" in
  `docs/spec/07-api.md` + a debug-build thread-id assertion in
  `QLangContext`. No model built.
- **CMake (F7):** `install()` + export rules so embedding doesn't require a
  source build; replace `file(GLOB)` with an explicit list.

---

## 5. What we are *not* doing

- **No value-representation rewrite (F8).** The 19-alternative `std::variant`
  is a performance ceiling, not a correctness defect. A compact tagged
  representation (16-byte boxed values, interning) is a later perf project
  with its own plan; at POC scale the current representation is acceptable
  and rewriting it now would double the risk of Phase 1 for no correctness
  gain.
- **No GC, no JIT, no threading model, no async.**
- **No spec/opcode/tag changes in Phases 0–3.** The opcode set, value tags,
  and `.qlc` version are untouched until Phase 4's single version bump.
- **No language semantics changes.** The explicit-capture tightening (4.3)
  is a *compilation* tightening toward the existing spec, not a new rule.

---

## 6. Verification & compliance

- Every phase: `cmake -B build && cmake --build build && ctest --test-dir
  build` green, plus `tests/perf` at/above `baseline.json` (Phase 1 should
  improve generator-heavy numbers).
- `docs/compliance/README.md` is re-hashed per AGENTS.md **after** each
  phase's tests pass — never before.
- Phase 4 updates `docs/SERIALIZATION.md` (version, encoding rules,
  non-durable rejection, R4 load-time constraint) and the compliance module
  it maps to.
- Phase 3 updates `docs/EXTERNAL_TYPES.md` §10.2 (UB → clean error) and
  `docs/EVENT_HANDLERS.md` §8 (budget reachable; finalizer drain point).

## 7. Sequencing & effort

| Phase | Effort | Risk | Depends on |
|-------|--------|------|------------|
| 0 — failing regression tests | days | low | — |
| 1 — substrate rewrite (1.1 + 1.2 + 1.3) | 2–4 weeks | **high** (core VM) | 0 |
| 2 — global table | days–week | medium | 1 |
| 3 — host API hardening | days–week | medium | 2 |
| 4 — `.qlc` + compiler hygiene | days–week | low–medium | 2 |
| 5 — paper cuts | days | low | 3, 4 |

Phase 1 is the only high-risk phase; it is bounded by the Phase 0 tests and
the existing suite. Phases 4 and 5 are independent of each other and can run
in parallel once 2 is done.

## 8. Open decisions (resolve during Phase 1 design)

1. Cell aliasing vs. copy-at-`MAKEFUNC` (1.2): aliasing the frame register
   until first pop keeps live write-through free; copy-at-create is simpler
   and cheaper to reason about. Decide with a micro-benchmark.
2. Global-table aliasing mechanism (Phase 2): main frame *is* the table's
   slot vector (zero indirection) vs. a per-instruction indirection through
   the table. The former is faster; the latter is simpler for `delete`
   semantics.
3. Finalizer drain point (3.2): automatic at `run()` start only, or also a
   per-tick engine hook. The engine contract (`EVENT_HANDLERS.md`) decides.
