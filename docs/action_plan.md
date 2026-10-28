# QLang Action Plan (excludes `$$` operator and C++ API / external data)

Basis: SPEC.md review (SHA256 `fe8c72fe…`, 1656 lines) cross-checked against
`SPEC_COMPLIANCE.md` (437 tests green) and the source tree. Everything below is
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
`slice`, `join`, `isset`, `assert`, `sprintf`, math `rand`), types (`long`,
`qword`, `dword` alias, `1.0f`/`44u` suffixes), import/from (flat, v1),
verbose levels.

Remaining gaps (this plan): `# error reporting`. (`set` ✓ done, Round 26+27; `enum` ✓ done, Round 28.)
Deferred: duplicate-declaration warning (see Deferred).

## Phase 1 — small fixes — DONE (Round 25, 2026-10-28)

1. ✅ **Valueless `local` defaults to 0** — compiler emits `CONST Int(0)`;
   typed valueless decl gets zero of its type via CONV.
2. ✅ **`fetch` reserved word** — `TokenType::Fetch` in `keywordType()`.
3. ✅ **math `rand()`** — `std::mt19937` in `QLangModules.cpp`, returns `Int`.
4. ⏸ **Duplicate-declaration warning** — implemented then **reverted/deferred**
   (see Deferred): the new `# warning` SPEC section requires warnings to be
   toggleable, so it waits for the warning infrastructure.

## Phase 2 — `set` type — DONE (Round 26+27, 2026-10-28)

- ✅ `TypeSet` lexer keyword (`set` reserved; callable as `set(...)`)
- ✅ `QLangValue::Type::Set` (ArrayValue storage, appended last)
- ✅ `Builtin::Set`: `set(1,5,7)` variadic + `set([..])` array coercion,
  dedup, numbers+strings only
- ✅ `s:set = [..]` typed-decl coercion (CONV); `s[] = v` dedup append
  (ARRAPP → shared `appendValue` helper at all 4 dispatch sites)
- ✅ `len` = count; empty → false; `__str__` → `set(1, 2, 3)`
- ✅ Round 27 (SPEC `a350a25c` closed the gaps): `x in set` (IN opcode,
  `setContains`), `for x in set` (seqGet Set branch), `set == set` (isEqual
  Set case — exact match, order-independent), `cast<set>(array)` +
  `cast<array>(set)` (parser cast table + `strict_convert`)
- ✅ 22 `spec_check2` cases (325 → 347)
- **No set gaps remain** — SPEC §sets is fully covered.

## Phase 3 — `enum` type (SPEC §enum, 2026-10-28) — ✅ DONE (Round 28)

Done as specified below; 16 `spec_check2` cases (347 → 363), all suites
green. Notes: `len(enum)` works via the existing table `lenOf` path (no new
case); member access folds to a compile-time constant in both `::` and `.`
notations; global enums are captured into function bodies via the
free-variable mechanism (hidden var, like global consts).

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

## Deferred (later plan)

- **Warning infrastructure + duplicate-declaration warning**
  (SPEC `# warning`, 2026-10-28: "compiler and interpreter warning can be
  togged off" + §declarations dup-decl warning for `local`/`let`):
  - warning toggle (on/off, default on) — QLang + C++ access, same shape as
    the verbose level (`set_verbose` pattern)
  - then the dup-decl check: explicit `local`/`let` for a name already
    declared in the same scope (current block scope, else frame map) →
    `warning: duplicate declaration of '<name>'` through the toggleable
    warning path; nested-block shadows silent; bare-assignment
    auto-declaration unaffected
  - a Round 25 prototype of the check (unconditional stderr at the
    Declaration site) was reverted — re-land it on the new infrastructure
  - tests: warning emitted when toggle on, silent when off, value semantics
    unchanged (`t = 5; local t = 9` → 9)

## Verification (every phase, per AGENTS.md)

- `cmake -B build && cmake --build build && ctest --test-dir build`
- add cases to `examples/spec_check2.cpp`; re-run all suites
- only then update `SPEC_COMPLIANCE.md` (flip items, bump SHA256, tally,
  checkpoint row)

## Suggested commit order

1. ~~Phase 1 (done, Round 25)~~
2. ~~`feat(set): unique-value container (constructor, coercion, append, __str__)`~~ (done, Round 26)
3. `feat(enum): immutable value collections with :: access`
4. `feat(errors): line/col, module and execution trace in error reporting`
5. (later plan) `feat(warnings): toggleable warning infra + dup-decl warning`
