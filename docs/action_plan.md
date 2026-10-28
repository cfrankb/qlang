# QLang Action Plan (excludes `$$` operator and C++ API / external data)

Basis: SPEC.md review (SHA256 `56e3b092…`, 1625 lines) cross-checked against
`SPEC_COMPLIANCE.md` (431 tests green) and the source tree. Everything below is
QLang-side only. `$$` indirect access and the whole C++ API / external-types
surface (`ctx.set`, `setFunc`, modules-from-C++, external type bindings, enum
injection) are **out of scope** per direction.

## Review summary

Conforming, no action needed: all operators (incl. `**`, `<`/`>` object-aware,
`==`/`!=` deep equality), control flow (if/while/for/next/step/switch/goto),
range encodings, infinite-loop guard (VM-wide step limit, default 100 000,
covers `while true {}`), block scope, global scope, f-strings, `$` operator,
generators, classes (closed fields, const, `__str__`, refcount, `__destroy__`,
`static` + `::`), builtins (`len` storage sizes, `delete` guards, `substr`,
`slice`, `join`, `isset`, `assert`, `sprintf`), types (`long`, `qword`,
`dword` alias, `1.0f`/`44u` suffixes), import/from (flat, v1), verbose levels.

Remaining gaps (this plan): `set` type, `enum` type, `# error reporting`,
valueless-`local` default 0, duplicate-declaration warning, `fetch` reserved
word, math `rand()`.

## Phase 1 — small fixes (one sitting)

1. **Valueless `local` defaults to 0** (SPEC §declarations: `local d` → int 0;
   implementation still declares nil).
   - `QLangCompiler`: when a declaration has no initializer, emit
     `CONST Int(0)` (typed declaration → zero of that type) instead of nil.
   - Tests: `type(z)` → `"int"`, `z` == 0, typed `local b:byte` → 0.
2. **Duplicate-declaration compile warning** (SPEC: later explicit
   `local`/`let` for a name already declared in the same scope → *warning*,
   not error; currently silent).
   - `QLangCompiler`: at the explicit-declaration site (not in
     `declareVar()` — bare assignment must stay implicit), check the name
     against the current scope map; emit `warning: duplicate declaration of
     '<name>'` to stderr.
   - Tests: `t = 5; local t = 9` → warning, value 9; block-scoped shadow in
     an `if` → no warning (different scope).
3. **`fetch` reserved word** (in SPEC §reserved words, not in
   `keywordType()`; `local fetch = 1` currently compiles).
   - One line: new `TokenType::Fetch` in `keywordType()`. Behavior is
     unspecified in the spec — reserving is all the spec asks.
4. **math `rand()`** (SPEC math table lists it; only min/max/sum registered).
   - One function in `src/QLangModules.cpp` (`std::mt19937`, returns `Int`).
   - Test: `r = rand()` is a number in range.
   - (Boundary note: this lives in the C++-registered module file, but the
     spec'd surface is the QLang builtin — include unless you want it cut.)

## Phase 2 — `set` type (SPEC §sets, 2026-10-28)

Fully specified: unique-value container of numbers and strings.

- Lexer: `set` → new token (pattern: `str`/`int`/`type` — reserved word that
  is also callable, via a `parsePrimary()` special case).
- Value: `QLangValue::Type::Set` appended **last** (keeps `.qlc` tags stable).
  Storage: `std::vector<QLangValue>` with linear dedup on insert — sets are
  small, order is implementation-defined, no comparator needed.
- Semantics:
  - `set(1,5,7,9)` variadic constructor; duplicates dropped
  - `local s:set = [1,2,4.7]` and `set([1,6,46,5])` — array coerced
  - `s[] = v` append (dedup)
  - `len(s)` = count; truthiness empty → false (by collection analogy)
  - element types: numbers and strings only; other types → type error
- Tests: ~8 `spec_check2` cases (constructor dedup, both coercions, append,
  len, truthiness, type rejection).
- **Spec gaps to flag (don't invent):** no rule for `in` membership,
  `str()`/`__str__` rendering of a set, `isset` on set elements, `==` between
  sets. Implement only what's written; ask before adding.

## Phase 3 — `enum` type (SPEC §enum, 2026-10-28)

- Lexer: `enum` keyword.
- Parser: `enum Name[:type] { A, B, C = 100, D }` — implied values 0,1,2…;
  explicit `= N` resumes the sequence at N+1. Valueless body → compile error.
  `local`/`let` prefix → block scope; bare → global (visible everywhere).
- Compiler: store as immutable `map<name, QLangValue>` (or parallel
  name/value vectors) in a hidden variable, like class prototypes.
  - redeclaration → compile error; unknown enum reference → compile error
  - member access reuses the existing `::` (`ScopeRes`) postfix path
  - `len(enum)` = member count (new `lenOf` case)
  - values behave as their declared numeric type (int default) — arithmetic
    falls out for free; no extra coercion rules needed
- Tests: ~10 `spec_check2` cases (implied values, `= N` resume, typed
  `:byte`, `::` access both notations, `len`, redeclare error, unknown error,
  valueless error, block scope).
- **Spec gap:** the C++ enum-injection API is "needs discussion" — out of
  scope here anyway.

## Phase 4 — `# error reporting` (biggest item, last)

SPEC requirements: (a) line + column when available, (b) module name,
(c) class/function/table context, (d) execution trace (stack frames with
line + function).

Current state: `QLangError` carries line/col but 152/162 throw sites pass
0,0 (all VM/runtime sites); no module, no frame chain.

- Step 1: `CompiledProgram` gains a pc → line/col table; the compiler stamps
  the current source line into every `emit()` (one field, one assignment).
- Step 2: VM throw sites attach pc → resolved line/col; `QLangError` gains
  module name + frame chain (name + line per frame, from `m_frames`).
- Step 3: top-level catch in `QLangContext::run()` formats the full message:
  `module 'main' line 12, col 5 in function 'foo': <message>` + trace
  (`at line N in <fn/module>` per frame, innermost first).
- Tests: assert messages contain the right line number and function name for
  a known-failing program (parse error, runtime type error, deep call chain).

## Verification (every phase, per AGENTS.md)

- `cmake -B build && cmake --build build && ctest --test-dir build`
- add cases to `examples/spec_check2.cpp`; re-run all suites
- only then update `SPEC_COMPLIANCE.md` (flip items, bump SHA256, tally,
  checkpoint row)

## Suggested commit order

1. `fix(decl): valueless local defaults to 0; dup-decl warning; fetch keyword; math rand`
2. `feat(set): unique-value container (constructor, coercion, append)`
3. `feat(enum): immutable value collections with :: access`
4. `feat(errors): line/col, module and execution trace in error reporting`
