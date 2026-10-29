# QLang Spec — Module Index

Entry point for the QLang language specification (migrated from root `SPEC.md`,
2026-10-29). Content is verbatim; line refs point at the original `SPEC.md`.

| Module | Domain | SPEC.md lines | SHA256 |
|--------|--------|---------------|--------|
| [01-types.md](01-types.md) | language intro, data types, strings, sets, append | 1–93 | `f7831d8a67d00f4785256d7a79424da949c6220f961616c464b4298e8c180bad` |
| [02-lexical.md](02-lexical.md) | reserved words, constants, enum, comments | 94–221 | `cc0742813914ef615d3d810ce0f7fb70911e107ee063e8273652f4efe6c026f6` |
| [03-declarations.md](03-declarations.md) | boolean evaluation, declarations and assignments | 222–342 | `f893d3c9d37b2b48d2da989aedd4ce3f70067abda209cf88d8f5ed32291c5892` |
| [04-control-flow.md](04-control-flow.md) | conditions, range, switch, loops, label/goto | 343–562 | `b5dc9fbaa96ef4441f36f2d64682dcfd062932a3b00948579a3ca87ae15766e8` |
| [05-operators.md](05-operators.md) | operators incl. `?`, `<`/`>`, `**`, cast, `==`/`!=`, `$`, `$$` | 563–940 | `42975989e813699bfcdb62bba4037115d05ff186f5cac679af50013ef24ec685` |
| [06-functions.md](06-functions.md) | built-in functions, qlang native functions | 941–1082 | `fcb6886fea6bb0e7f2c676d74551b071a0f61e5911a6724806235145bb38bcee` |
| [07-api.md](07-api.md) | module registration, `ctx.clear()`, verbose level | 1083–1364 | `f17654aa05252a24f7589027b35fa4cd461f9384714ad72e04d1037729e4e95f` |
| [08-generators.md](08-generators.md) | generators, PROTOTYPE 1, Syntax, EOF sentinel | 1365–1415 | `a99107445f3850d8b600e84cf5d0e5860babc4173cda0e2a183debab0123200d` |
| [09-modules.md](09-modules.md) | internal modules, import statement, math module | 1416–1478 | `3794d74156680d376d717051fb0aad2830ad9d347253501ebf01326a93de39d4` |
| [10-classes.md](10-classes.md) | user-defined classes, `static`, `::`, restrictions | 1479–1641 | `be8d03617993264a7795d1054497d872ac1cc46c6deead3f1961e5b5509f6874` |
| [11-external-types.md](11-external-types.md) | conformance with external (C++) types | 1642–1683 | `6c41d9b094d8924566acbbbe1d25b319e1cdd7e4fdfe09cdf65d0f861af4ef3c` |
| [12-errors-trace.md](12-errors-trace.md) | error reporting, requirements, execution trace, format | 1684–1712 | `db3ec239276a6b51638e82be2ade9071605259aafda451b117060213e6e57f4c` |

Compliance: see [../compliance/README.md](../compliance/README.md).
Migration plan: [../plans/spec_migration_plan.md](../plans/spec_migration_plan.md).
