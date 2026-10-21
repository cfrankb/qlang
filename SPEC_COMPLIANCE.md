# QLang SPEC.md — Compliance Reference

Purpose: hand-off notes for the next session. Records what was verified against
SPEC.md, what works, what does not, root causes, and recommended next steps.

Verified by building the project (CMake, C++17, clean) and empirically running
spec constructs through `QLangContext::run()` via `examples/spec_check.cpp` plus a
new `examples/spec_check2.cpp` harness (26 cases). The shipped `examples/hello.cpp`
smoke test passes.

## Reconciliation (this round)
The prior audit was written against an earlier source state. Since then the
lexer/parser/compiler advanced substantially. All items previously listed as
"Broken / missing vs spec" in this doc have now been resolved — the harness
passes 26/26.

Line numbers below are against the CURRENT SPEC.md (4810 bytes).

## Legend
- [x] works / matches spec (empirically verified this round)
- [ ] broken or missing vs spec (empirically verified this round)
- (!) edge case or ambiguity
- ★ added / newly verified this round

## Working (matches spec)
- [x] lexing: `#` comments, single/double/triple strings, char-as-byte, integers,
      floats (`1.13`), negative literals (`-124`), unary minus
- [x] `local a = 32;`, typed `local b:byte = 32` (+ `word/short/int/unsigned`)
- [x] `local s:str = "x"` — `str` alias recognized as string type (spec §15)
- [x] valueless `local z` declares nil (spec §106)
- [x] `local f = 1.13` real float; `toStr(f)` -> `"1.13"` (spec §23, §107)
- [x] arithmetic `+ - * / %`, comparisons `== != < > <= >=`, bitwise `& | ^`,
      shifts `<< >>`, logical `&& ||` / `|||`, `()` grouping, prefix `++`/`--`,
      unary `!` (inverse) (spec §206–208)
- [x] postfix `a++` / `a--` as statements (spec §205)
- [x] `for x = 1 to N ... next x` (inclusive, auto-increment)
- [x] `for i in [1,2,3] ... next i` — iterates array/table elements (spec §183–188)
- [x] `continue` / `break` loop-control keywords (spec §166, §169)
- [x] `if ... then ... end`, `while ... end`
- [x] `if (b in a)` membership test (spec §143–147)
- [x] `0xc` hex literal (spec §85)
- [x] `true` / `false` boolean keywords (spec §52–53)
- [x] string append when left operand is a string (`s += "abc"`, spec §111)
- [x] `sx += 123` -> `"test123"` (left-is-string concat; spec §117)
- [x] `sv = 123; sv += "test"` is a type error (spec §113–114)
- [x] array literal `[1,2,3]` / `[]`, array concat `aa + ab` -> new array (spec §88–89, §103)
- [x] array append `ab[] = 4` / `ab += 5`, index read/write `ab[0]`, `ab[0] = 55` (spec §99–101)
- [x] table literal `{name="Frank", age:byte=99, ...}` (spec §91)
- [x] table member read/write `t.name`, `t["age"]`, `t.name = "John"` (spec §93–96)
- [x] `const year = 2026;` + read `sz = year` (spec §58, §119)
- [x] `const month:byte = 10;` typed const (spec §59)
- [x] const immutability: `year = sx` -> "cannot assignment to constant" (spec §120)
- [x] `;` optional silent separator (spec §208)
- [x] `delete var` (spec §222)
- [x] `blob()` — creates empty blob (spec §16)
- [x] builtins: `toStr`, `toInt`, `type`, `print`, `println`, `printf`, `len` (spec §214–220)
- [x] `substr(str, start, len)` — 0-based substring (spec §222)
- [x] `def name(params) { ... }` / `def name(params) end` — global-scope named functions (spec §233–263)
- [x] full pipeline: source → lexer → parser → bytecode → register VM

## Functions (arrow closures + def) ★
SPEC.md §225–263 is now **partially implemented**:
- **Arrow-function closures** (`local f = (params) { }` and `local f = (params)[...] { }`):
  parsed, compiled, and callable via `callclo`. Explicit `[...]` capture by
  reference is supported.
- **`def` named global functions** (`def name(params) { ... }` / `def name(params) end`):
  parsed, compiled, and callable via `callfn`. Return-value propagation fixed
  (callee writes result to caller's dest register).
- **`def` is only valid in global scope, no closure**: per spec, `def` has no
  `[...]` capture. Verified by compilation.
- **Missing**: full live-binding semantics for captured variables across
  reassignment (capture reference is stored in the closure's activation record
  but the VM's `seqGet` path for captured vars may not re-read the outer
  register on each access).

## Resolved since prior audit
All items previously listed as "Broken / missing vs spec" are now resolved:

| Spec | Item | Symptom (before) | Fix |
|------|------|-------------------|-----|
| §16 | `blob()` | `parse error: expected '(' after 'blob'` | `parsePrimary` TypeBlob case now does `m_pos++` to consume `blob` before checking for `LParen` |
| §222 | `substr(str, start, len)` | `unknown function 'substr'` | Added `Builtin::Substr` + `substr` in `callBuiltin` |
| §85 | `0xc` hex literal | `use of undeclared variable 'xc'` | `QLangLexer::number()` now handles `0x` prefix |
| §143–147 | `in` operator | `expected ')' but found 'in'` | Added `TokenType::In` + precedence 6 + `IN` opcode in VM |
| §166 | `continue` | `use of undeclared variable 'continue'` | Added `TokenType::Continue` + `CONTINUE` opcode + LoopCtx stack in compiler (patched targets after body) |
| §169 | `break` | `use of undeclared variable 'break'` | Added `TokenType::Break` + `BREAK` opcode + LoopCtx patching |
| §183–189 | `for i in [...]` | `expected '=' but found 'in'` | Added `StmtKind::ForIn`, compiled via `LEN` + `SEQGET` loop with `LT` bound check |
| §207 | `%` modulo | `unexpected character '%'` | Added `TokenType::Modulo`, precedence 10, `MOD` opcode in VM (`fmod` for float, `%` for int) |
| §208 | `!` not | `unexpected character '!'` | Added `TokenType::Not`, included in `parseUnary`, `NOT` opcode in VM |
| §38 | `str` reserved word | `unknown type after ':'` | Added `TokenType::StrType` alias + recognized in `parseDeclaration`/`parseConst` as `String` type |
| §233–263 | `def` named global functions | assertion crash on call | Added `callDest` to Frame, fixed `RETURN` to write to caller's dest register; added `Def`/`Return` tokens, parser, compiler, VM dispatch |

## Harness
- `examples/spec_check.cpp` — original cases (const, nil, float, concat, for, if,
  while, operators, toStr/toInt).
- `examples/spec_check2.cpp` — 26 cases covering all spec features.
Compile against the static lib:
`g++ -std=c++17 -Iinclude examples/spec_check2.cpp build/libqlang.a -o /tmp/sc && /tmp/sc`
Re-run after each fix to confirm counts. Current result: **26 passed, 0 failed**.

## Open Items (not yet verified vs spec)

- [ ] `str(var)` / `int(var)` aliases (§236–237) — spec defines these alongside `toStr`/`toInt`; harness covers only `toStr`/`toInt`.
- [ ] Brace-form loops (§174–187) — `for x = 1 to 32 { ... }` and `while (a > 0) { ... }` with braces are in spec but not explicitly tested; harness covers only `while ... end` / `for ... next` forms.
- (!) `|||` operator — implemented and tested, but **not documented in SPEC.md** (spec only lists `||` at §213). Undocumented extension.
- (!) Closure live-binding — captured variables store a reference in the closure activation record, but `seqGet` for captured vars may not re-read the outer register on each access. Reassignment in the outer scope may not propagate into the closure. Partially working; needs regression test.
