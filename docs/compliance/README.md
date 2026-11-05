# QLang Compliance — Module Index

Entry point for the compliance reference (migrated from root `SPEC_COMPLIANCE.md`,
2026-10-29; last audit Round 46, 2026-11-05). Content is verbatim from the
original `SPEC_COMPLIANCE.md`. The recorded hashes are enforced by the
`doc_hashes` ctest entry (`cmake/check_doc_hashes.cmake`); re-record them after
a verified doc edit with `cmake -DQLANG_SOURCE_DIR=<src> -DDOCS_HASH_UPDATE=ON
-P cmake/check_doc_hashes.cmake`.

Per AGENTS.md, each module validates the **specific spec module(s)** listed
below (spec hashes tracked in [../spec/README.md](../spec/README.md)); the
`SHA256 (self)` column tracks the module file itself.

| Module | Content | Validates spec module(s) | SHA256 (self) |
|--------|---------|--------------------------|---------------|
| [status.md](status.md) | current status, changes since audit, legend, working/missing/not-yet, extensions, open items | all (global audit) | `fe17f184d499f05e5b4643946f2c08a5fd8dd1d36e50752fee061487b5a26a88` |
| [coverage.md](coverage.md) | reserved-word coverage, value-type coverage | 02-lexical, 01-types | `789a0b58112961a6f32e98426923c17c88be737f0cfbc3a809ad771b11c898eb` |
| [harness.md](harness.md) | test harness, C++ interface, fixes applied this round | 07-api (+ whole) | `756eeeef865754de8af8da23433626feb75b19ac574ac2e3e3777e88cceea4d0` |
| [history.md](history.md) | audit rounds 7–46 (append-only) | mixed (per round) | `e81b0052708dab5c9a92ac2e68eb1b79e598aa6ba88172a655ffda13d6591efb` |
| [classes.md](classes.md) | class implementation details, class spec conformance | 10-classes | `5bda8947b022dcb7f63ab159a9114faae311e9e17638b9ed1d6121e69038465f` |
| [tests.md](tests.md) | test suite layout, ctest matrix, perf baseline, CI matrix | 15-tests | `724881e1f21cc770ce95003953a8fda57e1439885270cdb8b112b6a87b4efe74` |
| [migration-value-semantics.md](migration-value-semantics.md) | copy→share migration note for `array`/`set`/`blob` (Round 43) | 01-types | `3b38a16ad6e8f04386092ee2f588adb27d5f1bdfcada749135c7168066547e7d` |

**New spec modules:** every new module in `docs/spec/` must get, at creation,
(a) a SHA256 row in [../spec/README.md](../spec/README.md) and (b) compliance
tracking here — a dedicated module (as `classes.md` is for 10-classes) or an
explicit Open Item in `status.md` (as `16-strict_typing` has).

Spec hashes validated (as of this migration — re-verify after any spec edit):
see the table in [../spec/README.md](../spec/README.md).
