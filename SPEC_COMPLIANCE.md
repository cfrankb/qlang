# QLang SPEC.md — Compliance Reference

Purpose: hand-off notes for the next session. Records what was verified against
SPEC.md, what works, what does not, root causes, and recommended next steps.

Verified by building the project (CMake, C++17, clean) and empirically running
spec constructs through `QLangContext::run()` via `examples/spec_check2.cpp`
(204 cases). The shipped `examples/hello.cpp` smoke test passes.

## Current Status

**204 passed, 0 failed** (harness `examples/spec_check2.cpp`, built against `libqlang.a`) + **33 passed, 0 failed** (`spec_check`) + **20 passed, 0 failed** (`spec_cast_test`) + **7 passed, 0 failed** (`test_module`) + **5 passed, 0 failed** (`gen_test`) + **22 passed, 0 failed** (`examples/range_check.cpp`, `range` ctest) = **291 total**. Re-verified 2026-10-25 against the current lib (HEAD `9cc0a3f`); all suites green. `spec_check2` now includes 19 `step` cases (for-to + for-in range, asc/desc, variable step, step-0 infinite guard, array-step error, continue/break), so `step` is test-verified. `$$` is fully specified (SPEC §operator $$) but **not implemented** — no harness cases yet.

Line numbers below are against the CURRENT SPEC.md (33724 bytes, 1249 lines, SHA256: 9cc5482b5ceb6edc275db26e8242ed1e133df6498f2cf0222d0d60316a1251fc). Working tree is DIRTY: the `## operator $$` section is drafted in full (uncommitted; replaces the TODO stub committed in `9cc0a3f`). The `import`/`from` reserved words, duplicate-declaration examples, `$$` operator-table row + TODO stub were committed in `9cc0a3f`; the `step` reserved word + range/loop `step` examples in `d07eb4f`/`21cc1c5`; the `step` implementation + inclusive-endpoint fix is `e955a8d`.

## SPEC.md Changes Since Last Audit

SHA256: `9cc5482b5ceb6edc275db26e8242ed1e133df6498f2cf0222d0d60316a1251fc` (1249 lines, 33724 bytes).

| Commit | Change |
|--------|--------|
| `uncommitted` | `## operator $$` drafted in full (replaces the TODO stub): definition (indirect access, no operand coercion vs `$`, first-class expression), example blocks (function/const/global-write-denied cases added), restrictions list, six-stage resolution/check pipeline with canonical `indirect variable error:` messages, resolution order (writes: frame locals only; reads: locals + `global`-exposed, global const visible everywhere), declaration-state rules (unique declaration event, no block scope, branch-dependent declarations, direct-vs-indirect forward-reference asymmetry), const/function binding rules, implementation notes (frame name map, declaration bit on existing store/init opcodes, `OP_INDIRECT_READ`/`OP_INDIRECT_WRITE`, optional compile-time fast path). Spec-only — **not implemented**, no harness cases. |
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

## Uncommitted SPEC.md Edits (working tree; not in git)

SPEC.md working-tree hash = `9cc5482b...` (1249 lines, 33724 bytes). Uncommitted edit:
1. `## operator $$` drafted in full — replaces the TODO stub from `9cc0a3f`. Adds:
   definition paragraph (indirect access; no operand coercion vs `$`; first-class
   expression), three new example blocks (function target, local const, global write
   denial), the restrictions list, the six-stage resolution/check pipeline table,
   resolution order, declaration-state rules (sequential declarations, no block
   scope, branch-dependent declarations, forward-reference asymmetry note),
   const/function binding rules, and compiler/VM implementation notes (frame name
   map, declaration bit, `OP_INDIRECT_READ`/`OP_INDIRECT_WRITE`, optional
   compile-time fast path for `$$"literal"`). Spec-only — implementation pending.

The `import`/`from` reserved words + duplicate-declaration examples + `$$` TODO stub
are now **committed** (`9cc0a3f`). The `step` reserved word + range/loop `step`
examples are **committed** (`d07eb4f`/`21cc1c5`) and **implemented** (`e955a8d`).
The `## verbose level` section and its implementation were committed in `4091c32`.
The prior header "instructions" → "agent instructions" edits and the build-files
allowlist are committed.

`docs/QLangVM.cpp.orig` and `docs/QLangVM.cpp.rej` are patch artifacts; the `.rej` hunk
was an extra `[DBG-YIELD-REG]` register-dump loop that was NOT applied. `docs/` also
holds handoff notes from the `step` implementation sessions (`step_plan.md`,
`hand_off.md`, `for_next_step_handoff.md`, `handoff_range_nostep_bug.md`,
`nostep_range_decode.md`, `compare_bytecode_if.md`, `for_loop_bug.md`, `notes.txt`).
The tally in "Current Status" (291 total) was re-run on 2026-10-25 and now covers the
`step` material (19 cases in `spec_check2`).

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
- [x] **user-defined classes:** `class Name { fields; methods }` — field assignments and arrow-closure methods with `self` param; constructor creates instance table, calls prototype, copies methods, invokes `__init__(self, args...)`; member calls `obj.method(args)` prepend `self` for known class methods

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
|| §operator $$ | `$$` indirect access operator | ✗ not implemented | Now **fully specified** (SPEC §operator $$, drafted 2026-10-25, uncommitted): six-stage check pipeline (string → identifier lexeme → reserved word → declared/visible → binding kind → local-only writes) with canonical `indirect variable error:` messages; resolution order (writes: frame locals only; reads: locals + `global`-exposed, global const visible everywhere); no variable creation; functions/lambdas never targetable, const read-only; sequential declaration state (declaration bit set at the unique declaration event; no block scope, so branch-dependent declarations can be `not declared`). Implementation per spec notes: per-frame name map (name → register, binding kind `var`/`const`/`function`/`lambda`, declaration bit), declaration bit as a flag on existing store/init opcodes (no new opcode for declaration state), two new opcodes `OP_INDIRECT_READ`/`OP_INDIRECT_WRITE`, optional compile-time fast path for a future `$$"literal"` form. No harness cases yet. |
|| §operator $ | `$` dynamic key resolution | ✗ not implemented | See Open Items. No `Dollar` token, no `OP_EVAL_KEY` opcode. |
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
| `bool` | `Bool` | ✓ | `true`/`false` literals compile to `Int(1/0)`; `local b:bool` parses; `type(b)` → `"bool"`; bools participate in arithmetic/comparisons via `isNumeric()` |
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
| `class` | `Class` | ✓ | **Implemented.** Parser handles `class Name { ... }` with field assignments and method definitions. Methods use arrow closure syntax `(params)[] { ... }` with `self` as first param. Constructor calls `__init__(self, args...)` automatically. Member calls `obj.method(args)` prepend `self`. Prototype function per class creates table and populates methods. |
| `fetch` | — | — | **Not a keyword.** Not implemented; no `TokenType::Fetch` in lexer/parser. Listed as reserved word in SPEC but the lexer does not recognize it. Gap.
| `step` | `Step` | ✓ | ★ **Implemented** (`e955a8d`). Reserved word: `local step = 1` → parse error. `for x = a to b step N` and `for i in range(a,b) step N`: direction from start/end; user step in the wrong direction → body runs exactly once; `step 0` → infinite loop, bounded by the VM step-limit guard (satisfies the spec's required "timeout guard that results in a runtime exception" — throws `runtime error: ... infinite loop (step limit)`); `for x in [..] step n` → `type error: step is not supported over array literals in for-in loops`. Constant + variable step; continue/break/next intact. 19 tests in `spec_check2`. |
| `$` | — | — | ★ **Not implemented.** SPEC §The $ Operator (dynamic key resolution / scalar-to-string evaluator): `$age` → `"33"`, table-literal dynamic keys `{ $property = "male" }`, scalar types only (array/table/class → compile/runtime error), requires a dedicated `OP_EVAL_KEY` opcode with a type-tag check. No `Dollar` token in the lexer, no `OP_EVAL_KEY` in `QLangBytecode.h`. Gap. |
| `$$` | — | — | ★ **Not implemented** — now **fully specified** (SPEC §operator $$, drafted 2026-10-25, uncommitted): indirect access operator, runtime name resolution from a string. No `DollarDollar` token, no `OP_INDIRECT_READ`/`OP_INDIRECT_WRITE` opcodes. Spec: six-stage check pipeline, local-only writes, reads locals + `global`-exposed (global const visible everywhere), no variable creation, functions/lambdas never targetable, const read-only, sequential declaration state. See Not Yet Implemented + Open Items. |

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
  while, operators, toStr/toInt). Result: **33 passed, 0 failed.**
- `examples/spec_check2.cpp` — 204 test cases covering all spec features including `bool`, `char`, `dword`, `switch`, `range`/`slice` reserved-word status, closures, `**`, `let`, goto, slice, range, method calls, `==`/`!=` nil-equality, function/generator returns, and 19 `step` loop cases. Result: **204 passed, 0 failed.**
- `examples/spec_cast_test.cpp` — 20 cast tests: numeric casts (int/float/byte/word/short/unsigned/dword/char/bool), string casts, invalid collection casts, nested casts, expression-context casts, nil cast. Result: **20 passed, 0 failed.**
- `examples/gen_test.cpp` — 5 generator tests: basic yield, second yield, exhaustion/EOF, loop accumulate, empty yield, outer variable capture, type check. Result: **5 passed, 0 failed.**
- `examples/range_check.cpp` — 22 range tests: `for i in range(a,b)` counter loop (sum/count/break/continue), `in range(a,b)` bounds check (inside/low/high/above/below/as-value), `arr = range(a,b)` materialization, for-in array membership, generator range. Result: **22 passed, 0 failed.**
Compile against the static lib:
`g++ -std=c++17 -Iinclude examples/spec_check2.cpp build/libqlang.a -o /tmp/sc && /tmp/sc 2>/dev/null`
Result: **204 passed, 0 failed.** All spec builtins implemented.
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

Core spec fully implemented. **204 passed, 0 failed** (spec_check2) + **33 passed, 0 failed** (spec_check) + **20 passed, 0 failed** (spec_cast_test) + **7 passed, 0 failed** (test_module) + **5 passed, 0 failed** (gen_test) + **22 passed, 0 failed** (range_check) = **291 total**. Since the prior audit: implemented the `step` loop construct (both `for` forms; 19 new cases) and made the `for` endpoint inclusive for all step sizes (`e955a8d`); SPEC.md gained the `step` reserved word/examples (`d07eb4f`/`21cc1c5`); `import`/`from` reserved words, duplicate-declaration examples, and the `$$` operator-table row + TODO stub committed in `9cc0a3f`; **`## operator $$` drafted in full** (uncommitted, replacing the TODO stub): definition, examples, restrictions, six-stage check pipeline, resolution order, sequential declaration-state rules, const/function binding rules, and compiler/VM implementation notes — spec-only, implementation and harness cases pending.

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

## Open Items

|||- **Reserved word `fetch`:** ✗ **NOT implemented.** Lexer/parser have no `TokenType::Fetch`; `fetch` tokenizes as `Ident` and silently works as an identifier rather than raising the reserved-word error the SPEC implies. Gap vs SPEC §reserved words.
||||- **`## verbose level` (**Priority one**):** ✓ **Implemented** (committed in `4091c32`; was uncommitted at last audit). Application-wide verbose level on `QLangContext` (`m_verboseLevel`, default 0): `setVerboseLevel(int)`, `verboseLevel()`, `isAt(int)`. QLang `set_verbose(level)` native fn (`QLangContext.cpp`) accepts int/dword/float and returns the new level. `QLangVM` gated emit helper `trace(minLevel, fmt, …)` (`QLangVM.h`); every stderr diagnostic in `QLangVM.cpp` converted from `fprintf` to `trace(T,…)` with tier T∈{1,2,3} (1 = frame/call events, 2 = VM loop+opcodes+returns, 3 = full detail: generators/table ops/bytecode); sites inside `callBuiltin` use `vm->trace(T,…)`; `trace()` short-circuits when `isAt(minLevel)` is false. Gates stderr trace only — program output (print/println/printf) is unaffected. Verified: build clean (`qlang` lib + `spec_check2`); runtime `set_verbose(0/1/2)` emits the correct tiers to stderr with stdout intact; C++ `setVerboseLevel(3)` → `verboseLevel()==3`, `isAt(2)==isAt(3)==true`. Gap vs SPEC resolved. |
- **Reserved word `step` + range/loop `step` (SPEC §reserved words, §range, §loops):** ✓ **Implemented** (`e955a8d`; committed, was uncommitted at last audit). `TokenType::Step` in `keywordType()` — `local step = 1` now raises the reserved-word error. `for x = a to b step N` and `for i in range(a,b) step N` parse (`stepExpr` in `parseFor()`, both forms) and compile via a 3-way fork: step==0 → infinite loop (guarded by the VM step limit → `runtime error: infinite loop (step limit)`, satisfying the spec's required timeout guard); step and direction agree → normal guarded loop (constant path `LE`-based, endpoint inclusive); user step in the wrong direction → body runs exactly once. `for x in [..] step n` → type error per spec. Variable step, continue/break/next all work. 19 tests in `spec_check2`.
- **`$` operator (SPEC §The $ Operator):** ✗ **NOT implemented.** No `Dollar` token, no `OP_EVAL_KEY` opcode. `$age` / dynamic table-literal keys unsupported. Gap.
- **`$$` indirect access operator (SPEC §operator $$):** ✗ **NOT implemented** — spec drafted in full this round (uncommitted, replaces the `9cc0a3f` TODO stub): indirect variable access from a runtime string. Six-stage check pipeline (string → identifier lexeme → reserved word → declared/visible → binding kind → local-only writes) with canonical `indirect variable error:` messages; writes local-only, reads locals + `global`-exposed (global const visible everywhere); no variable creation; functions/lambdas never targetable, const read-only; sequential declaration state (unique declaration event = explicit `local`/`let` or first plain assignment; no block scope, so branch-dependent declarations can be `not declared`; direct-vs-indirect forward-reference asymmetry). Implementation notes in the spec: per-frame name map (name → register, binding kind `var`/`const`/`function`/`lambda`, declaration bit), declaration bit as a flag on existing store/init opcodes, `OP_INDIRECT_READ`/`OP_INDIRECT_WRITE` opcodes, optional compile-time fast path for `$$"literal"`. No harness cases; implementation pending.
|||- **`range` context-dependent encoding (§range):** ✓ **Resolved.** Compiler emits a counter loop for `for i in range(a,b)` (`i=a; while i<=b { …; ++i }`, no array), a bounds check `(l>=a) && (l<=b)` for `l in range(a,b)` in any expression/condition context (including `while i in range` and `let x = b in range(0,10)`), materialization for `arr = range(a,b)`, and a generator for `for i in range { yield i }` (containsYield recurses into ForIn). `continue` in a range loop jumps to the implicit `INC`. Verified by `examples/range_check.cpp` (22 passed, 0 failed).
||- **`min`/`max`/`sum` via `import math`:** ✓ implemented. C++-registered module (`src/QLangModules.cpp`). `min`/`max`: return `Int` for values within int32_t range; return `Dword` when result exceeds ±2147483647 (prevents silent truncation). Reject mixed float+int with type error (§767/§770). Accept single array arg (`min([1,5,9])` → `1`). Forbid mixed char+byte. `sum`: flattens all array args (`sum([1,2,3])` and `sum([1,2], [3,4])` both work); returns `Int` for in-range, `Dword` on overflow; returns `Float` when any arg is float (§777); allows float+int mix (sums in double then returns float). |
- **`next` keyword form:** Resolved. `next` without a variable now works (assumes current loop variable). `next x` with explicit var still works. `next y` where `y != loopVar` is treated as `next` + statement `y` — `y` must be declared separately.
- **Module system (§604–661):** v1 implemented. `import math` (flat import all) and `import math.min` (single func) both work. Modules are C++-registered via `ctx.registerModule()`, resolved at compile time. No new opcodes. Qualified access (`math.min(...)`) is a future extension.
- Generators (§608–623): ✓ **Implemented.** `yield` is a reserved word (`TokenType::Yield`). Generator values are stored as `Type::Generator` using `std::function<std::optional<QLangValue>()>` — `std::nullopt` signals exhaustion (EOF). `yield expr` compiles to `YIELD` opcode with the expression result register. `yield` (empty) compiles to `YIELD` with a `CONST nil` temp. The VM handler calls the closure; `nullopt` returns `nil` as the EOF sentinel. `next(gen())` calls the generator. Verified: `def gen_range(mi,mx) { for i = mi to mx { yield i } }` then iterating with `while v != nil { v = gen() }` yields 1,2,3 then nil. Test cases in `gen_test.cpp` (5): basic yield, second yield, exhaustion/EOF, loop accumulate, empty yield, outer variable capture, type check. **Fix:** generator `RETURN` now keeps the saved frame marked `exhausted`; the generator loop early-returns nil for an already-exhausted frame; post-loop cleanup no longer erases the exhausted frame — so a consumed generator keeps returning nil instead of restarting the body from scratch.
- User-defined classes (§811–818): ✓ **Implemented.** `class Name { fields; methods }` syntax. Methods compiled as arrow closures with `self` as first param. Constructor: creates instance table, calls prototype function, copies methods, calls `__init__(self, args...)`. Member calls `obj.method(args)` prepend `self` for known class methods; plain closures in tables called without self. Supports `__init__`, `__destroy__`, `__str__` methods per spec prototype.
- `ctx.loadBuiltinModules()`: ✓ implemented. C++ API added after `ctx.clear()` to reload default modules.
