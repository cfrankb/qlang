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
// mov + right-operand kind (M2/S5): a literal compiles to one `mov` whose
// right operand is an inline immediate when the value's type has a v6 form for
// it (Int / Float / Bool / nil, plan decision D3) and a constant-pool read
// otherwise. The deprecated `movi` / `movc` identities are no longer emitted —
// they only still decode (plan §3 coexistence rule).
// ---------------------------------------------------------------------------
static CompiledProgram compileSrc(const char *src) {
  QLangContext ctx;
  auto program = ctx.parse(src);
  QLangCompiler compiler(std::move(program), &ctx);
  return compiler.compile();
}

// Decode every instruction in a function's code stream.
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

// First instruction with `op`, an immediate right operand of `form` and signed
// value `imm`; `out` receives it (size, kind, in-place arity) for further
// assertions.
static bool findImm(const Function &fn, Opcode op, ImmForm form, int64_t imm,
                    DecodedInstruction &out) {
  for (const auto &i : decodeAll(fn))
    if (i.op == op && i.rightKind == RightKind::Immediate &&
        i.immForm == form && i.imm == imm) {
      out = i;
      return true;
    }
  return false;
}

static bool hasImm(const Function &fn, Opcode op, ImmForm form, int64_t imm) {
  DecodedInstruction d;
  return findImm(fn, op, form, imm, d);
}

// An immediate whose form *is* the value (true / false / nil): it carries no
// operand bytes, so only the form and the payload tag identify it. `size`
// receives the instruction length (§11 item 4: 3 bytes).
static bool hasZeroImm(const Function &fn, Opcode op, ImmForm form,
                       ImmPayload payload, size_t *size = nullptr) {
  for (const auto &i : decodeAll(fn))
    if (i.op == op && i.rightKind == RightKind::Immediate &&
        i.immForm == form && i.immPayload == payload) {
      if (size)
        *size = i.size;
      return true;
    }
  return false;
}

static bool hasFloatImm(const Function &fn, Opcode op, double value) {
  for (const auto &i : decodeAll(fn))
    if (i.op == op && i.rightKind == RightKind::Immediate &&
        i.immForm == ImmForm::Float && i.immFloat == value)
      return true;
  return false;
}

// True when `fn` contains `op` with a constant-pool right operand (kind 2).
static bool hasPoolRight(const Function &fn, Opcode op) {
  for (const auto &i : decodeAll(fn))
    if (i.op == op && i.rightKind == RightKind::ConstPool)
      return true;
  return false;
}

// The v6 compiler emits only the unified spellings; the `i`/`c` identities
// survive as decoder-only aliases (plan §3).
static bool noDeprecatedSpellings(const CompiledProgram &p) {
  for (const auto &fn : p.functions)
    for (const auto &i : decodeAll(fn))
      if (i.op == Opcode::MOVI || i.op == Opcode::MOVC ||
          i.op == Opcode::ADDI || i.op == Opcode::SUBI ||
          i.op == Opcode::CMPI)
        return false;
  return true;
}

static bool poolHasInt(const Function &fn, int32_t v) {
  for (const auto &c : fn.constants)
    if (c.type() == QLangValue::Type::Int && c.asInt() == v)
      return true;
  return false;
}

// Number of pool entries of a given type (the pool-shrink assertions).
static int poolCountType(const Function &fn, QLangValue::Type t) {
  int n = 0;
  for (const auto &c : fn.constants)
    if (c.type() == t)
      n++;
  return n;
}

static void testMovImmediate() {
  try {
    // Int literal -> `mov r, i8 42`, no Int(42) pool slot (the pool shrinks).
    CompiledProgram p = compileSrc("local x = 42\n");
    DecodedInstruction d;
    check(findImm(p.functions[0], Opcode::MOV, ImmForm::I8, 42, d),
          "mov: int literal emits a signed immediate");
    check(!poolHasInt(p.functions[0], 42), "mov: no Int pool slot");
    check(noDeprecatedSpellings(p), "mov: no movi/movc emitted");

    // Bool literal -> the zero-byte true form, 3 bytes, no pool entry
    // (§11 item 4).
    CompiledProgram pb = compileSrc("local b = true\n");
    size_t boolSize = 0;
    check(hasZeroImm(pb.functions[0], Opcode::MOV, ImmForm::True,
                     ImmPayload::True, &boolSize),
          "mov: bool literal emits the true form");
    check(boolSize == 3, "mov: bool immediate costs 3 bytes");
    check(poolCountType(pb.functions[0], QLangValue::Type::Bool) == 0,
          "mov: bool literal leaves the pool");

    // nil -> the zero-byte nil form, no pool entry. A function with no return
    // statement is the construct that stores nil directly (the implicit
    // `return nil` lands in reg 0), so this looks at function #1.
    CompiledProgram pn = compileSrc("def f() {\n}\n");
    size_t nilSize = 0;
    check(hasZeroImm(pn.functions[1], Opcode::MOV, ImmForm::Nil,
                     ImmPayload::Nil, &nilSize),
          "mov: nil stores as the nil form");
    check(nilSize == 3, "mov: nil immediate costs 3 bytes");
    check(poolCountType(pn.functions[1], QLangValue::Type::Nil) == 0,
          "mov: nil leaves the pool");

    // Float literal -> the 8-byte float form, no pool entry.
    CompiledProgram pf = compileSrc("local f = 1.5\n");
    check(hasFloatImm(pf.functions[0], Opcode::MOV, 1.5),
          "mov: float literal emits the float form");
    check(poolCountType(pf.functions[0], QLangValue::Type::Float) == 0,
          "mov: float literal leaves the pool");

    // Typed decl: immediate into the target register + in-place CONV.
    CompiledProgram pt = compileSrc("local a:byte = 0\n");
    check(hasImm(pt.functions[0], Opcode::MOV, ImmForm::I8, 0),
          "mov: typed decl emits a signed immediate");

    // 64-bit literal (Qword) -> pool read: the immediate forms must not change
    // the value's type tag (D3).
    CompiledProgram pq = compileSrc("local q = 9223372036854775807\n");
    check(hasPoolRight(pq.functions[0], Opcode::MOV),
          "mov: 64-bit literal stays a pool read");

    // String literal -> pool read (strings never become immediates).
    CompiledProgram ps = compileSrc("local s = \"abc\"\n");
    check(hasPoolRight(ps.functions[0], Opcode::MOV),
          "mov: string literal is a pool read");

    // .qlc written at format version 6 (kQlcVersion); the reader accepts v6
    // only (v1-v5 and v7+ are rejected).
    std::ostringstream os(std::ios::binary);
    check(writeQlc(os, p, {}), "mov: writeQlc (v6)");
    std::string bytes = os.str();
    uint32_t version = static_cast<uint32_t>(static_cast<uint8_t>(bytes[4])) |
                       (static_cast<uint32_t>(static_cast<uint8_t>(bytes[5])) << 8) |
                       (static_cast<uint32_t>(static_cast<uint8_t>(bytes[6])) << 16) |
                       (static_cast<uint32_t>(static_cast<uint8_t>(bytes[7])) << 24);
    check(version == 6, "mov: format version 6");
    CompiledProgram back;
    std::vector<std::pair<int, std::string>> slots;
    std::string err;
    std::istringstream is(bytes, std::ios::binary);
    check(readQlc(is, back, slots, err),
          std::string("mov: readQlc v6 (") + err + ")");
    if (err.empty()) {
      bool same = back.functions.size() == p.functions.size();
      if (same) {
        const auto &a = p.functions[0];
        const auto &b = back.functions[0];
        same = a.code == b.code && a.instrOffsets == b.instrOffsets;
      }
      check(same, "mov: round-trip instruction equality");
    }
  } catch (const QLangError &e) {
    check(false, std::string("mov: ") + e.what());
  }
}

// ---------------------------------------------------------------------------
// Compiler-generated Ints (loop counters, class-metadata markers, per-field
// indices) also store as `mov` immediates, so the main function's constant
// pool holds no Type::Int at all.
// ---------------------------------------------------------------------------
static bool poolHasAnyInt(const Function &fn) {
  for (const auto &c : fn.constants)
    if (c.type() == QLangValue::Type::Int)
      return true;
  return false;
}

static void testCompilerMov() {
  try {
    // for-loop (unit/zero/direction/counter) + a two-field class (markers +
    // name->index metadata). Every compiler-generated Int is a `mov`
    // immediate; the pool must contain no Type::Int entries.
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
          "compiler-mov: no Type::Int in main pool (for + class)");
    // The for-loop's compiler-generated constants are `mov` immediates.
    check(hasImm(p.functions[0], Opcode::MOV, ImmForm::I8, 1) &&
              hasImm(p.functions[0], Opcode::MOV, ImmForm::I8, 0) &&
              hasImm(p.functions[0], Opcode::MOV, ImmForm::I8, -1),
          "compiler-mov: for step/zero/direction emitted as immediates");
    check(noDeprecatedSpellings(p), "compiler-mov: no movi/movc emitted");
  } catch (const QLangError &e) {
    check(false, std::string("compiler-mov: ") + e.what());
  }
}

// ---------------------------------------------------------------------------
// add, kind 0: compound `+=` with a literal RHS compiles to the 2-operand
// in-place form `add r, imm` (plan §2.6) — no pool slot, no third operand.
// A float RHS is now an immediate too (it left the pool in S5).
// ---------------------------------------------------------------------------
static void testAddInPlace() {
  try {
    // Int literal RHS -> `add r, i8 12`, no Int(12) pool slot.
    CompiledProgram p = compileSrc("local a = 10\na += 12\n");
    DecodedInstruction d;
    check(findImm(p.functions[0], Opcode::ADD, ImmForm::I8, 12, d),
          "add: int literal RHS emits a signed immediate");
    check(isInPlaceForm(d.op, d.rightKind) && d.size == 4,
          "add: immediate RHS is the in-place form (4 bytes)");
    check(!poolHasInt(p.functions[0], 12), "add: no Int pool slot");

    // Float literal RHS: the in-place fold stays Int-only, so the literal is
    // loaded as an inline float immediate and the add keeps its 3-operand form.
    // What matters for S5 is that the float never reaches the pool.
    CompiledProgram pf = compileSrc("local f = 1.5\nf += 1.5\n");
    check(hasFloatImm(pf.functions[0], Opcode::MOV, 1.5),
          "add: float literal RHS loads as the float form");
    check(poolCountType(pf.functions[0], QLangValue::Type::Float) == 0,
          "add: float RHS leaves the pool");
    check(noDeprecatedSpellings(pf), "add: no addi emitted");

    // .qlc round-trip of an in-place add program.
    std::ostringstream os(std::ios::binary);
    check(writeQlc(os, p, {}), "add: writeQlc");
    std::string bytes = os.str();
    CompiledProgram back;
    std::vector<std::pair<int, std::string>> slots;
    std::string err;
    std::istringstream is(bytes, std::ios::binary);
    check(readQlc(is, back, slots, err),
          std::string("add: readQlc (") + err + ")");
    if (err.empty()) {
      bool same = back.functions.size() == p.functions.size();
      if (same) {
        const auto &a = p.functions[0];
        const auto &b = back.functions[0];
        same = a.code == b.code && a.instrOffsets == b.instrOffsets;
      }
      check(same, "add: round-trip instruction equality");
    }
  } catch (const QLangError &e) {
    check(false, std::string("add: ") + e.what());
  }
}

// ---------------------------------------------------------------------------
// sub, kind 0: compound `-=` with a literal RHS compiles to the in-place
// `sub r, imm` (plan §2.6). Both the Int and the Float RHS are inline
// immediates now, so neither costs a pool slot (plan decision D3).
// ---------------------------------------------------------------------------
static void testSubInPlace() {
  try {
    // Int literal RHS -> `sub r, i8 12`, no Int(12) pool slot.
    CompiledProgram p = compileSrc("local a = 10\na -= 12\n");
    DecodedInstruction d;
    check(findImm(p.functions[0], Opcode::SUB, ImmForm::I8, 12, d),
          "sub: int literal RHS emits a signed immediate");
    check(isInPlaceForm(d.op, d.rightKind) && d.size == 4,
          "sub: immediate RHS is the in-place form (4 bytes)");
    check(!poolHasInt(p.functions[0], 12), "sub: no Int pool slot");

    // Float literal RHS: Int-only fold, so an inline float immediate plus the
    // 3-operand sub — and no pool entry.
    CompiledProgram pf = compileSrc("local f = 1.5\nf -= 1.5\n");
    check(hasFloatImm(pf.functions[0], Opcode::MOV, 1.5),
          "sub: float literal RHS loads as the float form");
    check(poolCountType(pf.functions[0], QLangValue::Type::Float) == 0,
          "sub: float RHS leaves the pool");
    check(noDeprecatedSpellings(pf), "sub: no subi emitted");

    // .qlc round-trip of an in-place sub program.
    std::ostringstream os(std::ios::binary);
    check(writeQlc(os, p, {}), "sub: writeQlc");
    std::string bytes = os.str();
    CompiledProgram back;
    std::vector<std::pair<int, std::string>> slots;
    std::string err;
    std::istringstream is(bytes, std::ios::binary);
    check(readQlc(is, back, slots, err),
          std::string("sub: readQlc (") + err + ")");
    if (err.empty()) {
      bool same = back.functions.size() == p.functions.size();
      if (same) {
        const auto &a = p.functions[0];
        const auto &b = back.functions[0];
        same = a.code == b.code && a.instrOffsets == b.instrOffsets;
      }
      check(same, "sub: round-trip instruction equality");
    }
  } catch (const QLangError &e) {
    check(false, std::string("sub: ") + e.what());
  }
}

// ---------------------------------------------------------------------------
// cmp + j*: an Int-vs-Int comparison in `if` fuses to one `cmp` plus a flag
// jump (no comparison-result temp, no pool slot for the boolean). The right
// operand's *kind* is what distinguishes the old spellings: a literal is an
// inline immediate (what `cmpi` used to mean), a variable stays a register.
// Non-Int comparisons are not fused at all (legacy EQ/GT + JMPIFFALSE).
// ---------------------------------------------------------------------------
static bool hasAnyOp(const Function &fn, std::initializer_list<Opcode> ops) {
  for (const auto &i : decodeAll(fn))
    for (Opcode o : ops)
      if (i.op == o)
        return true;
  return false;
}

// True when `fn` has no `cmp` with the given right-operand kind.
static bool noCmpKind(const Function &fn, RightKind kind) {
  for (const auto &i : decodeAll(fn))
    if (i.op == Opcode::CMP && i.rightKind == kind)
      return false;
  return true;
}

static void testCmpJump() {
  try {
    // Literal comparison in `if` -> `cmp r, i8 3` + a j* (no comparison-result
    // temp, no JMPIFFALSE for the condition, no `cmpi`).
    CompiledProgram p = compileSrc("local r = 0\nif (5 > 3) { r = 1 }\n");
    check(hasImm(p.functions[0], Opcode::CMP, ImmForm::I8, 3),
          "cmp: literal operand emits an inline immediate");
    check(noCmpKind(p.functions[0], RightKind::Register),
          "cmp: literal comparison has no register-right cmp");
    check(hasAnyOp(p.functions[0],
                   {Opcode::JE, Opcode::JNE, Opcode::JG, Opcode::JGE,
                    Opcode::JL, Opcode::JLE}),
          "cmp: int literal if emits a j*");
    check(!hasAnyOp(p.functions[0], {Opcode::JMPIFFALSE, Opcode::JMPIFTRUE}),
          "cmp: int literal if not JMPIFFALSE");
    check(noDeprecatedSpellings(p), "cmp: no cmpi emitted");

    // The loop-bound guard compares two registers (kind 1): `cmp x, end` + a
    // flag jump. (`if` does not fuse variable operands — inferType only records
    // Array/Table for a variable — so the loop is the register-right cmp path.)
    CompiledProgram pr = compileSrc("for x = 1 to 3 {\n}\n");
    check(!noCmpKind(pr.functions[0], RightKind::Register),
          "cmp: loop guard compares register-to-register");

    // Float comparison -> legacy path (no CMP/j* for the condition).
    CompiledProgram pf = compileSrc("local r = 0\nif (1.5 < 2.5) { r = 1 }\n");
    check(!hasAnyOp(pf.functions[0],
                    {Opcode::CMP, Opcode::CMPI, Opcode::JE, Opcode::JNE,
                     Opcode::JG, Opcode::JGE, Opcode::JL, Opcode::JLE}),
          "cmp: float comparison not fused");

    // .qlc round-trip of a fused program.
    std::ostringstream os(std::ios::binary);
    check(writeQlc(os, p, {}), "cmp: writeQlc");
    std::string bytes = os.str();
    CompiledProgram back;
    std::vector<std::pair<int, std::string>> slots;
    std::string err;
    std::istringstream is(bytes, std::ios::binary);
    check(readQlc(is, back, slots, err),
          std::string("cmp: readQlc (") + err + ")");
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
// Since S5 only the pool-resident types (strings, blobs, containers, closures)
// reach the pool at all, so the shrink is asserted alongside the dedup.
// ---------------------------------------------------------------------------
static bool poolHasDuplicates(const Function &fn) {
  for (size_t i = 0; i < fn.constants.size(); i++)
    for (size_t j = i + 1; j < fn.constants.size(); j++)
      if (valueEq(fn.constants[i], fn.constants[j]))
        return true;
  return false;
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

    // nil and float literals left the pool (plan decision D3): the call-scratch
    // clears store the zero-byte nil form, so two calls leave no nil entry at
    // all, and the float operands are inline immediates.
    CompiledProgram pn = compileSrc(
        "def f(x) { return x }\n"
        "f(1.5 + 2.5)\n"
        "f(3.5 + 4.5)\n");
    check(poolCountType(pn.functions[0], QLangValue::Type::Nil) == 0,
          "pool-dedup: nil clears leave no pool entry");
    check(poolCountType(pn.functions[0], QLangValue::Type::Float) == 0,
          "pool-dedup: float operands leave no pool entry");
    check(hasZeroImm(pn.functions[0], Opcode::MOV, ImmForm::Nil,
                     ImmPayload::Nil),
          "pool-dedup: nil clears use the nil immediate");

    // Repeated float/bool literals leave no pool entries at all, so there is
    // nothing left to dedup: every store carries the value inline.
    CompiledProgram pf = compileSrc(
        "local a = 1.5\n"
        "local b = 1.5\n"
        "local c = 2.5\n"
        "local d = true\n"
        "local e = true\n"
        "local f = false\n");
    check(poolCountType(pf.functions[0], QLangValue::Type::Float) == 0,
          "pool-dedup: float literals leave the pool");
    check(poolCountType(pf.functions[0], QLangValue::Type::Bool) == 0,
          "pool-dedup: bool literals leave the pool");
    check(hasFloatImm(pf.functions[0], Opcode::MOV, 1.5) &&
              hasFloatImm(pf.functions[0], Opcode::MOV, 2.5),
          "pool-dedup: both float values are inline immediates");
    check(hasZeroImm(pf.functions[0], Opcode::MOV, ImmForm::True,
                     ImmPayload::True) &&
              hasZeroImm(pf.functions[0], Opcode::MOV, ImmForm::False,
                         ImmPayload::False),
          "pool-dedup: both bool values are inline immediates");
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
    check(std::strcmp(Bytecode::name(Opcode::Nop), "nop") == 0, "nop: name");
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
    check(std::strcmp(Bytecode::name(Opcode::DEEPCOPY), "deepcopy") == 0,
          "deepcopy: name");
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
// v6 layout: code blob (empty) + offset table (empty) + loc table (empty).
static std::string minimalFile() {
  Bin f;
  f.b += "QLC1";
  f.u32(6); // version
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
  f.u32(6); // version
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
  badVersion[4] = 5;  // v6 is current; the previous format must be rejected
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
  // metadata + codeSize). Set it to 73: a valid 7-bit identity that is not a
  // defined opcode (kOpcodeUsedCount == 73; INDEXGET/INDEXPUT took 71/72). It
  // decodes as a 4-byte 3-operand form, then fails isValidOpcode.
  badOp[37] = 73;
  rejects("malformed: invalid opcode", badOp);

  // Truncated instruction: codeSize=3, but the ADD at offset 0 needs 4 bytes.
  Bin truncInstr;
  truncInstr.b += "QLC1";
  truncInstr.u32(6); // version
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
  dangling.u32(6); // version
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
    f.u32(6); // version
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
    f.u32(6); // version
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

// ---------------------------------------------------------------------------
// INDEXGET / INDEXPUT: polymorphic subscript (SPEC §bytecodes "OpCode
// indexget"/"indexput"). Appended last (identities 71, 72) so existing .qlc
// opcode numbers stay valid. Three register operands, no i32 slot. The
// compiler emits them for every postfix [...] read or write, replacing the
// ARRGET/ARRPUT emission; those remain valid opcodes.
// ---------------------------------------------------------------------------
static void testIndexSubscript() {
  try {
    check(kOpcodeUsedCount == 73, "index: kOpcodeUsedCount is 73");
    check(static_cast<uint8_t>(Opcode::INDEXGET) == 71,
          "indexget: identity 71 (appended last)");
    check(static_cast<uint8_t>(Opcode::INDEXPUT) == 72,
          "indexput: identity 72 (appended last)");
    check(std::strcmp(Bytecode::name(Opcode::INDEXGET), "indexget") == 0,
          "indexget: name");
    check(std::strcmp(Bytecode::name(Opcode::INDEXPUT), "indexput") == 0,
          "indexput: name");
    check(isValidOpcode(Opcode::INDEXGET) && isValidOpcode(Opcode::INDEXPUT),
          "index: both are valid opcodes");
    check(baseInstructionSize(Opcode::INDEXGET) == 4 &&
              baseInstructionSize(Opcode::INDEXPUT) == 4,
          "index: 4-byte base size (op + 3 register operands)");

    // 4-byte base-form encode/decode round-trip.
    DecodedInstruction in;
    in.op = Opcode::INDEXGET;
    in.a = 1;
    in.b = 2;
    in.c = 3;
    std::vector<uint8_t> buf;
    encodeInstruction(buf, in);
    check(buf.size() == 4 && buf[0] == 71 && buf[1] == 1 && buf[2] == 2 &&
              buf[3] == 3,
          "indexget: encodes to 4 bytes");
    DecodedInstruction out;
    decodeInstruction(buf.data(), 0, buf.size(), out);
    check(out.op == Opcode::INDEXGET && !out.escaped && out.size == 4 &&
              out.a == 1 && out.b == 2 && out.c == 3,
          "indexget: decode round-trip");

    in = DecodedInstruction();
    in.op = Opcode::INDEXPUT;
    in.a = 4;
    in.b = 5;
    in.c = 6;
    buf.clear();
    encodeInstruction(buf, in);
    check(buf.size() == 4 && buf[0] == 72 && buf[1] == 4 && buf[2] == 5 &&
              buf[3] == 6,
          "indexput: encodes to 4 bytes");
    decodeInstruction(buf.data(), 0, buf.size(), out);
    check(out.op == Opcode::INDEXPUT && !out.escaped && out.size == 4 &&
              out.a == 4 && out.b == 5 && out.c == 6,
          "indexput: decode round-trip");

    // No renumbering: the previously last opcode keeps its identity, and the
    // single-kind subscript opcodes stay valid (retained, no longer emitted).
    check(static_cast<uint8_t>(Opcode::DEEPCOPY) == 70,
          "index: deepcopy identity unchanged (70)");
    check(isValidOpcode(Opcode::ARRGET) && isValidOpcode(Opcode::ARRPUT),
          "index: arrget/arrput still valid opcodes");
  } catch (const QLangError &e) {
    check(false, std::string("index: ") + e.what());
  }
}

// The compiler lowers every postfix [...] to INDEXGET/INDEXPUT (SPEC
// §declarations: `t["age"] = 33` is equivalent to `t.age`), and the empty
// subscript `ab[] = v` still lowers to ARRAPP.
static void testIndexLowering() {
  try {
    QLangContext ctx;
    QLangCompiler compiler(
        ctx.parse(R"(
local t:table = {age=1}
local v = t["age"]
t["age"] = 2
local ab = [1,2]
ab[0] = 3
local w = ab[1]
)"),
        &ctx);
    CompiledProgram p = compiler.compile();
    const Function &fn = p.functions[0];
    int gets = 0, puts = 0, legacy = 0, appends = 0;
    for (size_t off : fn.instrOffsets) {
      DecodedInstruction d;
      decodeInstruction(fn.code.data(), off, fn.code.size(), d);
      if (d.op == Opcode::INDEXGET)
        gets++;
      if (d.op == Opcode::INDEXPUT)
        puts++;
      if (d.op == Opcode::ARRGET || d.op == Opcode::ARRPUT)
        legacy++;
      if (d.op == Opcode::ARRAPP)
        appends++;
    }
    check(gets == 2, "index-lowering: two INDEXGET emitted");
    check(puts == 2, "index-lowering: two INDEXPUT emitted");
    check(legacy == 0, "index-lowering: no ARRGET/ARRPUT emitted");
    check(appends == 0, "index-lowering: no spurious ARRAPP");

    QLangContext c2;
    QLangCompiler k(c2.parse("local ab = [1,2]\nab[] = 3\n"), &c2);
    CompiledProgram p2 = k.compile();
    const Function &f2 = p2.functions[0];
    bool hasAppend = false, hasPut = false;
    for (size_t off : f2.instrOffsets) {
      DecodedInstruction d;
      decodeInstruction(f2.code.data(), off, f2.code.size(), d);
      if (d.op == Opcode::ARRAPP)
        hasAppend = true;
      if (d.op == Opcode::INDEXPUT)
        hasPut = true;
    }
    check(hasAppend, "index-lowering: empty subscript lowers to ARRAPP");
    check(!hasPut, "index-lowering: empty subscript emits no INDEXPUT");
  } catch (const QLangError &e) {
    check(false, std::string("index-lowering: ") + e.what());
  }
}

int main() {
  testValues();
  testProgram();
  testMovImmediate();
  testCompilerMov();
  testAddInPlace();
  testSubInPlace();
  testCmpJump();
  testPoolDedup();
  testNop();
  testDeepcopy();
  testDeepcopyLowering();
  testIndexSubscript();
  testIndexLowering();
  testMalformed();
  std::cout << (fails == 0 ? "all codec checks passed\n"
                           : "codec checks FAILED\n");
  return fails == 0 ? 0 : 1;
}
