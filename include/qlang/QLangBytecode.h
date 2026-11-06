#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "QLangValue.h"

namespace qlang {

class QLangContext; // forward declaration for CompiledProgram::nativeFuncs

// Canonical serialized key of a value, used as the constant-pool dedup index
// (SPEC §bytecodes "constant pool": unique values, duplicates folded).
// Value types key structurally (recursively); Function keys on its compile
// time spec (funcIndex + capture regs — cells bind at MAKEFUNC time); the
// remaining reference types (Generator, Object, External, BoundMethod,
// TypeRef) key by identity so distinct objects never collide.
std::string valueKey(const QLangValue &v);

// Built-in function ids shared by the compiler (emits CALL with one of these)
// and the VM (dispatches on them). Order is stable and used as the operand.
enum class Builtin : int {
  ToStr = 0,
  ToInt,
  Type,
  Print,
  Println,
  Printf,
  Len,   // len(var) -> string/array/table length; other -> 1
  Blob,  // blob() -> empty binary blob
  Substr, // substr(str, start, len) -> substring
  Slice,   // slice(array, start, len) -> new array
  Range,   // range(start, end) -> [start, start+1, ..., end]
  Join,      // join(separator, array) -> string
  IsSet,     // isset(var_name) -> bool (checks scope existence)
  Sprintf,   // sprintf(fmt, ...) -> string (same format as printf, returns result)
  Format,    // format(template, ...) -> string (f-string interpolation)
  Assert,    // assert(bool, str) -> void (throws if bool is false)
  Next,      // next(gen) -> value (resume generator, returns yielded value or nil)
  SetMaxSteps, // set_max_steps(n) -> void (set the VM's infinite-loop step limit)
  GetMaxSteps, // get_max_steps() -> int (current infinite-loop step limit)
  Set,         // set(...) -> set (unique-value collection constructor)
  Release,     // release(x) -> nil (drop an owned external's shared_ptr)
  Deepcopy     // deepcopy(x) -> independent recursive copy (SPEC §functions "deepcopy")
};

// Register-machine opcodes. This is a small register VM: each script variable
// maps to a register, and arithmetic/comparison results are written to a
// register. Constants live in a separate pool and are loaded with MOVC.
enum class Opcode : uint8_t {
  // Data movement
  MOVC, // r[a] = constants[b]
  MOV,   // r[a] = r[b]
  CONV,  // r[a] = convert(r[b], type c)  -- enforce a typed declaration
  NEG,   // r[a] = -r[b]
  INC,   // r[a] += 1
  DEC,   // r[a] -= 1

  // Arithmetic
  ADD, // r[a] = r[b] + r[c]
  SUB,
  MUL,
  DIV,
  MOD, // r[a] = r[b] % r[c]  (modulo; integer or float)

  // Bitwise / shifts
  AND, // r[a] = r[b] & r[c]
  OR,
  XOR,
  SHL, // r[a] = r[b] << r[c]
  SHR, // r[a] = r[b] >> r[c]

  // Logical (combine truthiness; non-short-circuit)
  LAND, // r[a] = truth(r[b]) && truth(r[c])
  LOR,  // r[a] = truth(r[b]) || truth(r[c])
  NOT,  // r[a] = !truth(r[b])  (logical not)

  // Comparison -> boolean (1/0 stored as Int)
  EQ, // r[a] = (r[b] == r[c])
  NE,
  LT,
  GT,
  LE,
  GE,
  POW,       // r[a] = pow(r[b], r[c])  (exponential)
  INx,  // r[a] = (r[b] in r[c])  membership: array element / table key
        // (named INx, not IN: `IN` is a Windows SDK macro from rpcndr.h,
        // which breaks MSVC TUs that include <windows.h> first)

  // Built-in function call. r[a] = call(funcId=b, args in r[a+1 .. a+c]).
  CALL, // r[a] = builtin(b, r[a+1 .. a+c])

  // Variable removal. r[a] is dropped from the scope at end-of-run.
  DELETEx, // remove script variable mapped to register a
           // (named DELETEx, not DELETE: keeps the opcode list clear of
           // Windows/C++ reserved-word spellings for MSVC portability)

  // Runtime cast: strict type conversion (throws on invalid cast).
  CAST, // r[a] = strict_convert(r[b], type c)

  // Array operations (register holds an ArrayValue).
  ARRGET, // r[a] = arrays[b][c]               (subscript read)
  ARRPUT, // arrays[a][b] = c                   (subscript write)
  ARRAPP, // arrays[a].push_back(b)             (append single element)
  ARRAPPV, // arrays[a] += arrays[b]            (extend with another array)
  ARRLEN, // r[a] = arrays[b].size()            (length)
  LEN,    // r[a] = length(r[b])               — string/array/table/blob
  SEQGET, // r[a] = seqGet(collect[b], c)      — array[c] or the c-th table key

  // Table operations (register holds a TableValue).
  TABCREATE, // r[a] = new empty table
  TABGET, // r[a] = tables[b][c]                (member read)
  TABPUT, // tables[a][b] = c                   (member write)
  // Attach a finalizer to a class instance (Phase 7): if the table carries a
  // `__destroy__` method, re-wrap it so the method runs exactly once at final
  // release (no-op otherwise).
  TABFINALIZE, // finalize(r[a]) — install `__destroy__` deleter if present

  // Indirect access via $$ operator (read/write through dynamic key)
  INDGET, // r[a] = tables[b][c] via $$ (indirect read)
  INDPUT, // tables[a][b] = c via $$ (indirect write)

  // Dynamic key resolution via $ operator (evaluate identifier and stringify)
  EVALKEY, // r[a] = str(r[b]) - evaluate variable and convert to string

  // Control flow
  JMP,       // pc = a
  JMPIFFALSE, // if !truth(r[a]) pc = b
  JMPIFTRUE,  // if truth(r[a]) pc = b
  CONTINUE,  // pc = loop test label (next iteration)
  BREAK,     // pc = loop exit label

  // User-defined function call. r[a] = call function b, with arguments in
  // r[a+1 .. a+c] (c == paramCount). The callee runs in a fresh frame; its
  // return register (0) becomes the result stored in r[a].
  CALLFN, // r[a] = userfn(b, r[a+1 .. a+c])

  // Call a closure value held in r[a]; its funcIndex is read at runtime.
  // Arguments are r[a+1 .. a+c]. The result is written back into r[a].
  CALLCLOS, // r[a] = closure(r[a])(r[a+1 .. a+c])

  // Return from the current function: the value in r[a] is the result.
  RETURN, // return r[a]

  // Construct an arrow closure in r[a] from the constant at index b. The
  // constant holds the closure's function index and the enclosing-frame
  // register indices of its explicit capture list; the closure binds those
  // registers by reference at construction time.
  MAKEFUNC, // r[a] = closure(const[b])

  // Construct a generator object in r[a] from the function constant at index b.
  // The constant must hold a Function value; the VM wraps it in a GeneratorValue
  // that can be resumed via CALLCLOS until exhaustion.
  MAKEGEN, // r[a] = generator(const[b])

  // Load a captured-by-reference variable into r[a] via the closure capture
  // table entry at index b (live: reads the current value of the outer reg).
  REFLOAD, // r[a] = captures[b].frame->regs[captures[b].reg]

  // Store r[a] through the closure capture table entry at index b, writing the
  // enclosing frame's register (reference semantics / write-back).
  REFSTORE, // captures[b].frame->regs[captures[b].reg] = r[a]

  // Generator yield. Returns the value in r[b] to the caller; pauses
  // execution. r[c] is the generator register (used for self-reference).
  YIELD, // r[a] = generator(r[b]); r[c] = generator register

  // Load a 32-bit integer literal straight into a register, skipping the
  // constant pool. r[a] = Int(i32). Type::Int only: the value tag must match
  // the pool path (a Bool literal is Bool-typed, not Int). Appended last so
  // existing .qlc opcode numbers stay valid.
  MOVI, // r[a] = i32 immediate

  // In-place addition of a 32-bit integer immediate: r[a] = r[a] + Int(i32).
  // Same operand layout as MOVI (dest register in a, literal in i32; b/c
  // unused). Appended last so existing .qlc opcode numbers stay valid.
  ADDI, // r[a] = r[a] + i32 immediate

  // In-place subtraction of a 32-bit integer immediate: r[a] = r[a] - Int(i32).
  // Same operand layout as ADDI/MOVI (dest register in a, literal in i32; b/c
  // unused). Appended last so existing .qlc opcode numbers stay valid.
  SUBI, // r[a] = r[a] - i32 immediate

  // Comparison: set the frame's shared flags (GR/EQ/LT) from two Int values,
  // wiping any prior flags. CMP compares two registers (a, b); CMPI compares
  // a register (a) against a 32-bit Int immediate (i32). Int operands only.
  CMP,  // flags = cmp(regs[a], regs[b])   (a, b are registers)
  CMPI, // flags = cmp(regs[a], Int(i32))  (a is a register, i32 the literal)

  // Conditional jumps on the flags set by CMP/CMPI. The pc target is the
  // 32-bit i32 slot (like JMP). Each tests a distinct flag pattern:
  //   JG  gr             (a > b)     JGE  gr or eq (a >= b)
  //   JL  lt             (a < b)     JLE  lt or eq (a <= b)
  //   JE  eq             (a == b)    JNE  eq not set (a != b)
  JE,   // if flags == EQ            pc = i32
  JNE,  // if !(flags & EQ)          pc = i32
  JG,   // if flags == GR            pc = i32
  JGE,  // if flags == (GR|EQ)       pc = i32
  JL,   // if flags == LT            pc = i32
  JLE,  // if flags == (LT|EQ)       pc = i32

  // No-op: takes no action (SPEC §bytecodes "OpCode nop"). Zero operands.
  // Appended last so existing .qlc opcode numbers stay valid.
  Nop,

  // Emit a signal on an external object: deliver r[a+1..a+c] to every
  // connection of the signal named const[b] on the object in r[a]. Same
  // operand convention as CALL (dest in a, arg count in c, args in
  // r[a+1..a+c]). Appended last so existing .qlc opcode numbers stay valid.
  EMIT, // deliver signal const[b] on r[a] with c args in r[a+1..a+c]

  // Deep copy of a register value: r[a] = deep copy of r[b]. Two register
  // operands, no i32 slot (same shape as MOV). The compiler lowers a
  // `deepcopy(x)` call directly to this opcode (no CALL, no arg staging).
  // Copy semantics: SPEC §functions "deepcopy"; plan docs/plans/deepcopy-plan.md.
  // Appended last so existing .qlc opcode numbers stay valid.
  DEEPCOPY, // r[a] = deepcopy(r[b])

  // Polymorphic subscript: the base register is dispatched at runtime, so the
  // same opcode reads/writes an array element (integer index) or a table /
  // class-instance / external-object field (key stringified, resolved through
  // ObjectOps). This is what makes t["age"] and t.age the same operation
  // (SPEC §declarations). Three register operands, no i32 slot. The compiler
  // emits these for every postfix [...] read/write, replacing ARRGET/ARRPUT.
  // Distinct from the reserved INDGET/INDPUT ($$ indirect access) placeholders.
  // Appended last so existing .qlc opcode numbers stay valid.
  INDEXGET, // r[a] = r[b][r[c]]
  INDEXPUT  // r[a][r[b]] = r[c]
};

inline constexpr uint8_t kOpcodeEscape = 0x80;
inline constexpr uint8_t kOpcodeMax = 128; // identities 0..127
inline constexpr uint8_t kOpcodeUsedCount = 73; // defined identities 0..72

// ---------------------------------------------------------------------------
// `.qlc` v6 modifier byte (docs/plans/encoding-v6-and-indirect-plan.md §2,
// docs/spec/13-bytecodes.md `### bytecode encoding`).
//
// v5 reads the modifier byte as two nibbles (low = left-operand form, high =
// right-operand form). v6 re-splits the same byte into three fields:
//
//   bit 0-1  left operand (always a register): addressing width
//   bit 2-3  right-operand *kind*: immediate / register / const pool
//   bit 4-7  right-operand *form*: interpreted by the kind
//
// The kind field is what folds the `i`/`c` suffix opcodes (movi, movc, addi,
// subi, cmpi) into the unsuffixed opcodes. The tables and the `*V6` entry
// points below are the v6 encoding; the v5 codec (`instructionSize`,
// `encodeInstruction`, `decodeInstruction`) stays in force until the compiler,
// VM, codec and disassembler switch over together (plan §6 phase 1), so both
// exist side by side during the migration.
// ---------------------------------------------------------------------------

// Modifier bits 2-3: what the right operand *is*.
enum class RightKind : uint8_t {
  Immediate = 0, // inline literal; width and type selected by the form
  Register = 1,  // another register (today's base-form behaviour)
  ConstPool = 2, // a constant-pool index
  Reserved = 3   // decode error (plan §2.2, decision D7)
};

// Addressing width of the left operand (bits 0-1); a kind 1 / kind 2 right
// operand (bits 4-7) uses the same values 0..2.
enum class OperandWidth : uint8_t {
  Byte = 0,    // index 0-255
  Word = 1,    // extended index, 0-65 535
  DWord = 2,   // extended index (spec: "future version"; decoded here)
  Reserved = 3 // decode error
};

// Kind 0 (immediate) forms, modifier bits 4-7 (plan §2.3).
enum class ImmForm : uint8_t {
  U8 = 0,    U16 = 1,    U32 = 2,    U64 = 3, // unsigned: Unsigned / Qword
  I8 = 4,    I16 = 5,    I32 = 6,    I64 = 7, // signed: Int (1/2/4 B) / Long
  Float = 8, // IEEE-754 double
  False = 9, // zero-byte: the form *is* the value
  True = 10, // zero-byte
  Nil = 11,  // zero-byte
  Reserved = 12 // forms 12-15 are reserved (decision D7)
};

// QLang type an immediate form materializes.
enum class ImmType : uint8_t { Unsigned, Qword, Int, Long, Float, Bool, Nil };

// Which payload field of a decoded instruction carries its immediate.
enum class ImmPayload : uint8_t {
  None,     // no immediate right operand
  Signed,   // `imm` (sign-extended)
  Unsigned, // `immUnsigned`
  Float,    // `immFloat`
  True,     // zero-byte literal
  False,
  Nil
};

inline constexpr uint8_t kModifierLeftShift = 0;
inline constexpr uint8_t kModifierKindShift = 2;
inline constexpr uint8_t kModifierFormShift = 4;
inline constexpr uint8_t kModifierLeftMask = 0x03;
inline constexpr uint8_t kModifierKindMask = 0x0C;
inline constexpr uint8_t kModifierFormMask = 0xF0;

// `baseModifierV6` sentinel: the jump class does not use the 2/2/4 layout (its
// modifier byte is an offset-width selector) and a zero-operand instruction has
// no modifier at all.
inline constexpr uint8_t kNoBaseModifier = 0xFF;

// Assemble / read the three fields of a v6 modifier byte. `makeModifierV6`
// throws when a field value is reserved or the form is invalid for the kind.
uint8_t makeModifierV6(OperandWidth left, RightKind kind, uint8_t form);
OperandWidth modifierLeftForm(uint8_t e);
RightKind modifierRightKind(uint8_t e);
uint8_t modifierRightForm(uint8_t e);

// Operand bytes selected by a field value; throws on a reserved value.
size_t operandWidthBytes(OperandWidth w); // 1, 2 or 4
size_t immediateFormBytes(uint8_t form);  // 0 for False / True / Nil
ImmType immediateFormType(uint8_t form);

// The right-operand kind an opcode's base (unescaped) form means. The kind is
// fixed by the opcode identity because a base form carries no modifier byte to
// state it in: `mov r3, r5`, `mov r3, 5` and `mov r3, pool[5]` would otherwise
// be byte-identical (plan §2.4).
RightKind defaultRightKind(Opcode op);

// The modifier byte a base form is equivalent to (spec `### bytecode encoding`:
// register-right 0x04, immediate-right 0x40, const-right 0x08), or
// kNoBaseModifier for the jump class and zero-operand opcodes. CALL has no base
// form; it returns the modifier CALL always carries (kind 0, form 5).
uint8_t baseModifierV6(Opcode op);

// Bitmask of the right-operand kinds an opcode accepts (plan §2.5): bit 0 =
// immediate, bit 1 = register, bit 2 = const pool. A modifier naming a kind
// outside this mask is a decode error, so the reader rejects the file.
uint8_t permittedKinds(Opcode op);
bool kindPermitted(Opcode op, RightKind kind);

// Arity rule (plan §2.6): for the binary ALU and flag-compare families a right
// operand that is not a register means the in-place 2-operand form
// r[a] = r[a] op right (today's addi/subi semantics), so the trailing 1-byte
// `c` is absent. Every other 3-operand opcode keeps it (makegen's argument
// count, conv/cast's type id, seqget's index, ...).
bool isInPlaceForm(Opcode op, RightKind kind);

struct DecodedInstruction {
  Opcode op = Opcode::MOVC;
  bool escaped = false;   // true when the opcode byte carried the escape bit
  uint8_t modifier = 0;   // raw modifier byte; meaningful only when escaped
  // v6: what the right operand is. For a base form this is `defaultRightKind`
  // of the opcode; for an escaped form it is the modifier's kind field.
  RightKind rightKind = RightKind::Register;
  // v6: which payload field carries an immediate right operand.
  ImmPayload immPayload = ImmPayload::None;
  int32_t a = 0;          // left operand (register or constant index)
  int32_t b = 0;          // right operand (register, constant, or unused)
  int32_t c = 0;          // third operand (register; 3-operand ALU only)
  int64_t imm = 0;        // signed immediate (sign-extended to Int/Long)
  uint64_t immUnsigned = 0; // unsigned immediate (Unsigned/Qword forms)
  double immFloat = 0;    // float immediate (form 8)
  int32_t jumpOffset = 0; // relative offset for jump opcodes
  size_t size = 0;        // total byte size of this instruction in the stream
};

// Class predicates
bool isValidOpcode(Opcode op);
bool isJumpOpcode(Opcode op);
bool isImmediateOpcode(Opcode op);
bool isRegConstOpcode(Opcode op);

// Size computation (v5, shipped: `.qlc` v5 modifier nibbles)
size_t baseInstructionSize(Opcode op);
size_t escapedInstructionSize(Opcode op, uint8_t modifier);
size_t instructionSize(Opcode op, uint8_t modifier = 0);

// Size computation (v6, specified: 2/2/4 modifier byte). The base form is
// identical in both versions, so `baseInstructionSize` is shared; only the
// escaped forms differ. Unlike `instructionSize`, the escaped form cannot be
// announced by a modifier of 0: under the 2/2/4 split `e = 0x00` is a legal
// escaped modifier (kind immediate, form unsigned 1 byte — the only way to
// encode `mov r3, 200` in one operand byte), so the escape bit is passed
// explicitly. Size, encoder and decoder must agree byte-for-byte or `pc`
// desynchronises (plan §10).
size_t escapedInstructionSizeV6(Opcode op, uint8_t e);
size_t instructionSizeV6(Opcode op, bool escaped, uint8_t e);

// Jump width selection
size_t minJumpSize(int32_t offset);
// Returns 2 (jmp8), 4 (jmp16), or 6 (jmp32)

// Operand width
size_t registerAddressSize(int32_t index);  // 1 or 2
size_t constantAddressSize(int32_t index);  // 1 or 2

// Encode one instruction into a byte buffer (v5, shipped encoding)
void encodeInstruction(std::vector<uint8_t>& out, const DecodedInstruction& in);

// Decode one instruction from a byte buffer at position pc (v5, shipped
// encoding)
void decodeInstruction(const uint8_t* code, size_t pc, size_t codeSize,
                       DecodedInstruction& out);

// Encode / decode one instruction with the v6 modifier byte (plan §2). The
// stream framing is unchanged — flat byte array, opcode top bit = escape, one
// modifier byte — only the interpretation of that byte differs, plus the
// immediate payload forms (signed / unsigned / float / zero-byte bool and nil)
// and the in-place arity rule for a non-register right operand.
void encodeInstructionV6(std::vector<uint8_t>& out,
                         const DecodedInstruction& in);
void decodeInstructionV6(const uint8_t* code, size_t pc, size_t codeSize,
                         DecodedInstruction& out);

// A compiled user-defined function: its instruction stream, constant pool, and
// calling metadata. Index 0 of Bytecode::functions is the implicit "main"
// function (the top-level program). `registerCount` is the number of registers
// the callee frame must allocate (params + temporaries).
struct Function {
  std::string name;
  int paramCount = 0;
  int registerCount = 0;
  std::vector<uint8_t> code;                    // flat byte stream
  std::vector<size_t> instrOffsets;             // byte offset of each instruction
  // Parallel to instrOffsets: source location (line, col) per instruction.
  // Empty entries (0,0) mean "no source location". Only read on error.
  std::vector<std::pair<uint16_t, uint16_t>> loc;
  std::vector<QLangValue> constants;
  // Capture spec for nested `def` functions: each entry maps a captured
  // variable name to the register index in the enclosing frame where its value
  // lives at the time the closure is constructed.
  std::vector<CaptureRef> captures;
  // Names of the captured variables (parallel to `captures`): the compiler
  // uses them to materialize the def-time closure (Phase 1). Not serialized
  // by the .qlc codec (the capture regs carry the wire data).
  std::vector<std::string> captureNames;
  // True if the function body contains any `yield` statement; the VM creates
  // a generator instance for calls to such functions rather than executing
  // them inline.
  bool isGenerator = false;
};

// The compiled program: an instruction stream plus a constant pool.
class Bytecode {
public:
  // Append a constant, returning its pool index.
  int addConstant(QLangValue value);

  // Append a compiled function (a `def` body or an arrow-closure body). The
  // caller fills in the name/paramCount/registerCount; this appends the
  // instruction stream and constant pool and returns the new function index.
  int addFunction(const std::string &name, int paramCount, int registerCount,
                  std::vector<uint8_t> code,
                  std::vector<size_t> instrOffsets,
                  std::vector<QLangValue> constants);

  const std::vector<QLangValue> &constants() const { return m_consts; }
  // Replaces the pool.
  void setConstants(std::vector<QLangValue> consts);
  const std::vector<Function> &functions() const { return m_functions; }
  size_t size() const { return m_code.size(); }

  const std::vector<uint8_t>& code() const { return m_code; }
  std::vector<uint8_t>& code() { return m_code; }
  void setCode(std::vector<uint8_t> c) { m_code = std::move(c); }

  // Human-readable opcode mnemonic (for dumping/debugging).
  static const char *name(Opcode op);

private:
  std::vector<uint8_t> m_code;
  std::vector<QLangValue> m_consts;
  // Dedup index: serialized-value key -> pool index. Kept in sync with
  // m_consts by addConstant()/setConstants() so repeated identical
  // constants reuse one pool slot.
  std::unordered_map<std::string, int> m_constKeys;
  std::vector<Function> m_functions;
};

} // namespace qlang
