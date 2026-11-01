# QLang Spec — Module Index

Entry point for the QLang language specification (migrated from root `SPEC.md`,
2026-10-29). The `SHA256` column tracks each module — re-verify after any
edit.

Implementation standard: **C++20** — the root `CMakeLists.txt` sets
`CMAKE_CXX_STANDARD 20`. The C++ API ([07-api.md](07-api.md)) and the
external-type bindings ([11-external-types.md](11-external-types.md)) are
defined against C++20.

| Module | Domain | SHA256 |
|--------|--------|--------|
| [01-types.md](01-types.md) | language intro, data types, strings, sets, append | `f7831d8a67d00f4785256d7a79424da949c6220f961616c464b4298e8c180bad` |
| [02-lexical.md](02-lexical.md) | reserved words, constants, enum, comments | `a6af6ca9ba13b550d4af21ceb624f9e1b5f00b854b4c026a9b5f39a87615fa7a` |
| [03-declarations.md](03-declarations.md) | boolean evaluation, declarations and assignments | `f893d3c9d37b2b48d2da989aedd4ce3f70067abda209cf88d8f5ed32291c5892` |
| [04-control-flow.md](04-control-flow.md) | conditions, range, switch, loops, label/goto | `b5dc9fbaa96ef4441f36f2d64682dcfd062932a3b00948579a3ca87ae15766e8` |
| [05-operators.md](05-operators.md) | operators incl. `?`, `<`/`>`, `**`, cast, `==`/`!=`, `$`, `$$` | `42975989e813699bfcdb62bba4037115d05ff186f5cac679af50013ef24ec685` |
| [06-functions.md](06-functions.md) | built-in functions, qlang native functions, C++ API | `fcb6886fea6bb0e7f2c676d74551b071a0f61e5911a6724806235145bb38bcee` |
| [07-api.md](07-api.md) | module registration, QLang import syntax, `ctx.clear()`, verbose level | `e638b7aaa1e540ab0571c09fae9a33c1773c852b08029c78ef8444247a47e79d` |
| [08-generators.md](08-generators.md) | generators, PROTOTYPE 1, Syntax, EOF sentinel | `a99107445f3850d8b600e84cf5d0e5860babc4173cda0e2a183debab0123200d` |
| [09-modules.md](09-modules.md) | internal modules, import statement, math module | `e9c58a120983f52232e45f8de9de7acf858c8580ebc50f1f3df9363849a1f20a` |
| [10-classes.md](10-classes.md) | user-defined classes, `static`, `::`, restrictions | `67ab74fb80ed191a63eac7f68ba787896aef2ff1c953d1627fac28b0ba854beb` |
| [11-external-types.md](11-external-types.md) | conformance with external (C++) types | `43993db71abbc2f4145a8768c6a9c34d6245dd38ac3925744c333b8f51907b83` |
| [12-errors-trace.md](12-errors-trace.md) | error reporting, requirements, execution trace, format | `f0814e4811e60570546d4771c048ecb1e903e6737ad388ee752d9699b9e68ff8` |
| [13-bytecodes.md](13-bytecodes.md) | bytecodes, MOVI/ADDI/SUBI opcodes, alternate conditional jump (cmp/cmpi + je/jne/jg/jge/jl/jle) | `470bb91389c4c620559eba5ddc39e83a4558236fc0c134cdc653a4737c6f9114` |
| [14-scope.md](14-scope.md) | visibility scope. execution context | `6ce00980b534b4cdd1a069605a3493b3949c0efa8285c800bf804cbc8731762c` |

Compliance: see [../compliance/README.md](../compliance/README.md).
Migration plan: [../plans/spec_migration_plan.md](../plans/spec_migration_plan.md).
