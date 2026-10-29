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
| `bool` | `Bool` | ✓ | `true`/`false` literals compile to `Bool(1/0)` (Round 14); all boolean-producing operators (`==` `!=` `<` `>` `<=` `>=` `in` `&&` `\|\|` `!`) return `Bool` (Round 17); `local b:bool` parses; `type(true)` → `"bool"`; stringify as `"true"`/`"false"`; bools participate in arithmetic/comparisons via `isNumeric()` |
| `true` | `True` | ✓ | |
| `false` | `False` | ✓ | |
| `def` | `Def` | ✓ | |
| `let` | `Let` | ✓ | alias for `local` |
| `else` | `Else` | ✓ | |
| `range` | `Range` | ✓ | Reserved word; blocks identifier use; also used as builtin `range()` function. `local range = 1` throws parse error. |
| `slice` | — | — | **Not a keyword.** Removed from `keywordType()`. Tokenizes as `Ident`. Can be used as variable name (`local slice = 1`). `slice(a, 0, 1)` still works as builtin call via the `Ident` + `LParen` path in `parsePrimary()`. |
| `goto` | `Goto` | ✓ | |
| `char` | `TypeChar` | ✓ | Listed as type; `char` is a reserved keyword; `local c:char` parses; `type(c)` → `"char"`; signed -128..127; distinct from `byte` (uint8_t); `asInt()` accepts Char (stores in int32_t slot) |
| `type` | `Type` | ✓ | Reserved word; blocks identifier use. `local type = 1` → parse error. `type()` builtin call works via `parsePrimary()` special case (same pattern as `str`/`int`). `type(var)` returns type string. |
| `assert` | `Assert` | ✓ | Reserved word; blocks identifier use. `local assert = 1` → parse error. `assert(bool, str)` works as builtin call via `parsePrimary()` special case (same pattern as `str`/`int`/`type`). Throws runtime error on false condition. |
| `join` | `Join` | ✓ | Reserved word; blocks identifier use. `local join = 1` → parse error. `join(sep, arr)` works as builtin call. |
| `import` | `Import` | ✓ | ★ Added to SPEC reserved words in `9cc0a3f`. Already reserved in the lexer (`keywordType()` → `TokenType::Import`) for the import machinery — documentation gap closed, no implementation gap. |
| `from` | `From` | ✓ | ★ Added to SPEC reserved words in `9cc0a3f`. Already reserved in the lexer (`keywordType()` → `TokenType::From`). |
| `switch` | `Switch` | ✓ | `switch (expr) { case val: ... case in expr: ... default: ... }` — string/int cases via EQ, membership via IN, break exits switch via `m_breakJmps` patching, implicit JMP prevents fall-through |
| `case` | `Case` | ✓ | Reserved word; parsed in switch body |
| `default` | `Default` | ✓ | Reserved word; parsed in switch body as fallback |
| `qword` | `TypeQword` | ✓ | Reserved word; blocks identifier use. `local d:qword` parses; `type(d)` → `"qword"`; `uint64_t` storage (renamed from `dword` per spec errata, Round 21) |
| `long` | `TypeLong` | ✓ | Reserved word; blocks identifier use. `local l:long` parses; `type(l)` → `"long"`; `int64_t` storage (Round 23) |
| `global` | `Global` | ✓ | Reserved word; blocks identifier use at top level. `global a, b;` valid inside function scope for explicit global access |
| `const` | `Const` | ✓ | Reserved word; blocks identifier use. `const year = 2026;` declares constant; `year = sx` → compile error |
| `end` | `End` | ✓ | Reserved word; terminates `if/then/end`, `while/end`, `def/end` forms. Not used in brace-form syntax |
| `cast` | `Cast` | ✓ | Reserved word; blocks identifier use. `cast<int>(1.12)` works as expression via `parsePrimary()` special case |
| `class` | `Class` | ✓ | **Fully implemented** (conforms to expanded spec section `c83ebc8`). Closed field set (`__fields__`/`__const_fields__` descriptors in `QLangObjects.cpp`), `const` fields, unique method/dunder names (compile error), fixed arity, `__str__`, reference counting, `__destroy__`, `type(x)` = class name, **`static` members + `::`** (Round 12, new extension beyond v1). 42 tests at spec_check2 L449–L615 + 17 in `class_static_test`. See "user defined classes — spec conformance". |
| `fetch` | `Fetch` | ✓ | ★ **Implemented** (Round 25). `TokenType::Fetch` in `keywordType()` — `local fetch = 1` → parse error (reserved word). Behavior unspecified in SPEC; reserving is all the spec asks.
| `enum` | `Enum` | ✓ | ★ **Implemented** (Round 28). `TokenType::Enum` in `keywordType()` — `local enum = 1` → parse error (reserved word). `## enum` (2026-10-28 spec): immutable name→value collections, default int / typed `enum Name:byte`; implied sequential values, explicit `= N` resumes the sequence at N+1; member access via `::` **and** `.` (both fold to the compile-time constant); `len(Enum)` = member count (enum is an immutable table); redeclaration → compile error; unknown enum/member → compile error; valueless body → parse error; block-scoped with `local`/`let`, global enums visible everywhere (captured into function bodies like global consts). 16 tests at spec_check2 L791–L806. |
| `step` | `Step` | ✓ | ★ **Implemented** (`e955a8d`). Reserved word: `local step = 1` → parse error. `for x = a to b step N` and `for i in range(a,b) step N`: direction from start/end; user step in the wrong direction → body runs exactly once; `step 0` → infinite loop, bounded by the VM step-limit guard (satisfies the spec's required "timeout guard that results in a runtime exception" — throws `runtime error: ... infinite loop (step limit)`); `for x in [..] step n` → `type error: step is not supported over array literals in for-in loops`. Constant + variable step; continue/break/next intact. 19 tests in `spec_check2`. |
| `$` | `Dollar` | ✓ | ★ **Implemented** (`0c63f4b`). SPEC §The $ Operator: `$age` → `"33"` (scalar-to-string evaluator), table-literal dynamic keys `{ $property = "male" }`, `OP_EVALKEY` opcode dispatched in all 4 exec loops. Rejects array/table/class instances (runtime type error; instances are typed `Table` at compile time, so the check is runtime — documented deviation, Phase 9.2 in `class_action_plan.md`). 6 tests at spec_check2 L617–L626. |
| `$$` | — | — | ★ **Not implemented** — now **fully specified** (SPEC §operator $$, committed `c83ebc8`): indirect access operator, runtime name resolution from a string. No `DollarDollar` token, no `OP_INDIRECT_READ`/`OP_INDIRECT_WRITE` opcodes. Spec: six-stage check pipeline, local-only writes, reads locals + `global`-exposed (global const visible everywhere), no variable creation, functions/lambdas never targetable, const read-only, sequential declaration state. See Not Yet Implemented + Open Items. |
| `static` | `Static` | ✓ | ★ **Implemented** (Round 12, 2026-10-27; spec additions uncommitted). `TokenType::Static` in `keywordType()` — `local static = 1` → parse error (reserved word). `## static` (new extension beyond v1): static fields/methods in a shared `__statics__` table, `ClassName::name` via the `::` scope-resolution operator (`TokenType::ScopeRes`), instance-level static read/write, `static`+`const` allowed / `static`+`local`/`let` forbidden (parse error). 17 tests in `class_static_test`. See "user defined classes — spec conformance" + Open Items. |

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
| `qword (uint64_t)` | `Qword` | `TypeQword` | ✓ | `local d:qword = 4294967296` parses; `type(d)` → `"qword"`; `uint64_t` storage; participates in arithmetic; `toStr()` works (renamed from `dword`, Round 21) |
| `long (int64_t)` | `Long` | `TypeLong` | ✓ | `local l:long = 9007199254740993` parses; `type(l)` → `"long"`; `int64_t` storage; `len` 8; casts work (Round 23) |
| `array` | `Array` | `TypeArray` | ✓ |
| `float` | `Float` | `TypeFloat` | ✓ |
| `table` | `Table` | `TypeTable` | ✓ |
| `bool` | `Bool` | `TypeBool` | ✓ | `true`/`false` compile to `Bool(1/0)` (Round 14); comparison/logical operators return `Bool` (Round 17); `local b:bool` accepted; `type(true)` → `"bool"`; stringify as `"true"`/`"false"`; bools are numeric for arithmetic/comparisons |

