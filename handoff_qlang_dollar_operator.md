# Handoff: QLang `$` Operator Implementation

## Status
**IN PROGRESS** - The `$` operator for dynamic key resolution in table literals is partially implemented but has issues.

## Current State

### What Works
1. **Lexer**: The `$` token is correctly recognized as `TokenType::Dollar` (line 363 in QLangLexer.cpp)
2. **Parser**: The `$` operator is parsed as a unary operator in `parseUnary()` (line 875 in QLangParser.cpp)
3. **Compiler**: The `$` operator maps to `Opcode::EVALKEY` in `unOp()` (line 2075 in QLangCompiler.cpp)
4. **VM**: The `EVALKEY` opcode handler exists and should throw errors for arrays/tables/functions (lines 707-722 in QLangVM.cpp)

### Test Results
```
Test 1-5: PASS - $ operator works for int, string, float, bool, nil
Test 6: FAIL - $ operator with array should throw error but doesn't
Test 7: FAIL - $ operator with table should throw error but doesn't
Test 8: FAIL - $ operator in table literal fails to parse
```

### Issues Identified

1. **Tests 6 & 7**: The `$` operator is NOT throwing errors for arrays and tables. The VM code looks correct, but something is preventing the error from being thrown.

2. **Test 8**: The `$` operator is NOT being recognized in table literals. The parser expects an identifier as the key in table literals (line 1357 in QLangParser.cpp), not a `$` expression.

## Root Cause Analysis

### Issue 1: Why isn't EVALKEY throwing errors?
The VM code at lines 707-722 in QLangVM.cpp looks correct:
```cpp
case Opcode::EVALKEY: {
  switch (a.type()) {
  case QLangValue::Type::Array:
  case QLangValue::Type::Table:
  // ... throws error
  }
}
```

Need to verify:
- Is the EVALKEY opcode being emitted correctly?
- Is the operand being evaluated correctly?

### Issue 2: Why isn't $ working in table literals?
The `parseTableLiteral()` function (lines 1351-1375 in QLangParser.cpp) expects:
```cpp
std::string key = consume(TokenType::Ident, "table key").value;
```

This only accepts identifiers, not `$` expressions. Need to modify to support dynamic keys.

## Next Steps

1. **Debug EVALKEY**: Add debug output to verify the opcode is being emitted and executed correctly
2. **Fix table literal parsing**: Modify `parseTableLiteral()` to accept `$` expressions as keys
3. **Verify error handling**: Ensure the VM correctly throws errors for unsupported types

## Files to Examine
- `/home/cfrankb/toolkit/gcc/qlang-sandbox/src/QLangVM.cpp` - EVALKEY handler
- `/home/cfrankb/toolkit/gcc/qlang-sandbox/src/QLangParser.cpp` - parseTableLiteral()
- `/home/cfrankb/toolkit/gcc/qlang-sandbox/src/QLangCompiler.cpp` - compileExpr()
- `/home/cfrankb/toolkit/gcc/qlang-sandbox/examples/test_dollar_operator.cpp` - test file

## Handoff Date
2026-10-26