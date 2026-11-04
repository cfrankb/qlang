# Instruction Format — Escape-Byte Implementation Plan

Implements: `docs/plans/instruction-format-escape-byte-plan.md`
Status: Ready for implementation
Constraint: One atomic change — compiler, VM, codec, disassembler, tests land together.

---

## Step 1 — `include/qlang/QLangBytecode.h`: New encoding primitives

### 1a. Change `Opcode` to 7-bit identity

Replace:
```cpp
enum class Opcode : uint16_t {
```
With:
```cpp
enum class Opcode : uint8_t {
```

All existing enumerator values stay the same (they are already < 128). Remove `kOpcodeCount` sentinel; replace with:
```cpp
inline constexpr uint8_t kOpcodeEscape = 0x80;
inline constexpr uint8_t kOpcodeMax = 128; // identities 0..127
```

### 1b. Remove `usesI32()`

Delete the entire `inline bool usesI32(Opcode op)` function.

### 1c. Remove the fixed-width `Instruction` struct

Delete:
```cpp
struct Instruction {
  Opcode op = Opcode::MOVC;
  int16_t a = 0;
  union {
    struct { int16_t b; int16_t c; };
    int32_t i32 = 0;
  };
};
static_assert(sizeof(Instruction) == 8, ...);
```

### 1d. Add decoded instruction record

```cpp
struct DecodedInstruction {
  Opcode op = Opcode::MOVC;
  uint8_t modifier = 0;   // 0 = base form; non-zero only when escaped
  int32_t a = 0;          // left operand (register or constant index)
  int32_t b = 0;          // right operand (register, constant, or unused)
  int32_t c = 0;          // third operand (register; 3-operand ALU only)
  int64_t imm = 0;        // signed immediate (MOVI/ADDI/SUBI/CMPI)
  int32_t jumpOffset = 0; // relative offset for jump opcodes
  size_t size = 0;        // total byte size of this instruction in the stream
};
```

### 1e. Add encoding/decoding helper declarations

```cpp
// Class predicates
bool isJumpOpcode(Opcode op);
bool isImmediateOpcode(Opcode op);
bool isRegConstOpcode(Opcode op);

// Size computation
size_t baseInstructionSize(Opcode op);
size_t escapedInstructionSize(Opcode op, uint8_t modifier);
size_t instructionSize(Opcode op, uint8_t modifier = 0);

// Jump width selection
size_t minJumpSize(int32_t offset);
// Returns 2 (jmp8), 4 (jmp16), or 6 (jmp32)

// Operand width
size_t registerAddressSize(int32_t index);  // 1 or 2
size_t constantAddressSize(int32_t index);  // 1 or 2

// Encode one instruction into a byte buffer
void encodeInstruction(std::vector<uint8_t>& out, const DecodedInstruction& in);

// Decode one instruction from a byte buffer at position pc
void decodeInstruction(const uint8_t* code, size_t pc, size_t codeSize,
                       DecodedInstruction& out);
```

### 1f. Change `Function` storage

Replace:
```cpp
std::vector<Instruction> instructions;
```
With:
```cpp
std::vector<uint8_t> code;                    // flat byte stream
std::vector<size_t> instrOffsets;             // byte offset of each instruction
```

Keep `loc` as-is (parallel to `instrOffsets`).

### 1g. Change `Bytecode` class

Replace `std::vector<Instruction> m_instr;` with `std::vector<uint8_t> m_code;`.

Change public API:
- `void add(const Instruction& inst)` → remove
- `void add(Opcode op, int a = 0, int b = 0, int c = 0)` → remove
- `Instruction& at(size_t i)` → remove
- `size_t size() const` → return `m_code.size()` (byte count)
- Add: `const std::vector<uint8_t>& code() const { return m_code; }`
- Add: `std::vector<uint8_t>& code() { return m_code; }`
- Add: `void setCode(std::vector<uint8_t> c) { m_code = std::move(c); }`

Keep `addConstant`, `setConstants`, `constants`, `addFunction`, `functions` unchanged.

---

## Step 2 — `src/QLangBytecode.cpp`: Implement encoding helpers

### 2a. `isJumpOpcode`
Return true for: `JMP, JMPIFFALSE, JMPIFTRUE, CONTINUE, BREAK, JE, JNE, JG, JGE, JL, JLE`.

### 2b. `isImmediateOpcode`
Return true for: `MOVI, ADDI, SUBI, CMPI`.

### 2c. `isRegConstOpcode`
Return true for: `MOV, MOVC, MAKEFUNC, MAKEGEN, ADD, SUB, MUL, DIV, MOD, AND, OR, XOR, SHL, SHR, LAND, LOR, EQ, NE, LT, GT, LE, GE, POW, INx, ARRGET, ARRPUT, ARRAPP, ARRAPPV, ARRLEN, LEN, SEQGET, TABGET, TABPUT, INDGET, INDPUT, EVALKEY, CALL, CALLFN, CALLCLOS, RETURN, REFLOAD, REFSTORE, YIELD, NEG, NOT, INC, DEC, CONV, CAST, DELETEx, TABCREATE, TABFINALIZE`.

### 2d. `baseInstructionSize(Opcode op)`
- Jump opcodes: 2 (opcode + 1-byte offset)
- Immediate opcodes: 3 (opcode + 1-byte reg + 1-byte imm)
- 1-operand reg ops (`NEG, NOT, INC, DEC, EVALKEY, DELETEx, TABCREATE, TABFINALIZE, RETURN, YIELD`): 2
- 2-operand ops (`MOV, MOVC, MAKEFUNC, MAKEGEN, REFLOAD, REFSTORE, CMP, CMPI, ARRAPP, ARRLEN, LEN, SEQGET, ARRAPPV`): 3
- 3-operand ops (`ADD, SUB, MUL, DIV, MOD, AND, OR, XOR, SHL, SHR, LAND, LOR, EQ, NE, LT, GT, LE, GE, POW, INx, ARRGET, ARRPUT, TABGET, TABPUT, INDGET, INDPUT, CONV, CAST, CALL, CALLFN, CALLCLOS`): 4
- `JMPIFFALSE, JMPIFTRUE`: 3 (opcode + 1-byte reg + 1-byte offset)

### 2e. `escapedInstructionSize(Opcode op, uint8_t modifier)`
- Jump: `modifier==0` → 4, `modifier==1` → 6, else throw. Conditional jumps
  (JMPIFFALSE/JMPIFTRUE) add 1 byte for the always-1-byte condition register:
  5 / 7.
- Immediate: left form = `modifier & 0x0F`, imm form = `modifier >> 4`.
  - Left size: form 0 → 1, form 1 → 2, else throw.
  - Imm size: form 4 → 1, form 5 → 2, form 6 → 4, else throw.
  - Total: 2 (op+mod) + leftSize + immSize.
- RegConst: left form = `modifier & 0x0F`, right form = `modifier >> 4`.
  - Left size: form 0 → 1, form 1 → 2, else throw.
  - Right size: form 0 → 1, form 1 → 2, else throw.
  - Third operand (if 3-operand op): always 1.
  - Total: 2 + leftSize + rightSize + (third ? 1 : 0).

### 2f. `minJumpSize(int32_t offset)`
```cpp
if (offset >= -127 && offset <= 127) return 2;
if (offset >= -32767 && offset <= 32767) return 4;
return 6;
```

### 2g. `registerAddressSize` / `constantAddressSize`
```cpp
if (index < 0 || index > 65535) throw QLangError(...);
return (index <= 255) ? 1 : 2;
```

### 2h. `encodeInstruction`
Write bytes into `out`:
- If base form: write `static_cast<uint8_t>(op)`, then operands per `baseInstructionSize`.
- If escaped form: write `static_cast<uint8_t>(op) | kOpcodeEscape`, write `modifier`, then operands per `escapedInstructionSize`.
- Jumps: write signed offset as int8/int16/int32 little-endian.
- Immediates: write signed value as int8/int16/int32 little-endian.
- Registers/constants: write as uint8 or uint16 little-endian.

### 2i. `decodeInstruction`
Read from `code[pc]`:
- `uint8_t raw = code[pc]`; `Opcode op = static_cast<Opcode>(raw & 0x7F)`.
- If `raw & kOpcodeEscape`: read modifier byte at `pc+1`, decode per class.
- Else: decode base form.
- Set `out.size` to total bytes consumed.
- Throw `QLangError(QLangErrorKind::Io, ...)` on truncation or invalid modifier.

### 2j. `Bytecode::name(Opcode)`
Keep the existing switch. Remove the `kOpcodeCount` case. Add `default: return "???";`.

---

## Step 3 — `src/QLangCompiler.cpp`: Flat-stream emission

### 3a. Replace `m_code` type

`m_code` is currently `Bytecode`. After Step 1g, `Bytecode` wraps `std::vector<uint8_t>`. The compiler no longer calls `m_code.add(op, a, b, c)`.

### 3b. Introduce a per-function raw instruction buffer

Add a file-local struct:
```cpp
struct RawInstr {
  Opcode op;
  int32_t a = 0, b = 0, c = 0;
  int64_t imm = 0;
  int32_t jumpTarget = -1; // instruction index; -1 = not a jump
  uint16_t line = 0, col = 0;
};
```

Add a file-local class (or just a set of free functions operating on a `std::vector<RawInstr>&`):
```cpp
struct FnEmitter {
  std::vector<RawInstr> raw;
  std::vector<QLangValue> consts;
  uint16_t curLine = 0, curCol = 0;

  size_t emit(Opcode op, int32_t a = 0, int32_t b = 0, int32_t c = 0, int64_t imm = 0);
  size_t emitJump(Opcode op, int32_t target);
  void patchJump(size_t idx, int32_t target);
  int addConst(QLangValue v);
  void finish(Function& fn);
};
```

### 3c. `FnEmitter::emit`
- Append `RawInstr{op, a, b, c, imm, -1, curLine, curCol}`.
- Return index.

### 3d. `FnEmitter::emitJump`
- Append `RawInstr{op, 0, 0, 0, 0, target, curLine, curCol}`.
- Return index.

### 3e. `FnEmitter::patchJump`
- `raw[idx].jumpTarget = target;`

### 3f. `FnEmitter::finish(Function& fn)`
1. Validate: every `jumpTarget` is in `[0, raw.size()]`.
2. Compute initial sizes:
   - Non-jump: determine base vs escaped from operand widths; call `instructionSize`.
   - Jump: initial size = 6 (assume jmp32).
3. Compute `instrOffsets` from sizes (cumulative sum).
4. Fixpoint loop (max 4 iterations):
   - For each jump `i`: `offset = instrOffsets[raw[i].jumpTarget] - (instrOffsets[i] + sizes[i])`; `newSize = minJumpSize(offset)`; if changed, update and recompute offsets.
5. Encode: for each raw instr, build `DecodedInstruction`, call `encodeInstruction` into `fn.code`.
6. Set `fn.instrOffsets`, `fn.loc` (from `raw[i].line/col`), `fn.constants = std::move(consts)`.
7. Compute `fn.registerCount` = max(a, b, c) across all raw instrs + 1.

### 3g. Replace all `emit()` / `emitImm()` calls

Current `QLangCompiler::emit(Opcode op, int a, int b, int c)`:
- Change to call `m_emitter.emit(op, a, b, c)`.
- Stamp `m_emitter.curLine = m_curLine; m_emitter.curCol = m_curCol;` before each emit.

Current `QLangCompiler::emitImm(Opcode op, int a, int32_t imm)`:
- Change to call `m_emitter.emit(op, a, 0, 0, imm)`.

### 3h. Replace `patchJump`

Current: `static void patchJump(Instruction& ins, int target) { ins.i32 = target; }`
New: `m_emitter.patchJump(idx, target);` where `idx` is the instruction index.

All call sites that do `m_code.instructions()[j]` to find and patch jumps must be changed to track instruction indices at emit time. The loop compilation code already tracks indices (`jumpFalseIdx`, `bodyStart`, etc.) — those remain valid as indices into `m_emitter.raw`.

### 3i. Replace `m_code.size()` / `m_code.instructions().size()`

- `m_code.size()` → `m_emitter.raw.size()` (instruction count, for label/target computation).
- `m_code.instructions()[j]` → `m_emitter.raw[j]` (for patching).

### 3j. `compile()` finalization

At the end of `compile()`:
- Call `m_emitter.finish(mainFn)` to produce `mainFn.code`, `mainFn.instrOffsets`, `mainFn.loc`, `mainFn.constants`.
- For each function in `m_functions`, they were already finalized by `buildFunction`/`compileClass`.
- Set `out.registerCount = std::max(m_nextReg, 1)`.

### 3k. `buildFunction` / `compileClass`

These save/restore `m_code`. Change to save/restore the `FnEmitter` state (raw vector, consts, curLine/curCol). At the end, call `emitter.finish(fn)` before pushing to `m_functions`.

### 3l. Immediate selection in `emitImm`

```cpp
void QLangCompiler::emitImm(Opcode op, int a, int32_t imm) {
  m_emitter.curLine = m_curLine;
  m_emitter.curCol = m_curCol;
  m_emitter.emit(op, a, 0, 0, imm);
  // The encoder in finish() will choose base (1-byte) vs escaped (2/4-byte)
  // based on the value's range.
}
```

The width selection happens in `FnEmitter::finish` when building the `DecodedInstruction`:
- If `imm` fits in `int8_t` and `a <= 255`: base form.
- Else if `imm` fits in `int16_t`: escaped, imm form 5.
- Else: escaped, imm form 6.

### 3m. Register/constant width selection

In `FnEmitter::finish`, for each non-jump, non-immediate instruction:
- If `a <= 255 && b <= 255 && c <= 255`: base form.
- Else: escaped form with 2-byte addressing for any operand ≥ 256.
- If any operand > 65535: throw.

---

## Step 4 — `src/QLangVM.cpp` + `include/qlang/QLangVM.h`: Byte-offset PC

### 4a. `Frame::pc` type

Already `size_t`. No change needed.

### 4b. Main dispatch loop in `runFrames()`

Replace:
```cpp
const Instruction &i = instrs[frame.pc];
Opcode op = i.op;
```
With:
```cpp
DecodedInstruction i;
decodeInstruction(fn.code.data(), frame.pc, fn.code.size(), i);
frame.pc += i.size;
Opcode op = i.op;
```

### 4c. Operand access

Replace all `i.a`, `i.b`, `i.c`, `i.i32` with `i.a`, `i.b`, `i.c`, `i.imm`, `i.jumpOffset`:
- `MOVI`: `frame.regs[i.a] = QLangValue::Int(static_cast<int32_t>(i.imm));`
- `ADDI`: `frame.regs[i.a] = doBinary(Opcode::ADD, frame.regs[i.a], QLangValue::Int(static_cast<int32_t>(i.imm)), this);`
- `SUBI`: same with `SUB`.
- `CMPI`: `frame.flags = cmpFlags(frame.regs[i.a], QLangValue::Int(static_cast<int32_t>(i.imm)), this);`
- `MOVC`: `frame.regs[i.a] = consts[i.b];`
- `MAKEFUNC`/`MAKEGEN`: `const QLangValue& cv = consts[i.b];`

### 4d. Jump execution

Replace:
```cpp
case Opcode::JMP:
  frame.pc = static_cast<size_t>(i.i32);
  continue;
```
With:
```cpp
case Opcode::JMP:
  frame.pc += static_cast<size_t>(i.jumpOffset);
  continue;
```
Same pattern for `JMPIFFALSE`, `JMPIFTRUE`, `CONTINUE`, `BREAK`, `JE`, `JNE`, `JG`, `JGE`, `JL`, `JLE`.

### 4e. Error reporting: pc → instruction index

Add helper in `QLangVM`:
```cpp
size_t instructionIndexAt(const Function& fn, size_t pc) const;
```
Binary search `fn.instrOffsets` for the largest entry ≤ `pc`. Used in `vmThrowAt` to resolve line/col.

In `vmThrowAt`, replace:
```cpp
if (fr.pc < static_cast<int>(fn.loc.size()))
  fl = fn.loc[fr.pc].first;
```
With:
```cpp
size_t idx = instructionIndexAt(fn, fr.pc);
if (idx < fn.loc.size())
  fl = fn.loc[idx].first;
```

### 4f. `GeneratorState::pc`

Already `size_t`. No change. The saved/restored pc is a byte offset.

### 4g. `pushCall`

`cf.pc = 0;` — still valid (byte offset 0 = start of function code).

---

## Step 5 — `src/QLangBytecodeIO.cpp` + `include/qlang/QLangBytecodeIO.h`: v5 codec

### 5a. Version bump

In `include/qlang/QLangBytecodeIO.h`:
```cpp
constexpr uint32_t kQlcVersion = 5;
```

### 5b. Writer (`writeQlc`)

For each function, replace the per-instruction loop:
```cpp
putU32(os, static_cast<uint32_t>(f.instructions.size()));
for (const auto& ins : f.instructions)
  putInstruction(os, ins);
```
With:
```cpp
// Instruction byte stream
putU32(os, static_cast<uint32_t>(f.code.size()));
putBytes(os, f.code.data(), f.code.size());
// Instruction offsets
putU32(os, static_cast<uint32_t>(f.instrOffsets.size()));
for (size_t off : f.instrOffsets)
  putU32(os, static_cast<uint32_t>(off));
```

Keep the `loc` section and `constants` section unchanged.

Remove the `putInstruction` helper (no longer needed).

### 5c. Reader (`readQlc`)

For v5 files, replace the per-instruction read loop:
```cpp
uint32_t codeSize = r.u32();
fn.code.resize(codeSize);
for (uint32_t j = 0; j < codeSize; j++)
  fn.code[j] = r.u8();
uint32_t offCount = r.u32();
fn.instrOffsets.resize(offCount);
for (uint32_t j = 0; j < offCount; j++)
  fn.instrOffsets[j] = r.u32();
```

Validation after reading:
- `fn.loc.size() == fn.instrOffsets.size()`
- `fn.instrOffsets` is strictly increasing
- Decode every instruction to verify no truncation/invalid opcode
- Verify last instruction ends exactly at `fn.code.size()`

For v1–v4 files: keep the existing read path (fixed 8-byte instructions) and convert to the new format in-memory (decode each `Instruction` into bytes via `encodeInstruction`, build `instrOffsets`).

### 5d. Remove `putInstruction` and the old `Reader`-based instruction decode

The old `putInstruction` writes `u16 op, u16 a, i32 i32`. Remove it. The old reader loop that reads `u16 op, u16 a, i32 i32` per instruction is replaced by the flat blob read above.

---

## Step 6 — `examples/qlang_run.cpp`: Disassembler update

### 6a. `disassemble()`

Replace iteration over `fn.instructions` with iteration over `fn.instrOffsets`:
```cpp
for (size_t j = 0; j < fn.instrOffsets.size(); j++) {
  DecodedInstruction ins;
  decodeInstruction(fn.code.data(), fn.instrOffsets[j], fn.code.size(), ins);
  // ... print using ins.op, ins.a, ins.b, ins.c, ins.imm, ins.jumpOffset, ins.modifier
}
```

### 6b. `operandText()`

Rewrite to take `const DecodedInstruction&` instead of `const Instruction&`:
- For immediate ops: print `i.imm` as decimal.
- For jump ops: print `i.jumpOffset` as signed decimal + target byte offset.
- For register/const ops: print `i.a`, `i.b`, `i.c` as hex.
- If `i.modifier != 0`: prefix with `e=0xNN`.

### 6c. `operandMask()`

Remove. The new `operandText` handles all opcodes directly via `DecodedInstruction` fields.

### 6d. `pseudoText()`

Rewrite to take `const DecodedInstruction&`:
- `MOVI`: `r{a} = {imm}`
- `ADDI`: `r{a} = r{a} + {imm}`
- `SUBI`: `r{a} = r{a} - {imm}`
- `CMPI`: `cmpi r{a}, {imm}`
- Jumps: `pc += {jumpOffset}` (or `if ... pc += {jumpOffset}`)
- `MOVC`: `r{a} = pool[{b}]`
- All others: same as before but using `i.a`, `i.b`, `i.c`.

### 6e. `builtinName()`

Unchanged.

### 6f. Address column

Print `fn.instrOffsets[j]` as the byte offset (hex, 4 digits minimum) instead of the instruction index.

---

## Step 7 — Tests

### 7a. Update `tests/unit/gen_test.cpp`

No direct `Instruction` construction here; it uses `ctx.run(src)`. Should pass unchanged once the compiler/VM are updated.

### 7b. New test: `tests/unit/encoding_test.cpp`

Test `encodeInstruction` / `decodeInstruction` round-trip:
- Base `MOV 1, 2` → 3 bytes → decode → verify.
- Escaped `MOV 1, 300` (2-byte b) → 5 bytes → decode → verify.
- Base `MOVI 0, 42` → 3 bytes → decode → verify `imm == 42`.
- Escaped `MOVI 0, 200` (2-byte imm) → 5 bytes → decode → verify.
- Escaped `MOVI 0, 70000` (4-byte imm) → 7 bytes → decode → verify.
- Base `JMP` offset 5 → 2 bytes → decode → verify `jumpOffset == 5`.
- Escaped `JMP` offset 500 (jmp16) → 4 bytes → decode → verify.
- Escaped `JMP` offset 100000 (jmp32) → 6 bytes → decode → verify.
- Invalid: truncated buffer → throws.
- Invalid: reserved imm form 0 → throws.
- Invalid: reserved reg form 2 → throws.

### 7c. New script: `tests/scripts/ext_regs.ql`

Generate >256 simultaneously-live registers:
```ql
local r0 = 0
local r1 = 1
...
local r257 = 257
println(r0 + r257)
```
(258 `local` declarations in one scope.)

### 7d. New script: `tests/scripts/ext_consts.ql`

Use >256 distinct string constants:
```ql
local s0 = "c0"
local s1 = "c1"
...
local s257 = "c257"
println(s0)
println(s257)
```

### 7e. New script: `tests/scripts/jump_widths.ql`

Force all three jump widths:
- A tight loop (jmp8): `for i = 1 to 3 { }`
- A medium branch (jmp16): a function with ~200 instructions between if/else
- A far branch (jmp32): a function with >32767 bytes of code between branch and target

### 7f. Register in `CMakeLists.txt`

Add to `QL_SCRIPTS`:
```cmake
ext_regs.ql
ext_consts.ql
jump_widths.ql
```

Add new unit test:
```cmake
add_executable(encoding_test tests/unit/encoding_test.cpp)
target_link_libraries(encoding_test PRIVATE qlang)
add_test(NAME encoding COMMAND encoding_test)
```

---

## Step 8 — Verification gate

All of the following must pass:

```bash
cmake -B build && cmake --build build --config Release
ctest --test-dir build --output-on-failure
build/qlang_run --asm-level 5 --profile tests/scripts/hello.ql
build/qlang_run --output /tmp/t.qlc tests/scripts/hello.ql
build/qlang_run /tmp/t.qlc   # round-trip (if supported) or load via API
```

Specifically:
1. Every `tests/scripts/*.ql` produces byte-identical stdout to the pre-change build.
2. `.qlc` round-trip: write v5 → read → execute → stdout matches.
3. `ext_regs.ql` compiles and runs (exercises 2-byte register encoding).
4. `ext_consts.ql` compiles and round-trips (exercises 2-byte constant encoding).
5. `jump_widths.ql` runs correctly (exercises jmp8/jmp16/jmp32).
6. `--asm-level 0` through `5` produce well-formed listings.
7. `encoding_test` passes all round-trip and invalid-input cases.

---

## Invariants (completion criteria)

- No code path assumes fixed 8-byte instruction stride.
- `Function::loc.size() == Function::instrOffsets.size()` everywhere.
- Jump targets in encoded bytecode are relative offsets, not absolute addresses.
- Single register pool, single constant pool — no main/extended split.
- `examples/qlang_run.cpp` is updated in the same change.
- `.qlc` writer emits v5; reader accepts v1–v5, rejects v6+.
- `usesI32()` is removed.
- The `Instruction` struct (fixed 8-byte) no longer exists.
