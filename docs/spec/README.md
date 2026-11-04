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
| [02-lexical.md](02-lexical.md) | reserved words, constants, enum, comments | `9ef3b1fbb8633fb380fe53961ad9bbc3940bf1962ca1760de358537bdad50fa3` |
| [03-declarations.md](03-declarations.md) | boolean evaluation, declarations and assignments | `c34282cb0ada72560d199046e6157ddc7226f4ec047483ec96b9a44d8ffce4f5` |
| [04-control-flow.md](04-control-flow.md) | conditions, range, switch, loops, label/goto | `b5dc9fbaa96ef4441f36f2d64682dcfd062932a3b00948579a3ca87ae15766e8` |
| [05-operators.md](05-operators.md) | operators incl. `?`, `<`/`>`, `**`, cast, `==`/`!=`, `$`, `$$` | `42975989e813699bfcdb62bba4037115d05ff186f5cac679af50013ef24ec685` |
| [06-functions.md](06-functions.md) | built-in functions, qlang native functions, C++ API | `0060c47c03d579c793e216f9815753ca2c4f58659d682cd159ae1fec0848265d` |
| [07-api.md](07-api.md) | module registration, QLang import syntax, `ctx.clear()`, verbose level, step budget, threading contract | `f7c9932f5e7a4e2160310848c92bf5c2809b0a1a093e82521ebf5e525762b133` |
| [08-generators.md](08-generators.md) | generators, PROTOTYPE 1, Syntax, EOF sentinel | `a99107445f3850d8b600e84cf5d0e5860babc4173cda0e2a183debab0123200d` |
| [09-modules.md](09-modules.md) | internal modules, import statement, math module | `a3b3bec108d7482541448e27e31222dca58a8c1c588dded12ce26dfe6fb5bebf` |
| [10-classes.md](10-classes.md) | user-defined classes, `static`, `::`, restrictions | `67ab74fb80ed191a63eac7f68ba787896aef2ff1c953d1627fac28b0ba854beb` |
| [11-external-types.md](11-external-types.md) | conformance with external (C++) types | `43993db71abbc2f4145a8768c6a9c34d6245dd38ac3925744c333b8f51907b83` |
| [12-errors-trace.md](12-errors-trace.md) | error reporting, requirements, execution trace, format | `f0814e4811e60570546d4771c048ecb1e903e6737ad388ee752d9699b9e68ff8` |
| [13-bytecodes.md](13-bytecodes.md) | bytecodes, MOVI/ADDI/SUBI opcodes, alternate conditional jump (cmp/cmpi + je/jne/jg/jge/jl/jle), 8-byte layout + loc side-table | `c914c42aba5a0869dfa1b1b1e0dc14f4a299c616e57e9af1a3919871aa281a5b` |
| [14-scope.md](14-scope.md) | visibility scope. execution context | `ec1eadb21c4716b94590261fec12a16b1f11a715a5bfed53b5ca46232b6a0999` |
| [15-tests.md](15-tests.md) | tests | `dd9c8fe38cfe2167fb66ed032e63235ecc0a9a938934c225bdf83b7a381e6c23` |
| [16-strict_typing.md](16-strict_typing.md) | strict typing |  |

Compliance: see [../compliance/README.md](../compliance/README.md).
Migration plan: [../plans/spec_migration_plan.md](../plans/spec_migration_plan.md).
