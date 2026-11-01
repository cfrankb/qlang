# QLang Compliance — Module Index

Entry point for the compliance reference (migrated from root `SPEC_COMPLIANCE.md`,
2026-10-29; last audit Round 31, 2026-10-29). Content is verbatim; line refs
point at the original `SPEC_COMPLIANCE.md`.

Per AGENTS.md, each module tracks the SHA256 of the **specific spec module(s)**
it validates (from [../spec/README.md](../spec/README.md)).

| Module | Content | SPEC_COMPLIANCE.md lines | Validates spec module(s) | SHA256 |
|--------|---------|--------------------------|--------------------------|--------|
| [status.md](status.md) | current status, changes since audit, legend, working/missing/not-yet, extensions, open items | 1–206, 818–845 | all (global audit) | `32fea8b8bfc70a7a879339f8631a4e5cf8f31485b604e79bf3fd3609fe7fb791` |
| [coverage.md](coverage.md) | reserved-word coverage, value-type coverage | 207–280 | 02-lexical, 01-types | `b4216bca0917a7ca9a021a849c7fcafda49887f293c43a7c68a89487786ff0c9` |
| [harness.md](harness.md) | test harness, C++ interface, fixes applied this round | 281–354 | 07-api (+ whole) | `87b892fdcfda81987fd4b73704e4af4d209ae3ff500baabc275339c31231f4a4` |
| [history.md](history.md) | audit rounds 7–31 (append-only) | 355–738 | mixed (per round) | `a7d59b3de616a57221851e9961ff1a3fafecaec847bd7e4e4fc74103057786e8` |
| [classes.md](classes.md) | class implementation details, class spec conformance | 739–817 | 10-classes | `5bda8947b022dcb7f63ab159a9114faae311e9e17638b9ed1d6121e69038465f` |

Spec hashes validated (as of this migration — re-verify after any spec edit):
see the table in [../spec/README.md](../spec/README.md).
