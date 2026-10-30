#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "QLangValue.h"

namespace qlang {

class QLangContext; // forward declaration for CompiledProgram::nativeFuncs

// Canonical byte form of a value used as a constant-pool dedup key:
// same value -> same key, different values -> different keys (type tag +
// length-prefixed payloads; table entries in key-sorted order; Function
// closures keyed by funcIndex + capture regs — frame pointers are
// runtime-only). Generator is not durable and keys as Nil (same rule as the
// .qlc writer).
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
  Set         // set(...) -> set (unique-value collection constructor)
};

// Register-machine opcodes. This is a small register VM: each script variable
// maps to a register, and arithmetic/comparison results are written to a
// register. Constants live in a separate pool and are loaded with CONST.
enum class Opcode : uint16_t {
  // Data movement
  CONST, // r[a] = constants[b]
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
  IN,   // r[a] = (r[b] in r[c])  membership: array element / table key

  // Built-in function call. r[a] = call(funcId=b, args in r[a+1 .. a+c]).
  CALL, // r[a] = builtin(b, r[a+1 .. a+c])

  // Variable removal. r[a] is dropped from the scope at end-of-run.
  DELETE, // remove script variable mapped to register a

  // Runtime cast: strict type conversion (throws on invalid cast).
  CAST, // r[a] = strict_convert(r[b], type c)

  // Array operations (register holds an ArrayValue).
  ARRGET, // r[a] = arrays[b][c]               (subscript read)
  ARRPUT, // arrays[a][b] = c                   (subscript write)
  ARRAPP, // arrays[a].push_back(c)             (append single element)
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

  kOpcodeCount // sentinel: not a real opcode; valid ops are < kOpcodeCount
};

// True when the opcode carries a 32-bit parameter in the combined b/c slot
// (Instruction::i32) instead of int16_t a/b/c. Single source of truth shared
// by the compiler (emit/backpatch), the VM (decode), the disassembler, and
// the .qlc codec. Currently: the jump family (pc can exceed 32,767) and
// MOVI/ADDI (32-bit integer literal in i32, destination register in a).
inline bool usesI32(Opcode op) {
  switch (op) {
  case Opcode::JMP:
  case Opcode::JMPIFFALSE:
  case Opcode::JMPIFTRUE:
  case Opcode::CONTINUE:
  case Opcode::BREAK:
  case Opcode::MOVI:
  case Opcode::ADDI:
    return true;
  default:
    return false;
  }
}

// A single instruction: 12 bytes, alignment 4, no padding (the union sits at
// offset 4, so the raw little-endian bytes are the .qlc encoding). Operands
// are interpreted per-opcode (docs/spec/13-bytecodes.md "Instruction operand
// map"): register indices, constant-pool indices, type ids, and for the
// jump family a 32-bit pc target in the combined i32 slot (JMP/CONTINUE/
// BREAK; JMPIFFALSE/JMPIFTRUE keep the condition register in a).
struct Instruction {
  Opcode op = Opcode::CONST; // uint16_t
  int16_t a = 0;
  union {
    struct {
      int16_t b;
      int16_t c;
    };
    int32_t i32 = 0; // combined b/c for opcodes with 32-bit operands
  };
  uint16_t line = 0, col = 0;
};

static_assert(sizeof(Instruction) == 12, "Instruction must be 12 bytes");

// A compiled user-defined function: its instruction stream, constant pool, and
// calling metadata. Index 0 of Bytecode::functions is the implicit "main"
// function (the top-level program). `registerCount` is the number of registers
// the callee frame must allocate (params + temporaries).
struct Function {
  std::string name;
  int paramCount = 0;
  int registerCount = 0;
  std::vector<Instruction> instructions;
  std::vector<QLangValue> constants;
  // Capture spec for nested `def` functions: each entry maps a captured
  // variable name to the register index in the enclosing frame where its value
  // lives at the time the closure is constructed.
  std::vector<CaptureRef> captures;
  // True if the function body contains any `yield` statement; the VM wraps
  // calls to such functions in a GeneratorValue rather than executing inline.
  bool isGenerator = false;
};

// The compiled program: an instruction stream plus a constant pool.
class Bytecode {
public:
  void add(const Instruction &inst);
  void add(Opcode op, int a = 0, int b = 0, int c = 0);

  // Append a constant, returning its pool index.
  int addConstant(QLangValue value);

  // Append a compiled function (a `def` body or an arrow-closure body). The
  // caller fills in the name/paramCount/registerCount; this appends the
  // instruction stream and constant pool and returns the new function index.
  int addFunction(const std::string &name, int paramCount, int registerCount,
                  std::vector<Instruction> instrs,
                  std::vector<QLangValue> constants);

  const std::vector<Instruction> &instructions() const { return m_instr; }
  std::vector<Instruction> &instructions() { return m_instr; }
  void setInstructions(std::vector<Instruction> instrs) { m_instr = std::move(instrs); }
  const std::vector<QLangValue> &constants() const { return m_consts; }
  // Replaces the pool; the dedup key map is rebuilt to stay in sync.
  void setConstants(std::vector<QLangValue> consts);
  const std::vector<Function> &functions() const { return m_functions; }
  size_t size() const { return m_instr.size(); }

  // Mutable access to instruction i (used to patch jump targets after a
  // forward reference is resolved).
  Instruction &at(size_t i) { return m_instr[i]; }

  // Human-readable opcode mnemonic (for dumping/debugging).
  static const char *name(Opcode op);

private:
  std::vector<Instruction> m_instr;
  std::vector<QLangValue> m_consts;
  // Dedup index: serialized-value key -> pool index. Kept in sync with
  // m_consts by addConstant()/setConstants() so repeated identical
  // constants reuse one pool slot.
  std::unordered_map<std::string, int> m_constKeys;
  std::vector<Function> m_functions;
};

} // namespace qlang
