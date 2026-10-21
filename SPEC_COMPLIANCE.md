# QLang spec.md — Compliance Reference

Purpose: hand-off notes for the next session. Records what was verified against
spec.md, what works, what does not, root causes, and recommended next steps.

Verified by building the project (CMake, C++17, clean) and empirically running
every spec construct through `QLangContext::run()` via `examples/spec_check.cpp`
(a 33-case harness). The shipped `examples/hello.cpp` smoke test passes.

NOTE (reconciliation): spec.md was renamed to SPEC.md and expanded between the
audit and this file (1353 -> 1942 bytes). Line numbers below are against the
CURRENT SPEC.md. New spec requirements NOT covered by the original audit are
flagged with *** and were verified empirically as unimplemented gaps.
Diff history since prior audit:
  - helpers: `print(string)` -> `print(var)`; added `println(var)` (print + CR).
  - added `delete var` helper (delete a variable).
  - multiline strings now shown as assignments (`local ms = """..."""`, `ms = '''...'''`).
  - section headers gained `====` underlines; comments section reworded.

## Legend
- [x] works / matches spec
- [ ] broken or missing vs spec
- (!) edge case or ambiguity

## Working (matches spec)
- [x] lexing: `#` comments, single/double/triple strings, char-as-byte, numbers
- [x] `local a = 32;`, typed `local b:byte = 32` (+ `word/short/int/unsigned`)
- [x] arithmetic `+ - * /`, comparisons `== != < > <= >=`, bitwise `& | ^`,
      shifts `<< >>`, logical `&& ||`, `()` grouping, prefix `++`/`--`
- [x] `for x = 1 to N ... next x` (inclusive, auto-increment)
- [x] `if ... then ... end`, `while ... end`
- [x] string append when left operand is a string (`s += "abc"`)
- [x] int += string is a type error (spec §65–66)
- [x] full pipeline: source → lexer → parser → bytecode → register VM

## Broken / missing vs spec
| Spec | Item | Symptom | Root cause |
|------|------|---------|------------|
| §28–31 | `const year = 2026;` | `parse error: use of undeclared variable 'const'` | Lexer/parser know only `local`, no `const` keyword |
| §57 | `local z # this nil` | `parse error: expected '=' in declaration` | `parseDeclaration` requires a value |
| §69 | `sx += 123` → `"test123"` | `type error: cannot mix string and numeric` | `QLangVM::doBinary` ADD throws unless *both* are strings; should concat when left is string |
| §93–95 | `while (a>0) --a end` | `a` unchanged (loop body `--a` is a no-op) | Compiler bug: unary `++`/`--` as an ExprStmt writes to a throwaway temp reg, not the var reg (see note) |
| §109 | postfix `a++` / `a--` as statements | `parse error: unexpected token '' in expression` | `parseStatement` reads `a++` as compound assign `a ++ <expr>` and chokes on missing operand |
| §107 | `|||` three-bar logical or | not supported | Lexer only emits `||` |
| §21, §58 | `float` type | `local f = 1.13` truncates to int 1 | No `Float` variant; parser hard-truncates (see comment QLangParser.cpp:287) |
| §14, §20 | `blob`, `array` | no way to create values | `QLangValue` has storage/accessors but no lexer/parser syntax for blob or `[...]` array literals |
| §53–54 *** | array literal `local aa:array = [1,2,3]`, `local ab = [0,4,5]` | `parse error: unexpected token '[' in expression` | No array-literal parsing in `parsePrimary` |
| §55 *** | table literal `local t:table = {name="Frank", age=99, ...}` | `parse error: unknown type after ':'` (also `unterminated`/`[` errors) | `table` not a type keyword; no `{...}` literal parsing |
| §22 *** | `table` type | no support | Not in `QLangValue::Type` and no literal/declaration support |
| §71–72 *** | constants readable (`sz = year` valid) and immutable (`year = sx` errors) | both error with `use of undeclared variable 'const'` | `const` not a keyword; no const-storage or immutability tracking |
| §115–116 | `toStr(var)` / `toInt(var)` | `parse error: unexpected token '('` | No call-expression syntax, no builtins |
| §117–120 *** | `type(var)`, `print(var)`, `println(var)`, `printf(str, ...)` | `parse error: unexpected token '('` | No call-expression syntax, no builtins |
| §122 *** | `delete var` | `parse error: use of undeclared variable 'delete'` | `delete` not a keyword; parsed as an identifier/assignment |

## Note on the `while --a` compiler bug
Observed via dumped bytecode (`QLangCompiler::compileStmt` ExprStmt case emits the
unary result into a temp register; no writeback to the variable's register).
Bare `--a` outside a loop only "works" by register-allocation coincidence; inside
a loop the inc/dec lands on a comparison/temp register and the variable is not
updated. Fix target: `QLangCompiler::compileExpr` (Unary) / `compileStmt`
(ExprStmt) — detect `++`/`--` on a Variable and emit a writeback to the variable's
register (or compile statement-form inc/dec like assignment `r += 1`).

## Ambiguity to resolve before fixing
- **Bare assignment auto-declare?** spec §60/65/68 show `c = a`, `sv = 123`,
  `sx = "test"` without `local`, but the code throws `assignment to undeclared
  variable`. Decide whether bare `=` should auto-declare or if `local` is the
  only declaration path. (Affects how many of the "missing" items above are
  actually required.)
- **Operator table typos?** spec §105–107 lists `||` = binary-or and `|||` =
  logical-or; implementation uses conventional `|` = binary-or, `||` = logical-or.
  Confirm intended notation.
- **Resolved (no longer ambiguous):** constant semantics are now explicit —
  constants are readable (`sz = year` valid) but immutable (`year = sx` errors).
  Implement const storage + a write-guard on assignment.

## Recommended fix order (priority)
1. `while`/statement unary `++`/`--` writeback bug (breaks a core spec example).
2. `const` keyword (+ read semantics + immutability guard, spec §28–31, §71–72).
3. String + numeric concatenation (left-is-string rule, spec §63, §66, §69).
4. Call-expression syntax + builtins: `toStr`, `toInt`, `type`, `print`, `println`, `printf` (§115–120).
5. Array literal `[...]` + `array` type (§20, §53–54).
6. `table` type + `{...}` literal (§22, §55).
7. `float` type (add `Float` variant + real float literal handling, §21, §58).
8. `local z` nil declaration (allow valueless declaration, §57).
9. Postfix `a++`/`a--` statement parsing (§109).
10. `blob` source syntax (§14).
11. `delete var` keyword + runtime removal (§122).
12. Triple-quoted string at EOF false "unterminated" (QLangLexer.cpp stringLiteral).

## Harness
`examples/spec_check.cpp` (not yet wired into CMake). Compile against the static
lib: `g++ -std=c++17 -Iinclude examples/spec_check.cpp build/libqlang.a -o /tmp/sc && /tmp/sc`.
Re-run after each fix to confirm counts.
