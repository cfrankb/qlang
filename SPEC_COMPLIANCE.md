# QLang SPEC.md — Compliance Reference

Purpose: hand-off notes for the next session. Records what was verified against
SPEC.md, what works, what does not, root causes, and recommended next steps.

Verified by building the project (CMake, C++17, clean) and empirically running
spec constructs through `QLangContext::run()` via `examples/spec_check2.cpp`
(81 cases). The shipped `examples/hello.cpp` smoke test passes.

## Current Status

**103 passed, 0 failed** (harness `examples/spec_check2.cpp`, built against `build/libqlang.a`).

Line numbers below are against the CURRENT SPEC.md (5993 bytes, 373 lines).

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
- [x] `sx += 123` → `"test123"` (left-is-string concat)
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
- [x] `blob()` — creates empty blob
- [x] builtins: `toStr`, `toInt`, `type`, `print`, `println`, `printf`, `len`,
      `substr`, `slice`, `range`
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

| Spec | Item | Status |
|------|------|--------|
| §18–28 | `bool` — listed as type and reserved word | **Not implemented.** No `TypeBool` in `QLangValue`, no `TokenType::BoolType` in lexer. `true`/`false` literals work but `local b:bool` would fail to parse. |
| §18 | `char (int8_t)` — listed as type | **Not implemented.** `TokenType::Char` exists for `'x'` literal syntax, but no `TypeChar` in `QLangValue` and `char` is not in `keywordType()`. The spec distinguishes `char` (int8_t) from `byte` (uint8_t); both map to the same runtime `Byte` type currently. |
| §24 | `dword (uint64_t)` — listed as type | **Not implemented.** No `TypeDword` in `QLangValue`, no `TokenType::DwordType`. No 64-bit integer type exists in the value system. |
| §206–219 | `switch`/`case`/`default` — listed as reserved words; full switch syntax in spec | **Not implemented.** No lexer tokens, no parser, no compiler, no VM opcode. The spec shows `switch (var) { case "string": break; case in [1,2,3]: break; default: }` syntax but the compiler does not support it. |
| §59 | `range` — listed as reserved word | **Ambiguous.** Used as builtin function name `range()` but not in `keywordType()`. If spec intends it to block identifier use, the lexer is incomplete. |
| §60 | `slice` — listed as reserved word | **Ambiguous.** Used as builtin function name `slice()` but not in `keywordType()`. Same ambiguity as `range`. |

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
| `range` | `Range` | ✓ | Reserved word in spec; lexer recognizes it. Used as builtin `range()` function. Cannot be used as identifier. |
| `slice` | `Slice` | ✓ | Reserved word in spec; lexer recognizes it. Used as builtin `slice()` function. Cannot be used as identifier. |
| `goto` | `Goto` | ✓ | |
|| `char` | `TypeChar` | ✓ | Listed as type; `char` is a reserved keyword; `local c:char` parses; `type(c)` → `"char"`; signed -128..127; distinct from `byte` (uint8_t); `asInt()` accepts Char (stores in int32_t slot) |
| `dword` | `TypeDword` | ✓ | Listed as type; `dword` is a reserved keyword; `local d:dword` parses; `type(d)` → `"dword"`; `uint64_t`; arithmetic supported; overflow on int64_t conversion throws |
| `switch` | — | ✗ | Listed as reserved word; no token, parser, compiler, or VM support |
| `case` | — | ✗ | Listed as reserved word; no token |
| `default` | — | ✗ | Listed as reserved word; no token |

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
- `examples/spec_check2.cpp` — 81 cases covering all spec features tested, plus 15 new tests for `bool`, `char`, `dword`, and `range`/`slice` reserved-word status.
Compile against the static lib:
`g++ -std=c++17 -Iinclude examples/spec_check2.cpp build/libqlang.a -o /tmp/sc && /tmp/sc 2>/dev/null`
Result: **103 passed, 0 failed**.

## Fixes Applied This Round (since prior audit)

All items previously listed as "Broken / missing vs spec" have been resolved:

| Issue | Root cause | Fix |
|-------|-----------|-----|
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

## Open Items

| `bool` type (§18–28): The spec lists `bool` as a type and reserved word.
  `true`/`false` literals work; truthiness is evaluated correctly.
  `local b:bool` now parses and `type(b)` returns `"bool"`. Bool values are
  stored as `Int(1/0)` at runtime. Three options were considered and the
  implementation chose (1) — adding `TypeBool` + `TokenType::BoolType` and
  mapping to `Int(0/1)` at runtime. |
- **`char` vs `byte` (§18):** ✅ RESOLVED. Added `TypeChar` + `TokenType::TypeChar`. `char` (int8_t) is now distinct from `byte` (uint8_t) at the value and type level. `local c:char = -50` parses; `type(c)` returns `"char"`; `char` participates in arithmetic; signed range -128..127. `char` is also a reserved word (cannot be used as identifier).
- **`dword` type (§24):** ✅ RESOLVED. Added `TypeDword` + `TokenType::TypeDword`. `local d:dword = 4294967296` parses; `type(d)` → `"dword"`; stores in `uint64_t` variant slot; participates in arithmetic; `toStr()` works; overflow on conversion to int64_t throws Runtime error.
- **`switch`/`case`/`default` (§206–219):** Spec shows full switch syntax with
  string cases, `in`-cases, `range`-cases, and `default`. No lexer tokens,
  parser nodes, compiler logic, or VM opcodes exist. This is a substantial
  feature gap.
- **`range`/`slice` reserved-word ambiguity (§59, §60):** Both are listed as
  reserved words in the spec but are only used as builtin function names in
  the implementation. If the intent is to block identifier use (e.g., you
  cannot name a variable `range`), the lexer needs `TokenType::Range` and
  `TokenType::Slice` added to `keywordType()`. If they are just builtin names,
  the spec should remove them from the reserved-words list.
- **`next` keyword form:** The spec shows `for x = 1 to 32 \n next x` with an
  explicit loop variable. The implementation accepts `next` without a variable
  too (`for x = 1 to 3 \n next`). The spec's `next x` form is supported but
  the bare `next` form is also accepted — worth confirming which is intended.
