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
| [03-declarations.md](03-declarations.md) | boolean evaluation, declarations and assignments | `b574e9fba63221df31582ba5ced416982d7438a66a5421f27b36df5a5b21741a` |
| [04-control-flow.md](04-control-flow.md) | conditions, range, switch, loops, label/goto | `b531d6356ea43f7b470c3e22f4f58af887926b8987533b419ce91602c0e0d0e2` |
| [05-operators.md](05-operators.md) | operators incl. `?`, `<`/`>`, `**`, cast, `==`/`!=`, `$`, `$$` | `b16ac686b61309a6f9df7ffc31c4e121a4c6ea014658aec2041a3b6721874555` |
| [06-functions.md](06-functions.md) | built-in functions incl. `deepcopy`, qlang native functions, C++ API | `8e7ac79a3f42b0e91bab2578551ef66d40b437e9a46683522f1e63cc1de3fe8c` |
| [07-api.md](07-api.md) | module registration, QLang import syntax, `ctx.clear()`, verbose level, step budget, threading contract, module search paths, external search callback, output callback (universal) | `a48885b4d46882814f55a2d7e8035d8cba07dfbd001975bcd9fbdefad857d387` |
| [08-generators.md](08-generators.md) | generators, PROTOTYPE 1, Syntax, EOF sentinel | `a99107445f3850d8b600e84cf5d0e5860babc4173cda0e2a183debab0123200d` |
| [09-modules.md](09-modules.md) | internal modules, import statement, math module | `4396cb0275b2b18a87c6833bfbdb64512502ff0909e7a5543e32ad5030c849d7` |
| [10-classes.md](10-classes.md) | user-defined classes, `static`, `::`, `deepcopy`, restrictions | `6f2cacb5b2135951a9aa27b2d69aa9b898a3fa27aeeaa56559dd18522621c458` |
| [11-external-types.md](11-external-types.md) | conformance with external (C++) types | `380952dbb96bbe054adaf8ac948de683b4dbb778e28751bf49ce36135b12096e` |
| [12-errors-trace.md](12-errors-trace.md) | error reporting, requirements, execution trace, format | `f0814e4811e60570546d4771c048ecb1e903e6737ad388ee752d9699b9e68ff8` |
| [13-bytecodes.md](13-bytecodes.md) | bytecodes, MOVI/ADDI/SUBI opcodes, alternate conditional jump (cmp/cmpi + je/jne/jg/jge/jl/jle), escape-byte flat byte stream + loc side-table, `nop`, `deepcopy`, `indexget`/`indexput`, extended encoding (`.qlc` v6 modifier byte 2/2/4, immediate set, `eqf`…`gtef`/`tst`/`decl`) | `4e96fb7b0aec70e94271455678023b8120859a3cb6801524b8a6fdc803e514ae` |
| [14-scope.md](14-scope.md) | visibility scope. execution context | `ec1eadb21c4716b94590261fec12a16b1f11a715a5bfed53b5ca46232b6a0999` |
| [15-tests.md](15-tests.md) | tests | `dd9c8fe38cfe2167fb66ed032e63235ecc0a9a938934c225bdf83b7a381e6c23` |
| [16-strict_typing.md](16-strict_typing.md) | strict typing | `fe73b1ff53960150911c9941e9784b60ac76369f8deb85744ccb0de23f5b1dfa` |
| [17-signals.md](17-signals.md) | signals | `ede443b5ab88d73af308203e90444a56c0593e66459d8ed60d07f6fef0815e1d` |
| [18-qlang-run.md](18-qlang-run.md) | qlcc driver | `d578643176281ee9fb03351bf55bfd72e3bb86afe2e11b80c69e296bf667fe2a` |
| [19-qt6_integration.md](19-qt6_integration.md) | Qt6 integration (**draft 2026-11-13, unimplemented** — `QLANG_WITH_QT`: meta-driven Q_OBJECT class imports, dialogs/widgets, QFile + `:/` resources, Qt signal/slot bridge, GUI-thread contract; plan `docs/plans/qt-bridge-plan.md`) | `81849ea558dc0211da72219b77b578b40fdcc7cfa711f32869e28871ceefc518` |

Compliance: see [../compliance/README.md](../compliance/README.md).
Migration plan: [../plans/archive/spec_migration_plan.md](../plans/archive/spec_migration_plan.md).
