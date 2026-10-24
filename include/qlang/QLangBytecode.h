#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "QLangValue.h"

namespace qlang {

class QLangContext; // forward declaration for CompiledProgram::nativeFuncs

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
  Next       // next(gen) -> value (resume generator, returns yielded value or nil)
};

// Register-machine opcodes. This is a small register VM: each script variable
// maps to a register, and arithmetic/comparison results are written to a
// register. Constants live in a separate pool and are loaded with CONST.
enum class Opcode {
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
  YIELD // r[a] = generator(r[b]); r[c] = generator register
};

// A single instruction. Operands are plain ints interpreted per-opcode:
//   register indices for MOV/ADD/.../JMPIFFALSE's first operand,
//   a constant-pool index for CONST's second operand,
//   a program index for JMP/JMPIFFALSE's second operand.
struct Instruction {
  Opcode op = Opcode::CONST;
  int a = 0;
  int b = 0;
  int c = 0;
  int line = 0;
  int col = 0;
};

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
  void setConstants(std::vector<QLangValue> consts) { m_consts = std::move(consts); }
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
  std::vector<Function> m_functions;
};

} // namespace qlang
