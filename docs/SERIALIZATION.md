# QLang Serialization / Deserialization Spec

Status: **implemented** — `.qlc` program image (v1)
Covers: `.qlc` program image (v1) and state snapshot (v2, future)
Reference implementation: `src/QLangBytecodeIO.cpp` (library codec)
Architecture assessment of this change: [qlc-codec-assessment.md](qlc-codec-assessment.md)

## 1. Purpose

Two distinct artifacts, one codec family:

| Artifact | Magic | Content | Use |
|---|---|---|---|
| Program image | `QLC1` | compiled program (bytecode + constants + tables) | re-run a script without the source |
| State snapshot | `QLS1` | program image + live execution state | resume an interrupted run |

Both are little-endian binary. Both must be parseable by a deserializer that
treats the input as **untrusted** (see §8).

Out of scope: source-level caching, cross-version VM compatibility promises,
compression, signing.

## 2. Conventions

- All integers little-endian. `u8/u32/u64` unsigned, `i32/i64` two's-complement.
- `f64` is IEEE 754 double, little-endian.
- Strings: `u32 len` + `len` raw bytes, no terminator, no encoding guarantee
  (bytes are stored verbatim).
- Every count-prefixed section is independently bounds-checked on read.
- Byte offset in all error messages is 0-based from the start of the file.

## 3. Value encoding

Every `QLangValue` serializes as `u8 tag` + payload. Tags are the
`QLangValue::Type` enum values in declaration order and are **fixed**:

| Tag | Type    | Payload |
|-----|---------|---------|
| 0   | Nil     | — |
| 1   | Bool    | `i32` (0 or 1) |
| 2   | Char    | `i32` |
| 3   | Byte    | `u8` |
| 4   | Word    | `u32` (high 16 bits zero) |
| 5   | Short   | `i32` |
| 6   | Int     | `i32` |
| 7   | Unsigned/Dword| `u32` |
| 8   | Qword   | `u64` |
| 9   | String  | `u32 len` + `len` bytes |
| 10  | Blob    | `u32 len` + `len` bytes |
| 11  | Array   | `u32 count` + `count` × Value |
| 12  | Float   | `f64` |
| 13  | Table   | `u32 count` + `count` × (String key, Value) |
| 14  | Function| `i32 funcIndex` + `u32 capCount` + `capCount` × `i32 reg` |
| 15  | Generator| **not durable** — writer MUST emit tag 0 (Nil) |
| 16  | Object  | **not durable** — writer MUST emit tag 0 (Nil) |
| 17  | Eof     | — |
| 18  | Long    | `i64` |
| 19  | Set     | `u32 count` + `count` × Value |

Rules:

- **R1** New `QLangValue::Type` values are appended at the end of the enum and
  table; existing tags never change meaning.
- **R2** A value of a non-durable type (Generator, Object) is lossy by
  design: it round-trips as Nil. The writer MUST NOT emit tags 15/16; a
  reader MUST reject a file carrying them.
- **R3** Table keys are stored in `std::map` (lexicographic) order. Readers
  MUST NOT depend on entry order.
- **R4** For tag 14, `funcIndex` is an index into the program's function
  array (§4). Capture frame pointers are runtime-only; only register indices
  are stored. A reader reconstructing a closure starts with null frame
  pointers (valid only for programs where captures are re-bound at runtime).

## 4. Program image (magic `QLC1`, version 1)

```
offset  field            type   notes
──────  ───────────────  ─────  ──────────────────────────────────────────
0       magic            4s     "QLC1"
4       version          u32    = 1
8       funcCount        u32
…       functions        funcCount × Function
        nativeCount      u32
…       natives          nativeCount × NativeSlot
        varCount         u32
…       vars             varCount × VarEntry
        constVarCount    u32
…       constVars        constVarCount × String
        registerCount    i32
```

### 4.1 Function record

```
u32   nameLen, name bytes
i32   paramCount
i32   registerCount
u8    isGenerator          (0/1)
u32   capCount, capCount × i32 reg
u32   instrCount
      instrCount × Instruction      // raw 12-byte records
u32   constCount, constCount × Value
```

`Instruction` is written as its raw 12 bytes (little-endian, no padding —
`static_assert(sizeof(Instruction) == 12)`):

```
u16   op
i16   a
i32   i32               // combined b/c; jump-family opcodes carry the
                        // 32-bit pc target here (JMP/CONTINUE/BREAK;
                        // JMPIFFALSE/JMPIFTRUE keep the condition in `a`)
u16   line
u16   col
```

- `functions[0]` is the implicit `main`. The writer MUST emit it first; the
  reader MUST reject files where `functions[0].name != "main"` or
  `funcCount < 1`.
- `op` MUST be `< kOpcodeCount`.
- `line`/`col` are 0-based source positions (0,0 when unknown).

### 4.2 NativeSlot

```
i32   slot          index into CompiledProgram::nativeFuncs
u32   nameLen, name bytes
```

Native function **pointers are not stored** — only the name per slot. This
table is what makes the image loadable: bytecode references natives by slot
(`index = -1 - opcode_b`), and without names the slots are meaningless.

- Slots are written in ascending order; duplicates are a format error.
- Unbound (null) slots are omitted; the loader pads `nativeFuncs` with null.
- `name` MUST be non-empty.

### 4.3 VarEntry / constVars

```
VarEntry:  u32 nameLen, name bytes, i32 reg
constVars: u32 nameLen, name bytes
```

Backing containers are `std::unordered_map` / `std::unordered_set`: **order
within both sections is unspecified**. Readers MUST NOT depend on it.

## 5. Load contract (deserializer)

### 5.1 Validation sequence

1. At least 12 bytes present; magic == `QLC1`; `version <= 1` (higher →
   "unsupported version", never a parse attempt).
2. Every read is bounds-checked. Truncation → `QLangError(Io)` with the byte
   offset where the read failed.
3. Structural invariants: `funcCount >= 1`, `functions[0].name == "main"`,
   all `op < kOpcodeCount`, all closure `funcIndex` in `[0, funcCount)`,
   native slots unique and `>= 0`, all `registerCount >= 0`, all
   `paramCount >= 0`.
4. Value decoding enforces §8 limits (depth, sizes).

Any violation produces one structured error; the reader never partially
returns a corrupted `CompiledProgram` (out-parameter is left untouched).

### 5.2 Native binding

Binding is the loader's responsibility, in two steps:

1. **File read** (`readQlc`) fills `CompiledProgram` including the native
   *name* table (a side output, e.g. `std::vector<std::pair<int,std::string>>`).
2. **Context bind** (`QLangContext::loadProgram`) resolves each name against
   the context's registered natives:
   - all missing names are collected and reported in a **single** error
     (`missing native function(s): 'a', 'b'`) — not fail-on-first;
   - on success, `compiled.nativeFuncs[slot] = func` for each bound slot.

A context lacking a native the program calls is a load error, never a
mid-execution crash.

### 5.3 Errors

`QLangErrorKind` gains a new value **`Io`** (appended; existing values keep
their numeric positions). All codec failures throw / report
`QLangError(QLangErrorKind::Io, 0, 0, "qlc: <reason> (byte offset N)")`.
Line/col are 0 — these are not source positions.

## 6. Library API

```cpp
// include/qlang/QLangBytecodeIO.h
namespace qlang {

// Value codec — public so snapshot code (v2) reuses it.
void serializeValue(std::ostream &os, const QLangValue &v);
QLangValue deserializeValue(std::istream &is);  // throws QLangError(Io)

// Program codec.
bool writeQlc(std::ostream &os, const CompiledProgram &prog,
              const std::map<std::string, int> &nativeIds); // name -> id (slot = -1 - id)
bool readQlc(std::istream &is, CompiledProgram &prog,
             std::vector<std::pair<int, std::string>> &nativeSlots,
             std::string &err);

} // namespace qlang
```

```cpp
// QLangContext additions
void loadProgram(CompiledProgram &&prog);  // bind natives (validate), cache in pimpl
void execute();                            // VM over the cached program (top level)
```

`loadProgram` is the public inverse of the compile half of `run()`: it
performs the native wiring `run()` does today and stores the program in
`m_pimpl->compiled`. `execute()` runs `QLangVM(compiled, *this).run()`.
Existing `run()` and `call()` are unaffected.

## 7. State snapshot (magic `QLS1`) — v2, not implemented

File layout: the full `QLC1` program sections (§4) followed by:

```
32      programHash   SHA-256 over all preceding bytes
u64     steps         VM step counter
u32     deletedCount  + deletedCount × i32 reg
u32     frameCount
…       frames        frameCount × FrameRecord
u32     genCount
…       generators    genCount × (i32 funcIndex + FrameRecord)
u32     varCount
…       ctxVars       varCount × (String name + Value)   // context m_vars
```

FrameRecord:

```
i32   funcIndex
u64   pc
i32   callDest          (-1 for the main frame)
u8    exhausted         (generator flag)
u32   regCount + regCount × Value     // the frame's register file
u32   capCount + capCount × (i32 frameSlot, i32 reg)
```

Rules:

- **S1** `frameSlot` is a 0-based index into the frame array in file order.
  The deserializer materializes frames first, then patches
  `CaptureRef.frame` to the real `Frame*`.
- **S2** The snapshot is only valid for the embedded program. The
  `programHash` guards against tampering and against restoring a snapshot
  against a foreign program image (future API).
- **S3** Context variables (`m_vars`) carry **values**, unlike the program
  image which carries only the name→register declaration map.
- **S4** Because `Frame` is private to `QLangVM`, the snapshot codec lives in
  the VM, not in `QLangBytecodeIO`:

```cpp
// QLangVM additions
void snapshot(std::ostream &os) const;          // appends the state section
static QLangVM restore(std::istream &is, QLangContext &scope); // reads program+state
```

## 8. Robustness (untrusted input)

The deserializer MUST enforce:

| Limit | Value | Rationale |
|---|---|---|
| Value nesting depth | 64 | recursion bomb / stack exhaustion |
| Max String/Blob len | remaining bytes (inherent) + 4 GiB (u32) | no separate cap needed |
| Total decoded values | 64 Mi | memory bomb |
| Total instruction count | 64 Mi | memory bomb |
| Any `i32` used as a count | `>= 0` | negative-size rejection |

All limits are compile-time constants in the codec, overridable per call for
tests. Hitting any limit → `QLangError(Io)`, no partial state.

## 9. Test requirements

A codec is not "done" until all of these pass:

1. **Value round-trip**: every durable type, plus nested arrays/tables
   (depth ≥ 60), closures, empty containers → write/read → structural
   equality.
2. **Program round-trip**: for a corpus of `.ql` scripts — compile →
   `writeQlc` → `readQlc` → `loadProgram` → `execute` — stdout must be
   byte-identical to direct `ctx.run(source)`.
3. **Malformed-input suite** (each case → clean `Io` error, no crash, no
   partial out-param): truncated at every section boundary, bad magic,
   version 2, `op >= kOpcodeCount`, dangling closure index, duplicate native
   slot, negative count, 100-deep nested array, 70 Mi value count.
4. **Native mismatch**: program calling an unregistered native → single
   error naming all missing natives.
5. **(v2) Snapshot/resume**: run a script to a checkpoint, snapshot, restore
   in a fresh context, continue → final stdout equals uninterrupted run.

## 10. Implementation status

| Item | Status |
|---|---|
| Value codec (`serializeValue`/`deserializeValue`) | done — matches §3 |
| Program writer (`writeQlc`, §4 incl. native-slot table) | done |
| `QLangErrorKind::Io` | done |
| Reader (`readQlc`, §5.1 invariants) | done |
| App writer (POC `QlcWriter` in `qlang_run.cpp`) | deleted — replaced by `writeQlc` |
| Tests (`codec_check`: §9 items 1, 3) | done |
| `QLangContext::loadProgram` / `execute` (§5.2, §9 item 2/4) | not started |
| Snapshot (v2) | not started |

§9 item 2 (execute-from-image parity) and item 4 (native-mismatch report)
require `loadProgram`/`execute` and are pending with them.
