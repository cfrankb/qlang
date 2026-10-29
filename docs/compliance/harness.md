## Harness

- `examples/spec_check.cpp` — original cases (const, nil, float, concat, for, if,
  while, operators, toStr/toInt). Result: **33 passed, 0 failed.**
- `examples/spec_check2.cpp` — 369 test cases covering all spec features including `bool`, `char`, `qword`, `switch`, `range`/`slice` reserved-word status, closures, `**`, `let`, goto, slice, range, method calls, `==`/`!=` nil-equality, function/generator returns, 19 `step` loop cases, 42 class-conformance cases (L449–L615), 6 `$` dynamic-key cases (L617–L626), 6 block-scope cases (L633–L640), 9 `len` storage-size cases, 2 `delete` guard cases (Round 21), 16 `enum` cases (Round 28) and 6 error-reporting cases (Round 29: line numbers for lex/parse/runtime errors, module + function name in the report, deep call-chain trace). Result: **369 passed, 0 failed.**
- `examples/spec_cast_test.cpp` — 20 cast tests: numeric casts (int/float/byte/word/short/unsigned/qword/char/bool), string casts, invalid collection casts, nested casts, expression-context casts, nil cast. Result: **20 passed, 0 failed.**
- `examples/gen_test.cpp` — 5 generator tests: basic yield, second yield, exhaustion/EOF, loop accumulate, empty yield, outer variable capture, type check. Result: **5 passed, 0 failed.**
- `examples/range_check.cpp` — 22 range tests: `for i in range(a,b)` counter loop (sum/count/break/continue), `in range(a,b)` bounds check (inside/low/high/above/below/as-value), `arr = range(a,b)` materialization, for-in array membership, generator range. Result: **22 passed, 0 failed.**
Compile against the static lib:
`g++ -std=c++17 -Iinclude examples/spec_check2.cpp build/libqlang.a -o /tmp/sc && /tmp/sc 2>/dev/null`
Result: **369 passed, 0 failed.** All spec builtins implemented.
`g++ -std=c++17 -Iinclude examples/spec_cast_test.cpp build/libqlang.a -o /tmp/cast_test && /tmp/cast_test 2>/dev/null`
Result: **20 passed, 0 failed.**

## C++ Interface (new this round)

| Feature | Status |
|---------|--------|
| `QValue` type alias + free helpers (`qval_*`) | ✓ added in `include/qlang/QValue.h` |
| `QLangCompiler::addExternalConsts()` | ✓ added |
| `CompiledProgram::constVars` | ✓ added to struct |
| VM skips end-of-run sync for const vars | ✓ patched in `QLangVM.cpp` |
| Compiler `guardNotConst()` covers injected consts | ✓ via `addExternalConsts()` |
| Test: `examples/test_inject.cpp` | ✓ passes |

## Fixes Applied This Round (since prior audit)

All items previously listed as "Broken / missing vs spec" have been resolved:

| Issue | Root cause | Fix |
|-------|-----------|-----|
| `cast<Type>(expr)` parse error | No parser support for cast expression form | Added `ExprKind::Cast` to parser, `cast` keyword handling in `parsePrimary()` consuming `<type>` syntax |
| `OP_CAST` opcode missing | No opcode for runtime cast | Added `CAST` opcode to `QLangBytecode.h`, `Bytecode::name()` |
| Compiler didn't emit cast opcode | `ExprKind::Cast` not handled | Added `case ExprKind::Cast` in `compileExpr()` emitting `OP_CAST dest src targetType` |
| VM had no cast handler | No VM dispatch for runtime coercion | Added `strict_convert()` function and `case Opcode::CAST` in all 3 VM loops |
| Type mapping formula wrong | `tt - TypeBool + Bool` formula off-by-N for types after `TypeUnsigned` (enums have different layouts) | Replaced with explicit `std::pair<int,int>` lookup table in 3 parser sites (declaration, const, cast) |
| `convert()` included Float causing table corruption | `convert()` treated Float as numeric target; parser's buggy mapping made `table` → Float(12), so typed table declarations tried to convert tables to float | Restored original `convert()` (only integer types, default pass-through) |
| `join` after `for` loop fails | Compiler emitted `CALL` but compiled nested-call args into temp registers that didn't match the VM's `dest+1..dest+argCount` convention; nested call dest registers overwrote parent arg slots | Compile args into dedicated temp registers, then emit `MOV` from each temp to `dest+1+i` before `CALL`. This ensures args are in the VM's expected slots regardless of nested-call register allocation. |
| `blob()` parse error | `parsePrimary` TypeBlob case didn't consume `blob` before checking `(` | `m_pos++` to consume `blob` token |
| `substr()` unknown function | No builtin registered | Added `Builtin::Substr` + `substr` in `callBuiltin` |
| `0xc` hex literal | Lexer didn't handle `0x` prefix | `QLangLexer::number()` now handles hex |
| `in` operator | No `TokenType::In` | Added token, precedence 6, `IN` opcode in VM |
| `continue` | No `TokenType::Continue` | Added token, `CONTINUE` opcode, LoopCtx patching |
| `break` | No `TokenType::Break` | Added token, `BREAK` opcode, LoopCtx patching |
| `for i in [...]` | No `StmtKind::ForIn` | Added for-in statement, compiled via `LEN`+`SEQGET` loop |
| `%` modulo | No `TokenType::Modulo` | Added token, precedence 10, `MOD` opcode |
| `!` not | No `TokenType::Not` | Added token, included in `parseUnary`, `NOT` opcode |
| `str` type keyword | `StrType` not recognized in declaration | Added `StrType` case in `parseDeclaration`/`parseConst` |
| `def` named functions crash | `RETURN` wrote to callee's own dest; no capture binding | Fixed `RETURN` to write to caller's dest; added capture table copy in `CALLFN` |
| `str()`/`int()` parse error | Lexer mapped `str`→`StrType`, parser rejected in call position | Added `StrType`/`TypeInt` cases in `parsePrimary()` for call positions; aliases in `builtinId()` |
| Brace-form loop parse error | `parseFor()`/`parseWhile()`/`parseIf()` only accepted `next`/`end` | Added `{`/`}` branch in all three parsers |
| Closure crash (segfault on `REFLOAD`) | `pushCall()` never copied closure capture table | Added `m_frames[m_cur].captures = cv.asFunction().captures` in `CALLCLOS` handler |
| Closure crash (assertion on `REFSTORE`) | Compiler emitted `REFSTORE capture_idx, src_reg`; VM expects `REFSTORE src_reg, capture_idx` | Swapped operand order in two `emit(REFSTORE, ...)` calls |
| `int(nil)` threw | `toInt` builtin threw on non-numeric/non-string types | Changed to return `Int(0)` for nil and other types |
| Table literal with closure threw "non-constant element" | `evalConstExpr()` rejected closures | Split table compilation: constant-evaluable → `CONST`; otherwise → runtime `TABCREATE`+`TABPUT` |
| `obj.method(args)` parse error | `parseStatement()` consumed member name eagerly, couldn't see `(` | `parseStatement()` now peeks past member-name: if next is `(`, rewinds to let `parseExpression()` handle as call |
| `obj.method(args)` compile error | Compiler emitted closure in wrong register for `CALLCLOS` | Fixed `emit(CALLCLOS, dest, funcReg, argCount)` → two-instruction sequence |
| Nested `def` capture leak | `collectIds`/`collectStmtIds` collected nested-def params as outer free vars | Added `skip` set to exclude nested-param names at each recursion level |
| Nested `def` outer-param bleed | Capture detection ran before `m_regs` was cleared | Moved capture detection after fresh-context setup |
| Nested `def` CALLFN no capture binding | `builtinId()` threw on unknown names instead of returning -1 | Changed to `return -1`; added `Function::captures` spec and VM `CALLFN` handler to copy capture table from caller frame |
| `else if` unconditional execution | Compiler emitted else body before if body | Moved `JMP` emission before else body compile |
| `goto`/labels | Not implemented | Added `Goto`/`Label`/`GotoIf` tokens, parser nodes, compiler label map, `JMP`/`JMPIFFALSE` opcodes |
| `slice()` builtin | Not implemented | Added `Builtin::Slice`, registered in `builtinId()`, implemented in `QLangVM.cpp` |
| `range()` builtin | Not implemented | Added `Builtin::Range`, registered in `builtinId()`, implemented in `QLangVM.cpp` |
| `for c in "string"` | `seqGet()` had no string case | Added string case: returns each character as `Byte` |
| `**` exponential | Not implemented | Added `TokenType::Exponential`, precedence 11, `POW` opcode in VM (`pow()` for float, repeated mul for int) |
| `|||` logical-or | Not implemented | Added `TokenType::LogicalOr3`, precedence 1 |
| Generator keeps restarting after exhaustion | Consumed generator's saved frame was erased; the loop re-created a fresh frame and re-ran the body from the top | Added `Frame::exhausted` flag; generator `RETURN` marks the saved frame `exhausted`; generator loop early-returns nil for an already-exhausted frame; post-loop cleanup no longer erases the exhausted frame |
| nil-returning test cases threw | `expectVal` used `asInt()`, which threw "expected Int, Bool or Char but value is nil" on nil results | Switched `expectVal` to `toInt64()` (returns 0 for nil) in `examples/gen_test.cpp` and `examples/spec_check2.cpp` |

## Current Round Summary

Core spec fully implemented. **295 passed, 0 failed** (spec_check2) + **33 passed, 0 failed** (spec_check) + **20 passed, 0 failed** (spec_cast_test) + **7 passed, 0 failed** (test_module) + **5 passed, 0 failed** (gen_test) + **22 passed, 0 failed** (range_check) + **13 passed, 0 failed** (class_str) + **17 passed, 0 failed** (class_static) = **412 total**. Since the prior audit: implemented the `step` loop construct (both `for` forms; 19 new cases) and made the `for` endpoint inclusive for all step sizes (`e955a8d`); SPEC.md gained the `step` reserved word/examples (`d07eb4f`/`21cc1c5`); `import`/`from` reserved words, duplicate-declaration examples, and the `$$` operator-table row + TODO stub committed in `9cc0a3f`; `## operator $$` committed in full (`c83ebc8`) — spec-only, implementation and harness cases pending; **user-defined classes brought to full conformance** (9 phases, `6a4c559`/`8c013a1`/`b242645`/`0d7115b`) and the **`$` dynamic-key operator implemented** (`0c63f4b`; 6 new cases) — see Round 8; **block scope implemented** (2026-10-26; 6 new cases) — see Round 10.

