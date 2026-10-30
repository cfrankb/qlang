// .qlc codec check: value round-trips, program round-trip with native wiring,
// and a malformed-input robustness suite (docs/SERIALIZATION.md §9).
#include <climits>
#include <cstdint>
#include <cstring>
#include <functional>
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
    return x.asBlob() == y.asBlob();
  case T::Array:
  case T::Set: {
    const auto &a = (x.type() == T::Array) ? x.asArray() : x.asSet();
    const auto &b = (y.type() == T::Array) ? y.asArray() : y.asSet();
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
      {"blob empty", QLangValue::Blob({})},
      {"blob", QLangValue::Blob({0, 1, 2, 255})},
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
  QLangValue gen = QLangValue::Generator(std::make_shared<GeneratorValue>());
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
              a.instructions.size() == b.instructions.size();
      for (size_t k = 0; fnsOk && k < a.captures.size(); k++)
        fnsOk = a.captures[k].reg == b.captures[k].reg;
      for (size_t j = 0; fnsOk && j < a.instructions.size(); j++)
        fnsOk = std::memcmp(&a.instructions[j], &b.instructions[j],
                            sizeof(Instruction)) == 0;
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
// MOVI: Int literals must compile to MOVI (immediate in i32, no pool slot);
// everything else stays in the pool. The v2 format round-trips MOVI
// instruction-for-instruction (testProgram's memcmp covers the encoding).
// ---------------------------------------------------------------------------
static CompiledProgram compileSrc(const char *src) {
  QLangContext ctx;
  auto program = ctx.parse(src);
  QLangCompiler compiler(std::move(program), &ctx);
  return compiler.compile();
}

static bool hasMovi(const Function &fn, int32_t imm) {
  for (const auto &i : fn.instructions)
    if (i.op == Opcode::MOVI && i.i32 == imm)
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
    for (const auto &i : pb.functions[0].instructions)
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
    for (const auto &i : pq.functions[0].instructions)
      if (i.op == Opcode::MOVI)
        qNoMovi = false;
    check(qNoMovi, "movi: 64-bit literal not MOVI");

    // .qlc written at format version 2; a v1 reader (version <= 1 check)
    // would reject it, a v2 reader accepts it.
    std::ostringstream os(std::ios::binary);
    check(writeQlc(os, p, {}), "movi: writeQlc (v2)");
    std::string bytes = os.str();
    uint32_t version = 0;
    std::memcpy(&version, bytes.data() + 4, 4);
    check(version == 2, "movi: format version 2");
    CompiledProgram back;
    std::vector<std::pair<int, std::string>> slots;
    std::string err;
    std::istringstream is(bytes, std::ios::binary);
    check(readQlc(is, back, slots, err), std::string("movi: readQlc v2 (") + err + ")");
    if (err.empty()) {
      bool same = back.functions.size() == p.functions.size();
      if (same) {
        const auto &a = p.functions[0];
        const auto &b = back.functions[0];
        same = a.instructions.size() == b.instructions.size();
        for (size_t j = 0; same && j < a.instructions.size(); j++)
          same = std::memcmp(&a.instructions[j], &b.instructions[j],
                             sizeof(Instruction)) == 0;
      }
      check(same, "movi: round-trip instruction equality");
    }
  } catch (const QLangError &e) {
    check(false, std::string("movi: " ) + e.what());
  }
}

// ---------------------------------------------------------------------------
// ADDI: compound `+=` with an Int-literal RHS compiles to ADDI (immediate in
// i32, no pool slot); non-Int RHS stays pooled + ADD. The v2 format
// round-trips ADDI instruction-for-instruction (memcmp covers the encoding).
// ---------------------------------------------------------------------------
static bool hasAddi(const Function &fn, int32_t imm) {
  for (const auto &i : fn.instructions)
    if (i.op == Opcode::ADDI && i.i32 == imm)
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
    for (const auto &i : pf.functions[0].instructions)
      if (i.op == Opcode::ADDI)
        noAddi = false;
    check(noAddi, "addi: float literal not ADDI");

    // .qlc round-trip of an ADDI program (v2 format).
    std::ostringstream os(std::ios::binary);
    check(writeQlc(os, p, {}), "addi: writeQlc (v2)");
    std::string bytes = os.str();
    CompiledProgram back;
    std::vector<std::pair<int, std::string>> slots;
    std::string err;
    std::istringstream is(bytes, std::ios::binary);
    check(readQlc(is, back, slots, err), std::string("addi: readQlc v2 (") + err + ")");
    if (err.empty()) {
      bool same = back.functions.size() == p.functions.size();
      if (same) {
        const auto &a = p.functions[0];
        const auto &b = back.functions[0];
        same = a.instructions.size() == b.instructions.size();
        for (size_t j = 0; same && j < a.instructions.size(); j++)
          same = std::memcmp(&a.instructions[j], &b.instructions[j],
                             sizeof(Instruction)) == 0;
      }
      check(same, "addi: round-trip instruction equality");
    }
  } catch (const QLangError &e) {
    check(false, std::string("addi: ") + e.what());
  }
}

// ---------------------------------------------------------------------------
// Malformed-input suite: each case must fail cleanly, leave the out-params
// untouched, and say why.
// ---------------------------------------------------------------------------
struct Bin {
  std::string b;
  void u8(uint8_t v) { b += static_cast<char>(v); }
  void u16(uint16_t v) { b.append(reinterpret_cast<const char *>(&v), 2); }
  void i32(int32_t v) { b.append(reinterpret_cast<const char *>(&v), 4); }
  void u32(uint32_t v) { i32(static_cast<int32_t>(v)); }
  void str(const std::string &s) {
    u32(static_cast<uint32_t>(s.size()));
    b += s;
  }
};

// A valid file: one empty "main" function, no natives/vars, 1 register.
static std::string minimalFile() {
  Bin f;
  f.b += "QLC1";
  f.u32(1);
  f.u32(1); // funcCount
  f.str("main");
  f.i32(0); // paramCount
  f.i32(1); // registerCount
  f.u8(0);  // isGenerator
  f.u32(0); // captures
  f.u32(0); // instructions
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

// Valid file variant with one i16-operand instruction (op CONST, a=0 b=0 c=0).
static std::string fileWithOneInstr() {
  Bin f;
  f.b += "QLC1";
  f.u32(1);
  f.u32(1);
  f.str("main");
  f.i32(0);
  f.i32(1);
  f.u8(0);
  f.u32(0);
  f.u32(1); // one instruction
  f.u16(0); // op
  f.u16(0); // a
  f.i32(0); // i32
  f.u16(0); // line
  f.u16(0); // col
  f.u32(0); // constants
  f.u32(0);
  f.u32(0);
  f.u32(0);
  f.i32(1);
  return f.b;
}

static void testMalformed() {
  rejects("malformed: empty file", "");
  rejects("malformed: short header", "QLC1\x01\x00");

  std::string badMagic = minimalFile();
  badMagic[0] = 'X';
  rejects("malformed: bad magic", badMagic);

  std::string badVersion = minimalFile();
  badVersion[4] = 3; // v2 is the current format; v3 must be rejected
  rejects("malformed: unsupported version", badVersion);

  rejects("malformed: truncated", minimalFile().substr(0, 10));

  std::string noFuncs = minimalFile();
  noFuncs[8] = 0; // funcCount = 0
  rejects("malformed: empty function table", noFuncs);

  std::string notMain = minimalFile();
  notMain[13] = 'x'; // functions[0].name = "xain"
  rejects("malformed: functions[0] not 'main'", notMain);

  std::string badOp = fileWithOneInstr();
  // The single instruction starts at byte 37 (header + function metadata);
  // set its op to 0x01FF, which is beyond kOpcodeCount.
  badOp[37] = 0xFF;
  badOp[38] = 0x01;
  rejects("malformed: invalid opcode", badOp);

  std::string truncInstr = fileWithOneInstr();
  truncInstr.resize(truncInstr.size() - 3); // chop the last instruction
  rejects("malformed: truncated instruction", truncInstr);

  // Dangling closure: pool holds a Function value with funcIndex 5.
  Bin dangling;
  dangling.b += "QLC1";
  dangling.u32(1);
  dangling.u32(1);
  dangling.str("main");
  dangling.i32(0);
  dangling.i32(1);
  dangling.u8(0);
  dangling.u32(0);
  dangling.u32(0);
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
    f.u32(1);
    f.u32(1);
    f.str("main");
    f.i32(0);
    f.i32(1);
    f.u8(0);
    f.u32(0);
    f.u32(0);
    f.u32(0);
    f.u32(2);
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
    f.u32(1);
    f.u32(1);
    f.str("main");
    f.i32(0);
    f.i32(1);
    f.u8(0);
    f.u32(0);
    f.u32(0);
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
  testAddi();
  testMalformed();
  std::cout << (fails == 0 ? "all codec checks passed\n"
                           : "codec checks FAILED\n");
  return fails == 0 ? 0 : 1;
}
