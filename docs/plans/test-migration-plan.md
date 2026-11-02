# QLang Test Migration & Coverage Plan

Satisfies spec module [`docs/spec/15-tests.md`](../spec/15-tests.md) and closes the
coverage gaps found in the 2026-11-01 spec↔compliance comparison.

Requirements being met (15-tests.md):

1. All test source files and scripts reside in `tests/`
2. Tests run after every task (regression gate)
3. Regression tests exist
4. Unit tests exist (feature + edge cases)
5. Performance tests with an immutable baseline (parser, compiler, execution time), logged and compared
6. GitHub CI testing the library, smoke test app, and test suite against VS (MSVC), g++, mingw, wasm, clang

Out of scope (tracked, not implemented here): `$$` indirect operator (fully
specified, no implementation), duplicate-declaration warning (deferred pending
warning-toggle infra). Both stay open items in compliance.

---

## Current state (verified 2026-11-01)

| Fact | Evidence |
|------|----------|
| No `tests/` directory exists | `ls tests` → not found |
| 12 real harnesses live in `examples/` | spec_check, spec_check2 (421), spec_cast_test (20), gen_test (5), range_check (22), codec_check, class_str_test (13), class_static_test (17), test_module (7), test_external (24), test_inject, test_dollar_operator |
| Only 6 of 12 registered in ctest | `smoke, range, class_str, class_static, codec, external`; spec_check2/spec_check/spec_cast_test/gen_test/test_module built but not `add_test`-ed |
| 15 orphan .cpp files | `debug_*` (5), `repro_finalizer`, `test_arr`, `test_call`, `test_events`, `test_native`, `test_gen_simple` — not in CMake |
| 15 `.ql` test scripts in `examples/scripts/` | cast.ql, test_minmax.ql, test_assert*.ql, … — run ad hoc via `qlang_run`; no code references the paths (verified by grep) |
| No perf infrastructure | none found |
| `qlang_run` already times parse/compile/execute stages | `--profile` flag, exit codes 0–5 documented |
| Single CI workflow | `.github/workflows/msvc.yml` (MSVC/windows-latest only; uncommitted edit adds `test_external`); referenced by `docs/COMPILER_PORTABILITY.md` |
| Compliance doc drift | 7 stale spec hashes, 1 stale harness hash, broken README table row, root stubs stale (details in Phase 0) |

---

## Phase 0 — Doc hygiene (prerequisite)

Per AGENTS.md: tests first, hashes last.

1. Refresh the 7 stale SHA256s in `docs/spec/README.md` (02, 03, 06, 09, 13, 14) and add the missing `15-tests` hash (`177097ef…`).
2. Delete the stray broken table row `|docs/spec/15-tests.md` (README line 30).
3. Refresh the stale `harness.md` hash in `docs/compliance/README.md` (`8809a199…`).
4. Root stubs: `SPEC.md` "12 domain modules" → 15; `SPEC_COMPLIANCE.md` "Round 29 (2026-10-28)" → Round 31 (2026-10-29).
5. `AGENTS.md`: "12 domain modules" → 15; drop the dead `docs/action_plan.md` reference (file missing).
6. `.gitignore`: add root-level `*.qlc`, `*.qla`, `examples/qla/` (untracked build artifacts polluting `git status`).

**Commit:** `docs: refresh stale spec/compliance hashes, fix README table, gitignore build artifacts`

## Phase 1 — `tests/` layout + migration

Target layout:

```
tests/
  unit/            # all C++ harnesses
    spec_check.cpp spec_check2.cpp spec_cast_test.cpp gen_test.cpp
    range_check.cpp codec_check.cpp class_str_test.cpp class_static_test.cpp
    test_module.cpp test_external.cpp test_inject.cpp test_dollar_operator.cpp
  scripts/         # all .ql test scripts (moved from examples/scripts/)
  perf/            # Phase 4
```

Steps:

1. `git mv` the 12 harnesses `examples/*.cpp` → `tests/unit/`.
2. `git mv examples/scripts/*.ql` → `tests/scripts/`.
3. `examples/` keeps only real demos: `hello.cpp` (smoke app), `qlang_run.cpp` (runner).
4. CMakeLists.txt:
   - repoint the 9 existing `add_executable` paths to `tests/unit/`;
   - add missing `add_executable` + `add_test` for `spec_check2`, `spec_check`, `spec_cast_test`, `gen_test`, `test_module`, `test_inject`, `test_dollar_operator`.
   - ctest count: **6 → 13**.
5. Orphan files decision (confirm before executing):
   - `debug_*`, `repro_finalizer` → scratch; delete (or move to `examples/` untracked-by-CMake if preferred).
   - `test_arr`, `test_call`, `test_events`, `test_native`, `test_gen_simple` → review content; register the meaningful ones in CMake/ctest, delete the rest.
6. `AGENTS.md`: update "Build & test" and "CMake layout" sections to reference `tests/`.

Verification: `cmake -B build && cmake --build build && ctest --test-dir build` → 13/13 green.

**Commit:** `test: migrate harnesses + scripts to tests/, register all suites in ctest`

## Phase 2 — Script-level regression suite

Turns the ad-hoc `.ql` scripts into a gate (spec: "tests have to be run after every tasks").

1. CMake: `file(GLOB QL_SCRIPTS tests/scripts/*.ql)` → one `add_test(NAME script_<name> COMMAND qlang_run <script>)` per script (exit 0 = pass).
2. Keep `qlang_run` as the driver (already the "smoke test app" the CI requirement names).
3. Result: ~15 additional ctest entries; ctest total ≈ 28.

**Commit:** `test: add script-level regression suite driven by qlang_run`

## Phase 3 — CI matrix (VS, g++, mingw, wasm, clang)

New `.github/workflows/ci.yml`, matrix:

| Job | Runner | Toolchain | Scope |
|-----|--------|-----------|-------|
| `vs` | windows-latest | MSVC (VS 2022) | full build + ctest; keep the "Show compiler version" step feeding `docs/COMPILER_PORTABILITY.md` |
| `gxx` | ubuntu-latest | g++ (system) | full build + ctest + **perf baseline compare** (only job where perf is gated — avoids cross-machine noise) |
| `clang` | ubuntu-latest | clang++ | full build + ctest |
| `mingw` | windows-latest | MSYS2 mingw64 g++ | full build + ctest |
| `wasm` | ubuntu-latest | emsdk (emcc/em++) | build `libqlang.a` + `hello` with `-sALLOW_MEMORY_GROWTH`; run smoke via `node hello.js` if linking succeeds; full ctest documented as not applicable (no FS in wasm) |

- Each job: `cmake -B build && cmake --build build && ctest --test-dir build`.
- Fold in the uncommitted `msvc.yml` change (`test_external` target); **delete `msvc.yml`** once `ci.yml` is green; update its three references in `docs/COMPILER_PORTABILITY.md`.
- Perf result JSON uploaded as an artifact from the `gxx` job.

**Commit:** `ci: 5-toolchain matrix (vs/g++/clang/mingw/wasm) + perf gate`

## Phase 4 — Performance baseline

Spec: baseline for `parser`, `compiler`, `execution time` — immutable, logged, compared.

1. New `tests/perf/perf_bench.cpp` (links `libqlang.a`):
   - fixed corpus: `hello.ql`, `money.ql`, `cast.ql` + new `tests/scripts/perf_stress.ql` (fixed workload: nested loops, string ops, table/array churn — deterministic, no `rand`);
   - per stage (parse / compile / execute): N=5 runs, take **median**;
   - writes `perf_result.json` (schema: `{script, stage, median_ms, runs_ms[]}`);
   - compare vs `tests/perf/baseline.json`; fail if any stage exceeds tolerance (default **±20%**, overridable via `QL_PERF_TOLERANCE`);
   - `--update-baseline` flag regenerates `baseline.json` (deliberate act, always a separate commit).
2. Generate the initial `baseline.json` locally (g++), commit it — it is the immutable reference.
3. ctest entry `perf` (runs in the `gxx` CI job; locally optional).
4. Logging: bench prints the per-stage table to stdout; CI artifact carries the JSON.

**Commit:** `test: perf baseline harness + baseline.json`

**Done (2026-11-02).** Implementation notes / deviations:

- `tests/perf/perf_bench.cpp` (links `qlang`): corpus `hello.ql`, `money.ql`,
  `cast.ql`, `perf_stress.ql`; N=5 measured runs per stage **after one
  unmeasured warm-up pass per script** (warm-up stabilizes sub-ms stages).
- Gate rule: a stage fails when it exceeds the relative tolerance (±20%,
  `QL_PERF_TOLERANCE`) **and** an absolute floor (0.5 ms,
  `QL_PERF_EPS_MS`). The epsilon floor is an addition to the plan: pure
  ±20% on 30 µs stages flapped ±60% on the same machine; the floor keeps the
  gate meaningful without flapping.
- `perf_result.json` / `baseline.json` share the schema
  `{script, stage, median_ms, runs_ms[]}` (array of objects).
- ctest entry `perf`; the baseline is calibrated for g++/Linux Release, so:
  non-Release builds → `perf_bench` exits 77 and ctest reports Skipped
  (`SKIP_RETURN_CODE 77`); `clang` (ci.yml) and MSVC (ci-msvc.yml) jobs
  exclude `perf` (`ctest -E perf`) to avoid cross-compiler/cross-OS noise;
  the gxx job runs the gate and uploads `perf_result.json` as artifact
  `perf-result`.
- `perf_stress.ql` raises the VM step guard via `set_max_steps(10000000)`
  (workload deliberately exceeds the 100k default); it is also part of the
  script regression suite (16 scripts).
- ctest total: 30 entries (13 unit + 16 script + perf).

## Phase 5 — Extra coverage (test gaps)

New cases (extend `tests/unit/` suites; audit the existing 421 `spec_check2` cases first to avoid duplicates):

1. **Scope (spec 14):** function-scope isolation — `a ++` inside a function must not touch global `a` (the 2026-11-01 spec example); block-scope re-declaration in `for`/`while` bodies already covered — verify, add missing variants.
2. **Boolean evaluation table (spec 03):** truthiness of generator (true), function/lambda by return value, empty string/array/table/blob (false), nil (false) — add any uncovered rows.
3. **Errors/trace (spec 12):** nested-frame trace rendering beyond the 6 Round-29 cases (deep call chain already partial).
4. **Deliberately no cases:** `ja/jae/jb/jbe` (spec: not implemented), decompiler mnemonics (spec: do not implement), `$$` (unimplemented — harness cases remain pending per compliance).
5. Orphan-file triage from Phase 1.5 may add registered suites.

**Commit:** `test: add scope/boolean/trace coverage cases`

## Phase 6 — Compliance Round 32

Per AGENTS.md (test first, then hashes):

1. New `docs/compliance/tests.md` module — validates spec `15-tests.md` (layout, ctest matrix, perf baseline, CI matrix) with SHA256 tracking; add row to `docs/compliance/README.md`.
2. `status.md`: new totals (ctest 13 unit + ~15 script + perf), harness list updated to `tests/unit/` paths.
3. `history.md`: append-only Round 32 entry.
4. Re-verify every hash in both README tables with `sha256sum`; fix the root `SPEC_COMPLIANCE.md` stub's last-audit line.
5. `docs/spec/README.md`: "12" → "15" if not done in Phase 0.

**Commit:** `docs: compliance Round 32 — tests migration, CI matrix, perf baseline`

---

## Definition of done

- [ ] `tests/` contains all test sources + scripts; `examples/` holds demos only
- [ ] `ctest --test-dir build` ≥ 28 entries, all green, locally reproducible
- [ ] `ci.yml` green on all 5 toolchains (wasm = build + node smoke)
- [ ] `tests/perf/baseline.json` committed; `perf` ctest entry compares against it
- [ ] New coverage cases landed (scope, boolean table, trace)
- [ ] Compliance Round 32 recorded; all README hashes re-verified
- [ ] `git status` clean of build artifacts (`.gitignore` updated)

## Risks / notes

- **wasm:** C++20 lib under emscripten may need small portability patches; if linking fails, degrade the job to build-only and record the blocker in `docs/COMPILER_PORTABILITY.md`.
- **mingw:** MSYS2 setup adds job time; use the standard `mingw-w64` CMake package.
- **Perf tolerance:** CI machines vary; median-of-5 + ±20% keeps the gate meaningful without flapping.
- **No `.clang-format` file exists at repo root** despite AGENTS.md referencing one — new C++ follows Google style per AGENTS.md; optionally add a `.clang-format` (Google base) in Phase 1.
- Per AGENTS.md: commit names are suggestions; stage by concern; no auto-commits.

## Order & rough sizing

Phase 0 (small) → 1 (core, ~half the work) → 2 (small) → 3 (medium) → 4 (medium) → 5 (small) → 6 (small). Each phase ends green; no phase depends on a later one.
