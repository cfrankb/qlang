# QLang Compliance — Module Index

Entry point for the compliance reference (migrated from root `SPEC_COMPLIANCE.md`,
2026-10-29; last audit Round 38, 2026-11-04). Content is verbatim; line refs
point at the original `SPEC_COMPLIANCE.md`.

Per AGENTS.md, each module tracks the SHA256 of the **specific spec module(s)**
it validates (from [../spec/README.md](../spec/README.md)).

| Module | Content | SPEC_COMPLIANCE.md lines | Validates spec module(s) | SHA256 |
|--------|---------|--------------------------|--------------------------|--------|
| [status.md](status.md) | current status, changes since audit, legend, working/missing/not-yet, extensions, open items | 1–206, 818–845 | all (global audit) | `a6a6d1a6c89e2d64094ccbae2273f325eb500c007c3b03f4f6c752caa996a8d3` |
| [coverage.md](coverage.md) | reserved-word coverage, value-type coverage | 207–280 | 02-lexical, 01-types | `b4216bca0917a7ca9a021a849c7fcafda49887f293c43a7c68a89487786ff0c9` |
| [harness.md](harness.md) | test harness, C++ interface, fixes applied this round | 281–354 | 07-api (+ whole) | `756eeeef865754de8af8da23433626feb75b19ac574ac2e3e3777e88cceea4d0` |
| [history.md](history.md) | audit rounds 7–38 (append-only) | 355–738 | mixed (per round) | `b8c516b23c5de14843588d7353c9a79ac650d095bd5e93114f55dc1055a524bc` |
| [classes.md](classes.md) | class implementation details, class spec conformance | 739–817 | 10-classes | `5bda8947b022dcb7f63ab159a9114faae311e9e17638b9ed1d6121e69038465f` |
| [tests.md](tests.md) | test suite layout, ctest matrix, perf baseline, CI matrix | n/a (new, Round 32) | 15-tests | `d3a8e1fd3ee200c45ad6fd40850eea0ae7abe6e12d7ded50638df85ac112cf3c` |

Spec hashes validated (as of this migration — re-verify after any spec edit):
see the table in [../spec/README.md](../spec/README.md).
