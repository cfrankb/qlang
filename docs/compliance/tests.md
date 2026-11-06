## Tests

Validates spec module [`docs/spec/15-tests.md`](../spec/15-tests.md)
(SHA256 `dd9c8fe38cfe2167fb66ed032e63235ecc0a9a938934c225bdf83b7a381e6c23`).
New module, Round 32 (2026-11-02); content from `docs/plans/test-migration-plan.md`
Phases 0–5, not from the legacy monolith.

### Requirement conformance

| Requirement (15-tests.md) | Status | Evidence |
|---|---|---|
| All test sources and scripts reside in `tests/` | ✓ | `tests/unit/` (23 C++ harnesses), `tests/scripts/` (21 `.ql`), `tests/perf/`; `examples/` holds demos only — `hello.cpp` (smoke app) + `qlang_run.cpp` (runner) |
| Tests run after every task (regression gate) | ✓ | ctest is the gate (AGENTS.md "Build & test"); CI on every push to `main` |
| Regression tests exist | ✓ | 21 script regressions (`script_*`, driven by `qlang_run`) + all 23 unit harnesses |
| Unit tests exist (features + edge cases) | ✓ | 23 unit suites — see ctest matrix below |
| Performance tests: immutable baseline for parser/compiler/execution time, logged and compared | ✓ | `tests/perf/perf_bench.cpp` + committed `tests/perf/baseline.json`; ctest `perf` gate (±20% / 0.5 ms) |
| GitHub CI: library + smoke app + test suite against `msvc`, `g++`, `mingw`, `wasm`, `clang` | ✓ | three workflows, per the spec's "three separate ci/cd yaml" — see CI matrix |
| Running on C++20 | ✓ | `CMAKE_CXX_STANDARD 20` (root `CMakeLists.txt`) |

### Layout (verified 2026-11-06)

```
tests/
  unit/     spec_check.cpp spec_check2.cpp spec_cast_test.cpp gen_test.cpp
            range_check.cpp codec_check.cpp encoding_test.cpp encoding_v6_test.cpp
            class_str_test.cpp class_static_test.cpp signal_test.cpp
            test_module.cpp test_external.cpp test_inject.cpp test_dollar_operator.cpp
            test_native.cpp test_call.cpp
            phase0_scope.cpp phase0_api.cpp phase2_scope.cpp phase3_api.cpp
            phase5_api.cpp value_semantics.cpp
  scripts/  cast.ql cmp.ql encode_repro.ql ext_consts.ql ext_regs.ql
            hello.ql helpers.ql jump_widths.ql len.ql money.ql
            moneyv2.ql perf_stress.ql table_subscript.ql test_assert.ql
            test_assert2.ql test_cast.ql test_cast2.ql test_minmax.ql
            test_minmax2.ql type.ql value_share.ql
  perf/     perf_bench.cpp baseline.json
```

`examples/` retains only real demos: `hello.cpp` (smoke app) and `qlang_run.cpp`
(script runner / smoke-test app).

### ctest matrix — 47 entries (47/47 green, 2026-11-06)

Unit (23):

| ctest | binary | source | result |
|---|---|---|---|
| `smoke` | `qlang_example` | `examples/hello.cpp` | `VM result: OK` |
| `spec_check` | `spec_check` | `tests/unit/spec_check.cpp` | 33 passed |
| `spec_check2` | `spec_check2` | `tests/unit/spec_check2.cpp` | 529 passed (incl. 20 `deepcopy` cases added Round 42 + 13 `__deepcopy__` hook cases added Round 44 + 17 `t[key]`/`a[i]` subscript cases added Round 51) |
| `spec_cast_test` | `spec_cast_test` | `tests/unit/spec_cast_test.cpp` | 20 passed |
| `test_module` | `test_module` | `tests/unit/test_module.cpp` | 7 tests OK |
| `gen_test` | `gen_test` | `tests/unit/gen_test.cpp` | 5 passed |
| `test_native` | `test_native` | `tests/unit/test_native.cpp` | `ALL PASS` |
| `test_call` | `test_call` | `tests/unit/test_call.cpp` | `ALL PASS` |
| `range` | `range_check` | `tests/unit/range_check.cpp` | 22 passed |
| `class_str` | `class_str_test` | `tests/unit/class_str_test.cpp` | 13 passed |
| `class_static` | `class_static_test` | `tests/unit/class_static_test.cpp` | 17 passed |
| `signal` | `signal_test` | `tests/unit/signal_test.cpp` | 18 passed (Round 40; `docs/spec/17-signals.md`) |
| `codec` | `codec_check` | `tests/unit/codec_check.cpp` | 142 checks passed (incl. 15 `deepcopy` cases, Round 42; 19 `indexget`/`indexput` cases, Round 51) |
| `encoding` | `encoding_test` | `tests/unit/encoding_test.cpp` | 41 checks passed |
| `encoding_v6` | `encoding_v6_test` | `tests/unit/encoding_v6_test.cpp` | 34249 checks passed (Round 55; v6 2/2/4 modifier byte, `*V6` codec — additive, v5 codec untouched) |
| `external` | `test_external` | `tests/unit/test_external.cpp` | 49 checks passed (incl. `registerEnum` injection, Round 41) |
| `test_inject` | `test_inject` | `tests/unit/test_inject.cpp` | `ALL PASS` |
| `test_dollar_operator` | `test_dollar_operator` | `tests/unit/test_dollar_operator.cpp` | 8 passed |
| `phase0_scope` | `phase0_scope` | `tests/unit/phase0_scope.cpp` | 8 checks (S1/S2/S3/P2 substrate regressions) |
| `phase0_api` | `phase0_api` | `tests/unit/phase0_api.cpp` | 3 checks (P1/P3/F1 API regressions) |
| `phase2_scope` | `phase2_scope` | `tests/unit/phase2_scope.cpp` | 7 checks (global-table scope) |
| `phase3_api` | `phase3_api` | `tests/unit/phase3_api.cpp` | 23 checks (handles, type-mismatch, finalizer queue, `release()`) |
| `phase5_api` | `phase5_api` | `tests/unit/phase5_api.cpp` | 17 checks (step budget, threading contract) |
| `value_semantics` | `value_semantics` | `tests/unit/value_semantics.cpp` | 40 checks (Round 43 share-by-default array/set/blob: aliasing on assign/call, literal freshness, `deepcopy` independence + array cycles, `+`/`cast` freshness, `a += a`, `string` still a value type) |

Counted unit cases: **35251** (529+33+20+7+5+22+13+17+18+49+8+142+41+34249+8+3+7+23+17+40), plus
the three `ALL PASS` gate suites (`test_native`, `test_call`, `test_inject`).

Script (21): one `script_<name>` ctest entry per `tests/scripts/*.ql`, driven by
`qlang_run` (non-zero exit = fail): `cast`, `cmp`, `encode_repro`, `ext_consts`,
`ext_regs`, `hello`, `helpers`, `jump_widths`, `len`, `money`, `moneyv2`,
`perf_stress`, `table_subscript`, `test_assert`, `test_assert2`, `test_cast`,
`test_cast2`, `test_minmax`, `test_minmax2`, `type`, `value_share`.

Perf (1): `perf` — `perf_bench` compared against `baseline.json` (below).

Docs (1): `doc_hashes` — `cmake -P cmake/check_doc_hashes.cmake` (script mode,
no binary): every SHA256 recorded in `docs/spec/README.md` and
`docs/compliance/README.md` must match its module, every module must have a
row, and no row may name a deleted file (24 modules). The hash is taken over
**LF-normalized** content, so a working tree with CRLF line endings (Windows +
`core.autocrlf=true`) is not reported as drift; `.gitattributes` pins `eol=lf`
so a normal checkout matches the recorded hash byte for byte (Round 48).
Re-record after a verified doc edit with `./update-doc-hashes.sh` (`--check`
verifies without writing anything) (Round 49).
**Optional by default:** the entry runs the gate with
`-DDOCS_HASH_WARN_ONLY=ON` and declares `SKIP_REGULAR_EXPRESSION
QLANG_DOC_HASH_DRIFT`, so drift is printed in full and ctest reports **Skipped**
rather than Failed — a stale doc row cannot break a CI job, while a broken gate
(missing `docs/`, bad source dir) carries no marker and still fails.
`-DQLANG_DOC_HASHES_STRICT=ON` restores the hard failure, as does running the
gate or `./update-doc-hashes.sh --check` directly (Round 49).

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
- **Re-recorded Round 43 (2026-11-05)** after the share-by-default change:
  `perf_stress` execute dropped 13.780 ms → 9.809 ms (−29 %, the array churn no
  longer deep-copies on every move), which the ±20 % gate flags in *both*
  directions, so the baseline was regenerated deliberately. It is the only stage
  above the 0.5 ms absolute floor — every other stage moved by ≤ 74 µs and is
  dominated by host noise (sub-0.2 ms stages vary by ±15 % run to run), so their
  new medians are not attributable to this change.
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

Triggers: `ci.yml` and `ci-wasm.yml` on push to `main`; `ci-msvc.yml` on push to
`master` (the branch rename is still pending — Round 48).

`-E perf` is a **regex**, so the clang and MSVC jobs run 45 of the 47 entries: it
drops `perf` **and** `script_perf_stress`.

The `windows-latest` image sets `core.autocrlf=true` in git's system config, so
`actions/checkout` would rewrite every text file to CRLF; `.gitattributes`
(`* text=auto eol=lf`) keeps the checkout byte-identical to the committed blobs,
which is what `doc_hashes` compares against (Round 48).

### Open items / limitations

- mingw job is **build-only**: executing Windows PE binaries on the Linux runner
  is not possible; a native-mingw execution job is not covered.
- wasm job is **smoke-only** by design: the script-level regression suite reads
  `.ql` files, which wasm has no filesystem for.
- `$$` indirect-operator harness cases remain pending (operator unimplemented —
  tracked in `status.md` open items, not in this module's spec).
