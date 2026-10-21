# QLang SPEC.md — Compliance Reference

Purpose: hand-off notes for the next session. Records what was verified against
SPEC.md, what works, what does not, root causes, and recommended next steps.

Verified by building the project (CMake, C++17, clean) and empirically running
spec constructs through `QLangContext::run()` via `examples/spec_check.cpp` plus a
new `examples/spec_check2.cpp` harness (40+ cases). The shipped `examples/hello.cpp`
smoke test passes.

## Reconciliation (this round)
The prior audit was written against an earlier source state. Since then the
lexer/parser/compiler advanced substantially, so **many prior rows were STALE** —
`const`, `local z` nil, `float`, `sx += 123` string-concat, `[...]`/`{...}`
literals, `delete`, postfix `a++`/`a--`, `aa + ab` array-concat, and the
`while --a` writeback bug are all now implemented. Those rows are corrected here
and marked "Resolved since prior audit". Line numbers below are against the
CURRENT SPEC.md (4287 bytes).

Diff history since prior audit:
  - helpers: `print(string)` -> `print(var)`; added `println(var)` (print + CR).
  - added `delete var` helper (delete a variable).
  - multiline strings now shown as assignments (`local ms = """..."""`, `ms = '''...'''`).
  - section headers gained `====` underlines; comments section reworded.
  - **Added** `const` keyword, typed `const month:byte`, const immutability guard.
  - **Added** valueless `local z` (nil), real `float` literal, unary minus.
  - **Added** `[...]` array and `{...}` table literals + array/table access/append.
  - **Added** `blob()`, call-expression syntax, `len`/`type`/`toStr`/`toInt`/`print`/`println`/`printf`.
  - **Added** postfix `a++`/`a--`, statement-form `--a` writeback (fixes while bug).
  - **Added** function capture scope: the `[...]` capture list is OPTIONAL —
    `local f = (params) {` = no capture; `local f = (params)[x, y] {` = explicit
    capture, by reference, from the enclosing/caller context. Semantic: capture is
    ALWAYS only explicit — the `[...]` list is the complete and only set of
    captured variables; nothing is captured implicitly. Captured variables are
    bound by reference and that reference persists across reassignment of the
    outer variable (live binding). Closure is only available in function scope —
    the captured variables are accessible only within the function body; the
    capture does not escape the function scope. Named `def` is only valid in
    global scope and has no closure (no `[...]` capture); capture applies only to
    arrow-function assignments. `local` always binds to the current scope — distinct
    from capture, which pulls from the enclosing context. Function feature added in
    both forms: local arrow closures and named `def` global functions.

## Legend
- [x] works / matches spec (empirically verified this round)
- [ ] broken or missing vs spec (empirically verified this round)
- (!) edge case or ambiguity
- ★ added / newly verified this round

## Working (matches spec)
- [x] lexing: `#` comments, single/double/triple strings, char-as-byte, integers,
      floats (`1.13`), negative literals (`-124`), unary minus
- [x] `local a = 32;`, typed `local b:byte = 32` (+ `word/short/int/unsigned`)
- [x] valueless `local z` declares nil (spec §106)
- [x] `local f = 1.13` real float; `toStr(f)` -> `"1.13"` (spec §23, §107)
- [x] arithmetic `+ - * /`, comparisons `== != < > <= >=`, bitwise `& | ^`,
      shifts `<< >>`, logical `&& ||` / `|||`, `()` grouping, prefix `++`/`--`
- [x] postfix `a++` / `a--` as statements (spec §205)
- [x] `for x = 1 to N ... next x` (inclusive, auto-increment)
- [x] `if ... then ... end`, `while ... end`
- [x] string append when left operand is a string (`s += "abc"`, spec §111)
- [x] `sx += 123` -> `"test123"` (left-is-string concat; spec §117) — **fixed**
- [x] `sv = 123; sv += "test"` is a type error (spec §113–114)
- [x] array literal `[1,2,3]` / `[]`, array concat `aa + ab` -> new array (spec §88–89, §103)
- [x] array append `ab[] = 4` / `ab += 5`, index read/write `ab[0]`, `ab[0] = 55` (spec §99–101)
- [x] table literal `{name="Frank", age:byte=99, ...}` (spec §91)
- [x] table member read/write `t.name`, `t["age"]`, `t.name = "John"` (spec §93–96)
- [x] `const year = 2026;` + read `sz = year` (spec §58, §119) — **fixed**
- [x] `const month:byte = 10;` typed const (spec §59)
- [x] const immutability: `year = sx` -> "cannot assignment to constant" (spec §120)
- [x] `;` optional silent separator (spec §208)
- [x] `delete var` (spec §222)
- [x] builtins: `toStr`, `toInt`, `type`, `print`, `println`, `printf`, `len` (spec §214–220)
- [x] full pipeline: source → lexer → parser → bytecode → register VM

## Broken / missing vs spec
| Spec | Item | Symptom | Root cause |
|------|------|---------|------------|
| §16 | `blob()` | `parse error: expected '(' after 'blob'` (and standalone `blob()` -> `unexpected token 'blob' at start of statement`) | `QLangParser::parsePrimary` `TypeBlob` case calls `check(LParen)` **before** advancing past `blob`; `current()` is still the `TypeBlob` token, so the check always fails. Fix: `m_pos++` the `blob` token first, mirroring the `Ident` call case (parser ~line 467). |
| §221 | `substr(str, start, len)` | `parse error: unknown function 'substr'` | `substr` absent from `QLangCompiler::builtinId` and there is no `Builtin::Substr` in `QLangBytecode.h` / `callBuiltin` (VM). |
| §85 | `0xc` hex literal | `parse error: use of undeclared variable 'xc'` | `QLangLexer::number()` reads only `[0-9.]`; `0xc` lexes as Integer `0` then Ident `xc`. |
| §142–148 | `in` operator (`b in a`) | `parse error: expected ')' but found 'in'` | No `In` token; `in` lexes as Ident and `parseBinary` stops at it. |
| §166 | `continue` | `parse error: use of undeclared variable 'continue'` | Not a keyword; parsed as an Ident variable read. |
| §169 | `break` | `parse error: use of undeclared variable 'break'` | Not a keyword; parsed as an Ident variable read. |
| §182–188 | `for i in [...]` | `parse error: expected '=' but found 'in'` | `parseFor` requires `to`; no `for … in …` loop. |
| §206 | `%` modulo | `lex error: unexpected character '%'` | No `%` in `operatorToken`; no `binOp` case. |
| §207 | `!` not | `lex error: unexpected character '!'` | Only `!=` lexed; lone `!` unsupported; no `unOp` case. |
| §38 | `str` reserved word | `parse error: unknown type after ':'` (`local s:str = "x"`) | `str` absent from `keywordType` (only `string` maps to `TypeString`). |

## Functions (entirely unimplemented) ★
SPEC.md §225–263 is **not covered by the implementation at all**:

The function feature is implemented in **two forms** — both must be supported:
- **local** arrow-function closures: `local f = (params)[...] { }` (optional explicit
  `[...]` capture; closure, function-scope only).
- **def** named global functions: `def name(...) { }` / `def name(...) end` (global scope
  only, no closure, no `[...]` capture).
- `def name(params) … end` — `parse error: unexpected token 'end'` (no `Def` keyword).
- `def name(params) { … }` brace form — same.
- **`def` is only valid in global scope and has no closure** — no `[...]` capture; it is
  a plain global function. Capture/closure is supported only on arrow-function
  assignments (`local f = (params)[...] { }`).
- `local f = (params) { … }` arrow-function assignment — same.
- **capture scope** (NEW §247–263): the `[...]` capture list is OPTIONAL — `local f =
  (params) {` = no capture; `local f = (params)[x, y] {` = capture `x` and `y` by
  reference from the enclosing/caller context. Semantic per spec: **capture is always
  only explicit** — the `[...]` list is exhaustive; nothing is captured implicitly, and
  captured variables always come from the caller (enclosing) context. Reference is
  maintained even after the captured variable is reassigned (live binding). Closure
  is only available in function scope — captured variables are accessible only within
  the function body; the capture does not escape the function scope. Contrast: `local`
  always binds to the current scope — it never pulls from nor escapes to an enclosing
  scope. Capture (enclosing context) and `local` (current scope) are distinct mechanisms.
- `name(args)` call syntax + dispatch — **parses** (`Ident` + `LParen` -> `ExprKind::Call`)
  and dispatches to builtins only; there is no user-defined-function storage/closure,
  so calls to `def`-defined functions cannot exist.

To implement: add a `Def` token + `parseFunction` (name + params + body, brace or `…end`);
`def` is restricted to global scope and has no closure (no `[...]` capture). Arrow-function
assignment separately builds a closure with the optional explicit `[...]` capture list.
Add a function registry + `Call` dispatch to user functions (not just builtins), and a
closure/activation-record mechanism in the VM that binds the explicit capture list (by
reference) from the enclosing context — the reference must survive later reassignment of
the captured variable (live binding).

## Operators / keywords with no support ★ (summary)
From the tables above: `in`, `continue`, `break`, `for … in`, `%`, `!`, `str`,
and `def`/functions. Reserved words in SPEC.md §27–50 that the lexer does **not**
recognize: `str`, `continue`, `break`, `in`, and `def` (only `for/to/next/if/then/
while/end` + type keywords + `local/const/delete` are keywords).

## Resolved since prior audit (formerly listed as broken)
- **`while --a` writeback bug (§175–177):** fixed. `QLangCompiler::compileStmt`
  ExprStmt case now detects statement-form `++`/`--` on a Variable and emits
  `INC`/`DEC` directly to the variable's register. Verified: `while (a>0) --a end`
  drives `a` to `0`.
- **`const` (§53–59, §119–120):** now lexed (`Const`), parsed (`parseConst`, incl.
  typed), compiled (`StmtKind::Const` + `m_constVars`), and write-guarded
  (`guardNotConst`). Read works, assignment errors.
- **`local z` nil (§106):** `parseDeclaration` allows a valueless declaration;
  compiler emits nil. Verified.
- **`float` (§23, §107):** `parsePrimary` builds a real `Float` literal
  (`std::stod`); `toStr` prints the fractional part. Verified.
- **`sx += 123` → `"test123"` (§117):** `QLangVM::doBinary` ADD uses the
  left-is-string concat rule. Verified.
- **`[...]` / `{...}` literals, `blob()`, call syntax, `delete`, postfix inc/dec,
  array/table access & concat:** all now parse and compile (see Working list).
  (`blob()` itself is broken — see table — but the literal/array/table machinery works.)

## Ambiguity to resolve before fixing
- **Bare assignment auto-declare?** spec §109/§113/§116 show `c = a`, `sv = 123`,
  `sx = "test"` without `local`. The compiler *does* auto-declare implicit locals
  (`regOf < 0 -> declareVar`, spec §95), so these run. Confirm this is intended.
- **Operator table — resolved, no typo:** spec §201/§203 lists `|` = binary-or
  and `||` = logical-or, which matches the implementation. (`|||` three-bar is also
  accepted as logical-or.) No notation conflict.
- **Resolved (no longer ambiguous):** constant semantics are explicit — readable but
  immutable. Implemented via const storage + write-guard.

## Recommended fix order (priority)
1. `blob()` parser bug — one-line `m_pos++` before the `LParen` check (§16).
2. `substr` builtin — add `Builtin::Substr` + `substr` in `callBuiltin` + `builtinId` (§221).
3. `def` functions — keyword + parser + optional explicit `[...]` capture scope +
   function registry + VM closures (§225–263). Capture is always explicit only.
4. `in` operator + `for … in …` loop (§142–148, §182–188).
5. `continue` / `break` keywords + loop-control opcodes (§166, §169).
6. `%` modulo + `!` not operators (lexer + `binOp`/`unOp`) (§206, §207).
7. `str` reserved word in `keywordType` (§38).
8. `0x` hex literals in `QLangLexer::number` (§85).

## Harness
- `examples/spec_check.cpp` — original cases (const, nil, float, concat, for, if,
  while, operators, toStr/toInt).
- `examples/spec_check2.cpp` — this round's additions: const read/immutability,
  array concat/append, table member set, blob, substr, delete, postfix, while--a,
  and every gap item (hex, in, continue/break, for-in, modulo, not, str, def).
Compile against the static lib:
`g++ -std=c++17 -Iinclude examples/spec_check2.cpp build/libqlang.a -o /tmp/sc && /tmp/sc`
Re-run after each fix to confirm counts.
