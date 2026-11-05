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
| [01-types.md](01-types.md) | language intro, data types, value semantics, strings, sets, append | `4ed004cec48d9c1ec3626c48deb02919e9c3b6c6831b1eb7332fa11aa02b2c64` |
| [02-lexical.md](02-lexical.md) | reserved words, constants, enum, comments | `0e746264958922212c10368a5f8085f520c39e1712b46694f8ea92a8c218c46a` |
| [03-declarations.md](03-declarations.md) | boolean evaluation, declarations and assignments | `a50f56689fdc330b7d9bfe28faa7d8806ecb6801959d2b262cec312747c82c68` |
| [04-control-flow.md](04-control-flow.md) | conditions, range, switch, loops, label/goto | `b531d6356ea43f7b470c3e22f4f58af887926b8987533b419ce91602c0e0d0e2` |
| [05-operators.md](05-operators.md) | operators incl. `?`, `<`/`>`, `**`, cast, `==`/`!=`, `$`, `$$` | `5f71e2c3dc86ee9a6101caf87d71729c8e5c3dd195ca6736d0a0b7e00f2e0e46` |
| [06-functions.md](06-functions.md) | built-in functions incl. `deepcopy`, qlang native functions, C++ API | `8e7ac79a3f42b0e91bab2578551ef66d40b437e9a46683522f1e63cc1de3fe8c` |
| [07-api.md](07-api.md) | module registration, QLang import syntax, `ctx.clear()`, verbose level, step budget, threading contract | `f06b0fc1fe3d7f9c4e64004e453115a35a22d60647d47227e0a59591b5a2eada` |
| [08-generators.md](08-generators.md) | generators, PROTOTYPE 1, Syntax, EOF sentinel | `a99107445f3850d8b600e84cf5d0e5860babc4173cda0e2a183debab0123200d` |
| [09-modules.md](09-modules.md) | internal modules, import statement, math module | `a3b3bec108d7482541448e27e31222dca58a8c1c588dded12ce26dfe6fb5bebf` |
| [10-classes.md](10-classes.md) | user-defined classes, `static`, `::`, `deepcopy`, restrictions | `6f2cacb5b2135951a9aa27b2d69aa9b898a3fa27aeeaa56559dd18522621c458` |
| [11-external-types.md](11-external-types.md) | conformance with external (C++) types | `43993db71abbc2f4145a8768c6a9c34d6245dd38ac3925744c333b8f51907b83` |
| [12-errors-trace.md](12-errors-trace.md) | error reporting, requirements, execution trace, format | `f0814e4811e60570546d4771c048ecb1e903e6737ad388ee752d9699b9e68ff8` |
| [13-bytecodes.md](13-bytecodes.md) | bytecodes, MOVI/ADDI/SUBI opcodes, alternate conditional jump (cmp/cmpi + je/jne/jg/jge/jl/jle), escape-byte flat byte stream + loc side-table, `nop`, `deepcopy` | `c15b9c86779d0b02f0ce97d179da563a7adb95d728ec6d6675808ea1e2ccf68e` |
| [14-scope.md](14-scope.md) | visibility scope. execution context | `ec1eadb21c4716b94590261fec12a16b1f11a715a5bfed53b5ca46232b6a0999` |
| [15-tests.md](15-tests.md) | tests | `dd9c8fe38cfe2167fb66ed032e63235ecc0a9a938934c225bdf83b7a381e6c23` |
| [16-strict_typing.md](16-strict_typing.md) | strict typing | `9f0d96519a83b0daa3082159f3d2ac4a8c3e66fed6775922dcc41ab0896e98d5` |
| [17-signals.md](17-signals.md) | signals | `babcc5a0d3c4bbc5e005c88a654b8a01f2806bda7dea8fb9a04b1dd48462a262` |

Compliance: see [../compliance/README.md](../compliance/README.md).
Migration plan: [../plans/spec_migration_plan.md](../plans/spec_migration_plan.md).
