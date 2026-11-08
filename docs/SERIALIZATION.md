# QLang Serialization / Deserialization Spec

Status: **implemented** — `.qlc` program image (v6)
Covers: `.qlc` program image (v6) and state snapshot (v2, future)
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
| 20  | External| **not durable** — writer MUST emit tag 0 (Nil) |
| 21  | BoundMethod | **not durable** — writer MUST emit tag 0 (Nil) |
| 22  | TypeRef | **not durable** — writer MUST emit tag 0 (Nil) |

Rules:

- **R1** New `QLangValue::Type` values are appended at the end of the enum and
  table; existing tags never change meaning.
- **R2** A value of a non-durable type (Generator, Object, External,
  BoundMethod, TypeRef) cannot be durably encoded. At the **program** level
  `writeQlc` rejects a program whose constant pool holds one (returns
  `false`) rather than silently remapping it to Nil. At the **value** level
  `serializeValue` is lossy by design and remaps such a value to tag 0 (Nil).
  The writer MUST NOT emit tags 15, 16, 20, 21, 22; a reader MUST reject a
  file carrying them with a clear error (never a silent Nil). (Tags 17–19 —
  Eof, Long, Set — are durable and round-trip with their payloads.)
- **R3** Table keys are stored in `std::map` (lexicographic) order. Readers
  MUST NOT depend on entry order.
- **R4** For tag 14, `funcIndex` is an index into the program's function
  array (§4). Capture frame pointers are runtime-only; only register indices
  are stored. **Load-time rule:** a reader reconstructing a closure starts
  with null frame pointers; the loaded closure is only usable in a context
  that re-binds those captures to live frames at load/first-call time. A
  reader that cannot re-bind captures MUST treat a closure with a non-empty
  capture list as unloadable.
- **R5** The durable form stores **payloads**, never handles. In memory
  `Blob`, `Array` and `Set` are reference-counted payloads
  (`shared_ptr<vector<uint8_t>>` / `shared_ptr<vector<QLangValue>>`) so they
  share on assignment and on call (SPEC §types "value semantics"); on disk the
  writer dereferences and emits the bytes / element list exactly as before.
  The pointerization is therefore invisible to this format: tags 10, 11 and 19
  and the program image are unchanged by it, and files written before it still
  load (until a version bump such as v5 → v6 retires them). A reader always
  materialises **fresh** payloads — a decoded value
  never aliases another value's storage.

## 4. Program image (magic `QLC1`, version 6)

```
offset  field            type   notes
──────  ───────────────  ─────  ──────────────────────────────────────────
0       magic            4s     "QLC1"
4       version          u32    = 6  (kQlcVersion; readers accept 6 only)
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
u32   codeSize
      codeSize bytes        // raw escape-byte instruction stream
u32   offCount
      offCount × u32 offset // byte offset of each instruction (strictly increasing)
u32   locCount, locCount × (u16 line, u16 col)
u32   constCount, constCount × Value
```

The instruction stream is the **v6 escape-byte format** (see
`include/qlang/QLangBytecode.h`, §4.4): a flat byte stream where each instruction
starts with an opcode byte whose bit 7 is the escape flag and bits 0–6 are
the opcode identity (128 base identities, 79 defined). An escaped
instruction carries a modifier byte `e` immediately after the opcode byte
that states the operand layout — left-operand width, right-operand **kind**
(immediate / register / const pool) and right-operand **form** — or, for the
jump class, the offset width. The stream is stored **verbatim** — the raw
bytes are the on-disk encoding.

The offset table (`offCount` × `u32`) gives the byte offset of every
instruction in the stream. It is strictly increasing, and the last
instruction must end exactly at `codeSize`. The reader decodes every
instruction to validate the stream (catching truncation, invalid escape
modifiers, reserved kinds and forms, and unknown opcode identities).

The source location is a per-function side table (`loc`), parallel to the
offset table: `locCount` entries of `(u16 line, u16 col)` with
`locCount == offCount` (a `(0,0)` entry means "no source location").

- `functions[0]` is the implicit `main`. The writer MUST emit it first; the
  reader MUST reject files where `functions[0].name != "main"` or
  `funcCount < 1`.
- Every decoded opcode identity MUST be `< kOpcodeUsedCount` (79).
- `line`/`col` in the loc side table are 0-based source positions (0,0 when
  unknown).

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

### 4.4 Instruction stream (v6)

Each instruction is `[opcode byte] [modifier byte] [operands…]`. In the opcode
byte **bit 7 is the escape flag** and bits 0–6 are the opcode identity
(128 identities, `kOpcodeUsedCount = 79` of them defined). An unescaped
instruction uses the identity's **base form** (fixed operand widths, right
operand kind fixed by the identity); an escaped instruction states its layout
in the modifier byte `e` that follows the opcode byte.

`e` is split **2 / 2 / 4**:

| bits | field | meaning |
|---|---|---|
| 0–1 | left width | addressing width of the left operand (always a register) |
| 2–3 | right kind | what the right operand *is* |
| 4–7 | right form | interpreted by the kind |

Addressing widths — used by bits 0–1, and by bits 4–7 when the kind is a
register or a constant-pool index:

| value | width | bytes | range |
|---|---|---|---|
| 0 | Byte | 1 | 0–255 |
| 1 | Word | 2 | 0–65 535 |
| 2 | DWord | 4 | 0–2³¹−1 |
| 3 | Reserved | — | decode error |

Right kind (bits 2–3):

| value | kind | bits 4–7 then mean |
|---|---|---|
| 0 | Immediate | an `ImmForm` (below) |
| 1 | Register | an addressing width (above) |
| 2 | Const pool | an addressing width (above) |
| 3 | Reserved | decode error |

Immediate forms (kind 0 only):

| form | bytes | QLang type | form | bytes | QLang type |
|---|---|---|---|---|---|
| 0 `U8` | 1 | Unsigned | 6 `I32` | 4 | Int |
| 1 `U16` | 2 | Unsigned | 7 `I64` | 8 | Long |
| 2 `U32` | 4 | Unsigned | 8 `Float` | 8 | Float |
| 3 `U64` | 8 | Qword | 9 `False` | 0 | Bool |
| 4 `I8` | 1 | Int | 10 `True` | 0 | Bool |
| 5 `I16` | 2 | Int | 11 `Nil` | 0 | nil |

Forms 12–15 are reserved → decode error. The `False`/`True`/`Nil` forms carry
**no bytes** — the form *is* the value — which is why bool, nil and float
literals no longer need a constant-pool entry.

Every base form is equivalent to one escaped instruction, so a reader may treat
both uniformly:

| base form of the identity | equivalent `e` |
|---|---|
| register right (`mov r3, r5`) | `0x04` |
| immediate right (`movi r3, 5`) | `0x40` |
| const-pool right (`movc r3, pool[5]`) | `0x08` |
| jump class, zero-operand opcodes | none — `kNoBaseModifier` (`0xFF`) |

- **Jump class**: the modifier byte is an **offset-width selector**, not a
  2/2/4 layout — base form = 1-byte offset, `e = 0` = 2 bytes, `e = 1` = 4
  bytes. The deprecated conditional jumps carry a 1-byte condition register
  before the offset.
- **`CALL`** has no base form: it is always escaped with `e = 0x50` (kind 0,
  form 5 = 2-byte signed function id), followed by the 1-byte argument count.
  An unescaped `CALL` is rejected by both writer and reader.
- **Arity rule**: for the binary ALU and flag-compare families a right operand
  that is not a register means the in-place 2-operand form
  (`r[a] = r[a] op right`), so the trailing 1-byte `c` is absent. Every other
  3-operand opcode keeps it.
- **Widest instruction: 15 bytes** (`op` + `e` = 2, left = 4, right = 8, third
  = 1 — e.g. `CONV`); the reader's `codeSize` guard is derived from it.

## 5. Load contract (deserializer)

### 5.1 Validation sequence

1. At least 12 bytes present; magic == `QLC1`; `version == 6`
   (`kQlcVersion`; anything else → "unsupported version", never a parse
   attempt). Legacy v1–v5 files (fixed 8-byte instructions through v4, the
   two-nibble modifier byte in v5) are NOT accepted.
2. Every read is bounds-checked. Truncation → `QLangError(Io)` with the byte
   offset where the read failed.
3. Structural invariants: `funcCount >= 1`, `functions[0].name == "main"`,
   `codeSize <= 64 Mi × 15` (§8 instruction cap × the widest v6 instruction),
   every instruction decodes and its opcode identity is `< kOpcodeUsedCount`,
   offsets strictly increasing with the last instruction ending exactly at
   `codeSize`, `locCount == offCount`, all closure `funcIndex` in
   `[0, funcCount)`, native slots unique and `>= 0`, all `registerCount >= 0`,
   all `paramCount >= 0`. Decoding is what enforces the modifier rules of
   §4.4: reserved left width (3), reserved right kind (3), reserved immediate
   forms (12–15), a kind the opcode does not permit, and an unescaped `CALL`.
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
   version 5 (below `kQlcVersion`) and version 7 (above), opcode identity
   `>= kOpcodeUsedCount`, reserved modifier fields (left width 3, kind 3,
   immediate forms 12–15, unpermitted kind, unescaped `CALL`), truncated
   instruction, dangling closure index, duplicate native slot, negative count,
   100-deep nested array, 70 Mi value count.
4. **Native mismatch**: program calling an unregistered native → single
   error naming all missing natives.
5. **(v2) Snapshot/resume**: run a script to a checkpoint, snapshot, restore
   in a fresh context, continue → final stdout equals uninterrupted run.

## 10. Implementation status

| Item | Status |
|---|---|
| Value codec (`serializeValue`/`deserializeValue`) | done — matches §3 |
| Program writer (`writeQlc`, §4 incl. native-slot table) | done — v6, explicit little-endian, escape-byte code stream + offset table, loc side table |
| `QLangErrorKind::Io` | done |
| Reader (`readQlc`, §5.1 invariants) | done — explicit little-endian reads, accepts v6 only |
| App writer (POC `QlcWriter` in `qlang_run.cpp`) | deleted — replaced by `writeQlc` |
| Tests (`codec_check`: §9 items 1, 3) | done |
| `QLangContext::loadProgram` / `execute` (§5.2, §9 item 2/4) | not started |
| Snapshot (v2) | not started |

§9 item 2 (execute-from-image parity) and item 4 (native-mismatch report)
require `loadProgram`/`execute` and are pending with them.
