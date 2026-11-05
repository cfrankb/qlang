# QLang Compliance — Module Index

Entry point for the compliance reference (migrated from root `SPEC_COMPLIANCE.md`,
2026-10-29; last audit Round 40, 2026-11-05). Content is verbatim from the
original `SPEC_COMPLIANCE.md`.

Per AGENTS.md, each module validates the **specific spec module(s)** listed
below (spec hashes tracked in [../spec/README.md](../spec/README.md)); the
`SHA256 (self)` column tracks the module file itself.

| Module | Content | Validates spec module(s) | SHA256 (self) |
|--------|---------|--------------------------|---------------|
| [status.md](status.md) | current status, changes since audit, legend, working/missing/not-yet, extensions, open items | all (global audit) | `5da9c856cf6daf39a10a2690c236ccccdde64f126d6b29d330ea677e0f789ce0` |
| [coverage.md](coverage.md) | reserved-word coverage, value-type coverage | 02-lexical, 01-types | `789a0b58112961a6f32e98426923c17c88be737f0cfbc3a809ad771b11c898eb` |
| [harness.md](harness.md) | test harness, C++ interface, fixes applied this round | 07-api (+ whole) | `756eeeef865754de8af8da23433626feb75b19ac574ac2e3e3777e88cceea4d0` |
| [history.md](history.md) | audit rounds 7–40 (append-only) | mixed (per round) | `69545594188f594ab049825e24513554c020f5c57d34bc127827a70df1610ac3` |
| [classes.md](classes.md) | class implementation details, class spec conformance | 10-classes | `5bda8947b022dcb7f63ab159a9114faae311e9e17638b9ed1d6121e69038465f` |
| [tests.md](tests.md) | test suite layout, ctest matrix, perf baseline, CI matrix | 15-tests | `d30cd0539ca465b236d3c7501dcb10e351455b79f289bbb9ea627f995f57f399` |

**New spec modules:** every new module in `docs/spec/` must get, at creation,
(a) a SHA256 row in [../spec/README.md](../spec/README.md) and (b) compliance
tracking here — a dedicated module (as `classes.md` is for 10-classes) or an
explicit Open Item in `status.md` (as `16-strict_typing` has).

Spec hashes validated (as of this migration — re-verify after any spec edit):
see the table in [../spec/README.md](../spec/README.md).
