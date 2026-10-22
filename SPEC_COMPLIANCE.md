# QLang SPEC.md — Compliance Reference

Purpose: hand-off notes for the next session. Records what was verified against
SPEC.md, what works, what does not, root causes, and recommended next steps.

Verified by building the project (CMake, C++17, clean) and empirically running
spec constructs through `QLangContext::run()` via `examples/spec_check2.cpp`
(81 cases). The shipped `examples/hello.cpp` smoke test passes.

## Current Status

**138 passed, 0 failed** (harness `examples/spec_check2.cpp`, built against `build/libqlang.a`).

Line numbers below are against the CURRENT SPEC.md (7651 bytes, 456 lines).

## Legend

- [x] works / matches spec (empirically verified this round)
- [ ] broken or missing vs spec (empirically verified this round)
- (!) edge case or ambiguity
- ★ added / newly verified this round

## Working (matches spec)

- [x] lexing: `#` comments, single/double/triple strings, char-as-byte (`'x'`),
      integers, floats (`1.13`), negative literals (`-124`), unary minus
- [x] `local a = 32;`, typed `local b:byte = 32` (+ `word/short/int/unsigned`)
- [x] `local s:str = "x"` — `str` alias recognized as string type
- [x] valueless `local z` declares nil
- [x] `local f = 1.13` real float; `toStr(f)` → `"1.13"`
- [x] arithmetic `+ - * / %`, comparisons `== != < > <= >=`, bitwise `& | ^`,
      shifts `<< >>`, logical `&& ||` / `|||`, `()` grouping, prefix `++`/`--`,
      unary `!` (inverse), postfix `a++`/`a--`
- [x] `**` exponential (extension beyond spec; spec lists `*` but not `**`)
- [x] `for x = 1 to N ... next x` (inclusive, auto-increment)
- [x] `for x = 1 to N { ... }` brace form (same semantics)
- [x] `for i in [1,2,3] ... next i` — iterates array/table elements
- [x] `for c in "string" ... next c` — iterates each char as Byte value
- [x] `for i in range(1,10) ... next i` — range builtin
- [x] `continue` / `break` loop-control keywords (both `next`-form and brace-form)
- [x] `if ... then ... end`, `while ... end`
- [x] `if ... { } else if ... { } else { }` brace-form conditional chains
- [x] `if (b in a)` membership test
- [x] `if a in range(1,10)` range membership test
- [x] `0xc` hex literal
- [x] `true` / `false` boolean literals; truthiness for booleans, strings,
      arrays, tables, numbers (0 = false, non-zero = true)
- [x] string append when left operand is a string (`s += "abc"`)
- [x] `sx += 123` → `"test123"` — int/float/bool RHS coerced to string via VM ADD handler (left-is-string rule)
- [x] `sv = 123; sv += "test"` is a type error
- [x] array literal `[1,2,3]` / `[]`, array concat `aa + ab` → new array
- [x] array append `ab[] = 4` / `ab += 5`, index read/write `ab[0]`, `ab[0] = 55`
- [x] table literal `{name="Frank", age:byte=99, ...}`
- [x] table member read/write `t.name`, `t["age"]`, `t.name = "John"`
- [x] `const year = 2026;` + read `sz = year`
- [x] `const month:byte = 10;` typed const
- [x] const immutability: `year = sx` → "cannot assignment to constant"
- [x] `;` optional silent separator
- [x] `delete var`
|- [x] `blob()` — creates empty blob
- [x] builtins: `toStr`, `toInt`, `type`, `print`, `println`, `printf`, `len`,
      `substr`, `slice`, `range`, `join`, `isset`
||- [x] `sprintf(fmt, ...)` — returns formatted string via `snprintf`-backed parser; supports `%d/%f/%s/%c/%%` and `%.Nf` precision
- [x] `int(nil)` returns 0 (not throw)
- [x] `str()` / `int()` as function-call aliases (not just type keywords)
- [x] `def name(params) { ... }` / `def name(params) end` — global-scope named
      functions; return-value propagation to caller's dest register
- [x] `def` is only valid in global scope; no `[...]` capture
- [x] nested `def` (function closure) — inner `def` captures outer params correctly
- [x] full pipeline: source → lexer → parser → bytecode → register VM
- [x] closure in table literal + method call (`events.onLoad("test")`)
- [x] `obj.method(args)` parsed as call (not assignment) via peek-ahead in
      `parseStatement()`
- [x] arrow-function closures: `local f = (params)[] { }` (no capture) and
      `local f = (params)[x] { }` (explicit capture by reference)
- [x] closure live-binding: captured variable reassignments visible inside closure
      (`local x = 1; local f = (y)[x] { return x + y }; x = 10; f(20)` → 30)
- [x] label/goto: `:label`, `goto label`, `if cond goto label`; same-scope only;
      undefined-label error

## Missing vs Spec

|| Spec | Item | Status |
|------|------|--------|
| §C++ interface | `set(name, value, isConst=false)` | ✓ implemented | Merged injectConst/injectGlobal into single `set()` with third `isConst` param. Seeds var into scope; when true, adds to `m_injectedConsts` so VM skips end-of-run sync and compiler's `guardNotConst()` rejects QLang assignment. |
| §C++ interface | helpers to create `QValue` from common C++ types | ✓ implemented | `include/qlang/QValue.h` provides `using QValue = QLangValue;` and free helpers: `qval_nil`, `qval_bool`, `qval_byte`, `qval_char`, `qval_word`, `qval_short`, `qval_int`, `qval_unsigned`, `qval_dword`, `qval_float`, `qval_str` (3 overloads), `qval_blob`, `qval_array`, `qval_table`. |
| §C++ interface | `CompiledProgram::constVars` | ✓ implemented | New field on `CompiledProgram`; populated from `m_constVars` at compile time (includes both `const`-declared and injected consts). VM uses it to skip scope sync for const vars. |

## Extensions Beyond Spec (present in implementation, absent from spec)

| Feature | Notes |
|---------|-------|
| `|||` logical-or | Spec lists only `||`. `|||` is an extension (TokenType::LogicalOr3). |
| `**` exponential | Spec lists `*` for multiplication but not `**` for power. `**` is an extension (TokenType::Exponential). |
| `let` as alias for `local` | Spec lists `let` as reserved word and alias; implementation supports it. This is spec-conformant, not an extension. |
| `next x` loop delimiter | Spec shows `for x = 1 to 32 \n next x`; implementation supports it. Not an extension. |
| `{`/`}` brace-form loops and conditionals | Spec shows brace syntax in examples; implementation supports it. Not an extension. |

## Reserved-Word Coverage (SPEC.md §33–67 vs. lexer `keywordType()`)

| Spec keyword | TokenType | In `keywordType()` | Notes |
|---|---|---|---|
| `for` | `For` | ✓ | |
| `to` | `To` | ✓ | |
| `in` | `In` | ✓ | |
| `then` | `Then` | ✓ | |
| `while` | `While` | ✓ | |
| `continue` | `Continue` | ✓ | |
| `break` | `Break` | ✓ | |
| `return` | `Return` | ✓ | |
| `string` | `TypeString` | ✓ | |
| `str` | `StrType` | ✓ | alias |
| `byte` | `TypeByte` | ✓ | |
| `blob` | `TypeBlob` | ✓ | |
| `word` | `TypeWord` | ✓ | |
| `short` | `TypeShort` | ✓ | |
| `int` | `TypeInt` | ✓ | |
| `unsigned` | `TypeUnsigned` | ✓ | |
| `array` | `TypeArray` | ✓ | |
| `float` | `TypeFloat` | ✓ | |
| `table` | `TypeTable` | ✓ | |
| `bool` | `Bool` | ✓ | `true`/`false` literals compile to `Int(1/0)`; `local b:bool` parses; `type(b)` → `"bool"`; bools participate in arithmetic/comparisons via `isNumeric()` |
| `true` | `True` | ✓ | |
| `false` | `False` | ✓ | |
| `def` | `Def` | ✓ | |
| `let` | `Let` | ✓ | alias for `local` |
| `else` | `Else` | ✓ | |
|| `range` | `Range` | ✓ | Reserved word; blocks identifier use; also used as builtin `range()` function. `local range = 1` throws parse error. |
|| `slice` | — | — | **Not a keyword.** Removed from `keywordType()`. Tokenizes as `Ident`. Can be used as variable name (`local slice = 1`). `slice(a, 0, 1)` still works as builtin call via the `Ident` + `LParen` path in `parsePrimary()`. |
| `goto` | `Goto` | ✓ | |
|| `char` | `TypeChar` | ✓ | Listed as type; `char` is a reserved keyword; `local c:char` parses; `type(c)` → `"char"`; signed -128..127; distinct from `byte` (uint8_t); `asInt()` accepts Char (stores in int32_t slot) |
| `type` | `Type` | ✓ | Reserved word; blocks identifier use. `local type = 1` → parse error. `type()` builtin call works via `parsePrimary()` special case (same pattern as `str`/`int`). `type(var)` returns type string. |
|| `switch` | `Switch` | ✓ | `switch (expr) { case val: ... case in expr: ... default: ... }` — string/int cases via EQ, membership via IN, break exits switch via `m_breakJmps` patching, implicit JMP prevents fall-through |
|| `case` | `Case` | ✓ | Reserved word; parsed in switch body |
|| `default` | `Default` | ✓ | Reserved word; parsed in switch body as fallback |
## Value-Type Coverage (SPEC.md §14–28 vs. QLangValue::Type)

| Spec type | QLangValue::Type | Token type | Notes |
|---|---|---|---|
| `string/str` | `String` | `TypeString`/`StrType` | ✓ |
| `blob` | `Blob` | `TypeBlob` | ✓ |
|| `char (int8_t)` | `Char` | `TypeChar` | ✓ | `local c:char = -50` parses; `type(c)` → `"char"`; signed range -128..127; participates in arithmetic; distinct from `byte` (uint8_t) at type level |
| `byte (uint8_t)` | `Byte` | `TypeByte` | ✓ |
| `word (uint16_t)` | `Word` | `TypeWord` | ✓ |
| `short (int16_t)` | `Short` | `TypeShort` | ✓ |
| `int (int32_t)` | `Int` | `TypeInt` | ✓ |
| `unsigned (uint32_t)` | `Unsigned` | `TypeUnsigned` | ✓ |
| `dword (uint64_t)` | `Dword` | `TypeDword` | ✓ | `local d:dword = 4294967296` parses; `type(d)` → `"dword"`; `uint64_t` storage; participates in arithmetic; `toStr()` works |
| `array` | `Array` | `TypeArray` | ✓ |
| `float` | `Float` | `TypeFloat` | ✓ |
| `table` | `Table` | `TypeTable` | ✓ |
| `bool` | `Bool` | `TypeBool` | ✓ | `true`/`false` compile to `Int(1/0)`; `local b:bool` accepted; `type(b)` → `"bool"`; bools are numeric for arithmetic/comparisons |

## Harness

- `examples/spec_check.cpp` — original cases (const, nil, float, concat, for, if,
  while, operators, toStr/toInt).
- `examples/spec_check2.cpp` — 81 test cases covering all spec features including `bool`, `char`, `dword`, `switch`, `range`/`slice` reserved-word status, closures, `**`, `let`, goto, slice, range, and method calls.
Compile against the static lib:
`g++ -std=c++17 -Iinclude examples/spec_check2.cpp build/libqlang.a -o /tmp/sc && /tmp/sc 2>/dev/null`
Result: **138 passed, 0 failed.** All spec builtins implemented.

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

## Current Round Summary

Core spec fully implemented. **138 passed, 0 failed.**

## Open Items

- **`next` keyword form:** Resolved. `next` without a variable now works (assumes current loop variable). `next x` with explicit var still works. `next y` where `y != loopVar` is treated as `next` + statement `y` — `y` must be declared separately.
- **`sprintf(fmt, ...)` (§374):** Implemented. `Builtin::Sprintf` added to enum, registered in `builtinId()`, implemented in `QLangVM.cpp` mirroring `Printf` but returning `QLangValue::String(out)`. Supports `%d`, `%f` (with `%.Nf` precision), `%s`, `%c`, and `%%` literal. 9 test cases pass.
- **Nested call register allocation:** Fixed. Compiler now compiles call args into dedicated temp registers then emits `MOV` to the VM's expected `dest+1..dest+argCount` slots. This prevents nested calls from overwriting parent arg registers. Verified with `printf("%s %d", join(",", ar), len(ar))` after `for i in ar` loop.
- **`join(separator, arr)` (§367):** Implemented. `Builtin::Join` added to enum, registered in `builtinId()`, implemented in `QLangVM.cpp` via `callBuiltin`. Non-string elements converted via `valueToString`. 8 test cases pass.
- **`isset(var)` (§381):** Implemented. `Builtin::IsSet` added to enum, registered in `builtinId()`, implemented in `QLangVM.cpp` via `callBuiltin`. Accepts a string (variable name); checks `prog.vars` map and `m_deleted` set to determine existence during execution (vars live in registers, not scope, until `run()` completes). 5 test cases pass.
