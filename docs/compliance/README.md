# QLang Compliance — Module Index

Entry point for the compliance reference (migrated from root `SPEC_COMPLIANCE.md`,
2026-10-29; last audit Round 64, 2026-11-09). Content is verbatim from the
original `SPEC_COMPLIANCE.md`. The recorded hashes are checked by the
`doc_hashes` ctest entry (`cmake/check_doc_hashes.cmake`); re-record them after
a verified doc edit with `./update-doc-hashes.sh` (`--check` verifies only).
Each hash is taken over LF-normalized content and `.gitattributes` pins
`eol=lf`, so line endings never make a row drift (Round 48). The entry is
optional by default — drift is reported and the test **Skipped**, never Failed;
`-DQLANG_DOC_HASHES_STRICT=ON` makes it fail again (Round 49).

Per AGENTS.md, each module validates the **specific spec module(s)** listed
below (spec hashes tracked in [../spec/README.md](../spec/README.md)); the
`SHA256 (self)` column tracks the module file itself.

| Module | Content | Validates spec module(s) | SHA256 (self) |
|--------|---------|--------------------------|---------------|
| [status.md](status.md) | current status, changes since audit, legend, working/missing/not-yet, extensions, open items | all (global audit) | `7bb2984a01813120147f476d58caa9a0b5d50ce54d1488740b601c151de76dcc` |
| [coverage.md](coverage.md) | reserved-word coverage, value-type coverage | 02-lexical, 01-types | `789a0b58112961a6f32e98426923c17c88be737f0cfbc3a809ad771b11c898eb` |
| [harness.md](harness.md) | test harness, C++ interface, fixes applied this round | 07-api (+ whole) | `c4026c4de5dbe8d0234497bda9985153060e33a419b59bf4ab3cef80932bbe06` |
| [history.md](history.md) | audit rounds 7–64 (append-only) | mixed (per round) | `c43c0c88598fae8f2235063bce587433afb8cc667fb81e5a6b486c2e9647e203` |
| [classes.md](classes.md) | class implementation details, class spec conformance | 10-classes | `c95a835ae11f6e25c87facb30dd7210a18e928b56ab9b3592e25acf805e99b40` |
| [tests.md](tests.md) | test suite layout, ctest matrix, perf baseline, CI matrix | 15-tests | `38e7a1d39538f004e32fd97dbeb35cb3209f97f28306e5551c24f8022da39630` |
| [migration-value-semantics.md](migration-value-semantics.md) | copy→share migration note for `array`/`set`/`blob` (Round 43) | 01-types | `3b38a16ad6e8f04386092ee2f588adb27d5f1bdfcada749135c7168066547e7d` |

**New spec modules:** every new module in `docs/spec/` must get, at creation,
(a) a SHA256 row in [../spec/README.md](../spec/README.md) and (b) compliance
tracking here — a dedicated module (as `classes.md` is for 10-classes) or an
explicit Open Item in `status.md` (as `16-strict_typing` has).

Spec hashes validated (as of this migration — re-verify after any spec edit):
see the table in [../spec/README.md](../spec/README.md).
