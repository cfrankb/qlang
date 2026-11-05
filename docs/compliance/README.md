# QLang Compliance — Module Index

Entry point for the compliance reference (migrated from root `SPEC_COMPLIANCE.md`,
2026-10-29; last audit Round 39, 2026-11-04). Content is verbatim from the
original `SPEC_COMPLIANCE.md`.

Per AGENTS.md, each module validates the **specific spec module(s)** listed
below (spec hashes tracked in [../spec/README.md](../spec/README.md)); the
`SHA256 (self)` column tracks the module file itself.

| Module | Content | Validates spec module(s) | SHA256 (self) |
|--------|---------|--------------------------|---------------|
| [status.md](status.md) | current status, changes since audit, legend, working/missing/not-yet, extensions, open items | all (global audit) | `2f06e4de53b3d9dbc8ed3b9a8610f9e3676220da5bb84b3768747858dc766d1b` |
| [coverage.md](coverage.md) | reserved-word coverage, value-type coverage | 02-lexical, 01-types | `b4216bca0917a7ca9a021a849c7fcafda49887f293c43a7c68a89487786ff0c9` |
| [harness.md](harness.md) | test harness, C++ interface, fixes applied this round | 07-api (+ whole) | `756eeeef865754de8af8da23433626feb75b19ac574ac2e3e3777e88cceea4d0` |
| [history.md](history.md) | audit rounds 7–39 (append-only) | mixed (per round) | `1a728f62da90fd098b29895ed1b3fb453af29bf3635e3834d58f32c2ab14d61e` |
| [classes.md](classes.md) | class implementation details, class spec conformance | 10-classes | `5bda8947b022dcb7f63ab159a9114faae311e9e17638b9ed1d6121e69038465f` |
| [tests.md](tests.md) | test suite layout, ctest matrix, perf baseline, CI matrix | 15-tests | `d3a8e1fd3ee200c45ad6fd40850eea0ae7abe6e12d7ded50638df85ac112cf3c` |

**New spec modules:** every new module in `docs/spec/` must get, at creation,
(a) a SHA256 row in [../spec/README.md](../spec/README.md) and (b) compliance
tracking here — a dedicated module (as `classes.md` is for 10-classes) or an
explicit Open Item in `status.md` (as `16-strict_typing` has).

Spec hashes validated (as of this migration — re-verify after any spec edit):
see the table in [../spec/README.md](../spec/README.md).
