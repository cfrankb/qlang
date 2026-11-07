// Instruction-encoding tests: the 2/2/4 modifier-byte field split, the
// right-operand kind table, the full immediate set (signed / unsigned / float /
// zero-byte bool and nil) and the in-place arity rule
// (docs/plans/encoding-v6-and-indirect-plan.md §2, verification §11 items 1-4
// and 7). This is the only codec; the retired two-nibble encoding and its suite
// were removed with `.qlc` v6 (plan §6 phase 1).
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "qlang/QLangBytecode.h"
#include "qlang/QLangErrors.h"

using namespace qlang;

static int passes = 0, fails = 0;

static void check(bool ok, const std::string &name) {
  if (ok) {
    passes++;
  } else {
    std::cout << "[FAIL] " << name << "\n";
    fails++;
  }
}

// True when `f` raises a QLangError.
template <typename F> static bool throws(F f) {
  try {
    f();
  } catch (const QLangError &) {
    return true;
  }
  return false;
}

static std::string caseName(const char *what, Opcode op, int e) {
  return std::string(what) + " (op=" + std::to_string(static_cast<int>(op)) +
         ", e=0x" + [&] {
               const char *hex = "0123456789ABCDEF";
               std::string s;
               s.push_back(hex[(e >> 4) & 0xF]);
               s.push_back(hex[e & 0xF]);
               return s;
             }() + ")";
}

static DecodedInstruction ins(Opcode op, bool escaped, uint8_t e) {
  DecodedInstruction d;
  d.op = op;
  d.escaped = escaped;
  d.modifier = escaped ? e : 0;
  return d;
}

// Operand count of an opcode's base form (op byte excluded).
static size_t operandCount(Opcode op) { return baseInstructionSize(op) - 1; }

// The kind a form means: the modifier's kind field when escaped, otherwise the
// kind fixed by the opcode identity (plan §2.4).
static RightKind kindOf(const DecodedInstruction &in) {
  if (!in.escaped || isJumpOpcode(in.op))
    return defaultRightKind(in.op);
  return modifierRightKind(in.modifier);
}

// The right-operand form of a form: the modifier's form field when escaped; a
// base form uses the 1-byte default (signed for an immediate class) (D3).
static uint8_t formOf(const DecodedInstruction &in) {
  if (in.escaped)
    return modifierRightForm(in.modifier);
  return kindOf(in) == RightKind::Immediate
             ? static_cast<uint8_t>(ImmForm::I8)
             : static_cast<uint8_t>(0);
}

static ImmPayload payloadFor(uint8_t form) {
  switch (immediateFormType(form)) {
  case ImmType::Unsigned:
  case ImmType::Qword:
    return ImmPayload::Unsigned;
  case ImmType::Int:
  case ImmType::Long:
    return ImmPayload::Signed;
  case ImmType::Float:
    return ImmPayload::Float;
  case ImmType::Bool:
    return form == static_cast<uint8_t>(ImmForm::True) ? ImmPayload::True
                                                       : ImmPayload::False;
  case ImmType::Nil:
    return ImmPayload::Nil;
  }
  return ImmPayload::None;
}

// Encode with the v6 codec, decode it back, and require that the size table,
// the encoder and the decoder agree byte-for-byte and that every field
// survives the trip.
static void roundTrip(const DecodedInstruction &in, const std::string &name,
                      size_t expectedSize) {
  size_t size = 0;
  try {
    size = instructionSize(in.op, in.escaped, in.modifier);
  } catch (const QLangError &e) {
    check(false, name + ": size threw (" + e.what() + ")");
    return;
  }
  check(size == expectedSize,
        name + ": size " + std::to_string(size) + " == " +
            std::to_string(expectedSize));

  std::vector<uint8_t> buf;
  try {
    encodeInstruction(buf, in);
  } catch (const QLangError &e) {
    check(false, name + ": encode threw (" + e.what() + ")");
    return;
  }
  check(buf.size() == expectedSize,
        name + ": encoded " + std::to_string(buf.size()) + " bytes == " +
            std::to_string(expectedSize));
  if (buf.size() != expectedSize)
    return;

  DecodedInstruction out;
  try {
    decodeInstruction(buf.data(), 0, buf.size(), out);
  } catch (const QLangError &e) {
    check(false, name + ": decode threw (" + e.what() + ")");
    return;
  }

  RightKind kind = kindOf(in);
  bool ok = out.size == expectedSize && out.op == in.op &&
            out.escaped == in.escaped && out.modifier == in.modifier &&
            out.rightKind == kind;
  if (isJumpOpcode(in.op)) {
    ok = ok && out.jumpOffset == in.jumpOffset && out.a == in.a;
  } else if (in.op == Opcode::CALL) {
    // The function id is an immediate, but it is carried in `b` and followed by
    // the ordinary 1-byte argument count (plan §2.8).
    ok = ok && out.a == in.a && out.b == in.b && out.c == in.c &&
         out.immPayload == ImmPayload::None;
  } else {
    ok = ok && out.a == in.a;
    if (kind == RightKind::Immediate) {
      ok = ok && out.immPayload == payloadFor(formOf(in));
      switch (out.immPayload) {
      case ImmPayload::Signed:
        ok = ok && out.imm == in.imm;
        break;
      case ImmPayload::Unsigned:
        ok = ok && out.immUnsigned == in.immUnsigned;
        break;
      case ImmPayload::Float:
        // Bit-exact: -0.0 and NaN must survive unchanged.
        ok = ok && 0 == std::memcmp(&out.immFloat, &in.immFloat,
                                    sizeof(double));
        break;
      default:
        break; // zero-byte forms carry the value in the payload tag
      }
    } else {
      ok = ok && out.b == in.b && out.immPayload == ImmPayload::None;
    }
    // The third operand is present unless the arity rule collapsed the op to
    // its in-place 2-operand form (plan §2.6).
    ok = ok && (operandCount(in.op) == 3 && !isInPlaceForm(in.op, kind)
                    ? out.c == in.c
                    : out.c == 0);
  }
  check(ok, name + ": fields after round-trip");
}

// Encode `in` and require exactly `expect` bytes. A round-trip alone cannot
// catch a layout change (encoder and decoder move together), so the base forms
// are pinned to literal wire bytes here.
static void bytesAre(const DecodedInstruction &in,
                     const std::vector<uint8_t> &expect,
                     const std::string &name) {
  std::vector<uint8_t> buf;
  try {
    encodeInstruction(buf, in);
  } catch (const QLangError &e) {
    check(false, name + ": encode threw (" + e.what() + ")");
    return;
  }
  check(buf == expect, name + ": wire bytes");
}

int main() {
  // --- 1. Modifier-byte field layout (plan §2.1, spec `### bytecode encoding`)
  check(makeModifier(OperandWidth::Byte, RightKind::Register, 0) == 0x04,
        "modifier: register right = 0x04");
  check(makeModifier(OperandWidth::Byte, RightKind::Immediate,
                     static_cast<uint8_t>(ImmForm::I8)) == 0x40,
        "modifier: immediate i8 = 0x40");
  check(makeModifier(OperandWidth::Byte, RightKind::ConstPool, 0) == 0x08,
        "modifier: constant right = 0x08");
  check(makeModifier(OperandWidth::Byte, RightKind::Immediate,
                     static_cast<uint8_t>(ImmForm::I16)) == 0x50,
        "modifier: immediate i16 = 0x50 (the CALL modifier)");
  check(makeModifier(OperandWidth::Word, RightKind::Immediate,
                     static_cast<uint8_t>(ImmForm::I16)) == 0x51,
        "modifier: word left + i16 = 0x51");
  check(makeModifier(OperandWidth::Byte, RightKind::Immediate,
                     static_cast<uint8_t>(ImmForm::U8)) == 0x00,
        "modifier: immediate u8 = 0x00 (legal, so 0 is not a base-form "
        "marker)");
  check(makeModifier(OperandWidth::Byte, RightKind::Immediate,
                     static_cast<uint8_t>(ImmForm::Nil)) == 0xB0,
        "modifier: immediate nil = 0xB0");
  check(makeModifier(OperandWidth::DWord, RightKind::ConstPool,
                     static_cast<uint8_t>(OperandWidth::Word)) == 0x1A,
        "modifier: dword left + word constant = 0x1A");

  // Every legal field triple survives assemble -> disassemble.
  int triples = 0;
  for (int i = 0; i <= 0xFF; i++) {
    uint8_t e = static_cast<uint8_t>(i);
    OperandWidth left = modifierLeftForm(e);
    RightKind kind = modifierRightKind(e);
    uint8_t form = modifierRightForm(e);
    if (left == OperandWidth::Reserved || kind == RightKind::Reserved)
      continue;
    if (kind == RightKind::Immediate ? form >= static_cast<uint8_t>(
                                           ImmForm::Reserved)
                                     : form > static_cast<uint8_t>(
                                           OperandWidth::DWord))
      continue;
    check(makeModifier(left, kind, form) == e,
          "modifier fields round-trip for e=0x" + std::to_string(i));
    triples++;
  }
  check(triples == 3 * (12 + 3 + 3),
        "modifier: 54 legal field triples enumerated, got " +
            std::to_string(triples));

  check(throws([] {
            return makeModifier(OperandWidth::Reserved, RightKind::Register,
                                0);
          }),
        "makeModifier rejects the reserved left form");
  check(throws([] {
            return makeModifier(OperandWidth::Byte, RightKind::Reserved, 0);
          }),
        "makeModifier rejects the reserved kind (D7)");
  for (int form = 12; form <= 15; form++)
    check(throws([form] {
              return makeModifier(OperandWidth::Byte, RightKind::Immediate,
                                  static_cast<uint8_t>(form));
            }),
            "makeModifier rejects reserved immediate form " +
                std::to_string(form));
  for (int form = 3; form <= 15; form++) {
    check(throws([form] {
              return makeModifier(OperandWidth::Byte, RightKind::Register,
                                  static_cast<uint8_t>(form));
            }),
            "makeModifier rejects register addressing form " +
                std::to_string(form));
    check(throws([form] {
              return makeModifier(OperandWidth::Byte, RightKind::ConstPool,
                                  static_cast<uint8_t>(form));
            }),
            "makeModifier rejects constant addressing form " +
                std::to_string(form));
  }

  // --- 2. Width and form tables (plan §2.1, §2.3)
  check(operandWidthBytes(OperandWidth::Byte) == 1 &&
            operandWidthBytes(OperandWidth::Word) == 2 &&
            operandWidthBytes(OperandWidth::DWord) == 4,
        "addressing widths select 1 / 2 / 4 bytes");
  check(throws([] { return operandWidthBytes(OperandWidth::Reserved); }),
        "operandWidthBytes rejects the reserved form");

  static const size_t kImmBytes[12] = {1, 2, 4, 8, 1, 2, 4, 8, 8, 0, 0, 0};
  static const ImmType kImmTypes[12] = {
      ImmType::Unsigned, ImmType::Unsigned, ImmType::Unsigned, ImmType::Qword,
      ImmType::Int,      ImmType::Int,      ImmType::Int,      ImmType::Long,
      ImmType::Float,    ImmType::Bool,     ImmType::Bool,     ImmType::Nil};
  for (int form = 0; form < 12; form++) {
    check(immediateFormBytes(static_cast<uint8_t>(form)) == kImmBytes[form],
          "immediate form " + std::to_string(form) + " is " +
              std::to_string(kImmBytes[form]) + " bytes");
    check(immediateFormType(static_cast<uint8_t>(form)) == kImmTypes[form],
          "immediate form " + std::to_string(form) + " has the expected type");
  }
  for (int form = 12; form <= 15; form++) {
    uint8_t f = static_cast<uint8_t>(form);
    check(throws([f] { return immediateFormBytes(f); }),
          "immediateFormBytes rejects reserved form " + std::to_string(form));
    check(throws([f] { return immediateFormType(f); }),
          "immediateFormType rejects reserved form " + std::to_string(form));
  }

  // --- 3. Base-form equivalence (plan §2.4)
  struct BaseCase {
    Opcode op;
    RightKind kind;
    uint8_t e;
  };
  static const BaseCase kBase[] = {
      {Opcode::MOV, RightKind::Register, 0x04},
      {Opcode::ADD, RightKind::Register, 0x04},
      {Opcode::CMP, RightKind::Register, 0x04},
      {Opcode::ARRGET, RightKind::Register, 0x04},
      {Opcode::RETURN, RightKind::Register, 0x04},
      {Opcode::MOVC, RightKind::ConstPool, 0x08},
      {Opcode::MAKEFUNC, RightKind::ConstPool, 0x08},
      {Opcode::MAKEGEN, RightKind::ConstPool, 0x08},
      {Opcode::MOVI, RightKind::Immediate, 0x40},
      {Opcode::ADDI, RightKind::Immediate, 0x40},
      {Opcode::SUBI, RightKind::Immediate, 0x40},
      {Opcode::CMPI, RightKind::Immediate, 0x40},
      {Opcode::CALL, RightKind::Immediate, 0x50},
  };
  for (const BaseCase &c : kBase) {
    check(defaultRightKind(c.op) == c.kind,
          "default kind of op " + std::to_string(static_cast<int>(c.op)));
    check(baseModifier(c.op) == c.e,
          "base modifier of op " + std::to_string(static_cast<int>(c.op)) +
              " == 0x40/0x08/0x04 class value");
  }
  check(baseModifier(Opcode::CALL) == 0x50, "CALL always carries e = 0x50");
  for (Opcode op : {Opcode::JMP, Opcode::JMPIFFALSE, Opcode::JMPIFTRUE,
                    Opcode::CONTINUE, Opcode::BREAK, Opcode::JE, Opcode::JNE,
                    Opcode::JG, Opcode::JGE, Opcode::JL, Opcode::JLE,
                    Opcode::Nop})
    check(baseModifier(op) == kNoBaseModifier,
          "no base modifier for op " + std::to_string(static_cast<int>(op)));
  // The kind a base form means must agree with the modifier it is equivalent
  // to, otherwise the two spellings of the same instruction decode differently.
  for (int i = 0; i < kOpcodeUsedCount; i++) {
    Opcode op = static_cast<Opcode>(i);
    uint8_t e = baseModifier(op);
    if (e == kNoBaseModifier)
      continue;
    check(modifierRightKind(e) == defaultRightKind(op),
          "base modifier of op " + std::to_string(i) + " names its kind");
  }

  // --- 4. Permitted kinds (plan §2.5)
  struct KindCase {
    Opcode op;
    uint8_t mask;
  };
  static const KindCase kKinds[] = {
      {Opcode::MOVI, 0x1},  {Opcode::ADDI, 0x1},
      {Opcode::SUBI, 0x1},  {Opcode::CMPI, 0x1},
      {Opcode::CALL, 0x1},  {Opcode::MOVC, 0x4},
      {Opcode::MAKEFUNC, 0x4}, {Opcode::MAKEGEN, 0x4},
      {Opcode::MOV, 0x7},   {Opcode::CONV, 0x7},
      {Opcode::NEG, 0x7},   {Opcode::DEEPCOPY, 0x7},
      {Opcode::ADD, 0x7},   {Opcode::SUB, 0x7},
      {Opcode::POW, 0x7},   {Opcode::INx, 0x7},
      {Opcode::CMP, 0x7},   {Opcode::EQ, 0x7},
      {Opcode::LAND, 0x7},  {Opcode::ARRGET, 0x2},
      {Opcode::TABPUT, 0x2}, {Opcode::INDEXGET, 0x2},
      {Opcode::INDGET, 0x2}, {Opcode::SEQGET, 0x2},
      {Opcode::REFLOAD, 0x2}, {Opcode::RETURN, 0x2},
      {Opcode::Nop, 0x2},   {Opcode::CALLFN, 0x2},
  };
  for (const KindCase &c : kKinds)
    check(permittedKinds(c.op) == c.mask,
          "permitted kinds of op " + std::to_string(static_cast<int>(c.op)) +
              " == mask " + std::to_string(c.mask));
  check(kindPermitted(Opcode::MOV, RightKind::Immediate) &&
            kindPermitted(Opcode::MOV, RightKind::Register) &&
            kindPermitted(Opcode::MOV, RightKind::ConstPool),
        "mov accepts all three kinds");
  check(!kindPermitted(Opcode::ARRGET, RightKind::Immediate) &&
            !kindPermitted(Opcode::ARRGET, RightKind::ConstPool),
        "array/table ops accept only a register");
  check(!kindPermitted(Opcode::MOVC, RightKind::Register),
        "movc accepts only a constant");
  check(!kindPermitted(Opcode::MOVI, RightKind::ConstPool),
        "movi accepts only an immediate");
  for (Opcode op : {Opcode::MOV, Opcode::ADD, Opcode::ARRGET, Opcode::Nop})
    check(!kindPermitted(op, RightKind::Reserved),
          "reserved kind is never permitted (op " +
              std::to_string(static_cast<int>(op)) + ")");

  // --- 5. In-place arity rule (plan §2.6)
  check(isInPlaceForm(Opcode::ADD, RightKind::Immediate) &&
            isInPlaceForm(Opcode::ADD, RightKind::ConstPool),
        "add with a non-register right operand is in-place");
  check(!isInPlaceForm(Opcode::ADD, RightKind::Register),
        "add with a register right operand keeps the third operand");
  check(isInPlaceForm(Opcode::CMP, RightKind::Immediate),
        "cmp with an immediate right operand is in-place");
  check(!isInPlaceForm(Opcode::MOV, RightKind::Immediate) &&
            !isInPlaceForm(Opcode::CONV, RightKind::Immediate) &&
            !isInPlaceForm(Opcode::MAKEGEN, RightKind::ConstPool),
        "non-ALU opcodes keep their third operand for every kind");

  // --- 6. Escaped-form sizes (plan §2.3, §2.6, §2.7, §2.8)
  struct SizeCase {
    Opcode op;
    uint8_t e;
    size_t size;
  };
  static const SizeCase kSizes[] = {
      // mov: op + e + a + right
      {Opcode::MOV, 0x04, 4},  {Opcode::MOV, 0x05, 5},  {Opcode::MOV, 0x06, 7},
      {Opcode::MOV, 0x14, 5},  {Opcode::MOV, 0x08, 4},  {Opcode::MOV, 0x18, 5},
      {Opcode::MOV, 0x28, 7},  {Opcode::MOV, 0x00, 4},  {Opcode::MOV, 0x10, 5},
      {Opcode::MOV, 0x20, 7},  {Opcode::MOV, 0x30, 11}, {Opcode::MOV, 0x40, 4},
      {Opcode::MOV, 0x50, 5},  {Opcode::MOV, 0x60, 7},  {Opcode::MOV, 0x70, 11},
      {Opcode::MOV, 0x80, 11}, {Opcode::MOV, 0x90, 3},  {Opcode::MOV, 0xA0, 3},
      {Opcode::MOV, 0xB0, 3},
      // add: a non-register right operand drops the trailing `c`
      {Opcode::ADD, 0x04, 5},  {Opcode::ADD, 0x14, 6},  {Opcode::ADD, 0x40, 4},
      {Opcode::ADD, 0x50, 5},  {Opcode::ADD, 0x80, 11}, {Opcode::ADD, 0x08, 4},
      {Opcode::ADD, 0xB0, 3},
      // 3-operand non-ALU opcodes keep `c` for every kind
      {Opcode::CONV, 0x40, 5},   {Opcode::CONV, 0x04, 5},
      {Opcode::SEQGET, 0x04, 5}, {Opcode::MAKEGEN, 0x08, 5},
      {Opcode::MAKEFUNC, 0x08, 4},
      // single-operand opcodes: only the left field is meaningful
      {Opcode::RETURN, 0x04, 3}, {Opcode::RETURN, 0x05, 4},
      {Opcode::INC, 0x06, 6},
      // call: op + e + a + 2-byte function id + 1-byte argument count
      {Opcode::CALL, 0x50, 6}, {Opcode::CALL, 0x51, 7},
      // jump class: e selects the offset width only
      {Opcode::JMP, 0x00, 4},  {Opcode::JMP, 0x01, 6},
      {Opcode::JE, 0x01, 6},   {Opcode::JMPIFFALSE, 0x00, 5},
      {Opcode::JMPIFTRUE, 0x01, 7},
  };
  for (const SizeCase &c : kSizes) {
    size_t size = 0;
    try {
      size = escapedInstructionSize(c.op, c.e);
    } catch (const QLangError &err) {
      check(false, caseName("escaped size rejected", c.op, c.e) + " (" +
                       err.what() + ")");
      continue;
    }
    check(size == c.size,
          caseName("escaped size", c.op, c.e) + " == " +
              std::to_string(c.size) + ", got " + std::to_string(size));
  }

  struct BadCase {
    Opcode op;
    uint8_t e;
    const char *why;
  };
  static const BadCase kBad[] = {
      {Opcode::MOV, 0x0C, "reserved kind"},
      {Opcode::ADD, 0x0F, "reserved kind"},
      {Opcode::MOV, 0x03, "reserved left form"},
      {Opcode::MOV, 0xC0, "reserved immediate form 12"},
      {Opcode::MOV, 0xF0, "reserved immediate form 15"},
      {Opcode::MOV, 0x34, "reserved register addressing form"},
      {Opcode::MOV, 0x38, "reserved constant addressing form"},
      {Opcode::ARRGET, 0x40, "immediate kind not permitted"},
      {Opcode::ARRGET, 0x08, "constant kind not permitted"},
      {Opcode::TABPUT, 0x00, "immediate kind not permitted"},
      {Opcode::MOVC, 0x04, "register kind not permitted for movc"},
      {Opcode::MOVI, 0x04, "register kind not permitted for movi"},
      {Opcode::CMPI, 0x08, "constant kind not permitted for cmpi"},
      {Opcode::MAKEGEN, 0x40, "immediate kind not permitted for makegen"},
      {Opcode::MAKEFUNC, 0x04, "register kind not permitted for makefunc"},
      {Opcode::Nop, 0x04, "zero-operand opcode cannot be escaped"},
      {Opcode::RETURN, 0x40, "single-operand opcode has no right operand"},
      {Opcode::INC, 0x0C, "single-operand opcode: reserved kind"},
      {Opcode::CALL, 0x40, "call form is fixed to 5"},
      {Opcode::CALL, 0x54, "call kind is fixed to immediate"},
      {Opcode::CALL, 0x0C, "call: reserved kind"},
      {Opcode::JMP, 0x02, "invalid jump modifier"},
      {Opcode::JMP, 0x40, "jump class ignores the 2/2/4 layout"},
      {Opcode::JMPIFFALSE, 0x03, "invalid jump modifier"},
  };
  for (const BadCase &c : kBad) {
    check(throws([&] { return escapedInstructionSize(c.op, c.e); }),
          caseName(c.why, c.op, c.e) + " rejected by the size table");
    // The same modifier must be rejected by the encoder and by the decoder, so
    // a malformed stream fails at load (plan §8, §11 item 7).
    DecodedInstruction d = ins(c.op, true, c.e);
    d.a = 1;
    d.b = 2;
    d.c = 3;
    d.jumpOffset = 7;
    check(throws([&] {
              std::vector<uint8_t> buf;
              encodeInstruction(buf, d);
            }),
          caseName(c.why, c.op, c.e) + " rejected by the encoder");
    std::vector<uint8_t> buf;
    buf.push_back(static_cast<uint8_t>(static_cast<uint8_t>(c.op) |
                                       kOpcodeEscape));
    buf.push_back(c.e);
    buf.resize(16, 0); // enough operand bytes to reach the validation
    check(throws([&] {
              DecodedInstruction out;
              decodeInstruction(buf.data(), 0, buf.size(), out);
            }),
          caseName(c.why, c.op, c.e) + " rejected by the decoder");
  }

  // --- 7. Base forms: literal wire bytes (op byte, then 1-byte operands)
  {
    DecodedInstruction d = ins(Opcode::MOV, false, 0);
    d.a = 3;
    d.b = 5;
    roundTrip(d, "base mov r3, r5", 3);
    bytesAre(d, {static_cast<uint8_t>(Opcode::MOV), 3, 5}, "mov");
  }
  {
    DecodedInstruction d = ins(Opcode::ADD, false, 0);
    d.a = 1;
    d.b = 2;
    d.c = 4;
    roundTrip(d, "base add r1, r2, r4", 4);
    bytesAre(d, {static_cast<uint8_t>(Opcode::ADD), 1, 2, 4}, "add");
  }
  {
    DecodedInstruction d = ins(Opcode::MOVI, false, 0);
    d.a = 2;
    d.imm = -5;
    roundTrip(d, "base movi r2, -5", 3);
    bytesAre(d, {static_cast<uint8_t>(Opcode::MOVI), 2, 0xFB}, "movi");
  }
  {
    DecodedInstruction d = ins(Opcode::MOVC, false, 0);
    d.a = 1;
    d.b = 9;
    roundTrip(d, "base movc r1, pool[9]", 3);
    bytesAre(d, {static_cast<uint8_t>(Opcode::MOVC), 1, 9}, "movc");
  }
  {
    DecodedInstruction d = ins(Opcode::ARRGET, false, 0);
    d.a = 1;
    d.b = 2;
    d.c = 3;
    roundTrip(d, "base arrget r1, r2, r3", 4);
    bytesAre(d, {static_cast<uint8_t>(Opcode::ARRGET), 1, 2, 3}, "arrget");
  }
  {
    DecodedInstruction d = ins(Opcode::SEQGET, false, 0);
    d.a = 4;
    d.b = 5;
    d.c = 6;
    roundTrip(d, "base seqget r4, r5, 6", 4);
    bytesAre(d, {static_cast<uint8_t>(Opcode::SEQGET), 4, 5, 6}, "seqget");
  }
  {
    DecodedInstruction d = ins(Opcode::RETURN, false, 0);
    d.a = 7;
    roundTrip(d, "base return r7", 2);
    bytesAre(d, {static_cast<uint8_t>(Opcode::RETURN), 7}, "return");
  }
  {
    DecodedInstruction d = ins(Opcode::Nop, false, 0);
    roundTrip(d, "base nop", 1);
    bytesAre(d, {static_cast<uint8_t>(Opcode::Nop)}, "nop");
  }
  {
    DecodedInstruction d = ins(Opcode::JMP, false, 0);
    d.jumpOffset = -3;
    roundTrip(d, "base jmp -3", 2);
    bytesAre(d, {static_cast<uint8_t>(Opcode::JMP), 0xFD}, "jmp");
  }
  {
    DecodedInstruction d = ins(Opcode::JMPIFFALSE, false, 0);
    d.a = 2;
    d.jumpOffset = 5;
    roundTrip(d, "base jmpfalse r2, +5", 3);
    bytesAre(d, {static_cast<uint8_t>(Opcode::JMPIFFALSE), 2, 5}, "jmpfalse");
  }

  // --- 8. Immediate forms, boundary values (plan §2.3, D8)
  struct ImmCase {
    uint8_t e;
    int64_t s;
    uint64_t u;
    double f;
    size_t size;
    const char *name;
  };
  static const ImmCase kImms[] = {
      {0x00, 0, 0, 0, 4, "mov u8 0"},
      {0x00, 0, 255, 0, 4, "mov u8 255"},
      {0x10, 0, 0, 0, 5, "mov u16 0"},
      {0x10, 0, 65535, 0, 5, "mov u16 65535"},
      {0x20, 0, 0, 0, 7, "mov u32 0"},
      {0x20, 0, 4294967295ULL, 0, 7, "mov u32 max"},
      {0x30, 0, 0, 0, 11, "mov u64 0"},
      {0x30, 0, 18446744073709551615ULL, 0, 11, "mov u64 max"},
      {0x40, -128, 0, 0, 4, "mov i8 -128"},
      {0x40, 127, 0, 0, 4, "mov i8 127"},
      {0x50, -32768, 0, 0, 5, "mov i16 -32768"},
      {0x50, 32767, 0, 0, 5, "mov i16 32767"},
      {0x60, INT64_C(-2147483648), 0, 0, 7, "mov i32 min"},
      {0x60, INT64_C(2147483647), 0, 0, 7, "mov i32 max"},
      {0x70, INT64_MIN, 0, 0, 11, "mov i64 min"},
      {0x70, INT64_MAX, 0, 0, 11, "mov i64 max"},
      {0x80, 0, 0, 1.5, 11, "mov float 1.5"},
      {0x80, 0, 0, -0.0, 11, "mov float -0.0"},
      {0x80, 0, 0, 3.141592653589793, 11, "mov float pi"},
      {0x80, 0, 0, 1e308, 11, "mov float 1e308"},
      {0x90, 0, 0, 0, 3, "mov false"},
      {0xA0, 0, 0, 0, 3, "mov true"},
      {0xB0, 0, 0, 0, 3, "mov nil"},
  };
  for (const ImmCase &c : kImms) {
    DecodedInstruction d = ins(Opcode::MOV, true, c.e);
    d.a = 12;
    d.imm = c.s;
    d.immUnsigned = c.u;
    d.immFloat = c.f;
    roundTrip(d, c.name, c.size);
  }

  // Zero-byte immediates: the form *is* the value, so `mov r12, nil` costs 3
  // bytes and touches no pool slot (plan §9).
  for (uint8_t e : {static_cast<uint8_t>(0x90), static_cast<uint8_t>(0xA0),
                    static_cast<uint8_t>(0xB0)}) {
    DecodedInstruction d = ins(Opcode::MOV, true, e);
    d.a = 12;
    std::vector<uint8_t> buf;
    encodeInstruction(buf, d);
    check(buf.size() == 3, "zero-byte immediate e=0x" +
                               std::to_string(e) + " encodes in 3 bytes");
    DecodedInstruction out;
    decodeInstruction(buf.data(), 0, buf.size(), out);
    check(out.immPayload != ImmPayload::None && out.rightKind ==
                                               RightKind::Immediate,
          "zero-byte immediate e=0x" + std::to_string(e) +
              " reports its payload and kind");
  }

  // The same immediates through the ALU families: the in-place form drops the
  // trailing `c`, so `add r1, nil` is 3 bytes.
  {
    DecodedInstruction d = ins(Opcode::ADD, true, 0x40);
    d.a = 1;
    d.imm = 7;
    d.c = 9; // ignored: the in-place form has no third operand
    roundTrip(d, "in-place addi r1, 7", 4);
  }
  {
    DecodedInstruction d = ins(Opcode::ADD, true, 0x80);
    d.a = 1;
    d.immFloat = 2.5;
    roundTrip(d, "in-place add r1, 2.5", 11);
  }
  {
    DecodedInstruction d = ins(Opcode::ADD, true, 0x08);
    d.a = 1;
    d.b = 6;
    roundTrip(d, "in-place addc r1, pool[6]", 4);
  }
  {
    DecodedInstruction d = ins(Opcode::CMP, true, 0x60);
    d.a = 3;
    d.imm = 1000;
    roundTrip(d, "cmpi r3, 1000", 7);
  }
  {
    DecodedInstruction d = ins(Opcode::ADD, true, 0x04);
    d.a = 1;
    d.b = 2;
    d.c = 3;
    roundTrip(d, "escaped add r1, r2, r3", 5);
  }

  // Non-ALU 3-operand opcodes keep the third operand for every kind.
  {
    DecodedInstruction d = ins(Opcode::CONV, true, 0x40);
    d.a = 1;
    d.imm = 4;
    d.c = 7; // the target type id survives
    roundTrip(d, "conv r1, 4, type 7", 5);
  }
  {
    DecodedInstruction d = ins(Opcode::MAKEGEN, true, 0x08);
    d.a = 1;
    d.b = 6;
    d.c = 2; // the argument count survives
    roundTrip(d, "makegen r1, pool[6], 2 args", 5);
  }
  {
    DecodedInstruction d = ins(Opcode::SEQGET, true, 0x04);
    d.a = 1;
    d.b = 2;
    d.c = 3; // the index survives
    roundTrip(d, "escaped seqget r1, r2, 3", 5);
  }

  // --- 9. Extended addressing widths
  {
    DecodedInstruction d = ins(Opcode::MOV, true, 0x05);
    d.a = 300;
    d.b = 5;
    roundTrip(d, "mov r300, r5 (word left)", 5);
  }
  {
    DecodedInstruction d = ins(Opcode::MOV, true, 0x14);
    d.a = 3;
    d.b = 40000;
    roundTrip(d, "mov r3, r40000 (word right)", 5);
  }
  {
    DecodedInstruction d = ins(Opcode::MOV, true, 0x06);
    d.a = 100000;
    d.b = 5;
    roundTrip(d, "mov r100000, r5 (dword left)", 7);
  }
  {
    DecodedInstruction d = ins(Opcode::MOV, true, 0x28);
    d.a = 1;
    d.b = 100000;
    roundTrip(d, "mov r1, pool[100000] (dword constant)", 7);
  }
  {
    DecodedInstruction d = ins(Opcode::MOVC, true, 0x08);
    d.a = 1;
    d.b = 200;
    roundTrip(d, "escaped movc r1, pool[200]", 4);
  }
  {
    // e = 0x15: word left, register right, word right; `c` stays 1 byte.
    DecodedInstruction d = ins(Opcode::ADD, true, 0x15);
    d.a = 300;
    d.b = 400;
    d.c = 5;
    roundTrip(d, "escaped add r300, r400, r5", 7);
  }

  // --- 10. CALL (plan §2.8)
  {
    DecodedInstruction d = ins(Opcode::CALL, true, 0x50);
    d.a = 5;
    d.b = -1;
    d.c = 3;
    roundTrip(d, "call r5, native 0, 3 args", 6);
    std::vector<uint8_t> buf;
    encodeInstruction(buf, d);
    check(buf.size() == 6 && buf[0] == (static_cast<uint8_t>(Opcode::CALL) |
                                        kOpcodeEscape) &&
              buf[1] == 0x50 && buf[2] == 5 && buf[5] == 3,
          "call encodes as op|0x80, 0x50, a, id(2B), argc");
  }
  {
    DecodedInstruction d = ins(Opcode::CALL, true, 0x50);
    d.a = 0;
    d.b = INT16_MIN;
    d.c = 0;
    roundTrip(d, "call r0, INT16_MIN, 0 args", 6);
  }
  {
    DecodedInstruction d = ins(Opcode::CALL, true, 0x51);
    d.a = 300;
    d.b = 7;
    d.c = 0;
    roundTrip(d, "call r300, builtin 7 (word left)", 7);
  }
  check(throws([] {
            DecodedInstruction d = ins(Opcode::CALL, false, 0);
            d.a = 1;
            d.b = 2;
            std::vector<uint8_t> buf;
            encodeInstruction(buf, d);
          }),
        "an unescaped call is rejected by the encoder");
  check(throws([] {
            std::vector<uint8_t> buf = {static_cast<uint8_t>(Opcode::CALL), 1,
                                        2, 0, 0};
            DecodedInstruction out;
            decodeInstruction(buf.data(), 0, buf.size(), out);
          }),
        "an unescaped call is rejected by the decoder");

  // --- 11. Jump class keeps the historical widths (plan §2.7)
  {
    DecodedInstruction d = ins(Opcode::JMP, true, 0x00);
    d.jumpOffset = -32767;
    roundTrip(d, "jmp16 -32767", 4);
  }
  {
    DecodedInstruction d = ins(Opcode::JMP, true, 0x00);
    d.jumpOffset = 32767;
    roundTrip(d, "jmp16 +32767", 4);
  }
  {
    DecodedInstruction d = ins(Opcode::JMP, true, 0x01);
    d.jumpOffset = INT32_MIN;
    roundTrip(d, "jmp32 INT32_MIN", 6);
  }
  {
    DecodedInstruction d = ins(Opcode::JMP, true, 0x01);
    d.jumpOffset = INT32_MAX;
    roundTrip(d, "jmp32 INT32_MAX", 6);
  }
  {
    DecodedInstruction d = ins(Opcode::JMPIFFALSE, true, 0x00);
    d.a = 200;
    d.jumpOffset = -2;
    roundTrip(d, "jmpfalse r200, -2 (jmp16)", 5);
  }
  {
    DecodedInstruction d = ins(Opcode::JLE, true, 0x01);
    d.jumpOffset = 1000;
    roundTrip(d, "jle +1000 (jmp32)", 6);
  }
  for (Opcode op : {Opcode::JMP, Opcode::JE, Opcode::JNE, Opcode::JG,
                    Opcode::JGE, Opcode::JL, Opcode::JLE, Opcode::CONTINUE,
                    Opcode::BREAK}) {
    for (int32_t off : {INT32_MIN, -40000, 40000, INT32_MAX}) {
      DecodedInstruction d = ins(op, true, 0x01);
      d.jumpOffset = off;
      roundTrip(d, "jmp32 class op " + std::to_string(static_cast<int>(op)),
                6);
    }
  }

  // --- 12. Encoder range checks
  struct RejectCase {
    Opcode op;
    bool escaped;
    uint8_t e;
    int64_t a, b, c, imm;
    uint64_t u;
    int32_t off;
    const char *why;
  };
  static const RejectCase kRejects[] = {
      {Opcode::MOV, false, 0, 300, 0, 0, 0, 0, 0, "base form register > 255"},
      {Opcode::MOVI, false, 0, 2, 0, 0, 200, 0, 0, "base immediate > i8"},
      {Opcode::MOVI, false, 0, 2, 0, 0, -129, 0, 0, "base immediate < i8"},
      {Opcode::MOV, true, 0x04, 1, 300, 0, 0, 0, 0, "1-byte right > 255"},
      {Opcode::MOV, true, 0x05, 70000, 0, 0, 0, 0, 0, "2-byte left > 65535"},
      {Opcode::MOV, true, 0x06, -1, 0, 0, 0, 0, 0, "negative left index"},
      {Opcode::MOV, true, 0x14, 1, -1, 0, 0, 0, 0, "negative right index"},
      {Opcode::MOV, false, 0, -1, 0, 0, 0, 0, 0, "negative base-form index"},
      {Opcode::MOV, true, 0x40, 1, 0, 0, 200, 0, 0, "i8 immediate > 127"},
      {Opcode::MOV, true, 0x40, 1, 0, 0, -129, 0, 0, "i8 immediate < -128"},
      {Opcode::MOV, true, 0x50, 1, 0, 0, 40000, 0, 0, "i16 immediate > 32767"},
      {Opcode::MOV, true, 0x60, 1, 0, 0, INT64_C(4294967296), 0, 0,
       "i32 immediate out of range"},
      {Opcode::MOV, true, 0x00, 1, 0, 0, 0, 256, 0, "u8 immediate > 255"},
      {Opcode::MOV, true, 0x10, 1, 0, 0, 0, 65536, 0, "u16 immediate > 65535"},
      {Opcode::MOV, true, 0x20, 1, 0, 0, 0, INT64_C(4294967296), 0,
       "u32 immediate out of range"},
      {Opcode::ADD, true, 0x04, 1, 2, 300, 0, 0, 0, "third operand > 255"},
      {Opcode::CALL, true, 0x50, 1, 0, 0, 0, 0, 0, "function id > INT16_MAX"},
      {Opcode::JMP, false, 0, 0, 0, 0, 0, 0, 200, "jmp8 offset > 127"},
      {Opcode::JMP, true, 0x00, 0, 0, 0, 0, 0, 40000,
       "jmp16 offset out of range"},
  };
  for (const RejectCase &c : kRejects) {
    DecodedInstruction d = ins(c.op, c.escaped, c.e);
    d.a = static_cast<int32_t>(c.a);
    d.b = static_cast<int32_t>(c.b);
    d.c = static_cast<int32_t>(c.c);
    d.imm = c.imm;
    d.immUnsigned = c.u;
    d.jumpOffset = c.off;
    if (c.op == Opcode::CALL)
      d.b = INT32_C(32768); // the one case that needs a distinct value
    check(throws([&] {
              std::vector<uint8_t> buf;
              encodeInstruction(buf, d);
            }),
          std::string(c.why) + " rejected by the encoder");
  }

  // --- 13. Truncated streams fail instead of reading past the buffer
  struct TruncCase {
    std::vector<uint8_t> buf;
    const char *why;
  };
  const uint8_t movEsc =
      static_cast<uint8_t>(static_cast<uint8_t>(Opcode::MOV) | kOpcodeEscape);
  const uint8_t addEsc =
      static_cast<uint8_t>(static_cast<uint8_t>(Opcode::ADD) | kOpcodeEscape);
  const uint8_t jmpEsc =
      static_cast<uint8_t>(static_cast<uint8_t>(Opcode::JMP) | kOpcodeEscape);
  const uint8_t callEsc =
      static_cast<uint8_t>(static_cast<uint8_t>(Opcode::CALL) | kOpcodeEscape);
  static const TruncCase kTruncs[] = {
      {{movEsc}, "missing modifier byte"},
      {{movEsc, 0x60, 1, 0}, "truncated i32 immediate"},
      {{movEsc, 0x80, 1, 0, 0, 0, 0, 0, 0}, "truncated float immediate"},
      {{movEsc, 0xA0}, "missing operand byte of a zero-byte immediate"},
      {{movEsc, 0x05, 1}, "truncated extended index"},
      {{addEsc, 0x04, 1, 2}, "truncated third operand"},
      {{jmpEsc, 0x01, 0, 0, 0}, "truncated jmp32 offset"},
      {{callEsc, 0x50, 5, 0, 0}, "truncated call argument count"},
      {{callEsc, 0x50, 5, 0}, "truncated call function id"},
  };
  for (const TruncCase &c : kTruncs)
    check(throws([&] {
              DecodedInstruction out;
              decodeInstruction(c.buf.data(), 0, c.buf.size(), out);
            }),
          std::string(c.why) + " rejected by the decoder");

  // --- 14. Exhaustive size / encode / decode agreement (plan §10)
  // For every opcode and every modifier the size table accepts, the encoder
  // must emit exactly that many bytes and the decoder must consume them.
  int pairs = 0;
  for (int i = 0; i < kOpcodeUsedCount; i++) {
    Opcode op = static_cast<Opcode>(i);
    for (int j = 0; j <= 0xFF; j++) {
      uint8_t e = static_cast<uint8_t>(j);
      size_t size = 0;
      try {
        size = escapedInstructionSize(op, e);
      } catch (const QLangError &) {
        continue; // the table rejects the pair; encoder and decoder must too
      }
      DecodedInstruction d = ins(op, true, e);
      d.a = 1;
      d.b = 2;
      d.c = 3;
      d.imm = 5;
      d.immUnsigned = 5;
      d.immFloat = 1.5;
      d.jumpOffset = 7;
      std::vector<uint8_t> buf;
      try {
        encodeInstruction(buf, d);
      } catch (const QLangError &err) {
        check(false, caseName("size accepts but encode rejects", op, j) +
                         " (" + err.what() + ")");
        continue;
      }
      check(buf.size() == size,
            caseName("encoded length != table size", op, j));
      DecodedInstruction out;
      try {
        decodeInstruction(buf.data(), 0, buf.size(), out);
      } catch (const QLangError &err) {
        check(false, caseName("encode accepts but decode rejects", op, j) +
                         " (" + err.what() + ")");
        continue;
      }
      check(out.size == size, caseName("decoded length != table size", op, j));
      // The jump class does not use the 2/2/4 layout, so its kind stays the one
      // the opcode identity means.
      RightKind want =
          isJumpOpcode(op) ? defaultRightKind(op) : modifierRightKind(e);
      check(out.rightKind == want, caseName("decoded kind != modifier kind",
                                            op, j));
      pairs++;
    }
  }
  std::cout << "exhaustive (opcode, modifier) pairs validated: " << pairs
            << "\n";

  // A rejected pair must also be rejected by both directions, so no path can
  // desynchronise `pc`.
  int rejected = 0;
  for (int i = 0; i < kOpcodeUsedCount; i++) {
    Opcode op = static_cast<Opcode>(i);
    if (isJumpOpcode(op))
      continue; // the jump modifier is a width selector, not a 2/2/4 field
    for (int j = 0; j <= 0xFF; j++) {
      uint8_t e = static_cast<uint8_t>(j);
      bool sizeOk = true;
      try {
        escapedInstructionSize(op, e);
      } catch (const QLangError &) {
        sizeOk = false;
      }
      if (sizeOk)
        continue;
      DecodedInstruction d = ins(op, true, e);
      d.a = 1;
      d.b = 2;
      d.c = 3;
      check(throws([&] {
                std::vector<uint8_t> buf;
                encodeInstruction(buf, d);
              }),
            caseName("encoder accepted a pair the table rejects", op, j));
      std::vector<uint8_t> buf(16, 0);
      buf[0] = static_cast<uint8_t>(static_cast<uint8_t>(op) | kOpcodeEscape);
      buf[1] = e;
      check(throws([&] {
                DecodedInstruction out;
                decodeInstruction(buf.data(), 0, buf.size(), out);
              }),
            caseName("decoder accepted a pair the table rejects", op, j));
      rejected++;
    }
  }
  std::cout << "rejected (opcode, modifier) pairs cross-checked: " << rejected
            << "\n";

  // --- 15. Address-width and jump-width helpers (carried over from the
  // retired encoding suite; the emitter picks operand and offset widths with
  // these, so they are part of the encoding contract).
  check(baseInstructionSize(Opcode::MOV) == 3, "baseInstructionSize MOV == 3");
  check(baseInstructionSize(Opcode::MOVI) == 3, "baseInstructionSize MOVI == 3");
  check(baseInstructionSize(Opcode::JMP) == 2, "baseInstructionSize JMP == 2");
  check(baseInstructionSize(Opcode::ADD) == 4, "baseInstructionSize ADD == 4");
  check(minJumpSize(5) == 2, "minJumpSize(5) == 2");
  check(minJumpSize(500) == 4, "minJumpSize(500) == 4");
  check(minJumpSize(100000) == 6, "minJumpSize(100000) == 6");
  check(minJumpSize(-127) == 2, "minJumpSize(-127) == 2");
  check(minJumpSize(-128) == 4, "minJumpSize(-128) == 4");
  check(registerAddressSize(0) == 1, "registerAddressSize(0) == 1");
  check(registerAddressSize(255) == 1, "registerAddressSize(255) == 1");
  check(registerAddressSize(256) == 2, "registerAddressSize(256) == 2");
  check(registerAddressSize(65535) == 2, "registerAddressSize(65535) == 2");
  check(constantAddressSize(0) == 1, "constantAddressSize(0) == 1");
  check(constantAddressSize(255) == 1, "constantAddressSize(255) == 1");
  check(constantAddressSize(256) == 2, "constantAddressSize(256) == 2");

  std::cout << (fails == 0 ? "ALL PASS" : "FAILURES") << ": " << passes
            << " passed, " << fails << " failed\n";
  return fails == 0 ? 0 : 1;
}
