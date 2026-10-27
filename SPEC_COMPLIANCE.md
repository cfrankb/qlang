# QLang SPEC.md — Compliance Reference

Purpose: hand-off notes for the next session. Records what was verified against
SPEC.md, what works, what does not, root causes, and recommended next steps.

Verified by building the project (CMake, C++17, clean) and empirically running
spec constructs through `QLangContext::run()` via `examples/spec_check2.cpp`
(246 cases). The shipped `examples/hello.cpp` smoke test passes.

## Current Status

**262 passed, 0 failed** (harness `examples/spec_check2.cpp`, built against `libqlang.a`) + **33 passed, 0 failed** (`spec_check`) + **20 passed, 0 failed** (`spec_cast_test`) + **7 passed, 0 failed** (`test_module`) + **5 passed, 0 failed** (`gen_test`) + **22 passed, 0 failed** (`examples/range_check.cpp`, `range` ctest) + **13 passed, 0 failed** (`examples/class_str_test.cpp`, `class_str` ctest) + **17 passed, 0 failed** (`examples/class_static_test.cpp`, `class_static` ctest) = **379 total**. Re-verified 2026-10-27 (Round 15) against the current lib; all suites green. `spec_check2` now includes 19 `step` cases (for-to + for-in range, asc/desc, variable step, step-0 infinite guard, array-step error, continue/break), 42 class-conformance cases (L449–L615), 6 `$` dynamic-key cases (L617–L626), 6 block-scope cases (L633–L640), 6 `string == object` `__str__`-equality cases (L370–L375), and 4 `implicit __str__` collection-rendering cases (Round 15), so `step`, classes, `$`, block scope, `__str__`-equality, and implicit `__str__` rendering are test-verified. `$$` is fully specified (SPEC §operator $$, committed in `c83ebc8`) but **not implemented** — no harness cases yet. The 2026-10-27 `static` class-member additions (reserved word + `::` scope-resolution operator + `## static` section + class example, committed `a6c339a`/`6a40633`) are **implemented** (Round 12, 2026-10-27; `class_str` + `class_static` suites) — see "user defined classes — spec conformance" and Open Items.

Line numbers below are against the CURRENT SPEC.md (41751 bytes, 1485 lines, SHA256: 1edc77acb7a8aba88ab078609370cfef66ec52243b74dad3b314ca4768580bfa). Working tree **dirty** — the only uncommitted change is a SPEC.md heading rename: `# implicit __str__` → `# implicit __str__ rules` (content unchanged; section implemented this round, see Round 15). The `# implicit __str__` and `# error reporting` sections are committed in `649be35`; `# error reporting` remains **not implemented**, see Open Items. The Round 13 `==`/`!=` table + `string == object` `__str__`-equality work is now **committed** (`c62b8e4`); the `static` spec additions and implementation are committed (`a6c339a` spec, `6a40633` code); the four 2026-10-26 edits are committed in `dde51e4`: "SPEC.md is a living document." note added; duplicate-declaration examples softened from compile **error** to compile **warning** (wording now consistent); the "no block scope" clause removed from `## operator $$`; new `# block scope` section added — **not implemented**, see Open Items. The `$` section's nil-row fix (`"false"` → `"nil"`) is committed in `052b45e`. The `## operator $$` section (replacing the `9cc0a3f` TODO stub) and the expanded `## user defined classes` section are committed (`c83ebc8`, `e08005b`); the class implementation phases are `6a4c559` (unique method/dunder names), `8c013a1` (fixed arity), `b242645` (`__str__`), `0d7115b` (refcounting + `__destroy__`); the `$` operator fix (EVALKEY dispatch in all 4 exec loops + parser $key wrapping) is `0c63f4b`. The `import`/`from` reserved words, duplicate-declaration examples, `$$` operator-table row + TODO stub were committed in `9cc0a3f`; the `step` reserved word + range/loop `step` examples in `d07eb4f`/`21cc1c5`; the `step` implementation + inclusive-endpoint fix is `e955a8d`.

One section committed after the prior audit remains **not yet conforming**: `## operator $$` (fully specified, no implementation). The expanded `## user defined classes` section is now **fully conforming** — all 9 implementation phases landed (`6a4c559`, `8c013a1`, `b242645`, `0d7115b`, `0c63f4b`; see "user defined classes — spec conformance" below). The `# block scope` section is now **fully conforming** (implemented 2026-10-26, committed `5f458b2`; 6 harness cases). The 2026-10-27 `static` additions are **implemented** (Round 12, 2026-10-27; `class_static` suite, 17 cases); they are marked in the spec as a **new extension beyond v1** (v1 is fully implemented and stands as specified; the `## v1 restrictions` "no static / class methods" line is unchanged and no longer contradictory). Open gaps: the duplicate-declaration **warning** check (spec examples in `9cc0a3f`; requirement softened to a compile warning in the 2026-10-26 spec edits, `dde51e4`) is **not implemented** (re-verified 2026-10-26), and the new `# error reporting` section (uncommitted 2026-10-27) is **not implemented** — see Open Items.

## SPEC.md Changes Since Last Audit

SHA256: `1edc77acb7a8aba88ab078609370cfef66ec52243b74dad3b314ca4768580bfa` (1485 lines, 41751 bytes). Prior audit: `967b17e5fcc01b3e9b934bfc31a283781c8496fa7ec6268c28b5a9a843ed2909` (1471 lines, 41449 bytes).

| Commit | Change |
|--------|--------|
| (uncommitted, 2026-10-27) | SPEC.md: heading rename `# implicit __str__` → `# implicit __str__ rules` (no content change). The section is **implemented** (Round 15, this audit). |
| `649be35` | "fix(bool): literals+isset return Bool, stringify true/false; fix f-string/bool snprintf segfault; fix triple-quoted string leak" — the Round 14 implementation fixes committed (`QLangParser`/`QLangLexer`/`QLangVM` + `helpers.ql` + Round 14 compliance text), **plus the two new SPEC.md sections**: `# implicit __str__` (type/behavior table: numbers→string, strings unchanged, bool→"true"/"false", nil→"nil", array→`[0, 1, 2, 3, 4]`, table→`{name="Bob", age=9}`, class→`__str__`) and `# error reporting` (requirements + execution trace). `# implicit __str__` implemented in Round 15; `# error reporting` **not implemented**; see Open Items. |
| `c62b8e4` | "string == object via `__str__` fix" — the Round 13 work committed: SPEC.md `==`/`!=` 12-row example table + "equivalency worths both ways" note + `(byte)0x255`→`(byte)0xFF` typo fix; `QLangVM::valueEqual`/`doBinaryVm` (String vs object compares against `displayString`/`__str__`); 6 new `spec_check2` cases (L370–L375); CMake test targets; `moneyv2.ql`; Round 13 compliance text. |
| `a6c339a` | SPEC.md: the three waves of 2026-10-27 `static` additions **committed** (reserved word, `::` operator row, Money example statics + `__str__` interpolating `{banner}`, `## static` section) — previously uncommitted in this audit. Also `implementation plan v2.md`, `class static implementation.md` (later renamed into `docs/`), `examples/scripts/money.ql`, `.dshignore`. |
| `6a40633` | Code: the Round 12 `static`/`__str__` **implementation committed** (`QLangLexer`/`QLangParser`/`QLangCompiler`/`QLangObjects`/`QLangVM` + `class_str_test`/`class_static_test` + CMake targets + Round 12 compliance text) — previously uncommitted on top of `5f458b2`. |
| (uncommitted, 2026-10-27, three waves) | SPEC.md wave 1: `static` added to reserved words; Money class example gains `static banner = "GreatAccount";` field, `static getBanner()[]` / `static setBanner(newBanner)[]` methods, `Money::setBanner("OrangePumpkin")` / `println(Money::getBanner())` usage, and `__str__` now interpolates `{banner}`; new `## static` section (static members exist only in classes, are bound to the class, readable/modifiable by class functions; static functions addressed via the class name). Wave 2 (same day): `::` row added to the operator table ("scope resolution operator"); Money example extended with `println(money)` / `println(f"{money}")` (`__str__`), `println(f"{Money::banner}")` (static **field** read via class name, inside f-string), `printf(f"banner: {money.banner}")` (static field read via **instance**), and `money.banner = "Turtle"` (instance write mutates class-level storage — `Money::banner` then reads "Turtle"). Wave 3 (same day): `## static` gains two rules — `static` **can** combine with `const`, **cannot** combine with `local`/`let` — and an explicit note that `static`/`::` is a **new extension beyond v1** (v1 has no static members; `## v1 restrictions` unchanged — contradiction resolved per user direction: v1 is done, `static` is the extension). Spec-only — **not implemented**: no `TokenType::Static` in `keywordType()`, no `parseClass` static handling, no `::` token/parser path, no class-level static storage. |
| `5f458b2` | Code: block-scope implementation committed (`m_blockScopes` stack in `QLangCompiler`; pushed/popped at if/else/for/for-in/while/switch-case bodies) — the "uncommitted block-scope code" the prior audit referenced. No spec change in this commit. |
| `052b45e` | SPEC.md: `$` section's nil row committed as `"nil"` (was the prior audit's uncommitted edit). Also `SPEC_COMPLIANCE.md` cleanup; deleted stale handoff docs `class_action_plan.md` + `handoff_qlang_dollar_operator.md`. No code changes. |
| `c83ebc8` | SPEC.md: `## operator $$` committed in full (replaces the `9cc0a3f` TODO stub): definition (indirect access, no operand coercion vs `$`, first-class expression), example blocks (function/const/global-write-denied cases), restrictions list, six-stage resolution/check pipeline with canonical `indirect variable error:` messages, resolution order (writes: frame locals only; reads: locals + `global`-exposed, global const visible everywhere), declaration-state rules (unique declaration event, no block scope, branch-dependent declarations, direct-vs-indirect forward-reference asymmetry), const/function binding rules, implementation notes (frame name map, declaration bit on existing store/init opcodes, `OP_INDIRECT_READ`/`OP_INDIRECT_WRITE`, optional compile-time fast path). Spec-only — **not implemented**, no harness cases. Also: `## user defined classes` expanded from the single Money prototype example into the full section (class body, construction, field access, method calls, string conversion, lifetime/ownership, type identity, v1 restrictions, conformance table) — prototype exists, **non-conforming**; see "user defined classes — spec conformance". |
| `e08005b` | SPEC.md: class example fix — `spend += amount` → `spend -= amount` (the `spend` field is an expense counter); output comments added to the class example. No harness case covered the example, so no test change was needed. |
| `9cc0a3f` | SPEC.md: `import` + `from` added to reserved words (already reserved in the lexer — `keywordType()` → `TokenType::Import`/`From` — documentation gap closed, no implementation gap); duplicate-declaration examples (`local xbit` redeclared in branch → compile error; `t = 5; local t = 9` → compile error); `$$` row in the operator table + `## operator $$` TODO stub with examples/restrictions. Also: `.gitignore`, vscode tmLanguage, docs cleanup. |
| `4091c32` | feat(verbose): application-wide verbose level — `QLangContext::setVerboseLevel/verboseLevel/isAt` + `m_verboseLevel` (default 0); `set_verbose(level)` native fn (`QLangContext.cpp`, accepts int/dword/float, returns new level); `QLangVM::trace(minLevel,…)` gated emit helper with tiers 1 (frames/calls) / 2 (VM loop+opcodes+returns) / 3 (full detail) and the fprintf→trace conversion in `QLangVM.cpp`. Expanded `## verbose level` section in SPEC.md (§736) from a one-line placeholder into a full implementation spec (granularity table, QLang + C++ APIs, implementation notes, example session). **Committed here** (was uncommitted in the prior audit). Verified: build clean (`qlang` lib + `spec_check2`); `set_verbose(0/1/2)` emits the correct tiers to stderr with program output on stdout unaffected; C++ `setVerboseLevel(3)` → `verboseLevel()==3`, `isAt(2/3)==true`. |
| `e955a8d` | feat(step) implementation + fix: `for` endpoint inclusive for all step sizes. `TokenType::Step` + lexer keyword; parser `stepExpr` in both `for` forms; compiler 3-way fork (step==0 → infinite, step & direction agree → guarded, wrong-direction → body runs once); `for x in [..] step n` → type error; constant path `LT`→`LE` and runtime-path `-1` endpoint removed (ranges always inclusive); 19 `step` tests added to `spec_check2`. Verified 2026-10-25: 204/204. |
| `21cc1c5` | SPEC update: `step` reserved word + range/loop `step` examples — `for i in range(0,10) step 1`, `range(10,0) step -1`, `for x = 10 to 0 step -1`, `for x in [1,4,6] step 2` (documented as error); step rules (direction from start/end, wrong-direction runs once, `step 0` infinite, timeout guard → runtime exception). |
| `d07eb4f` | SPEC update: loops section rewritten — `next`-based loop forms, array-step error example, nested-range examples; removed the earlier buggy descending encode pseudocode (`i = 0; while (i <= 10) { --i }`), which contradicted its own "start at 10" comment and would loop forever. |
| `2f19a23` | feat(range): context-dependent range encoding — `for i in range(a,b)` counter loop (no array), `l in range(a,b)` bounds check; `containsYield` recurses into ForIn for generators; added `examples/range_check.cpp` (22 cases, `range` ctest) |
| `5001c1d` | checkpoint: add `yield`/`fetch` reserved words; add "everything is case sensitive." + "always ask for permission before touching this file."; boolean-table `generator`/`function,lambda` rows; `## declarations and assignments` rename + implicit-declaration example; new `## range` section (context-dependent encoding); `==`/`!=` nil-equality table; "no-return -> returns nil"; `## verbose level` (Priority one); generators rewritten (callable-closure prototype + `## Syntax` + `## EOF sentinel`) |
| `31fc707` | Add `==`/`!=` nil-equality rules (nil only equals nil) + document functions/lambdas/generators without a return statement returning nil (nil type) |
| `b195b06` | Add `class` to reserved words; fix math module typo; dd string module stub; expand Money class prototype |
| `8779c16` | Fix: math.min/max/sum overflow to Dword, sum flattens arrays |
| `a96121b` | SPEC.md: document `loadBuiltinModules()` |
| `2c87614` | SPEC_COMPLIANCE: update SHA256 for import form changes (prior audit) |

Minor edits: removed `slice => build in` from reserved words (already not a keyword); fixed comment typo in math module; added string module stub (`tba`); added user-defined classes TODO section with `Money` prototype; added "don't put build file in the source tree."

## SPEC.md Edits Since Last Audit (committed + uncommitted)

The 2026-10-27 `static` additions are **committed** (`a6c339a`; code in `6a40633`). The 12-row `==`/`!=` example table + `(byte)0x255`→`(byte)0xFF` typo fix are **committed** (`c62b8e4`). The `# implicit __str__` and `# error reporting` sections are **committed** (`649be35`); the only **uncommitted** SPEC.md edit (2026-10-27) is the heading rename `# implicit __str__` → `# implicit __str__ rules`. `# implicit __str__ rules` is **implemented** (Round 15); `# error reporting` remains **not implemented**, see Open Items. The four 2026-10-26 SPEC.md edits are committed in `dde51e4` ("SPEC.md is a living document." note, dup-decl error→warning wording, `## operator $$` "no block scope" clause removed, new `# block scope` section; see the hash/HEAD line). The previously uncommitted `## operator $$` draft is committed in `c83ebc8`, and the `## user defined classes` expansion plus the class example fix landed in `c83ebc8`/`e08005b` (details in the changes table above).

`docs/QLangVM.cpp.orig` and `docs/QLangVM.cpp.rej` are patch artifacts; the `.rej` hunk
was an extra `[DBG-YIELD-REG]` register-dump loop that was NOT applied. `docs/` also
holds handoff notes from the `step` implementation sessions (`step_plan.md`,
`hand_off.md`, `for_next_step_handoff.md`, `handoff_range_nostep_bug.md`,
`nostep_range_decode.md`, `compare_bytecode_if.md`, `for_loop_bug.md`, `notes.txt`).
`docs/EXTERNAL_TYPES.md` (external/C++ type bindings — the design baseline for class
conformance; §9 object dispatch foundation), `docs/EVENT_HANDLERS.md` and
`docs/SERIALIZATION.md` are design docs; `HANDOFF.md` is the current handoff note.
The tally in "Current Status" (339 total) was re-run on 2026-10-27 (HEAD `5f458b2`);
no code changed for the uncommitted `static` spec additions, so the suites are unaffected.

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
- [x] **equality `==` / `!=`: nil is ONLY equal to itself** — `nil == nil` → true;
      `nil == number` and `nil == collection` → false (SPEC §operator == and !=).
      Implemented in VM `isEqual()` (`QLangVM.cpp`): nil branch compares types,
      number branch compares int64 values, otherwise same type + same value.
      ★ `string == object` compares against the object's `__str__` value (Round 13,
      `QLangVM::valueEqual`/`doBinaryVm`; 6 cases L370–L375) — the new 12-row
      `==`/`!=` example table (uncommitted SPEC edit) fully conforms.
- [x] **function returns:** functions/lambdas/generators without a return
      statement return nil (nil type) (SPEC §"functions, lambdas and generators
      without a return statement will return nil"). Compiler emits implicit
      `CONST nil; RETURN` at end of every function/generator body.
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
- [x] **range context-dependent encodings (SPEC §range):** `for i in range(a,b)` -> counter loop `i=a; while i<=b { ...; ++i }` (no array materialized); `l in range(a,b)` -> bounds check `(l>=a) && (l<=b)` (no array materialized), used in `if`/`while` conditions and `let x = b in range(0,10)`; `arr = range(a,b)` -> materialized array; `for i in range { yield i }` -> generator. `continue` in a range counter loop jumps to the implicit `INC` (spec: "continue will increment the i"). Verified by `examples/range_check.cpp` (22 passed, 0 failed; `range` ctest): counter-loop sum/count/break/continue, bounds-check in/out/edge values, array materialization, generator. Range `step` variants — `for i in range(a,b) step N`, `range(10,0) step -1`, `range(10,0) step j` — are documented in SPEC but **NOT implemented** (parser parses no `step`); see Open Items.
- [x] `0xc` hex literal
- [x] `true` / `false` boolean literals; truthiness for booleans, strings,
      arrays, tables, numbers (0 = false, non-zero = true)
- [x] truthiness for `generator` => true; `function`/`lambda` => depends on
      return value and value type (SPEC §boolean evaluation)
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
- [x] `assert(bool, str)` — reserved word; builtin call via `parsePrimary()`; throws runtime error on falsy first arg with provided message string
- [x] **user-defined classes (full):** `class Name { fields; methods }` — closed field set, `const` fields, unique method/dunder names (compile error), fixed arity checks, `__str__`, reference counting (shared, never copied), `__destroy__` finalization, `type(x)` returns the class name. ★ Conforms to the expanded spec section (`c83ebc8`); 42 test cases at spec_check2 L449–L615; see "user defined classes — spec conformance"

## Missing vs Spec

All items below are implemented and verified. Table retained for traceability.

| Spec | Item | Status | Notes |
|------|------|--------|-------|
|| §builtins | `assert(bool, str)` | ✓ implemented | Added `TokenType::Assert`, `Builtin::Assert`. Parser handles `assert(...)` as expression call (both statement and expression context). VM throws `QLangError(Runtime)` with the message string when first arg is falsy. Uses truthiness check matching `isTrue`: nil→false, empty string→false, 0→false, everything else→true. Requires exactly 2 args; second arg must be string. |
| §C++ interface | `set(name, value, isConst=false)` | ✓ implemented | Merged `injectConst`/`injectGlobal` into single `set()` with third `isConst` param. Seeds var into scope; when true, adds to `m_injectedConsts` so VM skips end-of-run sync and compiler's `guardNotConst()` rejects QLang assignment. |
| §C++ interface | helpers to create `QValue` from common C++ types | ✓ implemented | `include/qlang/QValue.h` provides `using QValue = QLangValue;` and free helpers: `qval_nil`, `qval_bool`, `qval_byte`, `qval_char`, `qval_word`, `qval_short`, `qval_int`, `qval_unsigned`, `qval_dword`, `qval_float`, `qval_str` (3 overloads), `qval_blob`, `qval_array`, `qval_table`. |
| §C++ interface | `setFunc(name, func)` — inject C++ function | ✓ implemented | `QLangContext::setFunc()` registers a native function. Callback signature: `std::function<QLangValue(QLangContext&, const std::vector<QLangValue>&)>` — context first (Lua convention). Compiler assigns negative opcode indices (-1, -2, …); VM dispatches `CALL` with `i.b < 0` via `m_prog.nativeFuncs[idx]`. |
| §C++ interface | `CompiledProgram::nativeFuncs` | ✓ implemented | `std::vector<QLangContext::NativeFunc>` on `CompiledProgram`; populated from `m_nativeFuncs` at compile time in `QLangContext::run()`. VM reads it directly — no runtime lookup by name. |
| §C++ interface | `Context::call(name, args)` | ✓ implemented | Looks up native funcs first, then scans `compiled.functions[1..]` by name. Creates a fresh `QLangVM` and calls `callFunction()`. Cached compiled program stored in `Pimpl::compiled`. |
| §C++ interface | `VM::callFunction(funcIndex, args)` | ✓ implemented | Creates a host caller frame, pushes callee via `pushCall()`, runs manual opcode loop until callee returns (`m_cur` drops below callee frame index), captures result from `regs[0]`, restores state, returns result. |
| §C++ interface | `VM::runFrames(startFrame)` | ✓ implemented | Extracted VM loop from `run()` for reuse by `callFunction()`. Runs until `m_cur < startFrame`. |
| §C++ interface | `CompiledProgram::constVars` | ✓ implemented | New field on `CompiledProgram`; populated from `m_constVars` at compile time (includes both `const`-declared and injected consts). VM uses it to skip scope sync for const vars. |
| §global scope visibility | `global a, b` keyword | ✓ implemented | `TokenType::Global` in lexer; `keywordType()` maps `"global"`; parser handles `global a, b;` (semicolon optional) in function scope; compiler tracks `m_globalVars`/`m_globalRegMap`; VM routes through saved regs. Top-level `global` throws parse error. Undeclared global throws error. 9 new test cases pass. |
| §strings (f-strings) | Python-style f"Total: ${price:.2f}" | ✓ implemented | Lexer: FString token; fstringLiteral() parses ${...} interpolation blocks verbatim. Parser: ExprKind::FString with strParts (literal segments), exprParts (inner expression ASTs), and formatSpecs (per-expression format specs like ".2f"). Compiler: emits format(template, expr1, ...) via Builtin::Format; format specs are translated to printf placeholders (:2f -> %.2f, :d -> %d, :x -> %x, :X -> %X, :o -> %o). Width and flag parsing supported: :06x -> %06x (zero-pad), :#06x -> %#06x (alternate hex with 0x prefix), :5d -> %5d (width), :-5s -> %-5s (left-align). VM: format builtin passes full printf substring to snprintf, preserving all flags/width/precision. Verified: f"Total: ${price:.2f}" -> "Total: 19.99", f"${0xff:x}" -> "ff", f"${255:X}" -> "FF", f"${255:#06x}" -> "0x00ff", f"${255:06x}" -> "0000ff", f"${10:05d}" -> "00010", f"${3.14:.2f}" -> "3.14". |

## Not Yet Implemented vs Spec

|| Spec | Item | Status | Notes |
|------|------|--------|-------|
| §error reporting | `# error reporting` — line/col + module + class/function/table in errors, execution trace | ✗ not implemented | SPEC section committed in `649be35` (2026-10-27). 152/162 throw sites carry no line/col; no module name, no class/function/table context, no stack-frame trace in `QLangError`. See Open Items. |
| §implicit __str__ | `# implicit __str__ rules` — arrays/tables render contents in display contexts | ✓ implemented | Round 15 (2026-10-27): `QLangVM::displayString` renders arrays `[e1, e2, ...]` and plain tables `{k=v, ...}` (new recursive `renderElement`: strings double-quoted, nested collections recurse, scalars in plain display form). Covers print/println/printf/sprintf/f-string/join/`str()`. `valueToString` (the `+=` append path) keeps `"<array>"`/`"<table>"` per the `+=` spec table ("any other types → string<\type>"); `cast<string>` unchanged. **Caveat:** table keys render in lexicographic order (`std::map`), not the insertion order shown in the spec example — no table-ordering rule exists in the spec; insertion order would require reworking `TableValue` (shared_ptr<map> used code-wide). 4 new `spec_check2` cases; 3 prior `<table>` expectations updated (`class_str_test` ×2, `spec_check2` ×1). |
|| §operator $$ | `$$` indirect access operator | ✗ not implemented | Now **fully specified** (SPEC §operator $$, committed `c83ebc8`): six-stage check pipeline (string → identifier lexeme → reserved word → declared/visible → binding kind → local-only writes) with canonical `indirect variable error:` messages; resolution order (writes: frame locals only; reads: locals + `global`-exposed, global const visible everywhere); no variable creation; functions/lambdas never targetable, const read-only; sequential declaration state (declaration bit set at the unique declaration event; branch-dependent declarations can be `not declared` — the "no block scope" clause was removed in the 2026-10-26 spec edits (`dde51e4`); re-verify when implementing). Implementation per spec notes: per-frame name map (name → register, binding kind `var`/`const`/`function`/`lambda`, declaration bit), declaration bit as a flag on existing store/init opcodes (no new opcode for declaration state), two new opcodes `OP_INDIRECT_READ`/`OP_INDIRECT_WRITE`, optional compile-time fast path for a future `$$"literal"` form. No harness cases yet. |
|| §operator $ | `$` dynamic key resolution | ✓ resolved | Implemented in `0c63f4b`: `Dollar` token, `OP_EVALKEY` opcode (dispatched in all 4 exec loops), scalar stringify, dynamic table-literal keys, rejects collections/instances (runtime). 6 tests at spec_check2 L617–L626. |
| §reserved words / §range / §loops | `step` (all forms) | ✓ resolved | Implemented in `e955a8d` (19 cases in `spec_check2`); see Open Items. The prior gap rows in this table (no `step` in `keywordType()`, no `stepExpr` in `parseFor()`, array-step behavior) are superseded. |

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
| `bool` | `Bool` | ✓ | `true`/`false` literals compile to `Bool(1/0)` (Round 14); `local b:bool` parses; `type(true)` → `"bool"`; stringify as `"true"`/`"false"`; bools participate in arithmetic/comparisons via `isNumeric()` |
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
| `dword` | `TypeDword` | ✓ | Reserved word; blocks identifier use. `local d:dword` parses; `type(d)` → `"dword"`; `uint64_t` storage |
| `global` | `Global` | ✓ | Reserved word; blocks identifier use at top level. `global a, b;` valid inside function scope for explicit global access |
| `const` | `Const` | ✓ | Reserved word; blocks identifier use. `const year = 2026;` declares constant; `year = sx` → compile error |
| `end` | `End` | ✓ | Reserved word; terminates `if/then/end`, `while/end`, `def/end` forms. Not used in brace-form syntax |
| `cast` | `Cast` | ✓ | Reserved word; blocks identifier use. `cast<int>(1.12)` works as expression via `parsePrimary()` special case |
| `class` | `Class` | ✓ | **Fully implemented** (conforms to expanded spec section `c83ebc8`). Closed field set (`__fields__`/`__const_fields__` descriptors in `QLangObjects.cpp`), `const` fields, unique method/dunder names (compile error), fixed arity, `__str__`, reference counting, `__destroy__`, `type(x)` = class name, **`static` members + `::`** (Round 12, new extension beyond v1). 42 tests at spec_check2 L449–L615 + 17 in `class_static_test`. See "user defined classes — spec conformance". |
| `fetch` | — | — | **Not a keyword.** Not implemented; no `TokenType::Fetch` in lexer/parser. Listed as reserved word in SPEC but the lexer does not recognize it. Gap.
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
| `dword (uint64_t)` | `Dword` | `TypeDword` | ✓ | `local d:dword = 4294967296` parses; `type(d)` → `"dword"`; `uint64_t` storage; participates in arithmetic; `toStr()` works |
| `array` | `Array` | `TypeArray` | ✓ |
| `float` | `Float` | `TypeFloat` | ✓ |
| `table` | `Table` | `TypeTable` | ✓ |
| `bool` | `Bool` | `TypeBool` | ✓ | `true`/`false` compile to `Bool(1/0)` (Round 14); `local b:bool` accepted; `type(true)` → `"bool"`; stringify as `"true"`/`"false"`; bools are numeric for arithmetic/comparisons |

## Harness

- `examples/spec_check.cpp` — original cases (const, nil, float, concat, for, if,
  while, operators, toStr/toInt). Result: **33 passed, 0 failed.**
- `examples/spec_check2.cpp` — 252 test cases covering all spec features including `bool`, `char`, `dword`, `switch`, `range`/`slice` reserved-word status, closures, `**`, `let`, goto, slice, range, method calls, `==`/`!=` nil-equality, function/generator returns, 19 `step` loop cases, 42 class-conformance cases (L449–L615), 6 `$` dynamic-key cases (L617–L626), and 6 block-scope cases (L633–L640). Result: **252 passed, 0 failed.**
- `examples/spec_cast_test.cpp` — 20 cast tests: numeric casts (int/float/byte/word/short/unsigned/dword/char/bool), string casts, invalid collection casts, nested casts, expression-context casts, nil cast. Result: **20 passed, 0 failed.**
- `examples/gen_test.cpp` — 5 generator tests: basic yield, second yield, exhaustion/EOF, loop accumulate, empty yield, outer variable capture, type check. Result: **5 passed, 0 failed.**
- `examples/range_check.cpp` — 22 range tests: `for i in range(a,b)` counter loop (sum/count/break/continue), `in range(a,b)` bounds check (inside/low/high/above/below/as-value), `arr = range(a,b)` materialization, for-in array membership, generator range. Result: **22 passed, 0 failed.**
Compile against the static lib:
`g++ -std=c++17 -Iinclude examples/spec_check2.cpp build/libqlang.a -o /tmp/sc && /tmp/sc 2>/dev/null`
Result: **252 passed, 0 failed.** All spec builtins implemented.
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

Core spec fully implemented. **262 passed, 0 failed** (spec_check2) + **33 passed, 0 failed** (spec_check) + **20 passed, 0 failed** (spec_cast_test) + **7 passed, 0 failed** (test_module) + **5 passed, 0 failed** (gen_test) + **22 passed, 0 failed** (range_check) + **13 passed, 0 failed** (class_str) + **17 passed, 0 failed** (class_static) = **379 total**. Since the prior audit: implemented the `step` loop construct (both `for` forms; 19 new cases) and made the `for` endpoint inclusive for all step sizes (`e955a8d`); SPEC.md gained the `step` reserved word/examples (`d07eb4f`/`21cc1c5`); `import`/`from` reserved words, duplicate-declaration examples, and the `$$` operator-table row + TODO stub committed in `9cc0a3f`; `## operator $$` committed in full (`c83ebc8`) — spec-only, implementation and harness cases pending; **user-defined classes brought to full conformance** (9 phases, `6a4c559`/`8c013a1`/`b242645`/`0d7115b`) and the **`$` dynamic-key operator implemented** (`0c63f4b`; 6 new cases) — see Round 8; **block scope implemented** (2026-10-26; 6 new cases) — see Round 10.

## Round 15 (2026-10-27)

Spec drift audit: SPEC.md moved `967b17e5` (41449 B / 1471 lines) → `1edc77ac` (41751 B / 1485 lines).
- **Round 14 work committed** in `649be35` (bool literals/`isset` → `Bool`, f-string/bool snprintf segfault fix, triple-quoted string leak fix, `helpers.ql`, Round 14 compliance) — **plus the two new SPEC.md sections** `# implicit __str__` and `# error reporting`.
- **Uncommitted:** SPEC.md heading rename `# implicit __str__` → `# implicit __str__ rules` (content unchanged).
- **`# implicit __str__ rules` implemented:** `QLangVM::displayString` now renders arrays as `[e1, e2, ...]` and plain tables as `{k=v, ...}` via a new recursive `renderElement` (strings double-quoted, nested collections recurse, scalars in plain display form). All display contexts conform: print/println/printf %s/sprintf %s/f-string/join/`str()`. `str([0,1,2,3,4])` → `"[0, 1, 2, 3, 4]"`; `str({name="Bob", age=9})` → `"{age=9, name=\"Bob\"}"`; `f"{b}"` on array `b` → `[1, 2, 3]`. `valueToString` (the `+=` path) and `cast<string>` intentionally unchanged (the `+=` spec table says "any other types → string<\type>"). **Caveat:** table keys render lexicographic (`std::map`), not the spec example's insertion order — no ordering rule in the spec; flag if insertion order is required.
- 4 new `spec_check2` cases (258 → 262); 3 prior `<table>` expectations updated to `{a=1}` (`class_str_test` ×2, `spec_check2` ×1).
- Harness re-run: **262/262** (`spec_check2`), **33** (`spec_check`), **20** (`spec_cast_test`), **7** (`test_module`), **5** (`gen_test`), **22** (`range`), **13** (`class_str`), **17** (`class_static`), all green = **379 total**.
- `# error reporting` remains **not implemented** (now committed in the spec).

## Round 14 (2026-10-27)

Spec drift audit: SPEC.md moved `c46efc2f` (41038 B / 1457 lines) → `967b17e5` (41449 B / 1471 lines).
- **Round 13 work committed** in `c62b8e4` ("string == object via `__str__` fix"): the `==`/`!=` example table + typo fix (SPEC.md), `valueEqual`/`doBinaryVm` (code), 6 `spec_check2` cases, CMake test targets, `moneyv2.ql`, Round 13 compliance text.
- **New uncommitted SPEC.md section `# error reporting`** (requirements + execution trace) — **not implemented**: no module name in errors, no class/function/table context, no execution trace, and 152/162 throw sites carry no line/col. See Open Items.
- Harness re-run (no code changes): **258/258** (`spec_check2`), **33** (`spec_check`), **20** (`spec_cast_test`), **7** (`test_module`), **5** (`gen_test`), **22** (`range`), **13** (`class_str`), **17** (`class_static`), all green = **375 total**.
- Working tree: `M SPEC.md` (error reporting section); untracked `docs/class_action_plan.md`.
- **Impl fixes** (from `examples/scripts/helpers.ql` bug report; no spec change, hash unchanged): (1) triple-quoted strings (`"""..."""` and `f"""..."""`) leaked the first two opening quotes into the content — `QLangLexer::stringLiteral` **and** `fstringLiteral` consumed only one opening quote in the triple case, now consume three. (2) `isset` is now a **reserved keyword** (`TokenType::Isset` in `keywordType()`); `parsePrimary()` compiles a bare-identifier arg to its name string, so `isset(xx)` on an undeclared variable probes the existing `Builtin::IsSet` runtime check instead of `parse error: use of undeclared variable`. (3) **booleans are now proper `Bool` values**: `true`/`false` literals compile to `QLangValue::Bool(1/0)` (was `Int(1/0)`) and `isset` returns `Bool` — so `type(true)` → `"bool"`, `f"{true}"` → `"true"`, `s += true` → `"...true"` (spec: `$` matrix + `+=` table); equality/arithmetic/truthiness unchanged (`isEqual`/`isTrue` already handle `Bool`). (4) **segfault fix**: `format` builtin's `Bool` branch passed a `long long` to `snprintf` with a `%s` placeholder (`f"...{b}..."` on a `:bool`-typed var crashed in libc); now `%s`-spec bools render as `"true"`/`"false"` text, numeric specs stay 1/0. Self-check: `helpers.ql` (`qlang_run`), full suite re-run green (375).

## Round 13 (2026-10-27)

`string == object` `__str__`-equality implemented (SPEC §operator == and != new example table); harness re-run: **258/258** (`spec_check2`), **33** (`spec_check`), **20** (`spec_cast_test`), **7** (`test_module`), **5** (`gen_test`), **22** (`range`), **13** (`class_str`), **17** (`class_static`), all green = **375 total**.
- SPEC.md moved `204f0702` (40335 B / 1443 lines) → `c46efc2f` (41038 B / 1457 lines): 12-row `==`/`!=` example table + "equivalency worths both ways" note + `(byte)0x255`→`(byte)0xFF` typo fix (uncommitted). Every row except `string==object` already conformed (probed against the lib: `1==1.0` true, `"3"==3` false, `' '==32` true, `-1==(byte)0xFF` false, `1==(unsigned)1` true).
- **`string == object`:** `QLangVM::valueEqual` (member) — String vs object (table with `__str__` or `__class__`) compares against `displayString`; everything else → free `isEqual`. New member `doBinaryVm` routes EQ/NE through `valueEqual` in all 4 exec loops; other opcodes still go to the static `doBinary`. No new opcodes. `"toto" == C()` → true when `__str__` returns `"toto"`, false otherwise; `1 == C()` stays false; plain-table equality unchanged.
- 6 new `spec_check2` cases (252 → 258).
- Committed since Round 12: `a6c339a` (static spec additions + plan docs), `6a40633` (static/`__str__` implementation + Round 12 compliance). Uncommitted: `==` table + typo fix (SPEC.md), `valueEqual`/`doBinaryVm` + 6 cases (code), CMake test targets, `docs/` renames, untracked `.draft/`/`.hermes/`/`docs/class_action_plan.md`.

## Round 12 (2026-10-27)

Class `static` + `__str__` display dispatch implemented (plan: `class static implementation.md`); **no new opcodes** (reuses TABGET/TABPUT/TABCREATE/CALLCLOS). Harness re-run: **252/252** (`spec_check2`), **13** (`class_str`), **17** (`class_static`), **22** (`range`), **7** (`test_module`), all green = **369 total**.
- **`__str__` display dispatch fixed** (Part 1): `print`/`println`/`printf %s`/`sprintf %s`/f-string/`join` now route through `QLangVM::displayString` (same `__str__` call + class-name fallback as `str()`), with a `m_inDisplayString` re-entrancy guard so an `__str__` that displays `self` terminates. `table_ops::ToStr` dispatch kept honest. 13 cases in `class_str_test` (ctest `class_str`).
- **`static` keyword + `::` operator** (Part 2): `TokenType::Static` in `keywordType()` (`local static = 1` → parse error); `TokenType::ScopeRes` for `::` (lexed before single `:`, `parsePostfix` handles it → `Expr.scopeStatic`). `parseClass` accepts `static` field / `static const` / `static method`; `static`+`local`/`let` and duplicate/static-instance name collisions are parse errors. f-string format-spec splitter skips `::` (so `{Money::banner}` is not split at the scope colon).
- **Static storage + codegen** (Part 3): statics live in a class-level `__statics__` table (shared `shared_ptr` map) embedded in the prototype and copied **by reference** onto every instance. The prototype is created once at class definition into a hidden `__proto_<Name>` variable (shared by all instances + `::` access). Static methods are compiled with a hidden first parameter (the `__statics__` table) so they need no self; bare static names in method bodies resolve via `self.__statics__` (instance) or the anchor (static). `C::name` → two-hop TABGET; `C::name(args)` → TABGET closure + pass the statics table as anchor. `ClassRecord` gained `staticFields`/`staticConstFields`/`staticMethodFuncIndices`/`staticMethodArities`.
- **Instance-level static dispatch** (Part 4): `table_ops::getField`/`setField`/`callMethod` fall back to `__statics__` for names not in the instance field schema (so `money.banner` / `money.banner = "Turtle"` work and mutate shared state); `setField` rejects `static const` writes. No `ObjectOps` signature change (`callMethod` re-derives from-statics from the table).
- **Deviations from the plan:** (a) the plan's "hidden capture of `__statics__`" was replaced by a **hidden anchor parameter** for static methods — a capture by frame-register reference dangles once the prototype frame is popped (`deque::pop_back` destroys it), so the anchor (a shared `shared_ptr` passed as an arg) is lifetime-safe; (b) instance-call-of-a-static-method (`money.getBanner()`) is **not** routed to pass the anchor (the compiler's `obj.method()` path emits TABGET+CALLCLOS, not the VM `callMethod`); spec is silent and it is not in the required test set — left unsupported. See plan "Risks & Ceilings".
- 17 cases in `class_static_test` (ctest `class_static`): static field via class/instance, instance-write→class, cross-instance sharing, static method via class, arity error, f-string static (class+instance), `__str__` referencing a static, `static const` read (class+instance) + write error, `static local`/`static let`/`local static` parse errors, `C::missing` error, closed-field error intact.

## Round 11 (2026-10-27)

Spec drift audit (three waves same day): SPEC.md moved `efd53057` (38991 B / 1384 lines) → `30580ccf` (39692 B / 1426 lines) → `65df6095` (40105 B / 1437 lines) → `204f0702` (40335 B / 1443 lines); working tree dirty (uncommitted spec edits).
- Wave 1: `static` reserved word; Money example gains `static banner` + `static getBanner()`/`static setBanner()` + `Money::setBanner(...)` usage + `__str__` interpolating `{banner}`; new `## static` section.
- Wave 2: `::` row added to the operator table ("scope resolution operator"); Money example extended — static **field** read via class name (`f"{Money::banner}"`), static field read via **instance** (`f"{money.banner}"`), and instance write `money.banner = "Turtle"` mutates class-level storage (`Money::banner` then reads "Turtle").
- Wave 3: `## static` gains `static`+`const` allowed / `static`+`local`/`let` forbidden, plus an explicit note that `static`/`::` is a **new extension beyond v1** — added per user direction (v1 is implemented and stands; the `## v1 restrictions` contradiction is resolved by that note, no restriction line changed).
- Probed: `static` is **not** in `keywordType()` — tokenizes as `Ident`; no `::` token/parser path; no parser/VM support for static members or `ClassName::` calls. **Not implemented.**
- Harness re-run after each wave (no code changes): **252/252** (`spec_check2`), **33** (`spec_check`), **20** (`spec_cast_test`), **7** (`test_module`), **5** (`gen_test`), **22** (`range_check`) = **339 total**, all green.
- Open: `static` + `::` (new), duplicate-declaration warning (unchanged), `$$` (unchanged), `fetch` (unchanged).

## Round 10 (2026-10-26)

Block scope implemented (SPEC `# block scope`, committed `dde51e4`); harness re-run: **252/252** (`spec_check2`), all suites green (339 total).
- **Block scope: full conformance.** `QLangCompiler` gained `m_blockScopes` (stack of `map<name,reg>`) + `m_blockDepth`; `declareVar` pushes into the innermost block scope when depth > 0, else into `m_regs` (functions unchanged — closures/generators/mutation semantics intact). `regOf` consults the scope stack before `m_regs`; `undeclaredReg` consults it before treating a name as undeclared (plain-assignment crossing preserved). Scope pushed/popped at: `if`/`else` bodies, `for` body (loop var declared in enclosing scope per spec), `for-in` body (both range and array sub-paths — the range sub-path has an early case-exit that also pops), `while` body, and each `switch` case body + default body. 6 tests at spec_check2 L633–L640 (if isolation, nested shadow, else branch, while body, for body + loop-var visibility after loop, for-in shadow).
- No new opcodes; zero VM changes.

## Round 9 (2026-10-26)

Spec drift audit: SPEC.md moved `44e9fd2d` (38593 B / 1357 lines) → `600f11a8` (38921 B / 1382 lines), working tree dirty.
- `052b45e` (committed): `$` nil row `"false"` → `"nil"` (previously uncommitted) + compliance/handoff doc cleanup, stale docs deleted. No code changes; harness unaffected (333 total, last green run at HEAD `0c63f4b`).
- Uncommitted SPEC edits (3): duplicate-declaration error → **warning** (one example line self-contradictory); `## operator $$` "no block scope" clause removed; new `# block scope` section.
- Probed: dup-decl forms compile with no error and no warning; `local i = 3` in a block overwrites the outer `i` (`$i` → "3", spec expects "1") — **block scope not implemented**. Plain-assignment crossing works.
- Open: block scope (new section), duplicate-declaration warning (spec now asks for a warning, not an error), `$$` (unchanged), `fetch` (unchanged).

## Round 8 (2026-10-26)

Class conformance completed and the `$` operator implemented; harness re-run at HEAD `0c63f4b`: **246/246** (`spec_check2`), all suites green (333 total).
- **User-defined classes: full conformance.** Phases 3–7 landed this round: `6a4c559` (unique method/dunder names — compile error), `8c013a1` (fixed arity — constructor + method call checks), `b242645` (`__str__`), `0d7115b` (reference counting + `__destroy__` finalization). 42 class tests at spec_check2 L449–L615. Deviations from the plan documented in `class_action_plan.md` (Phase 0.2 no separate `Object` value type; Phase 9.2 `$` rejection on instances is runtime, not compile).
- **`$` dynamic-key operator implemented** (`0c63f4b`): `Dollar` token + `OP_EVALKEY` opcode; the commit fixed two bugs — EVALKEY was emitted but never dispatched (added to all 4 exec loops) and table-literal `$` keys degenerated to bare identifiers (parser now wraps the operand in a Unary node). 6 tests at spec_check2 L617–L626.
- **SPEC.md:** `$` section nil row corrected `"false"` → `"nil"` (matches implementation; **uncommitted** in working tree).
- **Re-verified gap:** duplicate-declaration compile check is still **not implemented** (`t = 5; local t = 9` compiles silently, prints `9`).
- `$$` indirect access: still **not implemented** (unchanged).

## Round 7 (2026-10-26)

Spec diff audit vs `21b1ce65` (1357 lines; `c83ebc8` + `e08005b` over `9cc0a3f`); harness re-run at HEAD `e08005b`: **204/204** (`spec_check2`), all suites green.
- `## operator $$` committed in full → tracked as **not implemented** (no `$$` token, no `OP_INDIRECT_READ`/`OP_INDIRECT_WRITE`, no declaration-state tracking, no harness cases).
- `## user defined classes` expanded → the existing **prototype is non-conforming** (closed field set, `const` fields, unique-name/arity compile checks, `__str__`, reference counting, `__destroy__`, `type(x)` all missing). Conformance baseline: `docs/EXTERNAL_TYPES.md` §9 — the `ObjectOps` dispatch layer must land first (classes are its first consumer).
- **New gap:** duplicate-declaration compile check is **not implemented** (spec examples committed in `9cc0a3f`; verified 2026-10-26 — duplicates silently compile, initializer becomes a plain assignment).
- `global c` unknown-binding example: ✓ implemented (compile error; message wording differs).
- `import`/`from` reserved words: ✓ implemented in the lexer (round 2).
- Class example `spend` fix (`+=` → `-=`): no harness case covered the example; no test change needed.

## Class Implementation Details

### Syntax
```
class Name {
  field = value;
  method(self, params)[] { body }
}
```

### Compilation
- Class body compiled in fresh context; prototype function creates table and populates fields/methods
- Constructor call: `TABCREATE` instance → `CALLFN` prototype → `TABGET`/`TABPUT` copy methods → `CALLFN` `__init__(self, args...)`
- Member calls (`obj.method(args)`): prepend `self` as first arg for known class methods; plain closures in tables called without self

### Verified
- `class Money { total = 0; __init__(self, amount)[] { self.total = amount; }; receive(self, amount)[] { self.total += amount; }; balance(self)[] { return self.total; } }`
- `local money = Money(125); money.receive(25); println(money.balance());` → outputs `150`
- Closure in table still works: `local events = { greet = (who)[] { return "hi " + who } }; events.greet("world")` → `"hi world"`

## user defined classes — spec conformance (c83ebc8)

The spec section was expanded from the original Money prototype example into a
full specification. **Status: fully conforming (2026-10-26).** All 9
implementation phases from `class_action_plan.md` are DONE (`6a4c559` unique
method/dunder names, `8c013a1` fixed arity, `b242645` `__str__`, `0d7115b`
refcounting + `__destroy__`; phases 0–2 earlier). The `ObjectOps` dispatch
layer (Phase 0, `docs/EXTERNAL_TYPES.md` §9) landed first as required;
instances are tables carrying an object-ops descriptor
(`__fields__`/`__const_fields__` in `QLangObjects.cpp`) — no separate `Object`
value type (documented deviation, Phase 0.2).

| Spec requirement | Status |
|---|---|
| `class Name { ... }` in global scope | ✓ implemented (`parseClass`) |
| field declarations → instance defaults | ✓ implemented |
| `__init__(self, ...)` at construction | ✓ implemented |
| `self` implicit in method calls | ✓ implemented |
| **closed field set** — undeclared `obj.f` read/write is a runtime error | ✓ implemented (tests L454–L456); `field 'x' not found on type 'T'` |
| **`const` fields** — read-only; `x.f = 1` → runtime (Type) error | ✓ implemented (tests L463–L470); `cannot assign to const field 'x'` |
| **unique method/dunder names** — compile error | ✓ implemented (`6a4c559`, tests L475–L477); `duplicate method 'm' defined at line X and line Y` |
| **fixed arity** — `ClassName(args)` vs `__init__` signature → error; class without `__init__` → zero args only | ✓ implemented (`8c013a1`, tests L495–L497) |
| **`__str__`** — `str(obj)`/f-string whole-object; class default = class name | ✓ implemented (`b242645`, tests L577–L583) |
| **reference counting** — share, never copy | ✓ implemented (`0d7115b`, tests L589–L592); `std::shared_ptr` descriptor, assignment shares |
| **`__destroy__`** — runs exactly once at final release | ✓ implemented (`0d7115b`, tests L597–L613) |
| **`type(x)`** — returns the class name | ✓ implemented (tests L539, L585) |
| instances are not tables — the `$` dynamic-key pipeline does not apply | ✓ implemented (tests L620–L625) — **deviation:** `$` on an instance is a **runtime** type error, not a compile error (instances are typed `Table` at compile time, so the compiler cannot distinguish them; Phase 9.2 in `class_action_plan.md`) |

### `static` class members + `::` (new extension beyond v1 — implemented Round 12, 2026-10-27)

Spec additions (2026-10-27, uncommitted): `static` reserved word, `::` scope-resolution
operator, `## static` section, and the Money example extended with `static banner`,
`static getBanner()`/`setBanner()`, `Money::setBanner(...)`, and `__str__`
interpolating `{banner}`. **Status: fully conforming** (17 cases in
`examples/class_static_test.cpp`, ctest `class_static`). No new opcodes.

| Spec requirement | Status |
|---|---|
| `static` is a reserved word (`local static = 1` → parse error) | ✓ `TokenType::Static` in `keywordType()` |
| `static banner = ...` static field (class-bound) | ✓ stored in shared `__statics__` table |
| `ClassName::field` read via the class name | ✓ two-hop TABGET (`__proto_<Name>` → `__statics__` → field) |
| `ClassName::method()` call via the class name | ✓ TABGET closure + pass `__statics__` as hidden anchor |
| static field read **through an instance** (`money.banner`) | ✓ `table_ops::getField` `__statics__` fallback |
| instance write **mutates class-level storage** (`money.banner = "Turtle"` → `Money::banner` reflects it) | ✓ `table_ops::setField` writes the shared map (by reference) |
| class write via instance **visible across instances** | ✓ every instance shares one `__statics__` map |
| `::` works inside f-string interpolation (`f"{Money::banner}"`) | ✓ (format-spec splitter skips `::`) |
| `static` + `const` allowed; `static` + `local`/`let` forbidden | ✓ const → `__static_const__` set (write → runtime error); `local`/`let` → parse error |
| `__str__` referencing a static (`f"balance: {self.total} at {banner}"`) | ✓ bare static name resolves via `self.__statics__` in method bodies |
| static method arity enforced (`Money::getBanner(1)` → compile error) | ✓ `staticMethodArities` (declared user params, no anchor) |
| `C::missing` → compile error | ✓ `'C' has no static member 'missing'` |
| instance closed-field error intact (statics not in the field schema) | ✓ statics excluded from `__fields__`; `money.notAField` still errors |

**Deviations:** the plan's "hidden capture" was replaced by a **hidden anchor
parameter** for static methods (a capture by frame-register reference dangles
once the prototype frame is popped). Instance-call-of-a-static-method
(`money.getBanner()`) is not routed to pass the anchor (compiler `obj.method()`
emits TABGET+CALLCLOS, not the VM `callMethod`); spec is silent, not in the
required test set.

## Open Items

- **`# error reporting` (new SPEC section, uncommitted 2026-10-27):** ✗ **NOT implemented.** Requirements: (a) report line+column where the error occurred (when available), (b) report the module, (c) report the class/function/table if applicable, (d) provide an execution trace — stack frame + execution chain, each node with line number and module/class/function/table. Probed (2026-10-27): `QLangError` carries line/col and formats them, but **152 of 162 throw sites pass `0, 0`** (nearly all VM/runtime throws — `QLangVM.cpp`, `QLangValue.cpp` accessors — have no line/col; parser/compiler/lexer errors do carry line info); no module name appears in any error message; no class/function/table context is attached; no execution trace / stack-frame chain is emitted on error (the VM keeps `m_frames` but nothing renders it into the error). Implementation sketch when tackled: attach the current pc→line/col table + module name + frame chain (function name, line) to `QLangError` at throw sites, formatted at the top-level catch in `QLangContext::run()`.
- **`string == object` via `__str__` (SPEC §operator == and !=, new example table):** ✓ **implemented** (Round 13, 2026-10-27; committed `c62b8e4`). `QLangVM::valueEqual` — when one side is a String and the other an object (a table carrying `__str__` or the `__class__` meta key), compare against `displayString` (`__str__` call + class-name fallback, re-entrancy guarded); every other combination delegates to the free `isEqual` (nil rules, numeric value equality, same-type values). New member `doBinaryVm` routes EQ/NE through `valueEqual` in all 4 exec loops (other opcodes → static `doBinary`). Plain tables (no `__str__`/`__class__`) keep the same-type rule — `"x" == {a=1}` → false. 6 new `spec_check2` cases (L370–L375): match, mismatch, reversed operand order, `!=` negation, number-vs-object, plain table. No new opcodes.
- **`static` class members + `::` scope resolution (SPEC §reserved words, §operators, `## static`; committed `a6c339a`/`6a40633`):** ✓ **implemented** (Round 12, 2026-10-27). `TokenType::Static` + `TokenType::ScopeRes`; `parseClass` accepts `static` field/const/method (`static`+`local`/`let` and name collisions → parse error); statics stored in a shared class-level `__statics__` table (prototype created once into `__proto_<Name>`, copied by reference onto every instance); `C::name` two-hop TABGET, `C::name(args)` passes the `__statics__` table as a hidden anchor to the static method; instance-level read/write via `table_ops::getField`/`setField`/`callMethod` fallback (instance writes mutate shared state); `static const` write → runtime error; f-string `::` handled. **Status: new extension beyond v1** (spec note added 2026-10-27 per user direction — v1 stands as specified; `## v1 restrictions` unchanged). 17 cases in `class_static_test`. **Deviations:** hidden anchor param (not the plan's frame-register capture, which dangles after the prototype frame is popped); instance-call-of-a-static-method not anchor-routed (spec-silent). See "user defined classes — spec conformance".
- **Duplicate declaration check (§scope/§variables; examples committed in `9cc0a3f`):** ✗ **NOT implemented.** Spec: the first declaration of a name is either an explicit `local`/`let` or its first plain assignment; a later explicit `local`/`let` for the same name in the same scope is a **compile error** (spec examples: `local xbit` redeclared in a branch; `t = 5; local t = 9`). Verified 2026-10-26: both compile with **no error** — the duplicate declaration is silently ignored and its initializer becomes a plain assignment (no duplicate-declaration check exists anywhere in `QLangCompiler.cpp`). **Spec change (committed `dde51e4`, 2026-10-26):** requirement softened from compile **error** to compile **warning** (wording now consistent after user fix); re-probed — still no error **and no warning**.
- **Block scope (`# block scope` — new section, committed `dde51e4`):** ✓ **implemented** (2026-10-26, Round 10). `local`/`let` inside a block belong to that block's scope; `local i = 3` in an `if` block no longer overwrites the outer `i` (example now yields "1"). Implementation: per-block `name → register` maps stacked in the compiler (`m_blockScopes`), pushed at `if`/`else`/`for`/`for-in`/`while`/`switch`-case bodies and popped after; `regOf`/`undeclaredReg` consult the stack before `m_regs`. Loop variables stay in the enclosing scope (visible after the loop, per spec). 6 harness cases; all suites green (339 total).
- **`global` of an unknown variable (spec example `global c` → "cannot bind unknown global"):** ✓ implemented (compile error at `QLangCompiler.cpp:1309`: `global variable 'c' is not declared` — message wording differs, behavior matches).

|||- **Reserved word `fetch`:** ✗ **NOT implemented.** Lexer/parser have no `TokenType::Fetch`; `fetch` tokenizes as `Ident` and silently works as an identifier rather than raising the reserved-word error the SPEC implies. Gap vs SPEC §reserved words.
||||- **`## verbose level` (**Priority one**):** ✓ **Implemented** (committed in `4091c32`; was uncommitted at last audit). Application-wide verbose level on `QLangContext` (`m_verboseLevel`, default 0): `setVerboseLevel(int)`, `verboseLevel()`, `isAt(int)`. QLang `set_verbose(level)` native fn (`QLangContext.cpp`) accepts int/dword/float and returns the new level. `QLangVM` gated emit helper `trace(minLevel, fmt, …)` (`QLangVM.h`); every stderr diagnostic in `QLangVM.cpp` converted from `fprintf` to `trace(T,…)` with tier T∈{1,2,3} (1 = frame/call events, 2 = VM loop+opcodes+returns, 3 = full detail: generators/table ops/bytecode); sites inside `callBuiltin` use `vm->trace(T,…)`; `trace()` short-circuits when `isAt(minLevel)` is false. Gates stderr trace only — program output (print/println/printf) is unaffected. Verified: build clean (`qlang` lib + `spec_check2`); runtime `set_verbose(0/1/2)` emits the correct tiers to stderr with stdout intact; C++ `setVerboseLevel(3)` → `verboseLevel()==3`, `isAt(2)==isAt(3)==true`. Gap vs SPEC resolved. |
- **Reserved word `step` + range/loop `step` (SPEC §reserved words, §range, §loops):** ✓ **Implemented** (`e955a8d`; committed, was uncommitted at last audit). `TokenType::Step` in `keywordType()` — `local step = 1` now raises the reserved-word error. `for x = a to b step N` and `for i in range(a,b) step N` parse (`stepExpr` in `parseFor()`, both forms) and compile via a 3-way fork: step==0 → infinite loop (guarded by the VM step limit → `runtime error: infinite loop (step limit)`, satisfying the spec's required timeout guard); step and direction agree → normal guarded loop (constant path `LE`-based, endpoint inclusive); user step in the wrong direction → body runs exactly once. `for x in [..] step n` → type error per spec. Variable step, continue/break/next all work. 19 tests in `spec_check2`.
- **`$` operator (SPEC §The $ Operator):** ✓ **Resolved** (`0c63f4b`). `Dollar` token + `OP_EVALKEY` opcode (dispatched in all 4 exec loops); scalar-to-string evaluator, dynamic table-literal keys; rejects array/table/class instances (runtime type error). 6 tests at spec_check2 L617–L626.
- **`$$` indirect access operator (SPEC §operator $$):** ✗ **NOT implemented** — spec committed in full in `c83ebc8` (replaces the `9cc0a3f` TODO stub): indirect variable access from a runtime string. Six-stage check pipeline (string → identifier lexeme → reserved word → declared/visible → binding kind → local-only writes) with canonical `indirect variable error:` messages; writes local-only, reads locals + `global`-exposed (global const visible everywhere); no variable creation; functions/lambdas never targetable, const read-only; sequential declaration state (unique declaration event = explicit `local`/`let` or first plain assignment; no block scope, so branch-dependent declarations can be `not declared`; direct-vs-indirect forward-reference asymmetry). Implementation notes in the spec: per-frame name map (name → register, binding kind `var`/`const`/`function`/`lambda`, declaration bit), declaration bit as a flag on existing store/init opcodes, `OP_INDIRECT_READ`/`OP_INDIRECT_WRITE` opcodes, optional compile-time fast path for `$$"literal"`. No harness cases; implementation pending.
|||- **`range` context-dependent encoding (§range):** ✓ **Resolved.** Compiler emits a counter loop for `for i in range(a,b)` (`i=a; while i<=b { …; ++i }`, no array), a bounds check `(l>=a) && (l<=b)` for `l in range(a,b)` in any expression/condition context (including `while i in range` and `let x = b in range(0,10)`), materialization for `arr = range(a,b)`, and a generator for `for i in range { yield i }` (containsYield recurses into ForIn). `continue` in a range loop jumps to the implicit `INC`. Verified by `examples/range_check.cpp` (22 passed, 0 failed).
||- **`min`/`max`/`sum` via `import math`:** ✓ implemented. C++-registered module (`src/QLangModules.cpp`). `min`/`max`: return `Int` for values within int32_t range; return `Dword` when result exceeds ±2147483647 (prevents silent truncation). Reject mixed float+int with type error (§767/§770). Accept single array arg (`min([1,5,9])` → `1`). Forbid mixed char+byte. `sum`: flattens all array args (`sum([1,2,3])` and `sum([1,2], [3,4])` both work); returns `Int` for in-range, `Dword` on overflow; returns `Float` when any arg is float (§777); allows float+int mix (sums in double then returns float). |
- **`next` keyword form:** Resolved. `next` without a variable now works (assumes current loop variable). `next x` with explicit var still works. `next y` where `y != loopVar` is treated as `next` + statement `y` — `y` must be declared separately.
- **Module system (§604–661):** v1 implemented. `import math` (flat import all) and `import math.min` (single func) both work. Modules are C++-registered via `ctx.registerModule()`, resolved at compile time. No new opcodes. Qualified access (`math.min(...)`) is a future extension.
- Generators (§608–623): ✓ **Implemented.** `yield` is a reserved word (`TokenType::Yield`). Generator values are stored as `Type::Generator` using `std::function<std::optional<QLangValue>()>` — `std::nullopt` signals exhaustion (EOF). `yield expr` compiles to `YIELD` opcode with the expression result register. `yield` (empty) compiles to `YIELD` with a `CONST nil` temp. The VM handler calls the closure; `nullopt` returns `nil` as the EOF sentinel. `next(gen())` calls the generator. Verified: `def gen_range(mi,mx) { for i = mi to mx { yield i } }` then iterating with `while v != nil { v = gen() }` yields 1,2,3 then nil. Test cases in `gen_test.cpp` (5): basic yield, second yield, exhaustion/EOF, loop accumulate, empty yield, outer variable capture, type check. **Fix:** generator `RETURN` now keeps the saved frame marked `exhausted`; the generator loop early-returns nil for an already-exhausted frame; post-loop cleanup no longer erases the exhausted frame — so a consumed generator keeps returning nil instead of restarting the body from scratch.
- User-defined classes: ✓ **Fully conforming** (2026-10-26; phases in `6a4c559`/`8c013a1`/`b242645`/`0d7115b`/`0c63f4b`; **`static` members + `::`** added Round 12, 2026-10-27). Closed field set, `const` fields, unique method/dunder names (compile error), fixed arity, `__str__` (now dispatched in all display paths), reference counting, `__destroy__`, `type(x)` = class name, `static` fields/methods + `::` scope resolution (new extension beyond v1). 42 tests at spec_check2 L449–L615 + 13 (`class_str`) + 17 (`class_static`). Instances are tables carrying an object-ops descriptor (`__fields__`/`__const_fields__`/`__statics__` in `QLangObjects.cpp`); `$` on an instance is a runtime type error (documented deviation, Phase 9.2 in `class_action_plan.md`). See "user defined classes — spec conformance".
- `ctx.loadBuiltinModules()`: ✓ implemented. C++ API added after `ctx.clear()` to reload default modules.
