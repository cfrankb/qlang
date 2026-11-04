// Escape-byte instruction encoding/decoding round-trip tests
// (docs/plans/instruction-format-escape-byte-implementation-plan.md Step 7b).
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "qlang/QLangBytecode.h"
#include "qlang/QLangErrors.h"

using namespace qlang;

static int passes = 0, fails = 0;

static void check(bool ok, const std::string &name) {
  if (ok) {
    std::cout << "[PASS] " << name << "\n";
    passes++;
  } else {
    std::cout << "[FAIL] " << name << "\n";
    fails++;
  }
  std::cout.flush();
}

// Encode a DecodedInstruction, decode it back, and verify the fields match.
static void roundTrip(const DecodedInstruction &in, const std::string &name,
                      size_t expectedSize) {
  std::vector<uint8_t> buf;
  try {
    encodeInstruction(buf, in);
  } catch (const QLangError &e) {
    check(false, name + " (encode threw: " + std::string(e.what()) + ")");
    return;
  }
  check(buf.size() == expectedSize,
        name + " size " + std::to_string(buf.size()) + " == " +
            std::to_string(expectedSize));
  if (buf.size() != expectedSize)
    return;

  DecodedInstruction out;
  try {
    decodeInstruction(buf.data(), 0, buf.size(), out);
  } catch (const QLangError &e) {
    check(false, name + " (decode threw: " + std::string(e.what()) + ")");
    return;
  }
  bool ok = out.size == expectedSize && out.op == in.op &&
            out.escaped == in.escaped && out.modifier == in.modifier;
  if (isJumpOpcode(in.op)) {
    ok = ok && out.jumpOffset == in.jumpOffset;
  } else if (isImmediateOpcode(in.op)) {
    ok = ok && out.a == in.a && out.imm == in.imm;
  } else {
    ok = ok && out.a == in.a && out.b == in.b && out.c == in.c;
  }
  check(ok, name + " fields");
}

// Verify that encoding throws for the given DecodedInstruction.
static void expectEncodeThrow(const DecodedInstruction &in,
                              const std::string &name) {
  std::vector<uint8_t> buf;
  bool threw = false;
  try {
    encodeInstruction(buf, in);
  } catch (const QLangError &) {
    threw = true;
  }
  check(threw, name);
}

// Verify that decoding throws for the given buffer.
static void expectDecodeThrow(const std::vector<uint8_t> &buf, size_t pc,
                              const std::string &name) {
  DecodedInstruction out;
  bool threw = false;
  try {
    decodeInstruction(buf.data(), pc, buf.size(), out);
  } catch (const QLangError &) {
    threw = true;
  }
  check(threw, name);
}

int main() {
  // --- Base form round-trips ---

  // Base MOV 1, 2 -> 3 bytes (op + 1B a + 1B b).
  {
    DecodedInstruction di;
    di.op = Opcode::MOV;
    di.escaped = false;
    di.a = 1;
    di.b = 2;
    roundTrip(di, "base MOV 1,2", 3);
  }

  // Base MOVI 0, 42 -> 3 bytes (op + 1B reg + 1B imm).
  {
    DecodedInstruction di;
    di.op = Opcode::MOVI;
    di.escaped = false;
    di.a = 0;
    di.imm = 42;
    roundTrip(di, "base MOVI 0,42", 3);
  }

  // Base JMP offset 5 -> 2 bytes (op + 1B offset).
  {
    DecodedInstruction di;
    di.op = Opcode::JMP;
    di.escaped = false;
    di.jumpOffset = 5;
    roundTrip(di, "base JMP off=5", 2);
  }

  // --- Escaped form round-trips ---

  // Escaped MOV 1, 300 (2-byte b) -> 5 bytes (op|0x80 + mod + 1B a + 2B b).
  // modifier = 0x10: leftForm=0 (1B a), rightForm=1 (2B b).
  {
    DecodedInstruction di;
    di.op = Opcode::MOV;
    di.escaped = true;
    di.modifier = 0x10;
    di.a = 1;
    di.b = 300;
    roundTrip(di, "escaped MOV 1,300", 5);
  }

  // Escaped MOVI 0, 200 (2-byte imm) -> 5 bytes (op|0x80 + mod + 1B reg + 2B imm).
  // modifier = 0x50: leftForm=0 (1B reg), immForm=5 (2B imm).
  {
    DecodedInstruction di;
    di.op = Opcode::MOVI;
    di.escaped = true;
    di.modifier = 0x50;
    di.a = 0;
    di.imm = 200;
    roundTrip(di, "escaped MOVI 0,200", 5);
  }

  // Escaped MOVI 0, 70000 (4-byte imm) -> 7 bytes (op|0x80 + mod + 1B reg + 4B imm).
  // modifier = 0x60: leftForm=0 (1B reg), immForm=6 (4B imm).
  {
    DecodedInstruction di;
    di.op = Opcode::MOVI;
    di.escaped = true;
    di.modifier = 0x60;
    di.a = 0;
    di.imm = 70000;
    roundTrip(di, "escaped MOVI 0,70000", 7);
  }

  // Escaped JMP offset 500 (jmp16) -> 4 bytes (op|0x80 + mod + 2B offset).
  // modifier = 0: jmp16.
  {
    DecodedInstruction di;
    di.op = Opcode::JMP;
    di.escaped = true;
    di.modifier = 0;
    di.jumpOffset = 500;
    roundTrip(di, "escaped JMP off=500 (jmp16)", 4);
  }

  // Escaped JMP offset 100000 (jmp32) -> 6 bytes (op|0x80 + mod + 4B offset).
  // modifier = 1: jmp32.
  {
    DecodedInstruction di;
    di.op = Opcode::JMP;
    di.escaped = true;
    di.modifier = 1;
    di.jumpOffset = 100000;
    roundTrip(di, "escaped JMP off=100000 (jmp32)", 6);
  }

  // --- 3-operand escaped (ADD with 2-byte a) ---

  // Escaped ADD 300, 1, 2 -> 6 bytes (op|0x80 + mod + 2B a + 1B b + 1B c).
  // modifier = 0x01: leftForm=1 (2B a), rightForm=0 (1B b).
  {
    DecodedInstruction di;
    di.op = Opcode::ADD;
    di.escaped = true;
    di.modifier = 0x01;
    di.a = 300;
    di.b = 1;
    di.c = 2;
    roundTrip(di, "escaped ADD 300,1,2", 6);
  }

  // --- Invalid: truncated buffer ---

  // Encode a base MOV (3 bytes), then decode from a 2-byte buffer.
  {
    DecodedInstruction di;
    di.op = Opcode::MOV;
    di.escaped = false;
    di.a = 1;
    di.b = 2;
    std::vector<uint8_t> buf;
    encodeInstruction(buf, di);
    // Truncate to 2 bytes (missing the last operand byte).
    expectDecodeThrow({buf[0], buf[1]}, 0, "truncated MOV buffer");
  }

  // --- Invalid: reserved imm form 0 ---

  // Escaped MOVI with modifier=0x00: leftForm=0 (valid), immForm=0 (reserved).
  {
    DecodedInstruction di;
    di.op = Opcode::MOVI;
    di.escaped = true;
    di.modifier = 0x00;
    di.a = 0;
    di.imm = 42;
    expectEncodeThrow(di, "reserved imm form 0");
  }

  // --- Invalid: reserved reg form 2 ---

  // Escaped MOV with modifier=0x02: leftForm=2 (reserved), rightForm=0 (valid).
  {
    DecodedInstruction di;
    di.op = Opcode::MOV;
    di.escaped = true;
    di.modifier = 0x02;
    di.a = 1;
    di.b = 2;
    expectEncodeThrow(di, "reserved reg form 2");
  }

  // --- Size helpers ---

  check(baseInstructionSize(Opcode::MOV) == 3, "baseInstructionSize MOV == 3");
  check(baseInstructionSize(Opcode::MOVI) == 3, "baseInstructionSize MOVI == 3");
  check(baseInstructionSize(Opcode::JMP) == 2, "baseInstructionSize JMP == 2");
  check(baseInstructionSize(Opcode::ADD) == 4, "baseInstructionSize ADD == 4");
  check(escapedInstructionSize(Opcode::MOV, 0x10) == 5,
        "escapedInstructionSize MOV 0x10 == 5");
  check(escapedInstructionSize(Opcode::MOVI, 0x50) == 5,
        "escapedInstructionSize MOVI 0x50 == 5");
  check(escapedInstructionSize(Opcode::MOVI, 0x60) == 7,
        "escapedInstructionSize MOVI 0x60 == 7");
  check(escapedInstructionSize(Opcode::JMP, 0) == 4,
        "escapedInstructionSize JMP 0 == 4");
  check(escapedInstructionSize(Opcode::JMP, 1) == 6,
        "escapedInstructionSize JMP 1 == 6");
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
  check(constantAddressSize(256) == 2, "constantAddressSize(256) == 2");

  std::cout << (fails == 0 ? "all encoding checks passed\n"
                           : "encoding checks FAILED\n");
  return fails == 0 ? 0 : 1;
}
