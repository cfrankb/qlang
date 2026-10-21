# QLang

Skeleton for a small scripting language whose interpreter is written in C++ and
is hosted by a `QLangContext`. This first version **parses** source text into an
AST and exposes a variable scope; execution over the AST is the next milestone.

Grammar and type list follow `spec.md`.

## Layout

```
include/qlang/   public headers
src/             implementation
examples/        runnable driver (qlang_example)
spec.md          language spec
```

| Component        | Responsibility                                             |
|------------------|------------------------------------------------------------|
| `QLangValue`     | Type-tagged value (variant over all script types)          |
| `QLangErrors`    | `QLangError` with kind + 1-based line/column               |
| `QLangLexer`     | Tokenization: comments, strings, chars, operators          |
| `QLangParser`    | Recursive-descent parser -> AST                            |
| `QLangContext`   | Interpreter host: owns scope, drives lex/parse/run         |

## Build

```sh
cmake -S . -B build
cmake --build build -j
./build/qlang_example     # parses a sample program and prints the AST
ctest --test-dir build    # smoke test
```

Requires a C++17 compiler.

## Supported grammar (from spec.md)

- `#` line comments
- Declarations: `local a = 32;`, typed `local b:byte = 32`
- Literals: int, `' '` char (byte), `"..."` string, `"""..."""` / `'''...'''` multiline
- Copy / compound assign: `c = a`, `a += 12`, `s += "abc"`
- `for x = 1 to 32 ... next x`
- `if ... then ... end` (comparisons, `&&`, `||`, `==`, `!=`, `<`, `>`, `<=`, `>=`)
- `while (cond) ... end`
- Operators: `= == != < > <= >= << >> ^ & | && || () -- ++` plus `+ - * /`

## Value types

`string`, `blob`, `byte`(uint8_t), `word`(uint16_t), `short`(int16_t),
`int`(int32_t), `unsigned`(uint32_t), `array`(mixed list).

## Usage

```cpp
#include "qlang/QLangContext.h"

qlang::QLangContext ctx;
std::vector<qlang::StmtPtr> program = ctx.parse(source); // throws qlang::QLangError
ctx.set("x", qlang::QLangValue::Int(7));
int32_t v = ctx.get("x").asInt();
```

## Status / next steps

- [x] Lexer + parser for the spec grammar
- [ ] AST executor (variable scope, control flow, operators)
- [ ] C++ <-> script FFI (expose host functions/objects to scripts)
- [ ] `run()` drives the executor instead of just parsing
