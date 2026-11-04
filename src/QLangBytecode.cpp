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
  case Opcode::JMPIFFALSE:
  case Opcode::JMPIFTRUE:
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

bool isImmediateOpcode(Opcode op) {
  switch (op) {
  case Opcode::MOVI:
  case Opcode::ADDI:
  case Opcode::SUBI:
  case Opcode::CMPI:
    return true;
  default:
    return false;
  }
}

bool isRegConstOpcode(Opcode op) {
  return !isJumpOpcode(op) && !isImmediateOpcode(op);
}

// Jumps that test a register: they carry a 1-byte condition register in
// addition to the offset, in every form (base and escaped). The jump-class
// modifier byte only selects the offset width, so the condition register has
// no form field of its own (like the 3-operand `c`).
static bool isConditionalJump(Opcode op) {
  return op == Opcode::JMPIFFALSE || op == Opcode::JMPIFTRUE;
}

// ---------------------------------------------------------------------------
// Size computation
// ---------------------------------------------------------------------------

// Number of operand bytes (excluding opcode byte) for a base-form instruction.
static size_t baseOperandCount(Opcode op) {
  if (isJumpOpcode(op)) {
    // JMPIFFALSE/JMPIFTRUE have a condition register + offset
    if (op == Opcode::JMPIFFALSE || op == Opcode::JMPIFTRUE)
      return 2; // reg + offset
    return 1; // offset only
  }
  if (isImmediateOpcode(op))
    return 2; // reg + imm
  // 1-operand
  switch (op) {
  case Opcode::INC:
  case Opcode::DEC:
  case Opcode::DELETEx:
  case Opcode::TABCREATE:
  case Opcode::TABFINALIZE:
  case Opcode::RETURN:
  case Opcode::YIELD:
    return 1;
  }
  // 2-operand (NEG/NOT/EVALKEY are r[a] = op(r[b]): the compiler emits them
  // with a=dest, b=src, so both operands must be stored). MAKEGEN and SEQGET
  // are 3-operand (MAKEGEN: c=argCount; SEQGET: c=index) and fall through to
  // the default below.
  switch (op) {
  case Opcode::MOV:
  case Opcode::MOVC:
  case Opcode::MAKEFUNC:
  case Opcode::REFLOAD:
  case Opcode::REFSTORE:
  case Opcode::CMP:
  case Opcode::ARRAPP:
  case Opcode::ARRLEN:
  case Opcode::LEN:
  case Opcode::ARRAPPV:
  case Opcode::NEG:
  case Opcode::NOT:
  case Opcode::EVALKEY:
    return 2;
  }
  // 3-operand (default): ADD..INx, CONV, CAST, ARRGET/ARRPUT, TABGET/TABPUT,
  // INDGET/INDPUT, CALL/CALLFN/CALLCLOS, MAKEGEN (c=argCount), SEQGET (c=index).
  return 3;
}

size_t baseInstructionSize(Opcode op) {
  return 1 + baseOperandCount(op);
}

size_t escapedInstructionSize(Opcode op, uint8_t modifier) {
  if (isJumpOpcode(op)) {
    if (modifier > 1)
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "invalid jump escape modifier");
    // op + modifier + offset (2 bytes for e=0/jmp16, 4 for e=1/jmp32);
    // conditional jumps add a 1-byte condition register.
    size_t size = 2 + (modifier == 0 ? 2 : 4);
    if (isConditionalJump(op))
      size += 1;
    return size;
  }
  if (isImmediateOpcode(op)) {
    uint8_t leftForm = modifier & 0x0F;
    uint8_t immForm = modifier >> 4;
    size_t leftSize = (leftForm == 0) ? 1 : (leftForm == 1) ? 2 : 0;
    size_t immSize = (immForm == 4) ? 1 : (immForm == 5) ? 2 : (immForm == 6) ? 4 : 0;
    if (leftSize == 0 || immSize == 0)
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "invalid immediate escape modifier");
    return 2 + leftSize + immSize;
  }
  // CALL: b is a signed 16-bit function ID (negative = native, index =
  // -1 - b; non-negative = builtin), not a register/constant index. It is
  // always escaped; the modifier's right field is fixed to 2 (the signed
  // 16-bit form) and the left field selects the `a` register width.
  if (op == Opcode::CALL) {
    uint8_t leftForm = modifier & 0x0F;
    uint8_t rightForm = modifier >> 4;
    size_t leftSize = (leftForm == 0) ? 1 : (leftForm == 1) ? 2 : 0;
    if (leftSize == 0 || rightForm != 2)
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "invalid CALL escape modifier");
    return 2 + leftSize + 2 + 1; // op+mod + a + b(2) + c(1)
  }
  // RegConst
  uint8_t leftForm = modifier & 0x0F;
  uint8_t rightForm = modifier >> 4;
  size_t leftSize = (leftForm == 0) ? 1 : (leftForm == 1) ? 2 : 0;
  size_t rightSize = (rightForm == 0) ? 1 : (rightForm == 1) ? 2 : 0;
  if (leftSize == 0 || rightSize == 0)
    throw QLangError(QLangErrorKind::Runtime, 0, 0,
                     "invalid regconst escape modifier");
  size_t third = 0;
  if (baseOperandCount(op) == 3)
    third = 1;
  return 2 + leftSize + rightSize + third;
}

size_t instructionSize(Opcode op, uint8_t modifier) {
  if (modifier == 0)
    return baseInstructionSize(op);
  return escapedInstructionSize(op, modifier);
}

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
// Encode
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

// Write a 1-, 2-, or 4-byte register/constant index in little-endian order.
static void writeIndex(std::vector<uint8_t> &out, int32_t v) {
  if (v < 0 || v > 65535)
    throw QLangError(QLangErrorKind::Runtime, 0, 0,
                     "operand index out of range");
  if (v <= 255) {
    out.push_back(static_cast<uint8_t>(v));
  } else {
    writeU16LE(out, static_cast<uint16_t>(v));
  }
}

void encodeInstruction(std::vector<uint8_t> &out, const DecodedInstruction &in) {
  uint8_t opByte = static_cast<uint8_t>(in.op);
  if (in.escaped)
    opByte |= kOpcodeEscape;
  out.push_back(opByte);
  if (in.escaped)
    out.push_back(in.modifier);

  if (isJumpOpcode(in.op)) {
    if (isConditionalJump(in.op)) {
      if (in.a < 0 || in.a > 255)
        throw QLangError(QLangErrorKind::Runtime, 0, 0,
                         "condition register out of range");
      out.push_back(static_cast<uint8_t>(in.a));
    }
    if (!in.escaped) {
      // jmp8: 1-byte signed offset
      if (in.jumpOffset < -128 || in.jumpOffset > 127)
        throw QLangError(QLangErrorKind::Runtime, 0, 0,
                         "jump offset does not fit in base form");
      out.push_back(static_cast<uint8_t>(in.jumpOffset & 0xFF));
    } else if (in.modifier == 0) {
      // jmp16: 2-byte signed offset
      if (in.jumpOffset < -32767 || in.jumpOffset > 32767)
        throw QLangError(QLangErrorKind::Runtime, 0, 0,
                         "jump offset does not fit in jmp16 form");
      writeU16LE(out, static_cast<uint16_t>(static_cast<int16_t>(in.jumpOffset)));
    } else if (in.modifier == 1) {
      // jmp32: 4-byte signed offset
      writeI32LE(out, in.jumpOffset);
    } else {
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "invalid jump escape modifier");
    }
    return;
  }

  if (isImmediateOpcode(in.op)) {
    if (!in.escaped) {
      // Base form: 1-byte register + 1-byte signed immediate
      if (in.a < 0 || in.a > 255)
        throw QLangError(QLangErrorKind::Runtime, 0, 0,
                         "register index out of range");
      if (in.imm < -128 || in.imm > 127)
        throw QLangError(QLangErrorKind::Runtime, 0, 0,
                         "immediate does not fit in base form");
      out.push_back(static_cast<uint8_t>(in.a));
      out.push_back(static_cast<uint8_t>(static_cast<int8_t>(in.imm)));
      return;
    }
    uint8_t leftForm = in.modifier & 0x0F;
    uint8_t immForm = in.modifier >> 4;
    if (leftForm > 1)
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "invalid immediate escape modifier");
    if (leftForm == 0) {
      if (in.a < 0 || in.a > 255)
        throw QLangError(QLangErrorKind::Runtime, 0, 0,
                         "register index out of range");
      out.push_back(static_cast<uint8_t>(in.a));
    } else {
      writeIndex(out, in.a);
    }
    if (immForm == 4) {
      if (in.imm < -128 || in.imm > 127)
        throw QLangError(QLangErrorKind::Runtime, 0, 0,
                         "immediate does not fit in 1-byte form");
      out.push_back(static_cast<uint8_t>(static_cast<int8_t>(in.imm)));
    } else if (immForm == 5) {
      if (in.imm < -32768 || in.imm > 32767)
        throw QLangError(QLangErrorKind::Runtime, 0, 0,
                         "immediate does not fit in 2-byte form");
      writeU16LE(out, static_cast<uint16_t>(static_cast<int16_t>(in.imm)));
    } else if (immForm == 6) {
      writeI32LE(out, static_cast<int32_t>(in.imm));
    } else {
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "invalid immediate escape modifier");
    }
    return;
  }

  // CALL: a = result register, b = signed 16-bit function ID, c = arg count.
  if (in.op == Opcode::CALL) {
    if (!in.escaped)
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "CALL must be escaped (signed function ID)");
    uint8_t leftForm = in.modifier & 0x0F;
    if (in.modifier >> 4 != 2)
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "invalid CALL escape modifier");
    if (leftForm == 0) {
      if (in.a < 0 || in.a > 255)
        throw QLangError(QLangErrorKind::Runtime, 0, 0,
                         "operand index out of range");
      out.push_back(static_cast<uint8_t>(in.a));
    } else {
      writeIndex(out, in.a);
    }
    if (in.b < INT16_MIN || in.b > INT16_MAX)
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "function ID out of range");
    writeU16LE(out, static_cast<uint16_t>(static_cast<int16_t>(in.b)));
    if (in.c < 0 || in.c > 255)
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "argument count out of range");
    out.push_back(static_cast<uint8_t>(in.c));
    return;
  }

  // RegConst
  size_t n = baseOperandCount(in.op);
  if (!in.escaped) {
    // Base form: every operand is a 1-byte index
    for (size_t i = 0; i < n; i++) {
      int32_t v = (i == 0) ? in.a : (i == 1) ? in.b : in.c;
      if (v < 0 || v > 255)
        throw QLangError(QLangErrorKind::Runtime, 0, 0,
                         "operand index out of range for base form");
      out.push_back(static_cast<uint8_t>(v));
    }
    return;
  }
  uint8_t leftForm = in.modifier & 0x0F;
  uint8_t rightForm = in.modifier >> 4;
  if (leftForm > 1 || rightForm > 1)
    throw QLangError(QLangErrorKind::Runtime, 0, 0,
                     "invalid regconst escape modifier");
  if (leftForm == 0) {
    if (in.a < 0 || in.a > 255)
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "operand index out of range");
    out.push_back(static_cast<uint8_t>(in.a));
  } else {
    writeIndex(out, in.a);
  }
  if (rightForm == 0) {
    if (in.b < 0 || in.b > 255)
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "operand index out of range");
    out.push_back(static_cast<uint8_t>(in.b));
  } else {
    writeIndex(out, in.b);
  }
  if (n == 3) {
    if (in.c < 0 || in.c > 255)
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "third operand must be a 1-byte register");
    out.push_back(static_cast<uint8_t>(in.c));
  }
}

// ---------------------------------------------------------------------------
// Decode
// ---------------------------------------------------------------------------

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

void decodeInstruction(const uint8_t *code, size_t pc, size_t codeSize,
                       DecodedInstruction &out) {
  if (pc >= codeSize)
    throw QLangError(QLangErrorKind::Io, 0, 0, "truncated instruction stream");
  uint8_t raw = code[pc];
  out.op = static_cast<Opcode>(raw & 0x7F);
  out.escaped = (raw & kOpcodeEscape) != 0;
  out.modifier = 0;
  out.a = 0;
  out.b = 0;
  out.c = 0;
  out.imm = 0;
  out.jumpOffset = 0;
  size_t pos = pc + 1;

  if (out.escaped) {
    if (pos >= codeSize)
      throw QLangError(QLangErrorKind::Io, 0, 0, "truncated modifier byte");
    out.modifier = code[pos++];
  }

  if (isJumpOpcode(out.op)) {
    if (isConditionalJump(out.op)) {
      if (pos >= codeSize)
        throw QLangError(QLangErrorKind::Io, 0, 0, "truncated condition register");
      out.a = code[pos++];
    }
    if (!out.escaped) {
      // jmp8: 1-byte signed offset
      if (pos >= codeSize)
        throw QLangError(QLangErrorKind::Io, 0, 0, "truncated jump offset");
      out.jumpOffset = static_cast<int8_t>(code[pos++]);
    } else if (out.modifier == 0) {
      // jmp16: 2-byte signed offset
      out.jumpOffset = static_cast<int16_t>(
          static_cast<uint16_t>(readULE(code, pos, 2, codeSize)));
      pos += 2;
    } else if (out.modifier == 1) {
      // jmp32: 4-byte signed offset
      out.jumpOffset = static_cast<int32_t>(readULE(code, pos, 4, codeSize));
      pos += 4;
    } else {
      throw QLangError(QLangErrorKind::Io, 0, 0, "invalid jump escape modifier");
    }
    out.size = pos - pc;
    return;
  }

  if (isImmediateOpcode(out.op)) {
    if (!out.escaped) {
      // Base form: 1-byte register + 1-byte signed immediate
      if (pos + 2 > codeSize)
        throw QLangError(QLangErrorKind::Io, 0, 0, "truncated immediate");
      out.a = code[pos];
      out.imm = static_cast<int8_t>(code[pos + 1]);
      pos += 2;
      out.size = pos - pc;
      return;
    }
    uint8_t leftForm = out.modifier & 0x0F;
    uint8_t immForm = out.modifier >> 4;
    if (leftForm > 1)
      throw QLangError(QLangErrorKind::Io, 0, 0,
                       "invalid immediate escape modifier");
    if (leftForm == 0) {
      if (pos >= codeSize)
        throw QLangError(QLangErrorKind::Io, 0, 0, "truncated register");
      out.a = code[pos++];
    } else {
      out.a = static_cast<uint32_t>(readULE(code, pos, 2, codeSize));
      pos += 2;
    }
    if (immForm == 4) {
      if (pos >= codeSize)
        throw QLangError(QLangErrorKind::Io, 0, 0, "truncated immediate");
      out.imm = static_cast<int8_t>(code[pos++]);
    } else if (immForm == 5) {
      out.imm = static_cast<int16_t>(
          static_cast<uint16_t>(readULE(code, pos, 2, codeSize)));
      pos += 2;
    } else if (immForm == 6) {
      out.imm = static_cast<int32_t>(readULE(code, pos, 4, codeSize));
      pos += 4;
    } else {
      throw QLangError(QLangErrorKind::Io, 0, 0,
                       "invalid immediate escape modifier");
    }
    out.size = pos - pc;
    return;
  }

  // CALL: a = result register, b = signed 16-bit function ID, c = arg count.
  if (out.op == Opcode::CALL) {
    if (!out.escaped)
      throw QLangError(QLangErrorKind::Io, 0, 0, "CALL must be escaped");
    uint8_t leftForm = out.modifier & 0x0F;
    if (out.modifier >> 4 != 2)
      throw QLangError(QLangErrorKind::Io, 0, 0,
                       "invalid CALL escape modifier");
    if (leftForm == 0) {
      if (pos >= codeSize)
        throw QLangError(QLangErrorKind::Io, 0, 0, "truncated operand");
      out.a = code[pos++];
    } else {
      out.a = static_cast<uint32_t>(readULE(code, pos, 2, codeSize));
      pos += 2;
    }
    out.b = static_cast<int16_t>(
        static_cast<uint16_t>(readULE(code, pos, 2, codeSize)));
    pos += 2;
    if (pos >= codeSize)
      throw QLangError(QLangErrorKind::Io, 0, 0, "truncated operand");
    out.c = code[pos++];
    out.size = pos - pc;
    return;
  }

  // RegConst
  size_t nOps = baseOperandCount(out.op);
  if (!out.escaped) {
    for (size_t i = 0; i < nOps; i++) {
      if (pos >= codeSize)
        throw QLangError(QLangErrorKind::Io, 0, 0, "truncated operand");
      uint8_t v = code[pos++];
      if (i == 0)
        out.a = v;
      else if (i == 1)
        out.b = v;
      else
        out.c = v;
    }
    out.size = pos - pc;
    return;
  }
  uint8_t leftForm = out.modifier & 0x0F;
  uint8_t rightForm = out.modifier >> 4;
  if (leftForm > 1 || rightForm > 1)
    throw QLangError(QLangErrorKind::Io, 0, 0,
                     "invalid regconst escape modifier");
  if (leftForm == 0) {
    if (pos >= codeSize)
      throw QLangError(QLangErrorKind::Io, 0, 0, "truncated operand");
    out.a = code[pos++];
  } else {
    out.a = static_cast<uint32_t>(readULE(code, pos, 2, codeSize));
    pos += 2;
  }
  if (rightForm == 0) {
    if (pos >= codeSize)
      throw QLangError(QLangErrorKind::Io, 0, 0, "truncated operand");
    out.b = code[pos++];
  } else {
    out.b = static_cast<uint32_t>(readULE(code, pos, 2, codeSize));
    pos += 2;
  }
  if (nOps == 3) {
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
    pushU32(static_cast<uint32_t>(b.size()));
    push(b.data(), b.size());
    break;
  }
  case T::Array:
  case T::Set:
  {
    const auto &a = v.asArray();
    pushU32(static_cast<uint32_t>(a.size()));
    for (const auto &e : a) {
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
  case Opcode::MOVC: return "movc";
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
  case Opcode::JMPIFFALSE: return "jmpfalse";
  case Opcode::JMPIFTRUE: return "jmptrue";
  case Opcode::CALLFN: return "callfn";
  case Opcode::CALLCLOS: return "callclo";
  case Opcode::RETURN: return "return";
  case Opcode::MAKEFUNC: return "makefunc";
  case Opcode::MAKEGEN: return "makegen";
  case Opcode::REFLOAD: return "refload";
  case Opcode::REFSTORE: return "refstore";
  case Opcode::MOVI: return "movi";
  case Opcode::ADDI: return "addi";
  case Opcode::SUBI: return "subi";
  case Opcode::CMP: return "cmp";
  case Opcode::CMPI: return "cmpi";
  case Opcode::JE: return "je";
  case Opcode::JNE: return "jne";
  case Opcode::JG: return "jg";
  case Opcode::JGE: return "jge";
  case Opcode::JL: return "jl";
  case Opcode::JLE: return "jle";
  default:
    return "???";
  }
}

} // namespace qlang
