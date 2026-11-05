// .qlc codec check: value round-trips, program round-trip with native wiring,
// and a malformed-input robustness suite (docs/SERIALIZATION.md §9).
#include <climits>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "qlang/QLangBytecodeIO.h"
#include "qlang/QLangCompiler.h"
#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"
#include "qlang/QLangVM.h"

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

// Deep value equality (QLangValue has no operator==).
static bool valueEq(const QLangValue &x, const QLangValue &y) {
  if (x.type() != y.type())
    return false;
  using T = QLangValue::Type;
  switch (x.type()) {
  case T::Nil:
  case T::Eof:
    return true;
  case T::Bool:
  case T::Int:
    return x.asInt() == y.asInt();
  case T::Char:
    return x.asChar() == y.asChar();
  case T::Byte:
    return x.asByte() == y.asByte();
  case T::Word:
    return x.asWord() == y.asWord();
  case T::Short:
    return x.asShort() == y.asShort();
  case T::Unsigned:
    return x.asUnsigned() == y.asUnsigned();
  case T::Qword:
    return x.asQword() == y.asQword();
  case T::Long:
    return x.asLong() == y.asLong();
  case T::Float:
    return x.asFloat() == y.asFloat();
  case T::String:
    return x.asString() == y.asString();
  case T::Blob:
    return *x.asBlob() == *y.asBlob();
  case T::Array:
  case T::Set: {
    const auto &a = *((x.type() == T::Array) ? x.asArray() : x.asSet());
    const auto &b = *((y.type() == T::Array) ? y.asArray() : y.asSet());
    if (a.size() != b.size())
      return false;
    for (size_t i = 0; i < a.size(); i++)
      if (!valueEq(a[i], b[i]))
        return false;
    return true;
  }
  case T::Table: {
    const auto &a = x.asTable();
    const auto &b = y.asTable();
    if (a->size() != b->size())
      return false;
    for (const auto &[k, v] : *a) {
      auto it = b->find(k);
      if (it == b->end() || !valueEq(v, it->second))
        return false;
    }
    return true;
  }
  case T::Function: {
    const auto &a = x.asFunction();
    const auto &b = y.asFunction();
    if (a.funcIndex != b.funcIndex || a.captures.size() != b.captures.size())
      return false;
    for (size_t i = 0; i < a.captures.size(); i++)
      if (a.captures[i].reg != b.captures[i].reg)
        return false;
    return true;
  }
  case T::Generator:
  case T::Object:
    return false; // not durable
  }
  return false;
}

static QLangValue roundTrip(const QLangValue &v) {
  std::ostringstream os(std::ios::binary);
  serializeValue(os, v);
  std::istringstream is(os.str(), std::ios::binary);
  return deserializeValue(is);
}

static void testValues() {
  struct Case {
    const char *name;
    QLangValue v;
  };
  auto mkTable = [] {
    auto m = std::make_shared<std::map<std::string, QLangValue>>();
    (*m)["a"] = QLangValue::Int(1);
    (*m)["b"] = QLangValue::String("two");
    (*m)["c"] = QLangValue::Array({QLangValue::Bool(1), QLangValue()});
    return QLangValue::Table(std::move(m));
  };
  auto closure = std::make_shared<Closure>();
  closure->funcIndex = 3;
  CaptureRef capA;
  capA.reg = 2;
  CaptureRef capB;
  capB.reg = 7;
  closure->captures.push_back(capA);
  closure->captures.push_back(capB);

  Case cases[] = {
      {"nil", QLangValue()},
      {"eof", QLangValue(QLangValue::Type::Eof)},
      {"bool true", QLangValue::Bool(1)},
      {"bool false", QLangValue::Bool(0)},
      {"int 0", QLangValue::Int(0)},
      {"int min", QLangValue::Int(INT32_MIN)},
      {"int max", QLangValue::Int(INT32_MAX)},
      {"char", QLangValue::Char('A')},
      {"byte 255", QLangValue::Byte(255)},
      {"word 0xFFFF", QLangValue::Word(0xFFFF)},
      {"short", QLangValue::Short(-12345)},
      {"unsigned max", QLangValue::Unsigned(0xFFFFFFFFu)},
      {"qword max", QLangValue::Qword(UINT64_MAX)},
      {"long max", QLangValue::Long(INT64_MAX)},
      {"long negative", QLangValue::Long(-5)},
      {"float pi", QLangValue::Float(3.14159)},
      {"float neg zero", QLangValue::Float(-0.0)},
      {"float big", QLangValue::Float(1e300)},
      {"string empty", QLangValue::String("")},
      {"string", QLangValue::String("héllo wörld\n")},
      {"blob empty", QLangValue::Blob()},
      {"blob", QLangValue::Blob(std::vector<uint8_t>{0, 1, 2, 255})},
      {"array nested",
       QLangValue::Array({QLangValue::Int(1),
                          QLangValue::Array({QLangValue::String("x")}),
                          QLangValue()})},
      {"table", mkTable()},
      {"set", QLangValue::Set({QLangValue::Int(2), QLangValue::Int(1)})},
      {"function", QLangValue::Function(closure)},
  };
  for (const auto &c : cases)
    check(valueEq(c.v, roundTrip(c.v)), std::string("value: ") + c.name);

  // Non-durable types serialize as Nil (R2).
  QLangValue gen = QLangValue::Generator(std::make_shared<GeneratorState>());
  check(roundTrip(gen).type() == QLangValue::Type::Nil,
        "value: generator -> nil");
  QLangValue obj = QLangValue::Object({});
  check(roundTrip(obj).type() == QLangValue::Type::Nil,
        "value: object -> nil");
}

// ---------------------------------------------------------------------------
// Program round-trip: compile a real program, write it with native wiring,
// read it back, and compare everything the format persists.
// ---------------------------------------------------------------------------
static void testProgram() {
  const char *src =
      "local x = 10\n"
      "def double(n) { return n * 2 }\n"
      "def add(a, b) { return a + b }\n"
      "local y = add(double(x), 4)\n"
      "println(y)\n";
  QLangContext ctx;
  CompiledProgram compiled;
  try {
    auto program = ctx.parse(src);
    QLangCompiler compiler(std::move(program), &ctx);
    compiled = compiler.compile();
  } catch (const QLangError &e) {
    check(false, std::string("program: compile: ") + e.what());
    return;
  }

  std::map<std::string, int> natives{{"abs", -2}, {"println", -1}};
  std::ostringstream os(std::ios::binary);
  check(writeQlc(os, compiled, natives), "program: writeQlc");

  CompiledProgram back;
  std::vector<std::pair<int, std::string>> slots;
  std::string err;
  std::istringstream is(os.str(), std::ios::binary);
  check(readQlc(is, back, slots, err), "program: readQlc (" + err + ")");
  if (err.empty()) {
    check(back.registerCount == compiled.registerCount,
          "program: registerCount");
    check(back.vars == compiled.vars, "program: vars");
    check(back.constVars == compiled.constVars, "program: constVars");
    check(back.functions.size() == compiled.functions.size(),
          "program: function count");
    bool fnsOk = back.functions.size() == compiled.functions.size();
    for (size_t i = 0; fnsOk && i < back.functions.size(); i++) {
      const auto &a = compiled.functions[i];
      const auto &b = back.functions[i];
      fnsOk = a.name == b.name && a.paramCount == b.paramCount &&
              a.registerCount == b.registerCount &&
              a.isGenerator == b.isGenerator &&
              a.captures.size() == b.captures.size() &&
              a.code == b.code && a.instrOffsets == b.instrOffsets;
      for (size_t k = 0; fnsOk && k < a.captures.size(); k++)
        fnsOk = a.captures[k].reg == b.captures[k].reg;
      fnsOk = fnsOk && a.constants.size() == b.constants.size();
      for (size_t j = 0; fnsOk && j < a.constants.size(); j++)
        fnsOk = valueEq(a.constants[j], b.constants[j]);
    }
    check(fnsOk, "program: functions identical");
    check(slots == std::vector<std::pair<int, std::string>>(
                       {{0, "println"}, {1, "abs"}}),
          "program: native slots");
  }
}

// ---------------------------------------------------------------------------
// MOVI: Int literals must compile to MOVI (signed immediate, no pool slot);
// everything else stays in the pool. The v5 format round-trips MOVI
// instruction-for-instruction (testProgram's code/offset comparison covers it).
// ---------------------------------------------------------------------------
static CompiledProgram compileSrc(const char *src) {
  QLangContext ctx;
  auto program = ctx.parse(src);
  QLangCompiler compiler(std::move(program), &ctx);
  return compiler.compile();
}

// Decode every instruction in a function's v5 code stream.
static std::vector<DecodedInstruction> decodeAll(const Function &fn) {
  std::vector<DecodedInstruction> out;
  out.reserve(fn.instrOffsets.size());
  for (size_t j = 0; j < fn.instrOffsets.size(); j++) {
    DecodedInstruction di;
    decodeInstruction(fn.code.data(), fn.instrOffsets[j], fn.code.size(), di);
    out.push_back(di);
  }
  return out;
}

static bool hasMovi(const Function &fn, int32_t imm) {
  for (const auto &i : decodeAll(fn))
    if (i.op == Opcode::MOVI && i.imm == imm)
      return true;
  return false;
}

static bool poolHasInt(const Function &fn, int32_t v) {
  for (const auto &c : fn.constants)
    if (c.type() == QLangValue::Type::Int && c.asInt() == v)
      return true;
  return false;
}

static void testMovi() {
  try {
    // Int literal -> MOVI, no Int(42) pool slot (the pool shrinks).
    CompiledProgram p = compileSrc("local x = 42\n");
    check(hasMovi(p.functions[0], 42), "movi: int literal emits MOVI");
    check(!poolHasInt(p.functions[0], 42), "movi: no Int pool slot");

    // Bool literal -> stays in the pool as Bool, no MOVI.
    CompiledProgram pb = compileSrc("local b = true\n");
    bool noMovi = true;
    bool poolHasBool = false;
    for (const auto &i : decodeAll(pb.functions[0]))
      if (i.op == Opcode::MOVI)
        noMovi = false;
    for (const auto &c : pb.functions[0].constants)
      if (c.type() == QLangValue::Type::Bool && c.asInt() == 1)
        poolHasBool = true;
    check(noMovi, "movi: bool literal not MOVI");
    check(poolHasBool, "movi: bool literal in pool");

    // Typed decl: MOVI into the target register + in-place CONV.
    CompiledProgram pt = compileSrc("local a:byte = 0\n");
    check(hasMovi(pt.functions[0], 0), "movi: typed decl emits MOVI");

    // 64-bit literal (Qword) -> pool, no MOVI.
    CompiledProgram pq = compileSrc("local q = 9223372036854775807\n");
    bool qNoMovi = true;
    for (const auto &i : decodeAll(pq.functions[0]))
      if (i.op == Opcode::MOVI)
        qNoMovi = false;
    check(qNoMovi, "movi: 64-bit literal not MOVI");

    // .qlc written at format version 5 (kQlcVersion); the reader accepts v5
    // only (v1-v4 and v6+ are rejected).
    std::ostringstream os(std::ios::binary);
    check(writeQlc(os, p, {}), "movi: writeQlc (v5)");
    std::string bytes = os.str();
    uint32_t version = static_cast<uint32_t>(static_cast<uint8_t>(bytes[4])) |
                       (static_cast<uint32_t>(static_cast<uint8_t>(bytes[5])) << 8) |
                       (static_cast<uint32_t>(static_cast<uint8_t>(bytes[6])) << 16) |
                       (static_cast<uint32_t>(static_cast<uint8_t>(bytes[7])) << 24);
    check(version == 5, "movi: format version 5");
    CompiledProgram back;
    std::vector<std::pair<int, std::string>> slots;
    std::string err;
    std::istringstream is(bytes, std::ios::binary);
    check(readQlc(is, back, slots, err), std::string("movi: readQlc v5 (") + err + ")");
    if (err.empty()) {
      bool same = back.functions.size() == p.functions.size();
      if (same) {
        const auto &a = p.functions[0];
        const auto &b = back.functions[0];
        same = a.code == b.code && a.instrOffsets == b.instrOffsets;
      }
      check(same, "movi: round-trip instruction equality");
    }
  } catch (const QLangError &e) {
    check(false, std::string("movi: " ) + e.what());
  }
}

// ---------------------------------------------------------------------------
// MOVI (step 4.4): compiler-generated Ints (loop counters, class-metadata
// markers, per-field indices) also compile to MOVI, so the main function's
// constant pool holds no Type::Int at all. User literals were already MOVI
// (Round 30); this removes the last compiler-generated pool entries.
// ---------------------------------------------------------------------------
static bool poolHasAnyInt(const Function &fn) {
  for (const auto &c : fn.constants)
    if (c.type() == QLangValue::Type::Int)
      return true;
  return false;
}

static void testCompilerMovi() {
  try {
    // for-loop (unit/zero/direction/counter) + a two-field class (markers +
    // name->index metadata). Every compiler-generated Int is MOVI; the pool
    // must contain no Type::Int entries.
    const char *src =
        "for x = 1 to 3 {\n"
        "  local s = 0\n"
        "}\n"
        "class C {\n"
        "  a = 1\n"
        "  b = 2\n"
        "}\n"
        "local c = C()\n";
    CompiledProgram p = compileSrc(src);
    check(!poolHasAnyInt(p.functions[0]),
          "compiler-movi: no Type::Int in main pool (for + class)");
    // The for-loop's compiler-generated constants are present as MOVI.
    check(hasMovi(p.functions[0], 1) && hasMovi(p.functions[0], 0) &&
          hasMovi(p.functions[0], -1),
          "compiler-movi: for step/zero/direction emitted as MOVI");
  } catch (const QLangError &e) {
    check(false, std::string("compiler-movi: ") + e.what());
  }
}

// ---------------------------------------------------------------------------
// ADDI: compound `+=` with an Int-literal RHS compiles to ADDI (signed
// immediate, no pool slot); non-Int RHS stays pooled + ADD. The v5 format
// round-trips ADDI instruction-for-instruction (code/offset comparison).
// ---------------------------------------------------------------------------
static bool hasAddi(const Function &fn, int32_t imm) {
  for (const auto &i : decodeAll(fn))
    if (i.op == Opcode::ADDI && i.imm == imm)
      return true;
  return false;
}

static void testAddi() {
  try {
    // Int literal RHS -> ADDI with i32 == 12, no Int(12) pool slot.
    CompiledProgram p = compileSrc("local a = 10\na += 12\n");
    check(hasAddi(p.functions[0], 12), "addi: int literal emits ADDI");
    check(!poolHasInt(p.functions[0], 12), "addi: no Int pool slot");

    // Float literal RHS -> stays pooled + ADD, no ADDI.
    CompiledProgram pf = compileSrc("local f = 1.5\nf += 1.5\n");
    bool noAddi = true;
    for (const auto &i : decodeAll(pf.functions[0]))
      if (i.op == Opcode::ADDI)
        noAddi = false;
    check(noAddi, "addi: float literal not ADDI");

    // .qlc round-trip of an ADDI program (v5 format).
    std::ostringstream os(std::ios::binary);
    check(writeQlc(os, p, {}), "addi: writeQlc (v5)");
    std::string bytes = os.str();
    CompiledProgram back;
    std::vector<std::pair<int, std::string>> slots;
    std::string err;
    std::istringstream is(bytes, std::ios::binary);
    check(readQlc(is, back, slots, err), std::string("addi: readQlc v5 (") + err + ")");
    if (err.empty()) {
      bool same = back.functions.size() == p.functions.size();
      if (same) {
        const auto &a = p.functions[0];
        const auto &b = back.functions[0];
        same = a.code == b.code && a.instrOffsets == b.instrOffsets;
      }
      check(same, "addi: round-trip instruction equality");
    }
  } catch (const QLangError &e) {
    check(false, std::string("addi: ") + e.what());
  }
}

// ---------------------------------------------------------------------------
// SUBI: compound `-=` with an Int-literal RHS compiles to SUBI (signed
// immediate, no pool slot); non-Int RHS stays pooled + SUB. The v5 format
// round-trips SUBI instruction-for-instruction (code/offset comparison).
// ---------------------------------------------------------------------------
static bool hasSubi(const Function &fn, int32_t imm) {
  for (const auto &i : decodeAll(fn))
    if (i.op == Opcode::SUBI && i.imm == imm)
      return true;
  return false;
}

static void testSubi() {
  try {
    // Int literal RHS -> SUBI with i32 == 12, no Int(12) pool slot.
    CompiledProgram p = compileSrc("local a = 10\na -= 12\n");
    check(hasSubi(p.functions[0], 12), "subi: int literal emits SUBI");
    check(!poolHasInt(p.functions[0], 12), "subi: no Int pool slot");

    // Float literal RHS -> stays pooled + SUB, no SUBI.
    CompiledProgram pf = compileSrc("local f = 1.5\nf -= 1.5\n");
    bool noSubi = true;
    for (const auto &i : decodeAll(pf.functions[0]))
      if (i.op == Opcode::SUBI)
        noSubi = false;
    check(noSubi, "subi: float literal not SUBI");

    // .qlc round-trip of a SUBI program (v5 format).
    std::ostringstream os(std::ios::binary);
    check(writeQlc(os, p, {}), "subi: writeQlc (v5)");
    std::string bytes = os.str();
    CompiledProgram back;
    std::vector<std::pair<int, std::string>> slots;
    std::string err;
    std::istringstream is(bytes, std::ios::binary);
    check(readQlc(is, back, slots, err), std::string("subi: readQlc v5 (") + err + ")");
    if (err.empty()) {
      bool same = back.functions.size() == p.functions.size();
      if (same) {
        const auto &a = p.functions[0];
        const auto &b = back.functions[0];
        same = a.code == b.code && a.instrOffsets == b.instrOffsets;
      }
      check(same, "subi: round-trip instruction equality");
    }
  } catch (const QLangError &e) {
    check(false, std::string("subi: ") + e.what());
  }
}

// ---------------------------------------------------------------------------
// CMP/CMPI + j*: an Int-vs-Int (literal) comparison in `if` fuses to
// cmp/cmpi + a conditional jump (no comparison-result temp, no pool slot for
// the boolean); non-Int comparisons stay on the legacy EQ/GT + JMPIFFALSE
// path. The v5 format round-trips the new opcodes instruction-for-instruction.
// ---------------------------------------------------------------------------
static bool hasAnyOp(const Function &fn, std::initializer_list<Opcode> ops) {
  for (const auto &i : decodeAll(fn))
    for (Opcode o : ops)
      if (i.op == o)
        return true;
  return false;
}

static void testCmpJump() {
  try {
    // Int literal comparison in `if` -> CMPI + a j* (no CMP, both literals;
    // no JMPIFFALSE for the condition).
    CompiledProgram p = compileSrc("local r = 0\nif (5 > 3) { r = 1 }\n");
    check(hasAnyOp(p.functions[0], {Opcode::CMPI}),
          "cmp: int literal if emits CMPI");
    check(hasAnyOp(p.functions[0],
                   {Opcode::JE, Opcode::JNE, Opcode::JG, Opcode::JGE,
                    Opcode::JL, Opcode::JLE}),
          "cmp: int literal if emits a j*");
    check(!hasAnyOp(p.functions[0], {Opcode::JMPIFFALSE, Opcode::JMPIFTRUE}),
          "cmp: int literal if not JMPIFFALSE");

    // Float comparison -> legacy path (no CMP/CMPI/j* for the condition).
    CompiledProgram pf = compileSrc("local r = 0\nif (1.5 < 2.5) { r = 1 }\n");
    check(!hasAnyOp(pf.functions[0],
                    {Opcode::CMP, Opcode::CMPI, Opcode::JE, Opcode::JNE,
                     Opcode::JG, Opcode::JGE, Opcode::JL, Opcode::JLE}),
          "cmp: float comparison not fused");

    // .qlc round-trip of a fused program (v5 format).
    std::ostringstream os(std::ios::binary);
    check(writeQlc(os, p, {}), "cmp: writeQlc (v5)");
    std::string bytes = os.str();
    CompiledProgram back;
    std::vector<std::pair<int, std::string>> slots;
    std::string err;
    std::istringstream is(bytes, std::ios::binary);
    check(readQlc(is, back, slots, err), std::string("cmp: readQlc v5 (") + err + ")");
    if (err.empty()) {
      bool same = back.functions.size() == p.functions.size();
      if (same) {
        const auto &a = p.functions[0];
        const auto &b = back.functions[0];
        same = a.code == b.code && a.instrOffsets == b.instrOffsets;
      }
      check(same, "cmp: round-trip instruction equality");
    }
  } catch (const QLangError &e) {
    check(false, std::string("cmp: ") + e.what());
  }
}

// ---------------------------------------------------------------------------
// Constant pool dedup (SPEC §bytecodes "constant pool"): constants in the
// pool must have unique values; duplicated entries fold into a single one.
// ---------------------------------------------------------------------------
static bool poolHasDuplicates(const Function &fn) {
  for (size_t i = 0; i < fn.constants.size(); i++)
    for (size_t j = i + 1; j < fn.constants.size(); j++)
      if (valueEq(fn.constants[i], fn.constants[j]))
        return true;
  return false;
}

static int poolCountType(const Function &fn, QLangValue::Type t) {
  int n = 0;
  for (const auto &c : fn.constants)
    if (c.type() == t)
      n++;
  return n;
}

static void testPoolDedup() {
  try {
    // Repeated string literals -> one pool entry.
    CompiledProgram p = compileSrc(
        "local a = \"hello\"\n"
        "local b = \"hello\"\n"
        "local c = \"world\"\n");
    check(poolCountType(p.functions[0], QLangValue::Type::String) == 2,
          "pool-dedup: repeated strings fold (2 unique, not 3)");
    check(!poolHasDuplicates(p.functions[0]),
          "pool-dedup: no duplicate pool entries (strings)");

    // Repeated nil -> one pool entry (call-scratch clears, one per call).
    CompiledProgram pn = compileSrc(
        "def f(x) { return x }\n"
        "f(1.5 + 2.5)\n"
        "f(3.5 + 4.5)\n");
    check(poolCountType(pn.functions[0], QLangValue::Type::Nil) == 1,
          "pool-dedup: repeated nil folds to one entry");

    // Repeated float/bool literals -> one entry each; distinct values stay.
    CompiledProgram pf = compileSrc(
        "local a = 1.5\n"
        "local b = 1.5\n"
        "local c = 2.5\n"
        "local d = true\n"
        "local e = true\n"
        "local f = false\n");
    check(poolCountType(pf.functions[0], QLangValue::Type::Float) == 2,
          "pool-dedup: repeated floats fold (2 unique, not 3)");
    check(poolCountType(pf.functions[0], QLangValue::Type::Bool) == 2,
          "pool-dedup: repeated bools fold (2 unique, not 3)");
    check(!poolHasDuplicates(pf.functions[0]),
          "pool-dedup: no duplicate pool entries (float/bool)");

    // Nested collections: identical array literals fold, distinct do not.
    CompiledProgram pa = compileSrc(
        "local a = [1.5, \"x\"]\n"
        "local b = [1.5, \"x\"]\n"
        "local c = [1.5, \"y\"]\n");
    check(poolCountType(pa.functions[0], QLangValue::Type::Array) == 2,
          "pool-dedup: identical arrays fold (2 unique, not 3)");
    check(!poolHasDuplicates(pa.functions[0]),
          "pool-dedup: no duplicate pool entries (arrays)");

    // Behavior unchanged: the folded pool loads the right values.
    QLangContext ctx;
    ctx.run("local a = \"hello\"\n"
            "local b = \"hello\"\n"
            "local c = 1.5\n"
            "local d = 1.5\n"
            "local e = [1.5, \"x\"]\n"
            "local f = [1.5, \"x\"]\n");
    check(ctx.get("a").asString() == "hello" &&
              ctx.get("b").asString() == "hello" &&
              ctx.get("c").asFloat() == 1.5 &&
              ctx.get("d").asFloat() == 1.5,
          "pool-dedup: folded scalars load correctly");
    check(valueEq(ctx.get("e"), ctx.get("f")),
          "pool-dedup: folded arrays load correctly");

    // A real program (functions + class metadata) stays duplicate-free.
    CompiledProgram pr = compileSrc(
        "def greet(name) { return \"hi \" + name }\n"
        "local x = greet(\"bob\")\n"
        "local y = greet(\"bob\")\n"
        "println(x)\n");
    bool allClean = true;
    for (const auto &fn : pr.functions)
      allClean = allClean && !poolHasDuplicates(fn);
    check(allClean, "pool-dedup: no duplicates in any function pool");
  } catch (const QLangError &e) {
    check(false, std::string("pool-dedup: ") + e.what());
  }
}

// ---------------------------------------------------------------------------
// Nop: a zero-operand no-op opcode (SPEC §bytecodes "OpCode nop"). Appended
// last (identity 68) so existing .qlc opcode numbers stay valid. The compiler
// never emits it; the codec must round-trip it and the VM must execute it.
// ---------------------------------------------------------------------------
static void testNop() {
  try {
    // Identity, name, and size.
    check(static_cast<uint8_t>(Opcode::Nop) == 68,
          "nop: identity 68 (appended last)");
    check(Bytecode::name(Opcode::Nop) == "nop", "nop: name");
    check(isValidOpcode(Opcode::Nop), "nop: valid opcode");
    check(baseInstructionSize(Opcode::Nop) == 1, "nop: 1-byte base size");

    // 1-byte encode/decode round-trip.
    DecodedInstruction in;
    in.op = Opcode::Nop;
    std::vector<uint8_t> buf;
    encodeInstruction(buf, in);
    check(buf.size() == 1 && buf[0] == 68, "nop: encodes to 1 byte");
    DecodedInstruction out;
    decodeInstruction(buf.data(), 0, buf.size(), out);
    check(out.op == Opcode::Nop && !out.escaped && out.size == 1,
          "nop: decode round-trip");

    // The escaped form is meaningless for a 0-operand opcode: rejected.
    bool rejected = false;
    try {
      DecodedInstruction esc;
      esc.op = Opcode::Nop;
      esc.escaped = true;
      esc.modifier = 0x11;
      std::vector<uint8_t> ebuf;
      encodeInstruction(ebuf, esc);
    } catch (const QLangError &) {
      rejected = true;
    }
    check(rejected, "nop: escaped form rejected");

    // .qlc round-trip of a hand-built program: Nop; MOVI 0, 42; RETURN 0.
    Function fn;
    fn.name = "main";
    fn.paramCount = 0;
    fn.registerCount = 1;
    DecodedInstruction d1;
    d1.op = Opcode::Nop;
    DecodedInstruction d2;
    d2.op = Opcode::MOVI;
    d2.a = 0;
    d2.imm = 42;
    DecodedInstruction d3;
    d3.op = Opcode::RETURN;
    d3.a = 0;
    for (const auto &d : {d1, d2, d3}) {
      fn.instrOffsets.push_back(fn.code.size());
      encodeInstruction(fn.code, d);
      fn.loc.emplace_back(1, 1);
    }
    CompiledProgram p;
    p.registerCount = 1;
    p.vars["x"] = 0;
    p.functions.push_back(fn);
    std::ostringstream os(std::ios::binary);
    check(writeQlc(os, p, {}), "nop: writeQlc");
    CompiledProgram back;
    std::vector<std::pair<int, std::string>> slots;
    std::string err;
    std::istringstream is(os.str(), std::ios::binary);
    check(readQlc(is, back, slots, err), std::string("nop: readQlc (") + err + ")");
    if (err.empty()) {
      const auto &b = back.functions[0];
      bool same = b.code == fn.code && b.instrOffsets == fn.instrOffsets;
      check(same, "nop: round-trip instruction equality");
      if (same) {
        DecodedInstruction di;
        decodeInstruction(b.code.data(), 0, b.code.size(), di);
        check(di.op == Opcode::Nop,
              "nop: first instruction survives round-trip");
      }
    }

    // The VM executes the no-op: the program runs and leaves 42 in x.
    QLangContext ctx;
    QLangVM vm(p, ctx);
    vm.run();
    check(ctx.get("x").asInt() == 42, "nop: VM executes the no-op");
  } catch (const QLangError &e) {
    check(false, std::string("nop: ") + e.what());
  }
}

// ---------------------------------------------------------------------------
// DEEPCOPY: r[a] = deep copy of r[b] (SPEC §bytecodes "OpCode deepcopy").
// Appended last (identity 70) so existing .qlc opcode numbers stay valid.
// Two register operands, no i32 slot (same shape as MOV). The compiler lowers
// a `deepcopy(x)` call directly to this opcode.
// ---------------------------------------------------------------------------
static void testDeepcopy() {
  try {
    // Identity, name, validity, and size.
    check(static_cast<uint8_t>(Opcode::DEEPCOPY) == 70,
          "deepcopy: identity 70 (appended last)");
    check(Bytecode::name(Opcode::DEEPCOPY) == "deepcopy", "deepcopy: name");
    check(isValidOpcode(Opcode::DEEPCOPY), "deepcopy: valid opcode");
    check(baseInstructionSize(Opcode::DEEPCOPY) == 3,
          "deepcopy: 3-byte base size (op + 2 reg operands)");

    // 3-byte encode/decode round-trip (a=dest reg, b=src reg).
    DecodedInstruction in;
    in.op = Opcode::DEEPCOPY;
    in.a = 3;
    in.b = 5;
    std::vector<uint8_t> buf;
    encodeInstruction(buf, in);
    check(buf.size() == 3 && buf[0] == 70 && buf[1] == 3 && buf[2] == 5,
          "deepcopy: encodes to 3 bytes");
    DecodedInstruction out;
    decodeInstruction(buf.data(), 0, buf.size(), out);
    check(out.op == Opcode::DEEPCOPY && !out.escaped && out.size == 3 &&
              out.a == 3 && out.b == 5,
          "deepcopy: decode round-trip");

    // .qlc round-trip of a hand-built program: MOVI 1, 7; DEEPCOPY 0, 1;
    // RETURN 0.
    Function fn;
    fn.name = "main";
    fn.paramCount = 0;
    fn.registerCount = 2;
    DecodedInstruction d1;
    d1.op = Opcode::MOVI;
    d1.a = 1;
    d1.imm = 7;
    DecodedInstruction d2;
    d2.op = Opcode::DEEPCOPY;
    d2.a = 0;
    d2.b = 1;
    DecodedInstruction d3;
    d3.op = Opcode::RETURN;
    d3.a = 0;
    for (const auto &d : {d1, d2, d3}) {
      fn.instrOffsets.push_back(fn.code.size());
      encodeInstruction(fn.code, d);
      fn.loc.emplace_back(1, 1);
    }
    CompiledProgram p;
    p.registerCount = 2;
    p.vars["x"] = 0;
    p.functions.push_back(fn);
    std::ostringstream os(std::ios::binary);
    check(writeQlc(os, p, {}), "deepcopy: writeQlc");
    CompiledProgram back;
    std::vector<std::pair<int, std::string>> slots;
    std::string err;
    std::istringstream is(os.str(), std::ios::binary);
    check(readQlc(is, back, slots, err),
          std::string("deepcopy: readQlc (") + err + ")");
    if (err.empty()) {
      const auto &b = back.functions[0];
      bool same = b.code == fn.code && b.instrOffsets == fn.instrOffsets;
      check(same, "deepcopy: round-trip instruction equality");
      if (same) {
        DecodedInstruction di;
        decodeInstruction(b.code.data(), b.instrOffsets[1], b.code.size(), di);
        check(di.op == Opcode::DEEPCOPY && di.a == 0 && di.b == 1,
              "deepcopy: instruction survives round-trip");
      }
    }

    // The VM executes it: r0 = deepcopy(r1) with r1 = Int 7 leaves 7 in x.
    QLangContext ctx;
    QLangVM vm(p, ctx);
    vm.run();
    check(ctx.get("x").asInt() == 7, "deepcopy: VM executes the copy");
  } catch (const QLangError &e) {
    check(false, std::string("deepcopy: ") + e.what());
  }
}

// ---------------------------------------------------------------------------
// deepcopy lowering + arity (SPEC §functions "deepcopy"): the compiler emits
// DEEPCOPY directly (no CALL, no arg staging); wrong arity is a compile error.
// ---------------------------------------------------------------------------
static void testDeepcopyLowering() {
  try {
    QLangContext ctx;
    QLangCompiler compiler(ctx.parse("local a = [1, 2]\nlocal b = deepcopy(a);\n"),
                           &ctx);
    CompiledProgram p = compiler.compile();
    const Function &fn = p.functions[0];
    bool hasDeepcopy = false, hasCall = false;
    for (size_t off : fn.instrOffsets) {
      DecodedInstruction d;
      decodeInstruction(fn.code.data(), off, fn.code.size(), d);
      if (d.op == Opcode::DEEPCOPY)
        hasDeepcopy = true;
      if (d.op == Opcode::CALL)
        hasCall = true;
    }
    check(hasDeepcopy, "deepcopy: compiler lowers the call to DEEPCOPY");
    check(!hasCall, "deepcopy: no CALL emitted for the builtin");

    // Arity: deepcopy() and deepcopy(a, b) are compile errors.
    bool zeroRejected = false, twoRejected = false;
    try {
      QLangContext c1;
      QLangCompiler k(c1.parse("local b = deepcopy();\n"), &c1);
      k.compile();
    } catch (const QLangError &) {
      zeroRejected = true;
    }
    try {
      QLangContext c2;
      QLangCompiler k(c2.parse("local a = [1]\nlocal b = deepcopy(a, a);\n"),
                      &c2);
      k.compile();
    } catch (const QLangError &) {
      twoRejected = true;
    }
    check(zeroRejected, "deepcopy: zero args rejected at compile time");
    check(twoRejected, "deepcopy: two args rejected at compile time");
  } catch (const QLangError &e) {
    check(false, std::string("deepcopy-lowering: ") + e.what());
  }
}

// ---------------------------------------------------------------------------
// Malformed-input suite: each case must fail cleanly, leave the out-params
// untouched, and say why.
// ---------------------------------------------------------------------------
// Test-file builder. Emits explicit little-endian (matching the codec, F1)
// so the malformed-input suite is host-endian independent.
struct Bin {
  std::string b;
  void u8(uint8_t v) { b += static_cast<char>(v); }
  void u16(uint16_t v) {
    b += static_cast<char>(v & 0xFF);
    b += static_cast<char>((v >> 8) & 0xFF);
  }
  void i32(int32_t v) {
    uint32_t u = static_cast<uint32_t>(v);
    b += static_cast<char>(u & 0xFF);
    b += static_cast<char>((u >> 8) & 0xFF);
    b += static_cast<char>((u >> 16) & 0xFF);
    b += static_cast<char>((u >> 24) & 0xFF);
  }
  void u32(uint32_t v) { i32(static_cast<int32_t>(v)); }
  void str(const std::string &s) {
    u32(static_cast<uint32_t>(s.size()));
    b += s;
  }
};

// A valid file: one empty "main" function, no natives/vars, 1 register.
// v5 layout: code blob (empty) + offset table (empty) + loc table (empty).
static std::string minimalFile() {
  Bin f;
  f.b += "QLC1";
  f.u32(5); // version
  f.u32(1); // funcCount
  f.str("main");
  f.i32(0); // paramCount
  f.i32(1); // registerCount
  f.u8(0);  // isGenerator
  f.u32(0); // captures
  f.u32(0); // codeSize (empty code stream)
  f.u32(0); // offCount (no instructions)
  f.u32(0); // locCount
  f.u32(0); // constants
  f.u32(0); // natives
  f.u32(0); // vars
  f.u32(0); // constVars
  f.i32(1); // registerCount
  return f.b;
}

static void rejects(const std::string &name, const std::string &bytes) {
  std::istringstream is(bytes, std::ios::binary);
  CompiledProgram prog;
  prog.registerCount = -42; // sentinel: must survive a failed read
  std::vector<std::pair<int, std::string>> slots;
  std::string err;
  bool ok = readQlc(is, prog, slots, err);
  check(!ok && !err.empty() && prog.registerCount == -42, name);
}

// Valid file variant with one 3-operand instruction (op ADD, a=0 b=0 c=0):
// a 4-byte code blob + a one-entry offset table + a one-entry loc table.
static std::string fileWithOneInstr() {
  Bin f;
  f.b += "QLC1";
  f.u32(5); // version
  f.u32(1); // funcCount
  f.str("main");
  f.i32(0); // paramCount
  f.i32(1); // registerCount
  f.u8(0);  // isGenerator
  f.u32(0); // captures
  f.u32(4); // codeSize
  f.u8(6);  // op = ADD
  f.u8(0);  // a
  f.u8(0);  // b
  f.u8(0);  // c
  f.u32(1); // offCount
  f.u32(0); // offset[0]
  f.u32(1); // locCount
  f.u16(0); // line
  f.u16(0); // col
  f.u32(0); // constants
  f.u32(0); // natives
  f.u32(0); // vars
  f.u32(0); // constVars
  f.i32(1); // registerCount
  return f.b;
}

static void testMalformed() {
  rejects("malformed: empty file", "");
  rejects("malformed: short header", "QLC1\x01\x00");

  std::string badMagic = minimalFile();
  badMagic[0] = 'X';
  rejects("malformed: bad magic", badMagic);

  std::string badVersion = minimalFile();
  badVersion[4] = 4; // v5 is the current format; v4 must be rejected
  rejects("malformed: unsupported version", badVersion);

  rejects("malformed: truncated", minimalFile().substr(0, 10));

  std::string noFuncs = minimalFile();
  noFuncs[8] = 0; // funcCount = 0
  rejects("malformed: empty function table", noFuncs);

  std::string notMain = minimalFile();
  notMain[13] = 'x'; // functions[0].name = "xain"
  rejects("malformed: functions[0] not 'main'", notMain);

  std::string badOp = fileWithOneInstr();
  // The single instruction's opcode byte is at byte 37 (header + function
  // metadata + codeSize). Set it to 71: a valid 7-bit identity that is not a
  // defined opcode (kOpcodeUsedCount == 71; DEEPCOPY took 70). It decodes as
  // a 4-byte 3-operand form, then fails isValidOpcode.
  badOp[37] = 71;
  rejects("malformed: invalid opcode", badOp);

  // Truncated instruction: codeSize=3, but the ADD at offset 0 needs 4 bytes.
  Bin truncInstr;
  truncInstr.b += "QLC1";
  truncInstr.u32(5);
  truncInstr.u32(1);
  truncInstr.str("main");
  truncInstr.i32(0);
  truncInstr.i32(1);
  truncInstr.u8(0);
  truncInstr.u32(0);
  truncInstr.u32(3); // codeSize = 3 (too short for a 4-byte ADD)
  truncInstr.u8(6);  // op = ADD
  truncInstr.u8(0);  // a
  truncInstr.u8(0);  // b
  truncInstr.u32(1); // offCount
  truncInstr.u32(0); // offset[0]
  truncInstr.u32(1); // locCount
  truncInstr.u16(0);
  truncInstr.u16(0);
  truncInstr.u32(0); // constants
  truncInstr.u32(0); // natives
  truncInstr.u32(0); // vars
  truncInstr.u32(0); // constVars
  truncInstr.i32(1);
  rejects("malformed: truncated instruction", truncInstr.b);

  // Dangling closure: pool holds a Function value with funcIndex 5.
  Bin dangling;
  dangling.b += "QLC1";
  dangling.u32(5);
  dangling.u32(1);
  dangling.str("main");
  dangling.i32(0);
  dangling.i32(1);
  dangling.u8(0);
  dangling.u32(0); // captures
  dangling.u32(0); // codeSize
  dangling.u32(0); // offCount
  dangling.u32(0); // locCount
  dangling.u32(1); // one constant
  dangling.u8(11); // Function
  dangling.i32(5); // funcIndex (only 1 function exists)
  dangling.u32(0); // captures
  dangling.u32(0);
  dangling.u32(0);
  dangling.u32(0);
  dangling.i32(1);
  rejects("malformed: dangling closure funcIndex", dangling.b);

  // Native slot table invariants.
  auto nativeFile = [&](int slotA, const char *nameA, int slotB,
                        const char *nameB) {
    Bin f;
    f.b += "QLC1";
    f.u32(5);
    f.u32(1);
    f.str("main");
    f.i32(0);
    f.i32(1);
    f.u8(0);
    f.u32(0); // captures
    f.u32(0); // codeSize
    f.u32(0); // offCount
    f.u32(0); // locCount
    f.u32(0); // constants
    f.u32(2); // natives
    f.i32(slotA);
    f.str(nameA);
    f.i32(slotB);
    f.str(nameB);
    f.u32(0);
    f.u32(0);
    f.i32(1);
    return f.b;
  };
  rejects("malformed: duplicate native slot", nativeFile(0, "a", 0, "b"));
  rejects("malformed: out-of-order native slots", nativeFile(1, "b", 0, "a"));
  rejects("malformed: empty native name", nativeFile(0, "a", 1, ""));

  // Trailing bytes.
  rejects("malformed: trailing bytes", minimalFile() + "x");

  // Value-level failures.
  auto valueFile = [&](std::function<void(Bin &)> emit) {
    Bin f;
    f.b += "QLC1";
    f.u32(5);
    f.u32(1);
    f.str("main");
    f.i32(0);
    f.i32(1);
    f.u8(0);
    f.u32(0); // captures
    f.u32(0); // codeSize
    f.u32(0); // offCount
    f.u32(0); // locCount
    f.u32(1); // one constant
    emit(f);
    f.u32(0);
    f.u32(0);
    f.u32(0);
    f.i32(1);
    return f.b;
  };
  rejects("malformed: invalid value tag",
          valueFile([](Bin &f) { f.u8(200); }));
  rejects("malformed: truncated array",
          valueFile([](Bin &f) {
            f.u8(12); // Array
            f.u32(5); // claims 5 elements
            f.u8(0);  // Nil
            f.u8(0);  // Nil
          }));
  rejects("malformed: nesting too deep",
          valueFile([](Bin &f) {
            for (int i = 0; i < 70; i++) {
              f.u8(12); // Array
              f.u32(1);
            }
            f.u8(0); // Nil
          }));
  rejects("malformed: short out of range",
          valueFile([](Bin &f) {
            f.u8(5); // Short
            f.i32(70000);
          }));
}

int main() {
  testValues();
  testProgram();
  testMovi();
  testCompilerMovi();
  testAddi();
  testSubi();
  testCmpJump();
  testPoolDedup();
  testNop();
  testDeepcopy();
  testDeepcopyLowering();
  testMalformed();
  std::cout << (fails == 0 ? "all codec checks passed\n"
                           : "codec checks FAILED\n");
  return fails == 0 ? 0 : 1;
}
