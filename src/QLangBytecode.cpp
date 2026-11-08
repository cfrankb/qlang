#include <climits>
#include <cstring>

#include "qlang/QLangBytecode.h"
#include "qlang/QLangErrors.h"

namespace qlang {

// ---------------------------------------------------------------------------
// Class predicates
// ---------------------------------------------------------------------------

bool isValidOpcode(Opcode op) {
  return static_cast<uint8_t>(op) < kOpcodeUsedCount;
}

bool isJumpOpcode(Opcode op) {
  switch (op) {
  case Opcode::JMP:
  case Opcode::CONTINUE:
  case Opcode::BREAK:
  case Opcode::JE:
  case Opcode::JNE:
  case Opcode::JG:
  case Opcode::JGE:
  case Opcode::JL:
  case Opcode::JLE:
    return true;
  default:
    return false;
  }
}

bool isRegConstOpcode(Opcode op) { return !isJumpOpcode(op); }

// ---------------------------------------------------------------------------
// Size computation
// ---------------------------------------------------------------------------

// Number of operand bytes (excluding opcode byte) for a base-form instruction
// (declared in QLangBytecode.h: the emitter reads it to pick a modifier).
size_t baseOperandCount(Opcode op) {
  if (isJumpOpcode(op))
    return 1; // offset only
  // 0-operand
  switch (op) {
  case Opcode::Nop:
    return 0;
  }
  // 1-operand
  switch (op) {
  case Opcode::INC:
  case Opcode::DEC:
  case Opcode::DELETEx:
  case Opcode::TABCREATE:
  case Opcode::TABFINALIZE:
  case Opcode::RETURN:
  case Opcode::YIELD:
  case Opcode::TST:
    return 1;
  }
  // 2-operand (NEG/NOT/EVALKEY are r[a] = op(r[b]): the compiler emits them
  // with a=dest, b=src, so both operands must be stored). The flag-setting
  // compares are r[a] <op> right -> flags: no destination, but the right
  // operand must be stored (and `c` stays unused, as for CMP). MAKEGEN and
  // SEQGET are 3-operand (MAKEGEN: c=argCount; SEQGET: c=index) and fall
  // through to the default below.
  switch (op) {
  case Opcode::MOV:
  case Opcode::MAKEFUNC:
  case Opcode::REFLOAD:
  case Opcode::REFSTORE:
  case Opcode::CMP:
  case Opcode::EQF:
  case Opcode::LTF:
  case Opcode::GTF:
  case Opcode::LTEF:
  case Opcode::GTEF:
  case Opcode::ARRAPP:
  case Opcode::ARRLEN:
  case Opcode::LEN:
  case Opcode::ARRAPPV:
  case Opcode::NEG:
  case Opcode::NOT:
  case Opcode::EVALKEY:
  case Opcode::DEEPCOPY:
    return 2;
  }
  // 3-operand (default): ADD..INx, CONV, CAST, ARRGET/ARRPUT, TABGET/TABPUT,
  // INDGET/INDPUT, INDEXGET/INDEXPUT, CALL/CALLFN/CALLCLOS, MAKEGEN
  // (c=argCount), SEQGET (c=index).
  return 3;
}

size_t baseInstructionSize(Opcode op) {
  return 1 + baseOperandCount(op);
}

// The escaped-form size table lives with the codec below: it is the single
// authority on what a legal modifier byte means for an opcode, and both the
// encoder and the decoder run an escaped form through it first.

// ---------------------------------------------------------------------------
// Jump width selection
// ---------------------------------------------------------------------------

size_t minJumpSize(int32_t offset) {
  if (offset >= -127 && offset <= 127) return 2;
  if (offset >= -32767 && offset <= 32767) return 4;
  return 6;
}

// ---------------------------------------------------------------------------
// Operand width
// ---------------------------------------------------------------------------

size_t registerAddressSize(int32_t index) {
  if (index < 0 || index > 65535)
    throw QLangError(QLangErrorKind::Runtime, 0, 0,
                     "register index out of range");
  return (index <= 255) ? 1 : 2;
}

size_t constantAddressSize(int32_t index) {
  if (index < 0 || index > 65535)
    throw QLangError(QLangErrorKind::Runtime, 0, 0,
                     "constant index out of range");
  return (index <= 255) ? 1 : 2;
}

// ---------------------------------------------------------------------------
// v6 modifier-byte tables (docs/plans/encoding-v6-and-indirect-plan.md §2,
// docs/spec/13-bytecodes.md `### bytecode encoding`)
// ---------------------------------------------------------------------------

uint8_t makeModifier(OperandWidth left, RightKind kind, uint8_t form) {
  if (left == OperandWidth::Reserved)
    throw QLangError(QLangErrorKind::Runtime, 0, 0,
                     "reserved left-operand addressing form");
  if (kind == RightKind::Reserved)
    throw QLangError(QLangErrorKind::Runtime, 0, 0,
                     "reserved right-operand kind");
  if (kind == RightKind::Immediate) {
    if (form >= static_cast<uint8_t>(ImmForm::Reserved))
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "reserved immediate form");
  } else if (form > static_cast<uint8_t>(OperandWidth::DWord)) {
    throw QLangError(QLangErrorKind::Runtime, 0, 0,
                     "reserved register/constant addressing form");
  }
  return static_cast<uint8_t>(
      (static_cast<uint8_t>(left) << kModifierLeftShift) |
      (static_cast<uint8_t>(kind) << kModifierKindShift) |
      (static_cast<uint8_t>(form) << kModifierFormShift));
}

OperandWidth modifierLeftForm(uint8_t e) {
  return static_cast<OperandWidth>((e >> kModifierLeftShift) &
                                   kModifierLeftMask);
}

RightKind modifierRightKind(uint8_t e) {
  return static_cast<RightKind>((e >> kModifierKindShift) & 0x03);
}

uint8_t modifierRightForm(uint8_t e) {
  return static_cast<uint8_t>((e >> kModifierFormShift) & 0x0F);
}

size_t operandWidthBytes(OperandWidth w) {
  switch (w) {
  case OperandWidth::Byte:
    return 1;
  case OperandWidth::Word:
    return 2;
  case OperandWidth::DWord:
    return 4;
  case OperandWidth::Reserved:
    break;
  }
  throw QLangError(QLangErrorKind::Runtime, 0, 0,
                   "reserved operand addressing form");
}

size_t immediateFormBytes(uint8_t form) {
  switch (form) {
  case 0:
  case 4:
    return 1;
  case 1:
  case 5:
    return 2;
  case 2:
  case 6:
    return 4;
  case 3:
  case 7:
  case 8:
    return 8;
  case 9:
  case 10:
  case 11:
    return 0; // the form *is* the value: bool false/true, nil
  default:
    break;
  }
  throw QLangError(QLangErrorKind::Runtime, 0, 0, "reserved immediate form");
}

ImmType immediateFormType(uint8_t form) {
  switch (form) {
  case 0:
  case 1:
  case 2:
    return ImmType::Unsigned;
  case 3:
    return ImmType::Qword;
  case 4:
  case 5:
  case 6:
    return ImmType::Int;
  case 7:
    return ImmType::Long;
  case 8:
    return ImmType::Float;
  case 9:
  case 10:
    return ImmType::Bool;
  case 11:
    return ImmType::Nil;
  default:
    break;
  }
  throw QLangError(QLangErrorKind::Runtime, 0, 0, "reserved immediate form");
}

RightKind defaultRightKind(Opcode op) {
  switch (op) {
  case Opcode::CALL: // always escaped; the function id is an immediate
    return RightKind::Immediate;
  case Opcode::MAKEFUNC:
  case Opcode::MAKEGEN:
    return RightKind::ConstPool;
  default:
    return RightKind::Register;
  }
}

uint8_t baseModifier(Opcode op) {
  if (isJumpOpcode(op) || op == Opcode::Nop)
    return kNoBaseModifier;
  if (op == Opcode::CALL)
    return makeModifier(OperandWidth::Byte, RightKind::Immediate,
                        static_cast<uint8_t>(ImmForm::I16));
  RightKind kind = defaultRightKind(op);
  return makeModifier(
      OperandWidth::Byte, kind,
      kind == RightKind::Immediate ? static_cast<uint8_t>(ImmForm::I8) : 0);
}

// The binary ALU and flag-compare families: the only opcodes whose right
// operand may be an immediate or a pool entry *and* whose 3-operand form
// collapses to the in-place 2-operand form when it is (plan §2.5, §2.6).
static bool isAluOrFlagCompare(Opcode op) {
  switch (op) {
  case Opcode::ADD:
  case Opcode::SUB:
  case Opcode::MUL:
  case Opcode::DIV:
  case Opcode::MOD:
  case Opcode::AND:
  case Opcode::OR:
  case Opcode::XOR:
  case Opcode::SHL:
  case Opcode::SHR:
  case Opcode::LAND:
  case Opcode::LOR:
  case Opcode::EQ:
  case Opcode::NE:
  case Opcode::LT:
  case Opcode::GT:
  case Opcode::LE:
  case Opcode::GE:
  case Opcode::POW:
  case Opcode::INx:
  case Opcode::CMP:
  // The flag-setting compares belong here too (plan M3 D5): one switch entry
  // buys both the §2.5 permitted-kind row (immediate / register / pool) and
  // the §2.6 in-place arity rule, and the OpMeta fast path inherits it. TST is
  // deliberately absent — it has no right operand at all, so any kind other
  // than register must stay a load-time rejection.
  case Opcode::EQF:
  case Opcode::LTF:
  case Opcode::GTF:
  case Opcode::LTEF:
  case Opcode::GTEF:
    return true;
  default:
    return false;
  }
}

uint8_t permittedKinds(Opcode op) {
  constexpr uint8_t kImm = 1u << static_cast<uint8_t>(RightKind::Immediate);
  constexpr uint8_t kReg = 1u << static_cast<uint8_t>(RightKind::Register);
  constexpr uint8_t kConst = 1u << static_cast<uint8_t>(RightKind::ConstPool);

  switch (op) {
  case Opcode::CALL: // form fixed to 5 (plan §2.8)
    return kImm;
  case Opcode::MAKEFUNC:
  case Opcode::MAKEGEN: // pool-index only (plan §2.5)
    return kConst;
  default:
    break;
  }

  // Copy family and the ALU / flag-compare families accept all three kinds.
  switch (op) {
  case Opcode::MOV:
  case Opcode::CONV:
  case Opcode::NEG:
  case Opcode::DEEPCOPY:
    return kImm | kReg | kConst;
  default:
    break;
  }
  if (isAluOrFlagCompare(op))
    return kImm | kReg | kConst;

  // Everything else: the kind field never reinterprets the operand, it only
  // ever selects its addressing width (control flow, array/table/closure ops,
  // indget/indput, ...).
  return kReg;
}

bool kindPermitted(Opcode op, RightKind kind) {
  return (permittedKinds(op) >> static_cast<uint8_t>(kind)) & 0x1u;
}

bool isInPlaceForm(Opcode op, RightKind kind) {
  if (kind == RightKind::Register)
    return false;
  return isAluOrFlagCompare(op);
}

// ---------------------------------------------------------------------------
// Little-endian byte helpers
// ---------------------------------------------------------------------------

static void writeU16LE(std::vector<uint8_t> &out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

static void writeI32LE(std::vector<uint8_t> &out, int32_t v) {
  uint32_t u = static_cast<uint32_t>(v);
  out.push_back(static_cast<uint8_t>(u & 0xFF));
  out.push_back(static_cast<uint8_t>((u >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>((u >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((u >> 24) & 0xFF));
}

// Read a little-endian unsigned integer of `n` bytes (1, 2, or 4) starting at
// code[pos]; throws on truncation.
static uint32_t readULE(const uint8_t *code, size_t pos, size_t n,
                        size_t codeSize) {
  if (pos + n > codeSize)
    throw QLangError(QLangErrorKind::Io, 0, 0, "truncated instruction stream");
  uint32_t v = 0;
  for (size_t i = n; i > 0; i--)
    v = (v << 8) | code[pos + i - 1];
  return v;
}

// ---------------------------------------------------------------------------
// Instruction codec (docs/plans/encoding-v6-and-indirect-plan.md §2)
//
// Flat byte array, opcode top bit = escape, one modifier byte read as the 2/2/4
// layout, the full immediate set (signed / unsigned / float / zero-byte bool and
// nil) and the in-place arity rule for a non-register right operand. The jump
// class keeps the historical widths (plan §2.7): there `e` selects the offset
// size only and is not read as 2/2/4. Both the encoder and the decoder run the
// escaped form through `escapedInstructionSize` first, so the three views of the
// encoding (size / encode / decode) cannot drift apart.
// ---------------------------------------------------------------------------

size_t escapedInstructionSize(Opcode op, uint8_t e) {
  // Jump class: `e` selects the offset width only and is not read as the 2/2/4
  // layout (plan §2.7).
  if (isJumpOpcode(op)) {
    if (e > 1)
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "invalid jump escape modifier");
    return 2 + (e == 0 ? 2 : 4); // jmp16 / jmp32
  }

  OperandWidth left = modifierLeftForm(e);
  RightKind kind = modifierRightKind(e);
  uint8_t form = modifierRightForm(e);
  size_t leftBytes = operandWidthBytes(left); // throws on the reserved value

  // CALL is the one documented exception (plan §2.8): always escaped, kind 0
  // with the form fixed to 5 (2-byte signed function id), then the 1-byte
  // argument count.
  if (op == Opcode::CALL) {
    if (kind != RightKind::Immediate ||
        form != static_cast<uint8_t>(ImmForm::I16))
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "invalid CALL escape modifier");
    return 2 + leftBytes + 2 + 1; // op+e + a + function id + arg count
  }

  size_t nOps = baseOperandCount(op);
  if (nOps == 0)
    throw QLangError(QLangErrorKind::Runtime, 0, 0,
                     "escaped form has no operands for this opcode");
  if (nOps == 1) {
    // A single-operand opcode has no right operand, so only the left field is
    // meaningful; the kind/form fields must stay at their base values.
    if (kind != RightKind::Register || form != 0)
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "invalid escape modifier for a single-operand opcode");
    return 2 + leftBytes;
  }
  if (kind == RightKind::Reserved || !kindPermitted(op, kind))
    throw QLangError(QLangErrorKind::Runtime, 0, 0,
                     "right-operand kind not permitted for this opcode");

  size_t rightBytes;
  if (kind == RightKind::Immediate) {
    rightBytes = immediateFormBytes(form); // throws on the reserved forms
  } else {
    if (form > static_cast<uint8_t>(OperandWidth::DWord))
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "reserved register/constant addressing form");
    rightBytes = operandWidthBytes(static_cast<OperandWidth>(form));
  }
  // The third operand is present unless the arity rule collapsed the op to its
  // in-place 2-operand form (plan §2.6).
  size_t third = (nOps == 3 && !isInPlaceForm(op, kind)) ? 1 : 0;
  return 2 + leftBytes + rightBytes + third;
}

size_t instructionSize(Opcode op, bool escaped, uint8_t e) {
  if (!escaped)
    return baseInstructionSize(op);
  return escapedInstructionSize(op, e);
}

static void writeU64LE(std::vector<uint8_t> &out, uint64_t v) {
  for (size_t i = 0; i < 8; i++)
    out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

// Read a little-endian unsigned integer of `n` bytes (0..8) at code[pos];
// throws on truncation.
static uint64_t readU64LE(const uint8_t *code, size_t pos, size_t n,
                          size_t codeSize) {
  if (n > 8 || pos + n > codeSize)
    throw QLangError(QLangErrorKind::Io, 0, 0, "truncated instruction stream");
  uint64_t v = 0;
  for (size_t i = n; i > 0; i--)
    v = (v << 8) | code[pos + i - 1];
  return v;
}

static int64_t signExtendLE(uint64_t raw, size_t n) {
  if (n == 0 || n >= 8)
    return static_cast<int64_t>(raw);
  uint64_t mask = (uint64_t{1} << (n * 8)) - 1;
  uint64_t sign = uint64_t{1} << (n * 8 - 1);
  raw &= mask;
  return static_cast<int64_t>((raw & sign) ? (raw | ~mask) : raw);
}

static void writeIndexWidth(std::vector<uint8_t> &out, int64_t v,
                            OperandWidth w) {
  int64_t limit;
  switch (w) {
  case OperandWidth::Byte:
    limit = 255;
    break;
  case OperandWidth::Word:
    limit = 65535;
    break;
  case OperandWidth::DWord:
    limit = INT32_MAX;
    break;
  default:
    throw QLangError(QLangErrorKind::Runtime, 0, 0,
                     "reserved operand addressing form");
  }
  if (v < 0 || v > limit)
    throw QLangError(QLangErrorKind::Runtime, 0, 0,
                     "operand index out of range for its addressing width");
  uint32_t u = static_cast<uint32_t>(v);
  for (size_t i = 0, n = operandWidthBytes(w); i < n; i++)
    out.push_back(static_cast<uint8_t>((u >> (8 * i)) & 0xFF));
}

static int32_t readIndexWidth(const uint8_t *code, size_t &pos, size_t codeSize,
                              OperandWidth w) {
  size_t n = operandWidthBytes(w); // throws on the reserved value
  uint64_t v = readU64LE(code, pos, n, codeSize);
  pos += n;
  if (v > static_cast<uint64_t>(INT32_MAX))
    throw QLangError(QLangErrorKind::Io, 0, 0, "operand index out of range");
  return static_cast<int32_t>(v);
}

static bool fitsSignedRange(int64_t v, size_t bytes) {
  switch (bytes) {
  case 1:
    return v >= -128 && v <= 127;
  case 2:
    return v >= -32768 && v <= 32767;
  case 4:
    return v >= INT32_MIN && v <= INT32_MAX;
  default:
    return true; // 8-byte form
  }
}

static void writeImmediate(std::vector<uint8_t> &out,
                           const DecodedInstruction &in, uint8_t form) {
  ImmType t = immediateFormType(form); // throws on the reserved forms
  size_t n = immediateFormBytes(form);
  uint64_t raw = 0;
  switch (t) {
  case ImmType::Unsigned:
  case ImmType::Qword:
    raw = in.immUnsigned;
    if (n < 8 && raw > (~uint64_t{0} >> (64 - 8 * n)))
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "unsigned immediate does not fit its form");
    break;
  case ImmType::Int:
  case ImmType::Long:
    if (!fitsSignedRange(in.imm, n))
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "signed immediate does not fit its form");
    raw = static_cast<uint64_t>(in.imm);
    break;
  case ImmType::Float:
    std::memcpy(&raw, &in.immFloat, sizeof raw);
    break;
  case ImmType::Bool:
  case ImmType::Nil:
    n = 0; // the form *is* the value: no operand bytes
    break;
  }
  for (size_t i = 0; i < n; i++)
    out.push_back(static_cast<uint8_t>((raw >> (8 * i)) & 0xFF));
}

static void readImmediate(const uint8_t *code, size_t &pos, size_t codeSize,
                          uint8_t form, DecodedInstruction &out) {
  ImmType t = immediateFormType(form);
  size_t n = immediateFormBytes(form);
  uint64_t raw = readU64LE(code, pos, n, codeSize);
  pos += n;
  switch (t) {
  case ImmType::Unsigned:
  case ImmType::Qword:
    out.immPayload = ImmPayload::Unsigned;
    out.immUnsigned = raw;
    break;
  case ImmType::Int:
  case ImmType::Long:
    out.immPayload = ImmPayload::Signed;
    out.imm = signExtendLE(raw, n);
    break;
  case ImmType::Float: {
    double d = 0;
    std::memcpy(&d, &raw, sizeof d);
    out.immPayload = ImmPayload::Float;
    out.immFloat = d;
    break;
  }
  case ImmType::Bool:
    out.immPayload = form == static_cast<uint8_t>(ImmForm::True)
                         ? ImmPayload::True
                         : ImmPayload::False;
    break;
  case ImmType::Nil:
    out.immPayload = ImmPayload::Nil;
    break;
  }
}

void encodeInstruction(std::vector<uint8_t> &out,
                       const DecodedInstruction &in) {
  // CALL is always escaped (plan §2.8): its 2-byte signed function id can only
  // be described by a modifier byte.
  if (in.op == Opcode::CALL && !in.escaped)
    throw QLangError(QLangErrorKind::Runtime, 0, 0,
                     "CALL must be escaped (signed function ID)");

  // Validate the opcode/modifier combination up front: the size table is the
  // single authority on what a legal escaped form is.
  size_t expect = 0;
  if (in.escaped)
    expect = escapedInstructionSize(in.op, in.modifier);

  size_t start = out.size();
  uint8_t opByte = static_cast<uint8_t>(in.op);
  if (in.escaped)
    opByte |= kOpcodeEscape;
  out.push_back(opByte);
  if (in.escaped)
    out.push_back(in.modifier);

  // Jump class: the modifier is a width selector here, not a 2/2/4 field
  // (plan §2.7).
  if (isJumpOpcode(in.op)) {
    if (!in.escaped) {
      if (in.jumpOffset < -128 || in.jumpOffset > 127)
        throw QLangError(QLangErrorKind::Runtime, 0, 0,
                         "jump offset does not fit in base form");
      out.push_back(static_cast<uint8_t>(in.jumpOffset & 0xFF));
    } else if (in.modifier == 0) {
      if (in.jumpOffset < -32767 || in.jumpOffset > 32767)
        throw QLangError(QLangErrorKind::Runtime, 0, 0,
                         "jump offset does not fit in jmp16 form");
      writeU16LE(out,
                 static_cast<uint16_t>(static_cast<int16_t>(in.jumpOffset)));
    } else {
      writeI32LE(out, in.jumpOffset);
    }
    return;
  }

  // A base form carries no modifier byte, so its kind is fixed by the opcode
  // identity and its form is the 1-byte default (immediates: signed 1 byte).
  RightKind kind =
      in.escaped ? modifierRightKind(in.modifier) : defaultRightKind(in.op);
  OperandWidth left =
      in.escaped ? modifierLeftForm(in.modifier) : OperandWidth::Byte;
  uint8_t form =
      in.escaped
          ? modifierRightForm(in.modifier)
          : (kind == RightKind::Immediate ? static_cast<uint8_t>(ImmForm::I8)
                                          : 0);

  if (in.op == Opcode::CALL) {
    writeIndexWidth(out, in.a, left);
    if (in.b < INT16_MIN || in.b > INT16_MAX)
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "function ID out of range");
    writeU16LE(out, static_cast<uint16_t>(static_cast<int16_t>(in.b)));
    if (in.c < 0 || in.c > 255)
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "argument count out of range");
    out.push_back(static_cast<uint8_t>(in.c));
    if (out.size() - start != expect)
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "encoder length disagrees with the size table");
    return;
  }

  size_t nOps = baseOperandCount(in.op);
  if (nOps == 0)
    return; // zero-operand opcode; an escaped form was rejected above
  writeIndexWidth(out, in.a, left);
  if (nOps == 1)
    return;
  if (kind == RightKind::Immediate) {
    writeImmediate(out, in, form);
  } else {
    writeIndexWidth(out, in.b,
                    static_cast<OperandWidth>(in.escaped ? form : 0));
  }
  if (nOps == 3 && !isInPlaceForm(in.op, kind)) {
    if (in.c < 0 || in.c > 255)
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "third operand must be a 1-byte register");
    out.push_back(static_cast<uint8_t>(in.c));
  }
  if (in.escaped && out.size() - start != expect)
    throw QLangError(QLangErrorKind::Runtime, 0, 0,
                     "encoder length disagrees with the size table");
}

// ---------------------------------------------------------------------------
// Decoder fast path
//
// The dispatch loop decodes once per step, so the decoder is the hottest
// function in the engine. Two things made the v6 decoder ~2.5x the v5 one:
// every opcode class question was a separate switch-based predicate call, and
// the escaped form ran the whole `escapedInstructionSize` table a second time
// before reading the operands. Both are folded here: the per-opcode metadata is
// derived once from the *same* normative predicates the size table uses (so it
// cannot drift), and the modifier is validated with bit tests in the same order
// `escapedInstructionSize` checks them, so every malformed byte still raises
// the identical message.
// ---------------------------------------------------------------------------

namespace {
struct OpMeta {
  uint8_t nOps;      // baseOperandCount
  uint8_t defKind;   // defaultRightKind
  uint8_t permitted; // permittedKinds bitmask
  bool jump;         // isJumpOpcode
  bool aluCmp;       // isAluOrFlagCompare (the in-place arity rule)
  bool call;         // CALL: always escaped, modifier fixed
};
struct OpMetaTable {
  OpMeta v[kOpcodeMax];
};

const OpMetaTable kOpMeta = [] {
  OpMetaTable t{};
  for (uint8_t i = 0; i < kOpcodeMax; i++) {
    const Opcode op = static_cast<Opcode>(i);
    OpMeta &m = t.v[i];
    m.nOps = static_cast<uint8_t>(baseOperandCount(op));
    m.defKind = static_cast<uint8_t>(defaultRightKind(op));
    m.permitted = permittedKinds(op);
    m.jump = isJumpOpcode(op);
    m.aluCmp = isAluOrFlagCompare(op);
    m.call = op == Opcode::CALL;
  }
  return t;
}();

// Addressing-width byte counts indexed by the 2-bit width field; 3 is reserved.
constexpr uint8_t kWidthBytes[4] = {1, 2, 4, 0};
// Immediate byte counts indexed by form; 12-15 are reserved.
constexpr uint8_t kImmBytes[16] = {1, 2, 4, 8, 1, 2, 4, 8,
                                   8, 0, 0, 0, 0, 0, 0, 0};

[[noreturn]] inline void truncErr() {
  throw QLangError(QLangErrorKind::Io, 0, 0, "truncated instruction stream");
}

inline void need(const uint8_t *, size_t pos, size_t n, size_t codeSize) {
  if (pos + n > codeSize)
    truncErr();
}

// Little-endian load of 0/1/2/4/8 bytes; the caller has bounds-checked.
inline uint64_t loadLE(const uint8_t *p, uint8_t n) {
  switch (n) {
  case 0:
    return 0;
  case 1:
    return p[0];
  case 2:
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8);
  case 4:
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
  default:
    break;
  }
  return uint64_t(p[0]) | (uint64_t(p[1]) << 8) | (uint64_t(p[2]) << 16) |
         (uint64_t(p[3]) << 24) | (uint64_t(p[4]) << 32) |
         (uint64_t(p[5]) << 40) | (uint64_t(p[6]) << 48) |
         (uint64_t(p[7]) << 56);
}

inline int32_t readIndexLE(const uint8_t *code, size_t &pos, size_t codeSize,
                           uint8_t n) {
  if (pos + n > codeSize)
    truncErr();
  const uint64_t v = loadLE(code + pos, n);
  pos += n;
  if (v > static_cast<uint64_t>(INT32_MAX))
    throw QLangError(QLangErrorKind::Io, 0, 0, "operand index out of range");
  return static_cast<int32_t>(v);
}

inline void readImmLE(const uint8_t *code, size_t &pos, size_t codeSize,
                      uint8_t form, DecodedInstruction &out) {
  const uint8_t n = kImmBytes[form];
  if (pos + n > codeSize)
    truncErr();
  const uint64_t raw = loadLE(code + pos, n);
  pos += n;
  switch (form) {
  case 0:
  case 1:
  case 2:
  case 3:
    out.immPayload = ImmPayload::Unsigned;
    out.immUnsigned = raw;
    break;
  case 4:
  case 5:
  case 6:
    out.immPayload = ImmPayload::Signed;
    out.imm = signExtendLE(raw, n);
    break;
  case 7:
    out.immPayload = ImmPayload::Signed;
    out.imm = static_cast<int64_t>(raw);
    break;
  case 8: {
    double d = 0;
    std::memcpy(&d, &raw, sizeof d);
    out.immPayload = ImmPayload::Float;
    out.immFloat = d;
    break;
  }
  case 9:
    out.immPayload = ImmPayload::False;
    break;
  case 10:
    out.immPayload = ImmPayload::True;
    break;
  default:
    out.immPayload = ImmPayload::Nil;
    break;
  }
}
} // namespace

void decodeInstruction(const uint8_t *code, size_t pc, size_t codeSize,
                       DecodedInstruction &out) {
  if (pc >= codeSize)
    truncErr();
  const uint8_t raw = code[pc];
  out = DecodedInstruction();
  const uint8_t id = raw & 0x7F;
  const OpMeta &m = kOpMeta.v[id];
  const Opcode op = static_cast<Opcode>(id);
  out.op = op;
  const bool escaped = (raw & kOpcodeEscape) != 0;
  out.escaped = escaped;
  size_t pos = pc + 1;
  uint8_t e = 0;
  RightKind kind = static_cast<RightKind>(m.defKind);

  // The jump class does not use the 2/2/4 layout: `e` selects the offset width
  // only and its kind field is never read (plan §2.7).
  if (m.jump) {
    if (escaped) {
      if (pos >= codeSize)
        throw QLangError(QLangErrorKind::Io, 0, 0, "truncated modifier byte");
      e = code[pos++];
      out.modifier = e;
      if (e > 1)
        throw QLangError(QLangErrorKind::Runtime, 0, 0,
                         "invalid jump escape modifier");
    }
    if (!escaped) {
      if (pos >= codeSize)
        throw QLangError(QLangErrorKind::Io, 0, 0, "truncated jump offset");
      out.jumpOffset = static_cast<int8_t>(code[pos++]);
    } else {
      const uint8_t n = (e == 0) ? 2 : 4;
      need(code, pos, n, codeSize);
      out.jumpOffset =
          (n == 2) ? static_cast<int32_t>(static_cast<int16_t>(
                         static_cast<uint16_t>(loadLE(code + pos, 2))))
                   : static_cast<int32_t>(static_cast<uint32_t>(
                         loadLE(code + pos, 4)));
      pos += n;
    }
    out.rightKind = kind;
    out.size = pos - pc;
    return;
  }

  uint8_t leftW = 0;
  uint8_t form = 0;
  if (escaped) {
    if (pos >= codeSize)
      throw QLangError(QLangErrorKind::Io, 0, 0, "truncated modifier byte");
    e = code[pos++];
    out.modifier = e;
    leftW = e & kModifierLeftMask;
    form = (e >> kModifierFormShift) & 0x0F;
    kind = static_cast<RightKind>((e >> kModifierKindShift) & 0x03);
    // Reject a malformed modifier exactly where escapedInstructionSize does, so
    // the message a caller sees does not depend on which view ran first.
    if (leftW == 3)
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "reserved operand addressing form");
    if (m.call) {
      if (kind != RightKind::Immediate ||
          form != static_cast<uint8_t>(ImmForm::I16))
        throw QLangError(QLangErrorKind::Runtime, 0, 0,
                         "invalid CALL escape modifier");
    } else if (m.nOps == 0) {
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "escaped form has no operands for this opcode");
    } else if (m.nOps == 1) {
      if (kind != RightKind::Register || form != 0)
        throw QLangError(QLangErrorKind::Runtime, 0, 0,
                         "invalid escape modifier for a single-operand opcode");
    } else {
      if (((m.permitted >> static_cast<uint8_t>(kind)) & 0x1u) == 0)
        throw QLangError(QLangErrorKind::Runtime, 0, 0,
                         "right-operand kind not permitted for this opcode");
      if (kind == RightKind::Immediate) {
        if (form >= static_cast<uint8_t>(ImmForm::Reserved))
          throw QLangError(QLangErrorKind::Runtime, 0, 0,
                           "reserved immediate form");
      } else if (form > static_cast<uint8_t>(OperandWidth::DWord)) {
        throw QLangError(QLangErrorKind::Runtime, 0, 0,
                         "reserved register/constant addressing form");
      }
    }
  } else if (m.call) {
    throw QLangError(QLangErrorKind::Io, 0, 0,
                     "CALL must be escaped (signed function ID)");
  } else if (kind == RightKind::Immediate) {
    form = static_cast<uint8_t>(ImmForm::I8); // the base immediate is i8
  }
  out.rightKind = kind;
  if (kind == RightKind::Immediate)
    out.immForm = static_cast<ImmForm>(form);

  const uint8_t nOps = m.nOps;
  if (m.call) {
    out.a = readIndexLE(code, pos, codeSize, kWidthBytes[leftW]);
    if (pos + 2 > codeSize)
      truncErr();
    out.b = static_cast<int16_t>(
        static_cast<uint16_t>(loadLE(code + pos, 2)));
    pos += 2;
    if (pos >= codeSize)
      throw QLangError(QLangErrorKind::Io, 0, 0, "truncated operand");
    out.c = code[pos++];
    out.size = pos - pc;
    return;
  }

  if (nOps == 0) {
    out.size = pos - pc;
    return;
  }
  out.a = readIndexLE(code, pos, codeSize, kWidthBytes[leftW]);
  if (nOps == 1) {
    out.size = pos - pc;
    return;
  }
  if (kind == RightKind::Immediate) {
    readImmLE(code, pos, codeSize, form, out);
  } else {
    out.b = readIndexLE(code, pos, codeSize,
                        escaped ? kWidthBytes[form] : 1);
  }
  // The third operand is present unless the arity rule collapsed the op to its
  // in-place 2-operand form (plan §2.6).
  if (nOps == 3 && !(m.aluCmp && kind != RightKind::Register)) {
    if (pos >= codeSize)
      throw QLangError(QLangErrorKind::Io, 0, 0, "truncated operand");
    out.c = code[pos++];
  }
  out.size = pos - pc;
}

// ---------------------------------------------------------------------------
// Constant-pool dedup key (SPEC §bytecodes "constant pool": unique values,
// duplicates folded into a single entry)
// ---------------------------------------------------------------------------

std::string valueKey(const QLangValue &v) {
  std::string k;
  auto push = [&k](const void *p, size_t n) { k.append(static_cast<const char *>(p), n); };
  auto pushU32 = [&push](uint32_t x) { push(&x, sizeof x); };
  auto pushI32 = [&push](int32_t x) { push(&x, sizeof x); };
  auto pushU64 = [&push](uint64_t x) { push(&x, sizeof x); };

  using T = QLangValue::Type;
  T t = v.type();
  k.push_back(static_cast<char>(static_cast<uint8_t>(t)));
  switch (t) {
  case T::Nil:
  case T::Eof:
    break;
  case T::Bool:
  case T::Int:
    pushI32(v.asInt());
    break;
  case T::Char:
    pushI32(v.asChar());
    break;
  case T::Short:
    pushI32(v.asShort());
    break;
  case T::Byte:
    k.push_back(static_cast<char>(v.asByte()));
    break;
  case T::Word:
    pushU32(v.asWord());
    break;
  case T::Unsigned:
    pushU32(v.asUnsigned());
    break;
  case T::Qword:
    pushU64(v.asQword());
    break;
  case T::Long:
    pushU64(static_cast<uint64_t>(v.asLong()));
    break;
  case T::Float:
  {
    // Bit-exact: equal doubles always key identically.
    uint64_t bits;
    std::memcpy(&bits, &v.asFloat(), sizeof bits);
    pushU64(bits);
    break;
  }
  case T::String:
  {
    pushU32(static_cast<uint32_t>(v.asString().size()));
    k.append(v.asString());
    break;
  }
  case T::Blob:
  {
    const auto &b = v.asBlob();
    pushU32(static_cast<uint32_t>(b->size()));
    push(b->data(), b->size());
    break;
  }
  case T::Array:
  case T::Set:
  {
    // Content key, not the handle: two literals with equal elements must fold
    // to one pool entry even though each carries its own payload (Phase 2).
    const auto &a = v.asArray();
    pushU32(static_cast<uint32_t>(a->size()));
    for (const auto &e : *a) {
      std::string sub = valueKey(e);
      pushU32(static_cast<uint32_t>(sub.size()));
      k.append(sub);
    }
    break;
  }
  case T::Table:
  {
    const auto &tab = v.asTable();
    pushU32(static_cast<uint32_t>(tab->size()));
    for (const auto &[keyName, val] : *tab) { // std::map: key-sorted order
      pushU32(static_cast<uint32_t>(keyName.size()));
      k.append(keyName);
      std::string sub = valueKey(val);
      pushU32(static_cast<uint32_t>(sub.size()));
      k.append(sub);
    }
    break;
  }
  case T::Function:
  {
    // Compile-time spec: the pool closure's cells bind at MAKEFUNC time, so
    // funcIndex + capture regs fully determine the value.
    const auto &c = v.asFunction();
    pushI32(c.funcIndex);
    pushU32(static_cast<uint32_t>(c.captures.size()));
    for (const auto &cap : c.captures)
      pushI32(cap.reg);
    break;
  }
  case T::Generator:
  case T::Object:
  case T::External:
  case T::BoundMethod:
  case T::TypeRef:
  {
    // Reference identity: distinct objects never fold.
    const void *id = nullptr;
    switch (t) {
    case T::Generator:
      id = &v.asGenerator();
      break;
    case T::Object:
      id = v.asObject().get();
      break;
    case T::External:
      id = v.asExternal().owned.get() != nullptr
               ? v.asExternal().owned.get()
               : v.asExternal().type.get();
      break;
    case T::BoundMethod:
      id = v.asBoundMethod().member;
      break;
    case T::TypeRef:
      id = v.asTypeRef().type.get();
      break;
    default:
      break;
    }
    pushU64(reinterpret_cast<uint64_t>(id));
    break;
  }
  }
  return k;
}

// ---------------------------------------------------------------------------
// Bytecode class methods
// ---------------------------------------------------------------------------

int Bytecode::addConstant(QLangValue value) {
  std::string key = valueKey(value);
  auto it = m_constKeys.find(key);
  if (it != m_constKeys.end())
    return it->second; // dedup: identical value already in the pool
  int idx = static_cast<int>(m_consts.size());
  m_consts.push_back(std::move(value));
  m_constKeys.emplace(std::move(key), idx);
  return idx;
}

void Bytecode::setConstants(std::vector<QLangValue> consts) {
  m_consts = std::move(consts);
  m_constKeys.clear();
  for (size_t i = 0; i < m_consts.size(); i++)
    m_constKeys.emplace(valueKey(m_consts[i]), static_cast<int>(i));
}

int Bytecode::addFunction(const std::string &name, int paramCount,
                          int registerCount, std::vector<uint8_t> code,
                          std::vector<size_t> instrOffsets,
                          std::vector<QLangValue> constants) {
  Function fn;
  fn.name = name;
  fn.paramCount = paramCount;
  fn.registerCount = registerCount;
  fn.code = std::move(code);
  fn.instrOffsets = std::move(instrOffsets);
  fn.constants = std::move(constants);
  int idx = static_cast<int>(m_functions.size());
  m_functions.push_back(std::move(fn));
  return idx;
}

const char *Bytecode::name(Opcode op) {
  switch (op) {
  case Opcode::MOV: return "mov";
  case Opcode::CONV: return "conv";
  case Opcode::NEG: return "neg";
  case Opcode::INC: return "inc";
  case Opcode::DEC: return "dec";
  case Opcode::ADD: return "add";
  case Opcode::SUB: return "sub";
  case Opcode::MUL: return "mul";
  case Opcode::DIV: return "div";
  case Opcode::AND: return "and";
  case Opcode::OR: return "or";
  case Opcode::XOR: return "xor";
  case Opcode::SHL: return "shl";
  case Opcode::SHR: return "shr";
  case Opcode::LAND: return "land";
  case Opcode::LOR: return "lor";
  case Opcode::EQ: return "eq";
  case Opcode::NE: return "ne";
  case Opcode::LT: return "lt";
  case Opcode::GT: return "gt";
  case Opcode::LE: return "le";
  case Opcode::GE: return "ge";
  case Opcode::MOD: return "mod";
  case Opcode::POW: return "pow";
  case Opcode::NOT: return "not";
  case Opcode::LEN: return "len";
  case Opcode::SEQGET: return "seqget";
  case Opcode::CONTINUE: return "continue";
  case Opcode::BREAK: return "break";
  case Opcode::YIELD: return "yield";
  case Opcode::CALL: return "call";
  case Opcode::DELETEx: return "delete";
  case Opcode::CAST: return "cast";
  case Opcode::ARRGET: return "arrget";
  case Opcode::ARRPUT: return "arrput";
  case Opcode::ARRAPP: return "arrapp";
  case Opcode::ARRAPPV: return "arrappv";
  case Opcode::ARRLEN: return "arrlen";
  case Opcode::INx: return "in";
  case Opcode::TABCREATE: return "tabcreate";
  case Opcode::TABGET: return "tabget";
  case Opcode::TABPUT: return "tabput";
  case Opcode::TABFINALIZE: return "tabfinalize";
  case Opcode::INDGET: return "indget";
  case Opcode::INDPUT: return "indput";
  case Opcode::EVALKEY: return "evalkey";
  case Opcode::JMP: return "jmp";
  case Opcode::CALLFN: return "callfn";
  case Opcode::CALLCLOS: return "callclo";
  case Opcode::RETURN: return "return";
  case Opcode::MAKEFUNC: return "makefunc";
  case Opcode::MAKEGEN: return "makegen";
  case Opcode::REFLOAD: return "refload";
  case Opcode::REFSTORE: return "refstore";
  case Opcode::CMP: return "cmp";
  case Opcode::JE: return "je";
  case Opcode::JNE: return "jne";
  case Opcode::JG: return "jg";
  case Opcode::JGE: return "jge";
  case Opcode::JL: return "jl";
  case Opcode::JLE: return "jle";
  case Opcode::Nop: return "nop";
  case Opcode::EMIT: return "emit";
  case Opcode::DEEPCOPY: return "deepcopy";
  case Opcode::INDEXGET: return "indexget";
  case Opcode::INDEXPUT: return "indexput";
  case Opcode::EQF: return "eqf";
  case Opcode::LTF: return "ltf";
  case Opcode::GTF: return "gtf";
  case Opcode::LTEF: return "ltef";
  case Opcode::GTEF: return "gtef";
  case Opcode::TST: return "tst";
  default:
    return "???";
  }
}

} // namespace qlang
