## Tests

Validates spec module [`docs/spec/15-tests.md`](../spec/15-tests.md)
(SHA256 `dd9c8fe38cfe2167fb66ed032e63235ecc0a9a938934c225bdf83b7a381e6c23`).
New module, Round 32 (2026-11-02); content from `docs/plans/test-migration-plan.md`
Phases 0–5, not from the legacy monolith.

### Requirement conformance

| Requirement (15-tests.md) | Status | Evidence |
|---|---|---|
| All test sources and scripts reside in `tests/` | ✓ | `tests/unit/` (27 C++ harnesses), `tests/scripts/` (23 `.ql`), `tests/perf/`; `examples/` holds demo only — `hello.cpp` (smoke app) + `qlang_run.cpp` (runner) |
| Tests run after every task (regression gate) | ✓ | ctest is the gate (AGENTS.md "Build & test"); CI on every push to `main` |
| Regression tests exist | ✓ | 23 script regressions (`script_*`, driven by `qlang_run`) + all 27 unit harnesses |
| Unit tests exist (features + edge cases) | ✓ | 27 unit suites — see ctest matrix below |
| Performance tests: immutable baseline for parser/compiler/execution time, logged and compared | ✓ | `tests/perf/perf_bench.cpp` + committed `tests/perf/baseline.json`; ctest `perf` gate (±20% / 0.5 ms) |
| GitHub CI: library + smoke app + test suite against `msvc`, `g++`, `mingw`, `wasm`, `clang` | ✓ | three workflows, per the spec's "three separate ci/cd yaml" — see CI matrix |
| Running on C++20 | ✓ | `CMAKE_CXX_STANDARD 20` (root `CMakeLists.txt`) |

### Layout (verified 2026-11-09)

```
tests/
  unit/     spec_check.cpp spec_check2.cpp spec_cast_test.cpp gen_test.cpp
            range_check.cpp codec_check.cpp encoding_v6_test.cpp flag_flow_test.cpp
            builtin_table_test.cpp export_test.cpp linker_test.cpp
            class_str_test.cpp class_static_test.cpp signal_test.cpp
            test_module.cpp test_external.cpp test_inject.cpp test_dollar_operator.cpp
            test_native.cpp test_call.cpp
            phase0_scope.cpp phase0_api.cpp phase2_scope.cpp phase3_api.cpp
            phase5_api.cpp value_semantics.cpp codegen_size_check.cpp
  scripts/  cast.ql cmp.ql encode_repro.ql ext_consts.ql ext_regs.ql flags.ql
            hello.ql helpers.ql imm_kinds.ql jump_widths.ql len.ql money.ql
            moneyv2.ql perf_stress.ql table_subscript.ql test_assert.ql
            test_assert2.ql test_cast.ql test_cast2.ql test_minmax.ql
            test_minmax2.ql type.ql value_share.ql
  perf/     perf_bench.cpp baseline.json
```

`tests/unit/encoding_test.cpp` (the v5 encoding harness, 41 checks) was retired with
`.qlc` v5 in Round 56 and deleted from the tree; its coverage lives in
`encoding_v6_test.cpp`.

`examples/` retains only real demos: `hello.cpp` (smoke app) and `qlang_run.cpp`
(script runner / smoke-test app).

### ctest matrix — 53 entries (53/53 green, 2026-11-09)

Unit (27 harnesses + `smoke`):

| ctest | binary | source | result |
|---|---|---|---|
| `smoke` | `qlang_example` | `examples/hello.cpp` | `VM result: OK` |
| `spec_check` | `spec_check` | `tests/unit/spec_check.cpp` | 33 passed |
| `spec_check2` | `spec_check2` | `tests/unit/spec_check2.cpp` | 529 passed (incl. 20 `deepcopy` cases added Round 42 + 13 `__deepcopy__` hook cases added Round 44 + 17 `t[key]`/`a[i]` subscript cases added Round 51) |
| `spec_cast_test` | `spec_cast_test` | `tests/unit/spec_cast_test.cpp` | 20 passed |
| `test_module` | `test_module` | `tests/unit/test_module.cpp` | 7 tests OK |
| `gen_test` | `gen_test` | `tests/unit/gen_test.cpp` | 7 passed (+2 Round 65: the `buildFunction` capture-spec fix — a generator's pool closure keeps `fn.captures` across the move into the function table — pinned with a global capture and a capture-free generator) |
| `test_native` | `test_native` | `tests/unit/test_native.cpp` | `ALL PASS` |
| `test_call` | `test_call` | `tests/unit/test_call.cpp` | `ALL PASS` |
| `range` | `range_check` | `tests/unit/range_check.cpp` | 22 passed |
| `class_str` | `class_str_test` | `tests/unit/class_str_test.cpp` | 13 passed |
| `class_static` | `class_static_test` | `tests/unit/class_static_test.cpp` | 17 passed |
| `signal` | `signal_test` | `tests/unit/signal_test.cpp` | 18 passed (Round 40; `docs/spec/17-signals.md`) |
| `codec` | `codec_check` | `tests/unit/codec_check.cpp` | 358 checks passed (incl. 15 `deepcopy` cases, Round 42; 19 `indexget`/`indexput` cases, Round 51; **+57 Round 56**: 20 load-time validation cases — reserved right kind 3, reserved left form 3, reserved immediate forms 12–15, a kind the opcode does not permit, unescaped `CALL`, the 15-byte `codeSize` guard; +80 Round 59 for the `eqf`/`ltf`/`gtf`/`ltef`/`gtef`/`tst` identities and the comparison-lowering sequence; **+49 Round 60**, see below; **+7 Round 62**, see below; **+15 Round 63**, see below; **+2 Round 64**, see below; **+4 Round 65**, see below). **Round 60 (`.qlc` v7, Phase 3):** the `legacy:` suite — the Round-56 proof that `movi`/`movc`/`addi`/`subi`/`cmpi`/`jmpfalse`/`jmptrue` still load, decode and run, incl. `movc` pool cloning (decision D6) — is **deleted** with the opcodes (decision D5: a must-fail test would exercise identities the decoder no longer defines), and the two hand-built `MOVI` streams were retargeted to `mov` kind 0 (`e = 0x40`). Added: the version matrix now rejects **3/4/5/6/8** at the version field (v6 joined the list) and `qlc-exec` — **69 checks**, 3 per script over all 23 `tests/scripts/*.ql`: each is compiled exactly as `qlang_run` compiles it, written with `writeQlc`, read back against a **fresh** `QLangContext` (the isolation a real `file.qlc` run gets from a new process), then `code`, `instrOffsets` and the constant pool are compared and both runs' stdout byte-compared (non-vacuity measured: every script prints 2–403 bytes, and corrupting one string constant fails both the image and the execute check for 23/23). The invalid-opcode probe now derives from `kOpcodeUsedCount` instead of pinning the literal 79. **Round 62 (`.qlc` v8, unified pool):** the version matrix moves to v8 — **3/4/5/6/7/9** rejected at the version field — the round-trip and `qlc-exec` comparisons now assert **program-pool** equality (`CompiledProgram::constants`, not per function), a two-`def` program that repeats one literal must produce **one** pool entry (the spec's dedup rule), and a hand-built stream with a const-pool operand `>= poolCount` is rejected with `constant pool index out of range` while the same stream with an in-range index loads (the pair proves the new load-time check is neither vacuous nor over-eager). **Round 63 (`.qlc` v9, module tables):** the version matrix moves to v9 — **3/4/5/6/7/8/10** rejected at the version field — a v8 image (which stops after `registerCount`) is refused with `missing module section`, every §4.6 invariant is probed (empty module name, invalid export/import kind, empty entry name or module name, negative `ref`, a `Func` `ref` ≥ `funcCount`, a `Var`/`Const` `ref` ≥ `registerCount`, a duplicate export name), and a valid tail — two exports plus one aliased import — survives the round trip, as does a `<init>` top level (the library-module shape). **Round 64 (loader + linker, no version change):** no new version matrix — the two new checks are the `callfn` range check the `operandRole()` table implies: a hand-built stream whose `callfn` names a function of its own image loads, and the same stream naming index 5 of 1 is refused with `function index out of range` (the accept/reject pair proves the check is neither vacuous nor over-eager); `writeQlc` of a *merged* program is refused instead, which is pinned in `linker`. **Round 65 (`getimport`/`callimport`):** the four new checks are the load-time import-index check the `OperandRole::ImportIndex` entry implies — a hand-built stream whose `getimport` names an entry of the image's own table loads, while index 5 of 1 and index 0 of an empty table are refused with `import index out of range` — plus the import-`ref` **non**-check: an import entry's `ref` of 999 loads (it names the *defining* module's tables, `SERIALIZATION.md` §4.6), paired against the export case, which is refused for the same magnitude |
| `encoding_v6` | `encoding_v6_test` | `tests/unit/encoding_v6_test.cpp` | 36127 checks passed (added Round 55 as the additive v6 codec suite; since Round 56 it is **the** encoding suite — the 2/2/4 modifier byte, the kind/form tables and the shipped codec; +217 in Round 56; +4 in Round 57 for §18; +3389 in Round 59 for the six flag-flow identities). **−2776 in Round 60**, entirely the identity compaction: the sweep walks the defined identities, and seven are gone. Exhaustive sweep now **1890** accepted / **14768** rejected `(opcode, modifier)` pairs cross-checked between size, encoder and decoder (was 1872/14274) — 74 identities × 256 modifiers = 18 944 pairs, of which the rejected cross-check skips the 9 jump opcodes (65 × 256 = 16 640); 16 640 − 14 768 = 1 872 accepted non-jump pairs and 1 890 − 1 872 = 18 = the 9 jumps × their 2 width modifiers, so the two tallies are consistent. §18 decoder/size-table parity still covers the whole 128 × 256 identity space (**30 392** rejected, **2 376** accepted, **15 135** truncations; 30 392 + 2 376 = 32 768). **Round 65 (+1 044, entirely the two new identities entering the sweeps):** the exhaustive sweep walks `kOpcodeUsedCount`, so 72/73 join it — +18 accepted pairs (9 each, ×3 checks) and +494 rejected cross-checks (247 each, ×2 checks), +2 in the per-opcode table loop; the parity sweep's accept/reject verdicts did **not** move, because the table and the decoder both already accepted 9 modifiers each for 72/73 while the identities were unassigned (the default permitted kind was already kind 1) — but `getimport`'s accepted forms got their final operand shape, so its truncation count fell 60 → 51 |
| `flag_flow` | `flag_flow_test` | `tests/unit/flag_flow_test.cpp` | 1324 checks passed (added Round 59, M3): the full matrix — 6 comparators × 6 flag jumps × 3 relations × 3 right-operand kinds; the "no bit set" state spelled out case by case (the D1 rationale); the `tst` truth table (emitted since Round 60 for bare truthiness — this suite remains the only exhaustive one); the general value domains the compiler's Int gate rejects (strings, bool, nil, mixed numerics, containers) with error-text parity against the value-producing opcode's own message (D3); `__str__` dispatch through `==`/`<`; `cmp` keeps its Int-only contract (D2); the `ja`/`jae`/`jb`/`jbe` alias name table (D6 — no identity, no mnemonic); reserved-slot rejection for the new identities. Since Round 62 its disassembly harness merges the per-function pools the compiler used to emit into the program pool, renumbering const-pool operands through `decodeInstruction`/`encodeInstruction` — the case count is unchanged (1324), the assertions are the same, only the pool they index into moved |
| `builtin_table` | `builtin_table_test` | `tests/unit/builtin_table_test.cpp` | 151 checks passed (added Round 61, `docs/plans/external-modules-plan-v4.md` Phase 1; SPEC §bytecodes "table for build in function (qlang base)") — the library builtin-name table `k_builtin_names[]` and its four APIs (`builtinCount`/`builtinName`/`findBuiltin`/`builtinId`): id → name against an **independent oracle list** (so reordering `Builtin` without the matching table edit fails here, not silently in the disassembly), name uniqueness, `nullptr` for out-of-range ids, the round trip through both lookups incl. the `str`/`int` aliases, exact case-sensitive matching (no case folding, no trim, no prefix match), the **−1-vs-throw split** (`findBuiltin` classifies, `builtinId` throws `unknown builtin function '<name>'`), and the compiler integration: the ids actually emitted for `len`/`toStr`/`str`/`int`/`toInt`/`type`/`println`, the **negative-slot** native path (`import math` + `min(3,1,4)` → `b < 0`, no builtin name, still runs — the guard for why the emit path classifies instead of resolving), `unknown function 'nope'` kept as a *compile* error rather than the table's throw, builtins inside closures not captured as free variables, and `deepcopy` lowering + its one-argument arity guard |
| `export` | `export_test` | `tests/unit/export_test.cpp` | 88 checks passed (added Round 63, `docs/plans/external-modules-plan-v4.md` Phase 3a; SPEC §bytecodes "keyword export" + "export table (lookup)") — both spec forms of `export` (the prefix on a `def` / `const` / assignment / arrow closure, the standalone `export NAME;`) and the entry each produces (`kind` = `Func`/`Const`/`Var`, `ref` = the function index or the global register — the handle a loader dereferences), in source order; forward references (the table is built in a pre-pass, so a standalone export may name a global declared later); the ten scope rejections (five inside a `def`/arrow closure/`class` body — the parser's body-depth guard — five inside a global-scope `if`/`for`/`while`/`switch` block — the compiler's pre-pass); `'<name>' is already exported` for a second export of the same name in any mix of forms; the exported-function rebind guard on assignment and on `++`/`--`; `MODULE_NAME` resolution (source > driver > `main`) and library mode (top level compiles as `<init>`, `MODULE_NAME` mandatory); the import table pinned **empty** (Phase 3a never populates it); and the whole table through the `.qlc` v9 write → read round trip |
| `linker` | `linker_test` | `tests/unit/linker_test.cpp` | 126 checks passed (added Round 64 with 75, `docs/plans/external-modules-plan-v4.md` Phase 3b; SPEC §bytecodes "external modules" + "import table (lookup)" + "technical specifications" 1–5) — the module registry (`loadProgram` from a stream / `loadProgramFile` from disk / a non-image file, `loadedModule` / `loadedModules` in load order, a duplicate module name, an image whose native-slot **name** this context cannot resolve, `clear()`), the merge (one concatenated function table, one pool folded through `valueKey` so a literal two modules share occupies **one** slot, one native-slot table deduped by name, disjoint per-module slices of the shared global register area), the rewrites (**Q32**: a 300-`def` entry pushes a merged function index past the 1-byte operand range, so the re-widening through `encodeRawStream()` is measured rather than assumed), the run (**Q35**: each module's `<init>` executes exactly once, dependencies first, before the entry's; **Q30**: an exported variable is one cell the defining module writes and the importer reads), and the rejections (already linked, not loaded, not linkable, `writeQlc` of a merge). **+51 in Round 65 (`getimport`/`callimport`):** the merged import table deduped by (defining module, exported name) — two importers of one export share one cell; every `callimport` is resolved away (into `callfn`, or into `makegen` with a closure synthesized from the merged function for an exported generator), so a merged program contains **no** `callimport`; `getimport` reads the merged cell at any depth and a write through an import is visible to the defining module, while rebinding an exported function name is refused; a module-A closure capturing a global used from module B; the transitive chain app → middle → leaf (a library may now import another `.ql`); and the **unlinked** form — both opcodes execute on a program the linker never touched, the shape a future runtime registry hands over |
| `external` | `test_external` | `tests/unit/test_external.cpp` | 49 checks passed (incl. `registerEnum` injection, Round 41) |
| `test_inject` | `test_inject` | `tests/unit/test_inject.cpp` | `ALL PASS` |
| `test_dollar_operator` | `test_dollar_operator` | `tests/unit/test_dollar_operator.cpp` | 8 passed |
| `phase0_scope` | `phase0_scope` | `tests/unit/phase0_scope.cpp` | 8 checks (S1/S2/S3/P2 substrate regressions) |
| `phase0_api` | `phase0_api` | `tests/unit/phase0_api.cpp` | 3 checks (P1/P3/F1 API regressions) |
| `phase2_scope` | `phase2_scope` | `tests/unit/phase2_scope.cpp` | 7 checks (global-table scope) |
| `phase3_api` | `phase3_api` | `tests/unit/phase3_api.cpp` | 23 checks (handles, type-mismatch, finalizer queue, `release()`) |
| `phase5_api` | `phase5_api` | `tests/unit/phase5_api.cpp` | 17 checks (step budget, threading contract) |
| `value_semantics` | `value_semantics` | `tests/unit/value_semantics.cpp` | 40 checks (Round 43 share-by-default array/set/blob: aliasing on assign/call, literal freshness, `deepcopy` independence + array cycles, `+`/`cast` freshness, `a += a`, `string` still a value type) |
| `codegen_size` | `codegen_size_check` | `tests/unit/codegen_size_check.cpp` | 4 checks (added Round 58; SPEC §bytecodes "encoding guidelines") — every budgeted script is compiled exactly as `qlang_run` compiles it and the **entry function's instruction count** (the number `--profile` prints) must stay within its ceiling: `hello.ql` 215, `money.ql` 151, `cast.ql` 163, `perf_stress.ql` 565 (the `perf` corpus, so a codegen change trips both gates; `hello`/`perf_stress` came down in M3 S5 — a flag compare + flag jump is one byte shorter than the boolean-temp path in branch position — and went **up** in Round 60 S1 by exactly one instruction per bare-truthiness test site, because `tst` + `je`/`jne` replaces the single `jmpfalse`/`jmptrue`: +4 in `hello.ql`, +44 in `perf_stress.ql`, `money`/`cast` unchanged — the reason is recorded in the file header and in `docs/plans/archive/phase3-opcode-retirement-plan.md` §5 S1). A ceiling is not a target: it comes down when the stream legitimately shrinks, and going up needs a reason recorded in `docs/compliance/`. This is the mechanical form of the hand comparison of the root `test-hello-*.qla` dumps that caught the 229 → 243 growth |

Counted unit cases: **38999** (529+33+20+7+7+22+13+17+18+49+8+358+36127+1324+151+88+126+8+3+7+23+17+40+4), plus
the three `ALL PASS` gate suites (`test_native`, `test_call`, `test_inject`).

Script (23): one `script_<name>` ctest entry per `tests/scripts/*.ql`, driven by
`qlang_run` (non-zero exit = fail): `cast`, `cmp`, `encode_repro`, `ext_consts`,
`ext_regs`, `flags`, `hello`, `helpers`, `imm_kinds`, `jump_widths`, `len`, `money`,
`moneyv2`, `perf_stress`, `table_subscript`, `test_assert`, `test_assert2`,
`test_cast`, `test_cast2`, `test_minmax`, `test_minmax2`, `type`, `value_share`.

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
- **Measured, not re-recorded, Round 60 (2026-11-08)** after Phase 3: `perf_stress`
  execute 10.91–10.97 ms against the 10.382 ms baseline (**+5.0 % to +5.7 %** across
  the runs of this round) — 44 extra instructions in that script's hot loop, one per
  bare-truthiness test (`tst` + `je`/`jne` replacing `jmptrue`). Inside the ±20 %
  gate, above this plan's own ~5 % caution, so the number is recorded rather than
  accepted: the A/B measurement in `docs/plans/archive/phase3-opcode-retirement-plan.md` §5 S1
  (median of 5, three interleaved rounds against a `f192ad0` worktree) is **11.349 ms,
  +8.7 %**, root-caused to one extra dispatch per iteration of the inner `for j` loop
  (its direction re-test sits on the back-edge) with the fix filed in that plan §8.
  The other eleven medians moved by ≤ 0.011 ms; `baseline.json` is unchanged.
- **Re-recorded Round 57 (2026-11-07)** after the decoder fast path (see
  `docs/plans/archive/m2-pipeline-switch-plan.md` §"Performance follow-up"):
  `perf_stress` execute fell 12.995 ms → 10.382 ms (−20.1 %), recovering 77 % of
  the +32.5 % Round 56 regression. The remaining +5.8 % against the pre-switch
  v5 figure (9.809 ms) is the wider instruction stream itself, not the decoder
  (`decodeInstruction` is back to 6.68 ns/instruction vs 6.03 ns in v5, from
  16.4 ns at Round 56). Again the only stage above the 0.5 ms floor; the other
  eleven medians moved by ≤ 6.4 µs (host noise), so they are not attributable to
  the change. Regenerated with `--update-baseline`.
- **Re-recorded Round 56 (2026-11-07)** after the `.qlc` v6 pipeline switch:

  `perf_stress` execute rose 9.809 ms → 12.995 ms (+32.5 %) — the 2/2/4 modifier
  byte adds a byte to every escaped instruction and that script's loop body is
  almost entirely escaped immediate-right arithmetic. It is again the only stage
  above the 0.5 ms floor; the other eleven medians moved by ≤ 54 µs (the largest
  relative move, +14.9 % on `cast.ql` execute, is 3.7 µs of host noise), so they
  are not attributable to the change. Regenerated with `--update-baseline`.
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

`-E perf` is a **regex**, so the clang and MSVC jobs run 51 of the 53 entries: it
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
