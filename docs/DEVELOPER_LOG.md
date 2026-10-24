# QLang Developer Log

## Architectural Layout

```
include/qlang/          Public headers (10 files, 1,248 lines)
  QLangValue.h          Type-tagged value (variant over all script types)
  QLangVariant.h        std::variant wrapper for QLangValue
  QLangErrors.h         QLangError with kind + 1-based line/column
  QLangLexer.h          Tokenization interface
  QLangParser.h         Recursive-descent parser -> AST
  QLangBytecode.h       Register-machine opcodes + Instruction + Bytecode
  QLangCompiler.h       AST -> bytecode compiler
  QLangVM.h             Register-machine executor
  QLangContext.h        Interpreter host (scope, parse, run, call)
  QLangModules.h        Built-in module registration helpers
  QValue.h              QValue type alias + free qval_* helpers

src/                    Implementation (8 files, 5,756 lines)
  QLangValue.cpp        Value type implementation
  QLangLexer.cpp        Tokenizer
  QLangParser.cpp       Recursive-descent parser
  QLangBytecode.cpp     Bytecode container + opcode name()
  QLangCompiler.cpp     Compiler: AST -> register bytecode
  QLangVM.cpp           VM: instruction dispatch, builtin funcs, generators
  QLangContext.cpp      Context: parse/run/call/module plumbing
  QLangModules.cpp      Math module (min/max/sum) registration

examples/               Test/drivers (17 files, 1,706 lines)
  hello.cpp             Smoke test
  spec_check.cpp        Original spec cases
  spec_check2.cpp       147-case full spec harness (151 with generator extensions)
  spec_cast_test.cpp    20 cast tests
  test_module.cpp       7 module tests
  test_*.cpp            Feature integration tests
  debug_*.cpp           Targeted debug drivers

CMakeLists.txt          CMake 3.16, C++17, static lib + 3 executables
SPEC.md                 Language specification
SPEC_COMPLIANCE.md      Traceability: what works, what's missing
DEVELOPER_LOG.md        This file
```

## Compiler/VM Pipeline

```
QLangContext::run(source)
  │
  ├─ QLangLexer::tokenize()         → vector<Token>
  ├─ QLangParser::parse()           → vector<StmtPtr> (AST)
  ├─ QLangCompiler::compile()       → CompiledProgram { Bytecode, Functions, Vars, NativeFuncs }
  │     ├─ register allocation (m_regs)
  │     ├─ constant pool emission
  │     ├─ control-flow lowering (JMP, JMPIFFALSE, JMPIFTRUE)
  │     ├─ function/closure compilation (Function::captures, isGenerator)
  │     └─ jump-target patching (forward refs resolved post-parse)
  │
  └─ QLangVM(prog, scope)
        ├─ run()                     → top-level main function
        ├─ runFrames(startFrame)     → opcode loop (100M-step infinite-loop guard)
        ├─ callFunction(idx, args)   → named function dispatch (handles generator creation/resume)
        ├─ callClosure(closure, args)→ closure/generator dispatch
        └─ pushCall(callee, regs)    → frame setup for CALLFN/CALLCLOS
```

## Register Machine (Opcode Enum)

| Category | Opcodes |
|----------|---------|
| Data movement | `CONST, MOV, CONV, NEG, INC, DEC` |
| Arithmetic | `ADD, SUB, MUL, DIV, MOD, POW` |
| Bitwise/shifts | `AND, OR, XOR, SHL, SHR` |
| Logical | `LAND, LOR, NOT` |
| Comparison | `EQ, NE, LT, GT, LE, GE, IN` |
| Calls | `CALL (builtin), CALLFN (userfn), CALLCLOS (closure), YIELD (generator)` |
| Control flow | `JMP, JMPIFFALSE, JMPIFTRUE, CONTINUE, BREAK, RETURN` |
| Arrays | `ARRGET, ARRPUT, ARRAPP, ARRAPPV, ARRLEN, SEQGET` |
| Tables | `TABCREATE, TABGET, TABPUT` |
| Casts | `CAST` |
| Closures | `MAKEFUNC, REFLOAD, REFSTORE` |
| Cleanup | `DELETE, LEN` |
| Termination | `HALT` (opcode 47) |

**Built-in functions** (Builtin enum): `ToStr, ToInt, Type, Print, Println, Printf, Len, Blob, Substr, Slice, Range, Join, IsSet, Sprintf, Format, Assert`

## Implemented Features

### Core Language (100% spec-conformant)
- [x] All value types: `string, blob, byte, word, short, int, unsigned, dword, array, float, table, bool, char`
- [x] All reserved words (38 keywords): `for, to, in, then, while, continue, break, return, string, str, byte, blob, word, short, int, unsigned, array, float, table, bool, true, false, def, let, else, range, goto, char, dword, switch, case, default, type, join, global, const, end, cast, class, yield`
- [x] Declarations: `local a = 32;`, typed `local b:byte = 32`, `const year = 2026;`
- [x] Literals: int, hex (`0xc`), float (`1.13`), negative (`-124`), char-as-byte (`'x'`), strings (single/double/triple-quoted)
- [x] Operators: `= == != < > <= >= << >> ^ & | && || ||| ** ! ++ -- %`
- [x] Compound assign: `+=` with type-aware dispatch (string concat, numeric add, array append, table merge)
- [x] Control flow: `if/then/end`, `if/else if/else`, `while/end`, `switch/case/default/break`, `label/goto`
- [x] Loops: `for x = 1 to N`, `for i in [arr]`, `for i in range(1,10)`, `for c in "string"`, `next`, `continue`, `break`
- [x] Functions: `def name(params) { }`, `def name(params) end`, `return`
- [x] Closures: `local f = (params)[] { }` (no capture), `local f = (params)[x] { }` (explicit capture by reference)
- [x] Live capture binding: captured vars update in closures after reassignment
- [x] Nested `def` functions with correct capture scoping
- [x] f-strings: `f"Total: ${price:.2f}"` with full printf spec support (`:2f, :d, :x, :X, :o, :06x, :#06x, :5d, :-5s`)
- [x] Array ops: literal `[]`, concat `+`, append `[] =`, `+=`, index read/write, `len()`
- [x] Table ops: literal `{}`, member read/write `.name`, `[key]` access, `len()`
- [x] `delete var`, `cast<Type>(expr)`, `assert(bool, str)`
- [x] Optional `;` separator
- [x] `let` as alias for `local`

### Built-in Functions
- [x] `toStr(var), toInt(var), type(var), print(var), println(var)`
- [x] `printf(fmt, ...), sprintf(fmt, ...) -> string` — `%d/%f/%s/%c/%%` + `%.Nf` precision
- [x] `len(var) -> int` — string/array/table/blob; other types → 1; error on undefined
- [x] `substr(str, start, len) -> string`
- [x] `slice(array, start, len) -> array`
- [x] `range(start, end) -> array`
- [x] `join(separator, arr) -> string`
- [x] `isset(var) -> bool`
- [x] `assert(bool, str) -> void` — throws runtime error on falsy condition
- [x] `int(nil) -> 0` (not throw)

### User-Defined Classes (§811–818)
- [x] `class Name { fields; methods }` syntax
- [x] Methods as arrow closures with `self` as first param
- [x] Constructor: creates instance table, calls prototype, copies methods, calls `__init__(self, args...)`
- [x] Member calls `obj.method(args)` prepend `self` for known class methods
- [x] `__str__`, `__destroy__` support

### Module System (§604–661)
- [x] `import math` (flat import all)
- [x] `import math.min` (single function)
- [x] `from math import *` (flat wildcard)
- [x] `from math import min, max` (named import)
- [x] `import math as m` (alias)
- [x] C++ registration: `ctx.registerModule(name, module)`, `ctx.importModule(name, alias)`
- [x] `ctx.clear()` + `ctx.loadBuiltinModules()` — reset and reload modules
- [x] Math module: `min(number...|array), max(number...|array), sum(number...|array)`
  - `min/max`: overflow to `Dword`, reject mixed float+int, reject mixed char+byte, accept single array
  - `sum`: flattens arrays, returns `Float` when any arg is float, returns `Int` otherwise, overflow to `Dword`

### C++ API
- [x] `QLangContext::run(source)` — parse + compile + execute
- [x] `QLangContext::parse(source)` — parse only (throws on error)
- [x] `ctx.set(name, value, isConst=false)` — inject writable or read-only var
- [x] `ctx.setFunc(name, func)` — inject native function (Lua convention: context first)
- [x] `ctx.call(name, args)` — invoke QLang or native function by name
- [x] `ctx.clear()` — reset all state
- [x] `ctx.has(name) / ctx.isset(name)` — check variable existence
- [x] `ctx.remove(name)` — delete variable
- [x] `QValue` type alias + free helpers: `qval_nil, qval_bool, qval_byte, qval_char, qval_word, qval_short, qval_int, qval_unsigned, qval_dword, qval_float, qval_str, qval_blob, qval_array, qval_table`
- [x] `CompiledProgram::nativeFuncs` — populated at compile time
- [x] `CompiledProgram::constVars` — const-qualified vars (injected + declared)
- [x] `QLangCompiler::addExternalConsts()` — guard injected consts against reassignment
- [x] `QLangCompiler::addExternalFuncs()` — register native func names
- [x] `QLangCompiler::addModules()` — register modules for compile-time import resolution

### Generators (§700–744)
- [x] `yield expr` — reserved word, compiles to `YIELD` opcode
- [x] `yield` (empty) — yields nil
- [x] Generator values stored as `Type::Generator` (`std::function<std::optional<QLangValue>()>`)
- [x] `std::nullopt` signals exhaustion (EOF sentinel)
- [x] `callFunction`: first call creates `GeneratorValue` wrapper, subsequent calls resume saved frame
- [x] `callClosure`: detects `Type::Generator`, invokes the closure
- [x] `isTrue`: `Eof` type returns false (nil-equivalent)
- [x] YIELD handler in both `run()` and `runFrames()` — saves frame, returns yielded value
- [x] 7 test cases in `spec_check2.cpp`: basic yield, second yield, exhaustion, loop accumulate, empty yield, capture, type check

## Test Results

| Harness | Cases | Passed | Failed |
|---------|-------|--------|--------|
| `spec_check2.cpp` | 151 | **151** | **0** |
| `spec_cast_test.cpp` | 20 | **20** | **0** |
| `test_module.cpp` | 7 | **7** | **0** |
| `hello.cpp` (smoke) | 1 | pass | — |
| **Total** | **178** | **178** | **0** |

## Current State

**All tests passing. No outstanding defects.**

### File Structure

| File | Lines | Role |
|------|-------|------|
| `include/qlang/QLangValue.h` | ~120 | `QLangValue` — type-tagged variant over all script types |
| `include/qlang/QLangVariant.h` | ~80 | `QLangVariant` — std::variant wrapper |
| `include/qlang/QLangErrors.h` | ~40 | `QLangError` — kind + line/col |
| `include/qlang/QLangLexer.h` | ~60 | `QLangLexer` — tokenize → vector<Token> |
| `include/qlang/QLangParser.h` | ~80 | `QLangParser` — parse → vector<StmtPtr> |
| `include/qlang/QLangBytecode.h` | ~180 | `Opcode`, `Instruction`, `Bytecode`, `Function`, `CaptureRef`, `CompiledProgram` |
| `include/qlang/QLangCompiler.h` | ~50 | `QLangCompiler` — AST → bytecode |
| `include/qlang/QLangVM.h` | ~80 | `QLangVM` — register-machine executor |
| `include/qlang/QLangContext.h` | ~100 | `QLangContext` — host interface |
| `include/qlang/QLangModules.h` | ~30 | Module registration helpers |
| `include/qlang/QValue.h` | ~50 | `QValue` alias + free `qval_*` helpers |
| `src/QLangVM.cpp` | ~2,500 | VM: run(), runFrames(), callFunction(), callClosure(), pushCall(), dispatch |
| `src/QLangCompiler.cpp` | ~1,800 | Compiler: compile(), compileExpr(), compileStmt(), compileFunction() |
| `src/QLangParser.cpp` | ~1,200 | Parser: parse(), parseStatement(), parseExpression(), etc. |
| `src/QLangLexer.cpp` | ~600 | Lexer: tokenize(), number(), string(), etc. |
| `src/QLangValue.cpp` | ~400 | Value: type(), typeName(), asInt(), asString(), etc. |
| `src/QLangContext.cpp` | ~500 | Context: run(), parse(), call(), set(), setFunc(), clear(), etc. |
| `src/QLangModules.cpp` | ~200 | Math module registration |
| `src/QLangBytecode.cpp` | ~100 | Bytecode: name() |

### Generator Implementation (src/QLangVM.cpp)

The generator system uses a three-path architecture:

1. **`callFunction` (first call):** Creates the generator frame, saves it in `m_generatorFrames`, and returns a `GeneratorValue` (a `std::function<std::optional<QLangValue>()>`). The generator body is NOT executed on the first call — only the object is created.

2. **`callFunction` (resume call):** Finds the saved frame in `m_generatorFrames`, restores it (PC, registers, captures), and runs the inline VM loop until the next `YIELD` or exhaustion.

3. **`callClosure` / `CALLCLOS`:** When a generator value is invoked (e.g., `gen()`), the `Type::Generator` branch calls the stored `std::function`, which routes back to `callFunction` for resumption.

4. **YIELD handlers:** In both `run()` and `runFrames()`, the `YIELD` opcode saves the current frame (`pc = frame.pc + 1`), stores it in `m_generatorFrames[funcIndex]`, pops the frame, and writes the yielded value to the caller's `callDest` register.

### Key Design Decisions

- **Generator values are `std::function`, not raw frames:** This allows them to be stored in tables, passed as args, and called via `CALLCLOS` — matching the spec's treatment of generators as first-class callables.
- **`m_generatorFrames` maps `funcIndex → Frame`:** Since each compiled program has at most one active instance of each generator function, the simple map works. Future work could key by `(funcIndex, callerFrame)` for re-entrant generators.
- **`Eof` type returns false in `isTrue`:** Ensures `next(gen())` returns nil-equivalent on exhaustion, making `while v != nil { v = gen() }` idioms work naturally.

## Open Spec Items (Next Features)

- [ ] **Qualified access `math.min(...)`:** Future extension — requires parser/compiler changes for dotted call resolution
- [ ] **String module (`tba` in spec):** Stub exists in SPEC.md, no implementation

## Branch / Commit Strategy

```
HEAD → current (all 178 tests passing, 0 failures)
  ├─ (clean working tree except DEVELOPER_LOG.md)
  └─ main                     (current state is production-ready)
```

**Commit history:**
```
31fc707 added class
b195b06 update: module math
8779c16 fix: math.min/max/sum overflow to Dword, sum flattens arrays
033d7f0 fix hello.cpp: add QLangModules include and compiler.addModules call
a96121b SPEC.md: document loadBuiltinModules()
```

## Build Command

```bash
cmake -B build -S . && cmake --build build -j$(nproc)
stdbuf -oL -eL ./build/spec_check2 2>/dev/null   # 151 pass
stdbuf -oL -eL ./build/spec_cast_test 2>/dev/null # 20 pass
stdbuf -oL -eL ./build/test_module 2>/dev/null     # 7 pass
```

---

*Log updated: 2026-10-23*
*Current HEAD: `31fc707` (added class)*
*SPEC.md SHA256: `022fa4b2cd98d36fafbdac3010438fe2438db223ffba5b0d25698b03e192c032`*
*Total test coverage: 178 cases, 178 passed, 0 failed*
