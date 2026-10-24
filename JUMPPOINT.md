# QLang Generator Bug — Jump Point

Resume state for a future session. Do NOT re-derive the root cause; it's
confirmed below. Awaiting a decision on which fix option to apply.

## Situation
- Repo: `/home/cfrankb/toolkit/gcc/qlang-sandbox` (QLang = register-based C++17 bytecode VM).
- Bug: a resumed generator's registers are corrupted — `reg[2..]` come back as `Nil`
  instead of their saved `Int` values, so e.g. `sum + g` throws
  `operator '+' operand must be numeric, got nil`.
- Scope: all non-generator spec tests pass; the 3 generator cases in
  `examples/spec_check2.cpp` fail.
- Debug output (~721 lines) already injected in `src/QLangVM.cpp` (`[DBG-*]`).

## Confirmed root cause: use-after-free in QLangVM::callFunction()
`m_generatorFrames` is `std::unordered_map<int, Frame>` (Frame stored by value).
In the generator-resume branch:

```cpp
auto sit = m_generatorFrames.find(funcIndex);
if (sit != m_generatorFrames.end()) {
  saved = &sit->second;          // pointer into the map's node
  m_generatorFrames.erase(sit);  // destroys node -> saved now DANGLING
}
...
Frame frame;
frame.regs.resize(args.size() + fn.registerCount);  // heap alloc, REUSES the freed block
for (k) frame.regs[k] = args[k];                    // reg[0],reg[1]=args; reg[2..]=0 (resize default)
...
if (saved) {
  frame.regs = saved->regs;   // reads freed memory -> gets reused block: args + zeroed tail
}
```

Evidence (same run): the saved frame's register addresses are IDENTICAL across
save and read (e.g. `reg[2]` = `0x...ac290` both times) but values flip
`Int val=1` -> `Nil val=0`. Same address + changed value == memory overwritten
in place, i.e. use-after-free, not a move/realloc. `frame.regs.resize()` reuses
the freed block, so `saved->regs` returns args in reg[0..1] and zeros in reg[2..]
(the loop var `i` + temporaries) — the nil that breaks the resume.

Type map (QLangValue::Type): 0=Nil, 6=Int. So `type6 val=1` = Int 1; `type0` = Nil.

## Fix options
- A (recommended, ~5 lines): copy the saved frame OUT of the map BEFORE erase,
  use the stable local.
  ```cpp
  Frame savedFrame; bool hasSaved = false;
  auto sit = m_generatorFrames.find(funcIndex);
  if (sit != m_generatorFrames.end()) { savedFrame = sit->second; hasSaved = true; m_generatorFrames.erase(sit); }
  ...
  if (hasSaved) { Frame *saved = &savedFrame; frame.pc = saved->pc; frame.regs = saved->regs; frame.captures = saved->captures; ... }
  ```
- B: read/copy from `sit->second` while valid, `erase` last. Same correctness.
- C: store frames as `unique_ptr<Frame>` / vector so erase never invalidates a
  live pointer. Overkill unless more save/restore sites are expected.
- D (secondary): after A, verify generator captures pointing via
  `&m_frames[savedCur]` remain valid (deque keeps them stable; likely fine).

Recommend A. Then run the 3 generator tests + full spec to confirm green.

## Build / run
```bash
cd /home/cfrankb/toolkit/gcc/qlang-sandbox
g++ -std=c++17 -Iinclude -g examples/spec_check2.cpp build/libqlang.a -o /tmp/sc
stdbuf -oL -eL /tmp/sc 2>/dev/null | grep -E "FAIL|PASS|type error|line 0"
# debug trace:  stdbuf -oL -eL /tmp/sc 2>&1 1>/dev/null | grep -E "DBG-GEN|DBG-YIELD|FAIL|type error"
```

## Status
AWAITING DECISION: which fix option (A/B/C/D) to apply.
