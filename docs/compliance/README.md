# QLang Compliance — Module Index

Entry point for the compliance reference (migrated from root `SPEC_COMPLIANCE.md`,
2026-10-29; last audit Round 31, 2026-10-29). Content is verbatim; line refs
point at the original `SPEC_COMPLIANCE.md`.

Per AGENTS.md, each module tracks the SHA256 of the **specific spec module(s)**
it validates (from [../spec/README.md](../spec/README.md)).

| Module | Content | SPEC_COMPLIANCE.md lines | Validates spec module(s) | SHA256 |
|--------|---------|--------------------------|--------------------------|--------|
| [status.md](status.md) | current status, changes since audit, legend, working/missing/not-yet, extensions, open items | 1–206, 818–845 | all (global audit) | `cf9a6184752a116cd395eaf79333c42f7b2a930bde3058aa2c7289cf17f53d3a` |
| [coverage.md](coverage.md) | reserved-word coverage, value-type coverage | 207–280 | 02-lexical, 01-types | `b4216bca0917a7ca9a021a849c7fcafda49887f293c43a7c68a89487786ff0c9` |
| [harness.md](harness.md) | test harness, C++ interface, fixes applied this round | 281–354 | 07-api (+ whole) | `8809a199df3e3d72e28ec66128cf410ecaadf114e4769bfb489a2d3578dc3cb5` |
| [history.md](history.md) | audit rounds 7–31 (append-only) | 355–738 | mixed (per round) | `a7d59b3de616a57221851e9961ff1a3fafecaec847bd7e4e4fc74103057786e8` |
| [classes.md](classes.md) | class implementation details, class spec conformance | 739–817 | 10-classes | `5bda8947b022dcb7f63ab159a9114faae311e9e17638b9ed1d6121e69038465f` |

Spec hashes validated (as of this migration — re-verify after any spec edit):
see the table in [../spec/README.md](../spec/README.md).
