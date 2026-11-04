# QLang Compliance — Module Index

Entry point for the compliance reference (migrated from root `SPEC_COMPLIANCE.md`,
2026-10-29; last audit Round 35, 2026-11-03). Content is verbatim; line refs
point at the original `SPEC_COMPLIANCE.md`.

Per AGENTS.md, each module tracks the SHA256 of the **specific spec module(s)**
it validates (from [../spec/README.md](../spec/README.md)).

| Module | Content | SPEC_COMPLIANCE.md lines | Validates spec module(s) | SHA256 |
|--------|---------|--------------------------|--------------------------|--------|
| [status.md](status.md) | current status, changes since audit, legend, working/missing/not-yet, extensions, open items | 1–206, 818–845 | all (global audit) | `a444b41822d4cbca38ebea4da61554d4f4fe9ea05382c2d3e006931d73ebf79f` |
| [coverage.md](coverage.md) | reserved-word coverage, value-type coverage | 207–280 | 02-lexical, 01-types | `b4216bca0917a7ca9a021a849c7fcafda49887f293c43a7c68a89487786ff0c9` |
| [harness.md](harness.md) | test harness, C++ interface, fixes applied this round | 281–354 | 07-api (+ whole) | `f7c9932f5e7a4e2160310848c92bf5c2809b0a1a093e82521ebf5e525762b133` |
| [history.md](history.md) | audit rounds 7–32 (append-only) | 355–738 | mixed (per round) | `1d369fc1c29119dd0b1580af1a67cd9658eb833570facfea21808f60be96e14f` |
| [classes.md](classes.md) | class implementation details, class spec conformance | 739–817 | 10-classes | `5bda8947b022dcb7f63ab159a9114faae311e9e17638b9ed1d6121e69038465f` |
| [tests.md](tests.md) | test suite layout, ctest matrix, perf baseline, CI matrix | n/a (new, Round 32) | 15-tests | `acf2dc8209b052c716d87d7dc45270be1fa899748400e924c8cf240095641dba` |

Spec hashes validated (as of this migration — re-verify after any spec edit):
see the table in [../spec/README.md](../spec/README.md).
