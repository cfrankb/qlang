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
| [03-declarations.md](03-declarations.md) | boolean evaluation, declarations and assignments | `f6996ecd1bd9ce15f29a000e65360c1932fa6acc098ed7717809e5a1a17917ae` |
| [04-control-flow.md](04-control-flow.md) | conditions, range, switch, loops, label/goto | `b531d6356ea43f7b470c3e22f4f58af887926b8987533b419ce91602c0e0d0e2` |
| [05-operators.md](05-operators.md) | operators incl. `?`, `<`/`>`, `**`, cast, `==`/`!=`, `$`, `$$` | `5f71e2c3dc86ee9a6101caf87d71729c8e5c3dd195ca6736d0a0b7e00f2e0e46` |
| [06-functions.md](06-functions.md) | built-in functions incl. `deepcopy`, qlang native functions, C++ API | `8e7ac79a3f42b0e91bab2578551ef66d40b437e9a46683522f1e63cc1de3fe8c` |
| [07-api.md](07-api.md) | module registration, QLang import syntax, `ctx.clear()`, verbose level, step budget, threading contract, module search paths, external search callback | `086f8502341e2447b07134bca17a61da765fc4cef61e675c35d8806d156c3fc0` |
| [08-generators.md](08-generators.md) | generators, PROTOTYPE 1, Syntax, EOF sentinel | `a99107445f3850d8b600e84cf5d0e5860babc4173cda0e2a183debab0123200d` |
| [09-modules.md](09-modules.md) | internal modules, import statement, math module | `0f2c8c45af7b83db34c053ca87c6bafe998df4a1f30ff80742db8ef513d31741` |
| [10-classes.md](10-classes.md) | user-defined classes, `static`, `::`, `deepcopy`, restrictions | `6f2cacb5b2135951a9aa27b2d69aa9b898a3fa27aeeaa56559dd18522621c458` |
| [11-external-types.md](11-external-types.md) | conformance with external (C++) types | `3a615c3fc1bc51698abf86a77899c36cc93a4ea623615e6f00f26b8969d1d519` |
| [12-errors-trace.md](12-errors-trace.md) | error reporting, requirements, execution trace, format | `f0814e4811e60570546d4771c048ecb1e903e6737ad388ee752d9699b9e68ff8` |
| [13-bytecodes.md](13-bytecodes.md) | bytecodes, MOVI/ADDI/SUBI opcodes, alternate conditional jump (cmp/cmpi + je/jne/jg/jge/jl/jle), escape-byte flat byte stream + loc side-table, `nop`, `deepcopy`, `indexget`/`indexput`, extended encoding (`.qlc` v6 modifier byte 2/2/4, immediate set, `eqf`…`gtef`/`tst`/`decl`) | `c4bbae784a33534aee336b185f384603f24d4f40dc0955455be5747fdda13c6d` |
| [14-scope.md](14-scope.md) | visibility scope. execution context | `ec1eadb21c4716b94590261fec12a16b1f11a715a5bfed53b5ca46232b6a0999` |
| [15-tests.md](15-tests.md) | tests | `dd9c8fe38cfe2167fb66ed032e63235ecc0a9a938934c225bdf83b7a381e6c23` |
| [16-strict_typing.md](16-strict_typing.md) | strict typing | `0e2c24e5d5121b784c31561d2a9b94bf32e52eb518df2b9f65531c11eafed4fd` |
| [17-signals.md](17-signals.md) | signals | `ede443b5ab88d73af308203e90444a56c0593e66459d8ed60d07f6fef0815e1d` |
| [18-qlang-run.md](18-qlang-run.md) | qlang_run loader | `7ea9a11fecf02668006fdb4091545d1c318864ca9ad7a93b42315060b1535b9a` |

Compliance: see [../compliance/README.md](../compliance/README.md).
Migration plan: [../plans/archive/spec_migration_plan.md](../plans/archive/spec_migration_plan.md).
