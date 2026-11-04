## Tests

Validates spec module [`docs/spec/15-tests.md`](../spec/15-tests.md)
(SHA256 `dd9c8fe38cfe2167fb66ed032e63235ecc0a9a938934c225bdf83b7a381e6c23`).
New module, Round 32 (2026-11-02); content from `docs/plans/test-migration-plan.md`
Phases 0–5, not from the legacy monolith.

### Requirement conformance

| Requirement (15-tests.md) | Status | Evidence |
|---|---|---|
| All test sources and scripts reside in `tests/` | ✓ | `tests/unit/` (20 C++ harnesses), `tests/scripts/` (19 `.ql`), `tests/perf/`; `examples/` holds demos only — `hello.cpp` (smoke app) + `qlang_run.cpp` (runner) |
| Tests run after every task (regression gate) | ✓ | ctest is the gate (AGENTS.md "Build & test"); CI on every push to `main` |
| Regression tests exist | ✓ | 19 script regressions (`script_*`, driven by `qlang_run`) + all 20 unit suites |
| Unit tests exist (features + edge cases) | ✓ | 20 unit suites — see ctest matrix below |
| Performance tests: immutable baseline for parser/compiler/execution time, logged and compared | ✓ | `tests/perf/perf_bench.cpp` + committed `tests/perf/baseline.json`; ctest `perf` gate (±20% / 0.5 ms) |
| GitHub CI: library + smoke app + test suite against `msvc`, `g++`, `mingw`, `wasm`, `clang` | ✓ | three workflows, per the spec's "three separate ci/cd yaml" — see CI matrix |
| Running on C++20 | ✓ | `CMAKE_CXX_STANDARD 20` (root `CMakeLists.txt`) |

### Layout (verified 2026-11-04)

```
tests/
  unit/     spec_check.cpp spec_check2.cpp spec_cast_test.cpp gen_test.cpp
            range_check.cpp codec_check.cpp encoding_test.cpp
            class_str_test.cpp class_static_test.cpp
            test_module.cpp test_external.cpp test_inject.cpp test_dollar_operator.cpp
            test_native.cpp test_call.cpp
            phase0_scope.cpp phase0_api.cpp phase2_scope.cpp phase3_api.cpp
            phase5_api.cpp
  scripts/  cast.ql cmp.ql encode_repro.ql ext_consts.ql ext_regs.ql
            hello.ql helpers.ql jump_widths.ql len.ql money.ql
            moneyv2.ql perf_stress.ql test_assert.ql test_assert2.ql test_cast.ql
            test_cast2.ql test_minmax.ql test_minmax2.ql type.ql
  perf/     perf_bench.cpp baseline.json
```

`examples/` retains only real demos: `hello.cpp` (smoke app) and `qlang_run.cpp`
(script runner / smoke-test app).

### ctest matrix — 41 entries (41/41 green, 2026-11-04)

Unit (20):

| ctest | binary | source | result |
|---|---|---|---|
| `smoke` | `qlang_example` | `examples/hello.cpp` | `VM result: OK` |
| `spec_check` | `spec_check` | `tests/unit/spec_check.cpp` | 33 passed |
| `spec_check2` | `spec_check2` | `tests/unit/spec_check2.cpp` | 463 passed (incl. 42 scope/boolean/trace cases added Round 32) |
| `spec_cast_test` | `spec_cast_test` | `tests/unit/spec_cast_test.cpp` | 20 passed |
| `test_module` | `test_module` | `tests/unit/test_module.cpp` | 7 tests OK |
| `gen_test` | `gen_test` | `tests/unit/gen_test.cpp` | 5 passed |
| `test_native` | `test_native` | `tests/unit/test_native.cpp` | `ALL PASS` |
| `test_call` | `test_call` | `tests/unit/test_call.cpp` | `ALL PASS` |
| `range` | `range_check` | `tests/unit/range_check.cpp` | 22 passed |
| `class_str` | `class_str_test` | `tests/unit/class_str_test.cpp` | 13 passed |
| `class_static` | `class_static_test` | `tests/unit/class_static_test.cpp` | 17 passed |
| `codec` | `codec_check` | `tests/unit/codec_check.cpp` | 85 checks passed |
| `encoding` | `encoding_test` | `tests/unit/encoding_test.cpp` | 41 checks passed |
| `external` | `test_external` | `tests/unit/test_external.cpp` | 23 checks passed |
| `test_inject` | `test_inject` | `tests/unit/test_inject.cpp` | `ALL PASS` |
| `test_dollar_operator` | `test_dollar_operator` | `tests/unit/test_dollar_operator.cpp` | 8 passed |
| `phase0_scope` | `phase0_scope` | `tests/unit/phase0_scope.cpp` | 8 checks (S1/S2/S3/P2 substrate regressions) |
| `phase0_api` | `phase0_api` | `tests/unit/phase0_api.cpp` | 3 checks (P1/P3/F1 API regressions) |
| `phase2_scope` | `phase2_scope` | `tests/unit/phase2_scope.cpp` | 7 checks (global-table scope) |
| `phase3_api` | `phase3_api` | `tests/unit/phase3_api.cpp` | 23 checks (handles, type-mismatch, finalizer queue, `release()`) |
| `phase5_api` | `phase5_api` | `tests/unit/phase5_api.cpp` | 17 checks (step budget, threading contract) |

Counted unit cases: **795** (463+33+20+7+5+22+13+17+23+8+85+41+8+3+7+23+17), plus
the three `ALL PASS` gate suites (`test_native`, `test_call`, `test_inject`).

Script (19): one `script_<name>` ctest entry per `tests/scripts/*.ql`, driven by
`qlang_run` (non-zero exit = fail): `cast`, `cmp`, `encode_repro`, `ext_consts`,
`ext_regs`, `hello`, `helpers`, `jump_widths`, `len`, `money`, `moneyv2`,
`perf_stress`, `test_assert`, `test_assert2`, `test_cast`, `test_cast2`,
`test_minmax`, `test_minmax2`, `type`.

Perf (1): `perf` — `perf_bench` compared against `baseline.json` (below).

### Performance baseline

- Harness `tests/perf/perf_bench.cpp` (links `libqlang.a`); fixed corpus:
  `hello.ql`, `money.ql`, `cast.ql`, `perf_stress.ql` (from `tests/scripts/`).
- Per stage (parse / compile / execute): one unmeasured warm-up pass per script,
  then N=5 measured runs; **median** taken.
- Result written to `build/perf_result.json` (schema `{script, stage, median_ms,
  runs_ms[]}`, array of objects — same schema as `baseline.json`).
- Gate: a stage fails when it exceeds the relative tolerance (default ±20%,
  overridable via `QL_PERF_TOLERANCE`) **and** the absolute floor (0.5 ms,
  `QL_PERF_EPS_MS`). The floor keeps the gate meaningful on sub-ms stages
  without flapping.
- `--update-baseline` regenerates `tests/perf/baseline.json` (a deliberate act,
  always a separate commit). `baseline.json` is the immutable reference
  (calibrated g++/Linux Release).
- Build-type handling: non-Release builds → `perf_bench` exits 77 → ctest reports
  **Skipped** (`set_tests_properties(perf PROPERTIES SKIP_RETURN_CODE 77)`).
  Cross-compiler jobs exclude the gate (`ctest -E perf`); the gxx job runs it and
  uploads `perf_result.json` as the `perf-result` CI artifact.

### CI matrix (three workflows, per spec)

| Workflow | Toolchain | Runner | Scope |
|---|---|---|---|
| `.github/workflows/ci.yml` | g++ (system) | ubuntu-latest | full build + full ctest (incl. `perf` gate) + `perf-result` artifact |
| `.github/workflows/ci.yml` | clang++ | ubuntu-latest | full build + ctest (`-E perf`) |
| `.github/workflows/ci.yml` | mingw-w64 cross g++ | ubuntu-latest | build-only (cross-compiled PE binaries cannot run on a Linux runner) |
| `.github/workflows/ci-msvc.yml` | MSVC x64 (VS 2022) | windows-latest | full build + ctest (`-E perf`); "Show compiler version" step feeds `docs/COMPILER_PORTABILITY.md` |
| `.github/workflows/ci-wasm.yml` | emscripten (emcc/em++) | ubuntu-latest | build `qlang` + `qlang_example` (`-sALLOW_MEMORY_GROWTH -fexceptions`); smoke via `node build/qlang_example.js`; full ctest not applicable (no filesystem in wasm) |

All workflows trigger on push to `main` only.

### Open items / limitations

- mingw job is **build-only**: executing Windows PE binaries on the Linux runner
  is not possible; a native-mingw execution job is not covered.
- wasm job is **smoke-only** by design: the script-level regression suite reads
  `.ql` files, which wasm has no filesystem for.
- `$$` indirect-operator harness cases remain pending (operator unimplemented —
  tracked in `status.md` open items, not in this module's spec).
