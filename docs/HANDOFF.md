# QLang Generator Bug - Handoff for New Session

## Goal
Fix generator frame register corruption in QLang VM.

## Bug Description
When a generator yields and is resumed, the saved frame's registers are corrupted. Specifically:
- `reg[2]` (and other registers beyond the first two) change from their correct values to nil (type0 val=0)
- This happens between the YIELD handler saving the frame and `callFunction()` restoring it

## Evidence from Debug Output

**After YIELD saves frame:**
```
[DBG-YIELD-SAVE] func=1 saved in map: pc=8 regs.size=9 regs.ptr=0x55dbe4aaf0b0
[DBG-YIELD-SAVE]   map reg[0]=type6 val=1 ptr=0x55dbe4aaf0b0
[DBG-YIELD-SAVE]   map reg[1]=type6 val=3 ptr=0x55dbe4aaf0e0
[DBG-YIELD-SAVE]   map reg[2]=type6 val=1 ptr=0x55dbe4aaf110
```

**Before `callFunction()` restores frame:**
```
[DBG-GEN-SAVED] func=1 saved frame: pc=8 regs.size=9 regs.ptr=0x56283fa18230
[DBG-GEN-SAVED]   saved reg[0]=type6 val=1 ptr=0x56283fa18230
[DBG-GEN-SAVED]   saved reg[1]=type6 val=3 ptr=0x56283fa18260
[DBG-GEN-SAVED]   saved reg[2]=type0 val=0 ptr=0x56283fa18290
```

Same pointer address, different value (type6→type0). Corruption occurs between these two points.

## Current State

### Test Cases (Failing)
File: `examples/spec_check2.cpp`

```cpp
// Test 1: gen second yield -> FAIL
def gen_range(mi, mx) { for i = mi to mx { yield i } }
local gen = gen_range(1, 3)
local r = gen(); r = gen(); // second yield fails

// Test 2: gen exhausted returns nil -> FAIL
local r = gen(); gen(); local r = gen(); // third yield fails

// Test 3: gen loop accumulate -> FAIL
local sum = 0
for g = gen_range(1, 3) { sum = sum + g }
```

All fail with: `type error at line 0,0: operator '+' operand must be numeric, got nil`

### Key Files
- `src/QLangVM.cpp` - VM implementation with generator support
- `include/qlang/QLangVM.h` - VM header with Frame struct and m_generatorFrames
- `examples/spec_check2.cpp` - Test file

### Generator Flow (How It Works)
1. `MAKEGEN` (line ~1371): Creates a `GeneratorValue` lambda that calls `callFunction()`
2. `YIELD` (line ~1409): Saves current frame to `m_generatorFrames[funcIndex]`, pops frame, returns yielded value
3. `CALLCLOS` (line ~1016/1709): When calling a generator, invokes the lambda which calls `callFunction()`
4. `callFunction()` (line ~1484): Restores saved frame from `m_generatorFrames` and enters generator loop

### Where Corruption Happens
Between:
- Line ~1418: `m_generatorFrames[frame.funcIndex] = std::move(savedFrame);`
- Line ~1517: `if (saved) {` (in `callFunction()`)

The main VM continues executing after YIELD returns, then hits CALLCLOS at pc=11, which triggers the generator resume.

## Debug Patches Applied
721 lines of debug output added to `src/QLangVM.cpp`:
- `[DBG-YIELD-*]` - YIELD handler debug
- `[DBG-GEN-CALL]` / `[DBG-GEN-SAVED]` / `[DBG-GEN-RESTORE]` - callFunction debug
- `[DBG-NEXT]` - next() builtin debug
- Type string helpers for Function, Generator, Eof
- `isTrue()` now handles Eof type

## What's Been Investigated

### Things Checked
1. ✅ Frame copy semantics - `Frame` struct has `std::vector<QLangValue> regs`, should copy by value
2. ✅ Memory addresses - saved frame registers have different addresses than main frame
3. ✅ `m_frames` deque - using deque to keep frame pointers stable
4. ✅ `m_generatorFrames` map - stores Frame by value, not pointer
5. ✅ QLangVariant - std::variant, should copy by value

### Things NOT Yet Checked
1. The lambda capture in MAKEGEN - does it capture by reference something that gets invalidated?
2. Is `m_generatorFrames` being reallocated when inserting?
3. Is the Frame copy constructor doing something unexpected?
4. Could the `regs` vector be sharing memory via SSO or some other optimization?

## Key Code Sections

### YIELD Handler (line ~1409-1430)
```cpp
case Opcode::YIELD: {
  QLangValue yielded = frame.regs.empty() ? QLangValue() : frame.regs[0];
  Frame savedFrame = m_frames[m_cur];
  savedFrame.pc = frame.pc + 1;
  m_generatorFrames[frame.funcIndex] = std::move(savedFrame);
  m_frames.pop_back();
  m_cur--;
  if (m_cur >= 0 && m_frames[m_cur].callDest >= 0)
    m_frames[m_cur].regs[m_frames[m_cur].callDest] = yielded;
  // ... debug output ...
}
```

### callFunction() Restore (line ~1491-1533)
```cpp
if (fn.isGenerator) {
  Frame *saved = nullptr;
  auto sit = m_generatorFrames.find(funcIndex);
  if (sit != m_generatorFrames.end()) {
    saved = &sit->second;
    m_generatorFrames.erase(sit);
  }
  // ...
  if (saved) {
    frame.pc = saved->pc;
    frame.regs = saved->regs;  // <-- corruption happens before this
    frame.captures = saved->captures;
    // ...
  }
}
```

### MAKEGEN Lambda (line ~1383)
```cpp
*gen = [this, funcIdx = spec.funcIndex, captureSpec = spec.captures, capturedArgs = std::move(callArgs)]() -> std::optional<QLangValue> {
  return callFunction(funcIdx, capturedArgs);
};
```

## Hypotheses

1. **Lambda capture issue:** The lambda captures `this`, but something about how it's invoked causes the saved frame to be corrupted.

2. **Map reallocation:** When `m_generatorFrames` is inserted into, it might reallocate, but the saved frame is stored by value, so this shouldn't matter.

3. **Frame copy is shallow:** The `Frame` struct's `regs` vector might be sharing memory somehow.

4. **Main frame reusing register memory:** After YIELD pops the generator frame, the main frame's register vector might be reallocated and somehow overlap with the saved frame.

## Build Commands
```bash
cd /home/cfrankb/toolkit/gcc/qlang-sandbox
g++ -std=c++17 -Iinclude -g examples/spec_check2.cpp build/libqlang.a -o /tmp/sc
stdbuf -oL -eL /tmp/sc 2>/dev/null | grep -E "FAIL|DBG"
```

## Next Steps
1. Add debug to print saved frame registers immediately after YIELD saves and immediately before `callFunction()` reads them
2. Check if the lambda in MAKEGEN is capturing something incorrectly
3. Verify the Frame copy is deep (not sharing vector memory)
4. Consider storing generator frames differently (e.g., in a separate vector instead of unordered_map)
