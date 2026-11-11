// .qlc codec check: value round-trips, program round-trip with native wiring,
// the v8 version boundary and the v6-encoding load-time validation (modifier
// fields, permitted kinds, the size guard), a malformed-input robustness
// suite, and the script-corpus .qlc round-trip + execute gate
// (docs/SERIALIZATION.md §9, docs/plans/m2-pipeline-switch-plan.md §11,
//  docs/plans/phase3-opcode-retirement-plan.md S4).
#include <climits>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "qlang/QLangBytecodeIO.h"
#include "qlang/QLangCompiler.h"
#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"
#include "qlang/QLangVM.h"

#ifndef QLANG_TEST_SCRIPTS_DIR
#error "QLANG_TEST_SCRIPTS_DIR must be defined by the build (tests/scripts)"
#endif

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

// True when two constant pools hold the same values in the same order (the
// v8 module pool is ordered, so a round trip must preserve it exactly).
static bool poolEq(const std::vector<QLangValue> &a,
                   const std::vector<QLangValue> &b) {
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); i++)
    if (!valueEq(a[i], b[i]))
      return false;
  return true;
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
    }
    check(fnsOk, "program: functions identical");
    // v8: the pool is the program's, so the round trip compares one table.
    check(poolEq(compiled.constants, back.constants),
          "program: constant pool identical");
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

// The pool helpers below take CompiledProgram::constants: since v8 a program
// has one pool, not one per function.
static bool poolHasInt(const std::vector<QLangValue> &pool, int32_t v) {
  for (const auto &c : pool)
    if (c.type() == QLangValue::Type::Int && c.asInt() == v)
      return true;
  return false;
}

// Number of pool entries of a given type (the pool-shrink assertions).
static int poolCountType(const std::vector<QLangValue> &pool,
                         QLangValue::Type t) {
  int n = 0;
  for (const auto &c : pool)
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
    check(!poolHasInt(p.constants, 42), "mov: no Int pool slot");

    // Bool literal -> the zero-byte true form, 3 bytes, no pool entry
    // (§11 item 4).
    CompiledProgram pb = compileSrc("local b = true\n");
    size_t boolSize = 0;
    check(hasZeroImm(pb.functions[0], Opcode::MOV, ImmForm::True,
                     ImmPayload::True, &boolSize),
          "mov: bool literal emits the true form");
    check(boolSize == 3, "mov: bool immediate costs 3 bytes");
    check(poolCountType(pb.constants, QLangValue::Type::Bool) == 0,
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
    check(poolCountType(pn.constants, QLangValue::Type::Nil) == 0,
          "mov: nil leaves the pool");

    // Float literal -> the 8-byte float form, no pool entry.
    CompiledProgram pf = compileSrc("local f = 1.5\n");
    check(hasFloatImm(pf.functions[0], Opcode::MOV, 1.5),
          "mov: float literal emits the float form");
    check(poolCountType(pf.constants, QLangValue::Type::Float) == 0,
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

    // .qlc written at format version 9 (kQlcVersion); the reader accepts v9
    // only (v1-v8 and v10+ are rejected).
    std::ostringstream os(std::ios::binary);
    check(writeQlc(os, p, {}), "mov: writeQlc (v9)");
    std::string bytes = os.str();
    uint32_t version = static_cast<uint32_t>(static_cast<uint8_t>(bytes[4])) |
                       (static_cast<uint32_t>(static_cast<uint8_t>(bytes[5])) << 8) |
                       (static_cast<uint32_t>(static_cast<uint8_t>(bytes[6])) << 16) |
                       (static_cast<uint32_t>(static_cast<uint8_t>(bytes[7])) << 24);
    check(version == 9, "mov: format version 9");
    CompiledProgram back;
    std::vector<std::pair<int, std::string>> slots;
    std::string err;
    std::istringstream is(bytes, std::ios::binary);
    check(readQlc(is, back, slots, err),
          std::string("mov: readQlc v9 (") + err + ")");
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
static bool poolHasAnyInt(const std::vector<QLangValue> &pool) {
  for (const auto &c : pool)
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
    check(!poolHasAnyInt(p.constants),
          "compiler-mov: no Type::Int in main pool (for + class)");
    // The for-loop's compiler-generated constants are `mov` immediates.
    check(hasImm(p.functions[0], Opcode::MOV, ImmForm::I8, 1) &&
              hasImm(p.functions[0], Opcode::MOV, ImmForm::I8, 0) &&
              hasImm(p.functions[0], Opcode::MOV, ImmForm::I8, -1),
          "compiler-mov: for step/zero/direction emitted as immediates");
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
    check(!poolHasInt(p.constants, 12), "add: no Int pool slot");

    // Float literal RHS: the in-place fold stays Int-only, so the literal is
    // loaded as an inline float immediate and the add keeps its 3-operand form.
    // What matters for S5 is that the float never reaches the pool.
    CompiledProgram pf = compileSrc("local f = 1.5\nf += 1.5\n");
    check(hasFloatImm(pf.functions[0], Opcode::MOV, 1.5),
          "add: float literal RHS loads as the float form");
    check(poolCountType(pf.constants, QLangValue::Type::Float) == 0,
          "add: float RHS leaves the pool");

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
    check(!poolHasInt(p.constants, 12), "sub: no Int pool slot");

    // Float literal RHS: Int-only fold, so an inline float immediate plus the
    // 3-operand sub — and no pool entry.
    CompiledProgram pf = compileSrc("local f = 1.5\nf -= 1.5\n");
    check(hasFloatImm(pf.functions[0], Opcode::MOV, 1.5),
          "sub: float literal RHS loads as the float form");
    check(poolCountType(pf.constants, QLangValue::Type::Float) == 0,
          "sub: float RHS leaves the pool");

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
// compare + j*: a comparison in `if` fuses to one compare plus a flag jump (no
// comparison-result temp, no pool slot for the boolean). The right operand's
// *kind* is what distinguishes the old spellings: a literal is an inline
// immediate (what `cmpi` used to mean), a variable stays a register.
// Both operands statically Int keep `cmp`; anything else lowers to the flag
// compare whose set bits are the condition's false states (the *dual*
// relation) plus the same jump, so no guard pays for a boolean temporary
// (plan docs/plans/m3-flag-flow-plan.md D1/D2).
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
    // temp and no register-right cmp for the condition).
    CompiledProgram p = compileSrc("local r = 0\nif (5 > 3) { r = 1 }\n");
    check(hasImm(p.functions[0], Opcode::CMP, ImmForm::I8, 3),
          "cmp: literal operand emits an inline immediate");
    check(noCmpKind(p.functions[0], RightKind::Register),
          "cmp: literal comparison has no register-right cmp");
    check(hasAnyOp(p.functions[0],
                   {Opcode::JE, Opcode::JNE, Opcode::JG, Opcode::JGE,
                    Opcode::JL, Opcode::JLE}),
          "cmp: int literal if emits a j*");

    // The loop-bound guard compares two registers (kind 1): `cmp x, end` + a
    // flag jump. (`if` keeps `cmp` only when both operands are statically Int,
    // which inferType pins down for Int literals alone.)
    CompiledProgram pr = compileSrc("for x = 1 to 3 {\n}\n");
    check(!noCmpKind(pr.functions[0], RightKind::Register),
          "cmp: loop guard compares register-to-register");

    // Float comparison -> the dual flag compare `gtef r, f64 2.5` + `jge`: the
    // literal still rides inline and no boolean temp is built.
    CompiledProgram pf = compileSrc("local r = 0\nif (1.5 < 2.5) { r = 1 }\n");
    check(hasFloatImm(pf.functions[0], Opcode::GTEF, 2.5),
          "cmp: float comparison fuses to an inline-immediate gtef");
    check(hasAnyOp(pf.functions[0], {Opcode::JGE}),
          "cmp: float comparison emits its dual jump");
    check(!hasAnyOp(pf.functions[0], {Opcode::CMP, Opcode::EQ}),
          "cmp: float comparison builds no cmp/EQ pair");

    // String comparison against a pool constant -> `eqf r, c#k` + `jne`
    // (plan M3 D5: a flag compare accepts a pool right operand).
    CompiledProgram ps =
        compileSrc("local r = 0\nlocal s = \"a\"\nif (s == \"a\") { r = 1 }\n");
    check(hasPoolRight(ps.functions[0], Opcode::EQF),
          "cmp: string comparison fuses to a pool-right eqf");
    check(hasAnyOp(ps.functions[0], {Opcode::JNE}),
          "cmp: string comparison emits jne");
    check(!hasAnyOp(ps.functions[0], {Opcode::EQ}),
          "cmp: string comparison builds no boolean EQ temp");

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
static bool poolHasDuplicates(const std::vector<QLangValue> &pool) {
  for (size_t i = 0; i < pool.size(); i++)
    for (size_t j = i + 1; j < pool.size(); j++)
      if (valueEq(pool[i], pool[j]))
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
    check(poolCountType(p.constants, QLangValue::Type::String) == 2,
          "pool-dedup: repeated strings fold (2 unique, not 3)");
    check(!poolHasDuplicates(p.constants),
          "pool-dedup: no duplicate pool entries (strings)");

    // nil and float literals left the pool (plan decision D3): the float
    // operands are inline immediates, and a scratch clear (when one exists)
    // stores the zero-byte nil form, never a pool entry. A call-free argument
    // (`1.5 + 2.5`) compiles straight into its call-arg slot, so no scratch is
    // allocated and no clear is emitted at all.
    CompiledProgram pn = compileSrc(
        "def f(x) { return x }\n"
        "f(1.5 + 2.5)\n"
        "f(3.5 + 4.5)\n");
    check(poolCountType(pn.constants, QLangValue::Type::Nil) == 0,
          "pool-dedup: nil clears leave no pool entry");
    check(poolCountType(pn.constants, QLangValue::Type::Float) == 0,
          "pool-dedup: float operands leave no pool entry");
    check(!hasZeroImm(pn.functions[0], Opcode::MOV, ImmForm::Nil,
                      ImmPayload::Nil),
          "pool-dedup: call-free arg lands in its slot (no scratch clear)");

    // A call-shaped argument before the last one still needs the shared scratch:
    // its own arg slots would land on the enclosing call's remaining slots. That
    // scratch is cleared with the nil immediate, so it still leaves no entry.
    CompiledProgram ps = compileSrc(
        "def f(x, y) { return x }\n"
        "def g() { return 1.5 }\n"
        "f(g(), 2.5)\n"
        "f(g(), 3.5)\n");
    check(poolCountType(ps.constants, QLangValue::Type::Nil) == 0,
          "pool-dedup: scratch clears leave no pool entry");
    check(hasZeroImm(ps.functions[0], Opcode::MOV, ImmForm::Nil,
                     ImmPayload::Nil),
          "pool-dedup: scratch clears use the nil immediate");

    // Repeated float/bool literals leave no pool entries at all, so there is
    // nothing left to dedup: every store carries the value inline.
    CompiledProgram pf = compileSrc(
        "local a = 1.5\n"
        "local b = 1.5\n"
        "local c = 2.5\n"
        "local d = true\n"
        "local e = true\n"
        "local f = false\n");
    check(poolCountType(pf.constants, QLangValue::Type::Float) == 0,
          "pool-dedup: float literals leave the pool");
    check(poolCountType(pf.constants, QLangValue::Type::Bool) == 0,
          "pool-dedup: bool literals leave the pool");
    check(hasFloatImm(pf.functions[0], Opcode::MOV, 1.5) &&
              hasFloatImm(pf.functions[0], Opcode::MOV, 2.5),
          "pool-dedup: both float values are inline immediates");
    check(hasZeroImm(pf.functions[0], Opcode::MOV, ImmForm::True,
                     ImmPayload::True) &&
              hasZeroImm(pf.functions[0], Opcode::MOV, ImmForm::False,
                         ImmPayload::False),
          "pool-dedup: both bool values are inline immediates");
    check(!poolHasDuplicates(pf.constants),
          "pool-dedup: no duplicate pool entries (float/bool)");

    // Nested collections: identical array literals fold, distinct do not.
    CompiledProgram pa = compileSrc(
        "local a = [1.5, \"x\"]\n"
        "local b = [1.5, \"x\"]\n"
        "local c = [1.5, \"y\"]\n");
    check(poolCountType(pa.constants, QLangValue::Type::Array) == 2,
          "pool-dedup: identical arrays fold (2 unique, not 3)");
    check(!poolHasDuplicates(pa.constants),
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
    // v8: the program has ONE pool, so the duplicate scan covers values that
    // live in different functions as well (SPEC §bytecodes "unified constant
    // table").
    check(!poolHasDuplicates(pr.constants),
          "pool-dedup: no duplicates in the program pool");

    // Cross-def dedup: the same string literal in two `def` bodies folds to
    // one slot. Before v8 each function had its own pool, so the literal was
    // stored once per function.
    CompiledProgram px = compileSrc(
        "def a() { return \"shared\" }\n"
        "def b() { return \"shared\" }\n"
        "local x = a()\n"
        "local y = b()\n");
    check(px.functions.size() == 3, "pool-dedup: cross-def program has 3 functions");
    int shared = 0;
    for (const auto &c : px.constants)
      if (c.type() == QLangValue::Type::String && c.asString() == "shared")
        shared++;
    check(shared == 1,
          "pool-dedup: one literal used by two defs occupies one pool slot");
    check(!poolHasDuplicates(px.constants),
          "pool-dedup: cross-def pool is duplicate-free");
  } catch (const QLangError &e) {
    check(false, std::string("pool-dedup: ") + e.what());
  }
}

// ---------------------------------------------------------------------------
// Nop: a zero-operand no-op opcode (SPEC §bytecodes "OpCode nop"). The compiler
// never emits it; the codec must round-trip it and the VM must execute it.
// Identity 61 after the Phase 3 S3 compaction (it was 68 before the deprecated
// spellings were retired).
// ---------------------------------------------------------------------------
static void testNop() {
  try {
    // Identity, name, and size.
    check(static_cast<uint8_t>(Opcode::Nop) == 61,
          "nop: identity 61 (after the S3 compaction)");
    check(std::strcmp(Bytecode::name(Opcode::Nop), "nop") == 0, "nop: name");
    check(isValidOpcode(Opcode::Nop), "nop: valid opcode");
    check(baseInstructionSize(Opcode::Nop) == 1, "nop: 1-byte base size");

    // 1-byte encode/decode round-trip.
    DecodedInstruction in;
    in.op = Opcode::Nop;
    std::vector<uint8_t> buf;
    encodeInstruction(buf, in);
    check(buf.size() == 1 && buf[0] == 61, "nop: encodes to 1 byte");
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

    // .qlc round-trip of a hand-built program: Nop; mov r0, 42 (kind 0, i8);
    // RETURN 0.
    Function fn;
    fn.name = "main";
    fn.paramCount = 0;
    fn.registerCount = 1;
    DecodedInstruction d1;
    d1.op = Opcode::Nop;
    DecodedInstruction d2;
    d2.op = Opcode::MOV;
    d2.escaped = true;
    d2.modifier = 0x40;  // left byte, right kind immediate, form i8
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
// Two register operands, no i32 slot (same shape as MOV). The compiler lowers
// a `deepcopy(x)` call directly to this opcode. Identity 63 after the Phase 3
// S3 compaction (it was 70 before the deprecated spellings were retired).
// ---------------------------------------------------------------------------
static void testDeepcopy() {
  try {
    // Identity, name, validity, and size.
    check(static_cast<uint8_t>(Opcode::DEEPCOPY) == 63,
          "deepcopy: identity 63 (after the S3 compaction)");
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
    check(buf.size() == 3 && buf[0] == 63 && buf[1] == 3 && buf[2] == 5,
          "deepcopy: encodes to 3 bytes");
    DecodedInstruction out;
    decodeInstruction(buf.data(), 0, buf.size(), out);
    check(out.op == Opcode::DEEPCOPY && !out.escaped && out.size == 3 &&
              out.a == 3 && out.b == 5,
          "deepcopy: decode round-trip");

    // .qlc round-trip of a hand-built program: mov r1, 7 (kind 0, i8);
    // DEEPCOPY 0, 1; RETURN 0.
    Function fn;
    fn.name = "main";
    fn.paramCount = 0;
    fn.registerCount = 2;
    DecodedInstruction d1;
    d1.op = Opcode::MOV;
    d1.escaped = true;
    d1.modifier = 0x40;  // left byte, right kind immediate, form i8
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
  // The §4.6 tail of a v9 image: the module name and the export and import
  // tables (here both empty).
  void tail(const std::string &module = "main") {
    str(module);
    u32(0); // exports
    u32(0); // imports
  }
};

// A valid file without its §4.6 tail: one empty top-level function, no
// natives/vars, 1 register. v9 layout: code blob (empty) + offset table
// (empty) + loc table (empty), then the module-level pool section (empty)
// before the native-slot table, the variable map and the tail. `topName`
// selects the runnable `main` or a library module's `<init>`.
static std::string minimalFileTailless(const char *topName = "main") {
  Bin f;
  f.b += "QLC1";
  f.u32(kQlcVersion); // version
  f.u32(1); // funcCount
  f.str(topName);
  f.i32(0); // paramCount
  f.i32(1); // registerCount
  f.u8(0);  // isGenerator
  f.u32(0); // captures
  f.u32(0); // codeSize (empty code stream)
  f.u32(0); // offCount (no instructions)
  f.u32(0); // locCount
  f.u32(0); // module pool
  f.u32(0); // natives
  f.u32(0); // vars
  f.u32(0); // constVars
  f.i32(1); // registerCount
  return f.b;
}

// minimalFileTailless plus the standard §4.6 tail: a complete valid v9 image.
static std::string minimalFile() {
  Bin f;
  f.b = minimalFileTailless();
  f.tail();
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
  f.u32(kQlcVersion); // version
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
  f.u32(0); // module pool
  f.u32(0); // natives
  f.u32(0); // vars
  f.u32(0); // constVars
  f.i32(1); // registerCount
  f.tail(); // §4.6 module name + export/import tables
  return f.b;
}

static void testMalformed() {
  rejects("malformed: empty file", "");
  rejects("malformed: short header", "QLC1\x01\x00");

  std::string badMagic = minimalFile();
  badMagic[0] = 'X';
  rejects("malformed: bad magic", badMagic);

  std::string badVersion = minimalFile();
  badVersion[4] = 7; // v8 is current; the previous format must be rejected
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
  // metadata + codeSize). Set it to kOpcodeUsedCount: a valid 7-bit identity
  // just past the last defined one (74 since Phase 3c added getimport and
  // callimport). It decodes as a 4-byte 3-operand form, then fails
  // isValidOpcode.
  badOp[37] = kOpcodeUsedCount;
  rejects("malformed: invalid opcode", badOp);

  // Truncated instruction: codeSize=3, but the ADD at offset 0 needs 4 bytes.
  Bin truncInstr;
  truncInstr.b += "QLC1";
  truncInstr.u32(kQlcVersion); // version
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
  truncInstr.u32(0); // module pool
  truncInstr.u32(0); // natives
  truncInstr.u32(0); // vars
  truncInstr.u32(0); // constVars
  truncInstr.i32(1);
  truncInstr.tail();
  rejects("malformed: truncated instruction", truncInstr.b);

  // Dangling closure: pool holds a Function value with funcIndex 5.
  Bin dangling;
  dangling.b += "QLC1";
  dangling.u32(kQlcVersion); // version
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
  dangling.tail();
  rejects("malformed: dangling closure funcIndex", dangling.b);

  // Native slot table invariants.
  auto nativeFile = [&](int slotA, const char *nameA, int slotB,
                        const char *nameB) {
    Bin f;
    f.b += "QLC1";
    f.u32(kQlcVersion); // version
    f.u32(1);
    f.str("main");
    f.i32(0);
    f.i32(1);
    f.u8(0);
    f.u32(0); // captures
    f.u32(0); // codeSize
    f.u32(0); // offCount
    f.u32(0); // locCount
    f.u32(0); // module pool
    f.u32(2); // natives
    f.i32(slotA);
    f.str(nameA);
    f.i32(slotB);
    f.str(nameB);
    f.u32(0);
    f.u32(0);
    f.i32(1);
    f.tail();
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
    f.u32(kQlcVersion); // version
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
    f.tail();
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

  // §4.6 (v9): the module identity and the export/import tables. Every entry
  // is a handle into this image, so names, kinds and references are validated
  // at load time (external-modules plan Phase 3a).
  auto readTail = [](const std::string &bytes, CompiledProgram &prog,
                     std::string &why) {
    std::istringstream is(bytes, std::ios::binary);
    std::vector<std::pair<int, std::string>> slots;
    return readQlc(is, prog, slots, why);
  };
  // A v8 image stops after the register count: no §4.6 section at all.
  rejects("malformed: missing module section", minimalFileTailless());

  Bin noName;
  noName.str(""); // empty module name
  noName.u32(0);
  noName.u32(0);
  rejects("malformed: empty module name", minimalFileTailless() + noName.b);

  // One export entry, described the way §4.6 spells it.
  auto oneExport = [](uint8_t kind, const char *name, int32_t ref) {
    Bin t;
    t.str("main");
    t.u32(1); // one export
    t.u8(kind);
    t.str(name);
    t.i32(ref);
    t.u32(0); // no imports
    return minimalFileTailless() + t.b;
  };
  rejects("malformed: invalid export kind", oneExport(3, "f", 0));
  rejects("malformed: empty export name", oneExport(2, "", 0));
  rejects("malformed: negative export reference", oneExport(2, "f", -1));
  // The image has one function and one register, so index 1 is out of range
  // for either kind of reference.
  rejects("malformed: export function index out of range",
          oneExport(static_cast<uint8_t>(ExportKind::Func), "f", 1));
  rejects("malformed: export register out of range",
          oneExport(static_cast<uint8_t>(ExportKind::Var), "v", 1));
  {
    Bin t;
    t.str("main");
    t.u32(2);
    t.u8(static_cast<uint8_t>(ExportKind::Var));
    t.str("v");
    t.i32(0);
    t.u8(static_cast<uint8_t>(ExportKind::Const));
    t.str("v"); // the same name twice
    t.i32(0);
    t.u32(0);
    rejects("malformed: duplicate export name", minimalFileTailless() + t.b);
  }

  auto oneImport = [](uint8_t kind, const char *mod, const char *name,
                      int32_t ref) {
    Bin t;
    t.str("main");
    t.u32(0); // no exports
    t.u32(1); // one import
    t.str(mod);
    t.str(name);
    t.u8(kind);
    t.i32(ref);
    t.str(""); // no alias
    return minimalFileTailless() + t.b;
  };
  rejects("malformed: invalid import kind", oneImport(3, "m", "f", 0));
  rejects("malformed: empty import module", oneImport(2, "", "f", 0));
  rejects("malformed: empty import name", oneImport(2, "m", "", 0));
  rejects("malformed: negative import reference", oneImport(2, "m", "f", -1));

  // A valid tail survives the round trip.
  {
    Bin t;
    t.str("my_module");
    t.u32(2);
    t.u8(static_cast<uint8_t>(ExportKind::Func));
    t.str("greeter");
    t.i32(0);
    t.u8(static_cast<uint8_t>(ExportKind::Const));
    t.str("AUTHOR");
    t.i32(0);
    t.u32(1);
    t.str("math");
    t.str("min");
    t.u8(static_cast<uint8_t>(ExportKind::Func));
    t.i32(0);
    t.str("minimum"); // reserved alias slot — a non-empty one must read past
    CompiledProgram prog;
    std::string why;
    bool ok = readTail(minimalFileTailless() + t.b, prog, why);
    check(ok && prog.moduleName == "my_module" && prog.exports.size() == 2 &&
              prog.exports[0].kind == ExportKind::Func &&
              prog.exports[0].name == "greeter" && prog.exports[0].ref == 0 &&
              prog.exports[1].kind == ExportKind::Const &&
              prog.imports.size() == 1 && prog.imports[0].module == "math" &&
              prog.imports[0].name == "min",
          "v9: module name and the tables survive, reserved alias slot read "
          "past (" +
              why + ")");
  }

  // A library image: functions[0] is `<init>`, not `main`.
  {
    Bin f;
    f.b = minimalFileTailless("<init>");
    f.tail("my_module");
    CompiledProgram prog;
    std::string why;
    bool ok = readTail(f.b, prog, why);
    check(ok && prog.functions[0].name == "<init>" &&
              prog.moduleName == "my_module",
          "v9: '<init>' is a valid top-level function (" + why + ")");
  }
}

// ---------------------------------------------------------------------------
// INDEXGET / INDEXPUT: polymorphic subscript (SPEC §bytecodes "OpCode
// indexget"/"indexput"). Three register operands, no i32 slot. The compiler
// emits them for every postfix [...] read or write, replacing the
// ARRGET/ARRPUT emission; those remain valid opcodes. Identities 64/65 after
// the Phase 3 S3 compaction (they were 71/72).
// ---------------------------------------------------------------------------
static void testIndexSubscript() {
  try {
    check(kOpcodeUsedCount == 74, "index: kOpcodeUsedCount is 74");
    check(static_cast<uint8_t>(Opcode::INDEXGET) == 64,
          "indexget: identity 64 (after the S3 compaction)");
    check(static_cast<uint8_t>(Opcode::INDEXPUT) == 65,
          "indexput: identity 65 (after the S3 compaction)");
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
    check(buf.size() == 4 && buf[0] == 64 && buf[1] == 1 && buf[2] == 2 &&
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
    check(buf.size() == 4 && buf[0] == 65 && buf[1] == 4 && buf[2] == 5 &&
              buf[3] == 6,
          "indexput: encodes to 4 bytes");
    decodeInstruction(buf.data(), 0, buf.size(), out);
    check(out.op == Opcode::INDEXPUT && !out.escaped && out.size == 4 &&
              out.a == 4 && out.b == 5 && out.c == 6,
          "indexput: decode round-trip");

    // The subscript opcodes sit directly above deepcopy, and the single-kind
    // subscript opcodes stay valid (retained, no longer emitted).
    check(static_cast<uint8_t>(Opcode::DEEPCOPY) == 63,
          "index: deepcopy identity is 63 (after the S3 compaction)");
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

// ---------------------------------------------------------------------------
// Flag-flow identities (plan docs/plans/m3-flag-flow-plan.md S1; spec
// `### extended OpCode eq, lt, gt, lte, gte` and `### OpCode tst`). The five
// flag-setting compares join the ALU / flag-compare family — permitted kinds
// immediate|register|pool plus the in-place arity rule — while `tst` keeps the
// single-kind register shape (decision D5). Additive: nothing emits them yet
// (the compiler starts to in S5), so no corpus stream changes.
// ---------------------------------------------------------------------------
static bool throwsError(const std::function<void()> &f) {
  try {
    f();
  } catch (const QLangError &) {
    return true;
  }
  return false;
}

static void testFlagFlowIdentities() {
  try {
    const Opcode compares[] = {Opcode::EQF, Opcode::LTF, Opcode::GTF,
                               Opcode::LTEF, Opcode::GTEF};
    const char *names[] = {"eqf", "ltf", "gtf", "ltef", "gtef"};
    for (size_t i = 0; i < 5; i++) {
      const Opcode op = compares[i];
      const std::string n = names[i];
      const int want = 66 + static_cast<int>(i);  // 66..70 after S3
      check(static_cast<int>(op) == want, n + ": identity " + std::to_string(want));
      check(std::strcmp(Bytecode::name(op), names[i]) == 0, n + ": name");
      check(isValidOpcode(op), n + ": valid identity");
      // Left register + right operand; no destination register, and `c` stays
      // unused exactly as for `cmp`.
      check(baseOperandCount(op) == 2 && baseInstructionSize(op) == 3,
            n + ": 2 operands, 3-byte base form");
      check(permittedKinds(op) == 0x7 &&
                kindPermitted(op, RightKind::Immediate) &&
                kindPermitted(op, RightKind::Register) &&
                kindPermitted(op, RightKind::ConstPool),
            n + ": kinds immediate | register | pool (0x7)");
      check(defaultRightKind(op) == RightKind::Register &&
                baseModifier(op) == 0x04,
            n + ": base form is register-right (e = 0x04)");
      check(isInPlaceForm(op, RightKind::Immediate) &&
                isInPlaceForm(op, RightKind::ConstPool) &&
                !isInPlaceForm(op, RightKind::Register),
            n + ": in-place arity when the right operand is not a register");
      check(!kindPermitted(op, RightKind::Reserved),
            n + ": reserved kind 3 is not permitted");
      check(throwsError([&] { escapedInstructionSize(op, 0x0C); }),
            n + ": reserved kind 3 rejected by the size table");
    }

    check(static_cast<int>(Opcode::TST) == 71,
          "tst: identity 71 (last defined, after the S3 compaction)");
    check(std::strcmp(Bytecode::name(Opcode::TST), "tst") == 0, "tst: name");
    check(isValidOpcode(Opcode::TST), "tst: valid identity");
    check(baseOperandCount(Opcode::TST) == 1 &&
              baseInstructionSize(Opcode::TST) == 2,
          "tst: 1 operand, 2-byte base form");
    check(permittedKinds(Opcode::TST) == 0x2 &&
              !kindPermitted(Opcode::TST, RightKind::Immediate) &&
              !kindPermitted(Opcode::TST, RightKind::ConstPool) &&
              !kindPermitted(Opcode::TST, RightKind::Reserved),
          "tst: register kind only (0x2) — it has no right operand");
    check(baseModifier(Opcode::TST) == 0x04, "tst: base form e = 0x04");
    check(!isInPlaceForm(Opcode::TST, RightKind::Immediate),
          "tst: not a member of the in-place family");
    check(throwsError([] { escapedInstructionSize(Opcode::TST, 0x40); }),
          "tst with an immediate right operand is rejected");
    check(throwsError([] { escapedInstructionSize(Opcode::TST, 0x08); }),
          "tst with a pool right operand is rejected");

    // Sizes (S1 exit criteria). An immediate right operand must be *announced*
    // by the escape bit, so `eqf r12, 5` is the escaped 4-byte form and never a
    // 3-byte base form (decision D5).
    check(instructionSize(Opcode::EQF, false, 0x04) == 3,
          "eqf base form is 3 bytes");
    check(instructionSize(Opcode::EQF, true, 0x40) == 4,
          "eqf with an i8 immediate right operand is 4 bytes");
    check(instructionSize(Opcode::TST, false, 0x04) == 2,
          "tst base form is 2 bytes");

    // Encode / decode round-trips of both spellings.
    DecodedInstruction in;
    in.op = Opcode::LTF;
    in.a = 2;
    in.b = 1;
    std::vector<uint8_t> buf;
    encodeInstruction(buf, in);
    check(buf.size() == 3 && buf[0] == 67 && buf[1] == 2 && buf[2] == 1,
          "ltf r2, r1 encodes to [43][02][01]");
    DecodedInstruction out;
    decodeInstruction(buf.data(), 0, buf.size(), out);
    check(out.op == Opcode::LTF && !out.escaped && out.size == 3 && out.a == 2 &&
              out.b == 1 && out.rightKind == RightKind::Register,
          "ltf base form decode round-trip");

    in = DecodedInstruction();
    in.op = Opcode::EQF;
    in.escaped = true;
    in.modifier = 0x40; // left byte-width, kind immediate, form i8
    in.a = 12;
    in.imm = 5;
    buf.clear();
    encodeInstruction(buf, in);
    check(buf.size() == 4 && buf[0] == (0x42 | kOpcodeEscape) &&
              buf[1] == 0x40 && buf[2] == 12 && buf[3] == 5,
          "eqf r12, 5 encodes to [C2][40][0C][05] (4 bytes, decision D5)");
    decodeInstruction(buf.data(), 0, buf.size(), out);
    check(out.op == Opcode::EQF && out.escaped && out.size == 4 && out.a == 12 &&
              out.rightKind == RightKind::Immediate &&
              out.immPayload == ImmPayload::Signed && out.imm == 5,
          "eqf immediate decode round-trip");

    in = DecodedInstruction();
    in.op = Opcode::GTEF;
    in.escaped = true;
    in.modifier = 0x08; // left byte-width, kind pool, form byte-width
    in.a = 4;
    in.b = 3;
    buf.clear();
    encodeInstruction(buf, in);
    check(buf.size() == 4 && buf[0] == (0x46 | kOpcodeEscape) &&
              buf[1] == 0x08 && buf[2] == 4 && buf[3] == 3,
          "gtef r4, pool[3] encodes to [C6][08][04][03] (no third operand)");
    decodeInstruction(buf.data(), 0, buf.size(), out);
    check(out.op == Opcode::GTEF && out.escaped && out.size == 4 && out.a == 4 &&
              out.b == 3 && out.rightKind == RightKind::ConstPool,
          "gtef pool decode round-trip");

    in = DecodedInstruction();
    in.op = Opcode::TST;
    in.a = 3;
    buf.clear();
    encodeInstruction(buf, in);
    check(buf.size() == 2 && buf[0] == 71 && buf[1] == 3,
          "tst r3 encodes to [47][03]");
    decodeInstruction(buf.data(), 0, buf.size(), out);
    check(out.op == Opcode::TST && out.size == 2 && out.a == 3,
          "tst decode round-trip");

    // The flag-flow family sits directly above the subscript opcodes.
    check(static_cast<uint8_t>(Opcode::INDEXPUT) == 65,
          "flag-flow: indexput identity is 65 (after the S3 compaction)");
  } catch (const QLangError &e) {
    check(false, std::string("flag-flow: ") + e.what());
  }
}

// ---------------------------------------------------------------------------
// v6 load-time validation (plan §8 "Validation", §11 item 7): readQlc decodes
// every instruction of every function, so a malformed modifier byte is a *load*
// error — the VM is never handed a stream whose pc would desynchronise. The
// encoder refuses to produce any of these images, so each is hand-assembled.
// ---------------------------------------------------------------------------

// A one-function program ("main") wrapping a hand-encoded code stream.
static CompiledProgram programWithCode(std::vector<uint8_t> code,
                                       std::vector<size_t> offsets,
                                       int registerCount = 8) {
  CompiledProgram p;
  Function f;
  f.name = "main";
  f.registerCount = registerCount;
  f.code = std::move(code);
  f.instrOffsets = std::move(offsets);
  f.loc.assign(f.instrOffsets.size(), {1, 0});
  p.functions.push_back(std::move(f));
  p.registerCount = registerCount;
  return p;
}

static std::string writeImage(const CompiledProgram &p) {
  std::ostringstream os(std::ios::binary);
  if (!writeQlc(os, p, {}))
    return {};
  return os.str();
}

static bool readImage(const std::string &bytes, CompiledProgram &p,
                      std::string &err) {
  std::istringstream is(bytes, std::ios::binary);
  std::vector<std::pair<int, std::string>> slots;
  return readQlc(is, p, slots, err);
}

static void testV6LoadValidation() {
  // mov r3, 200 / mov r3, true / return r3 — the plan §2.9 golden bytes.
  const std::vector<uint8_t> code = {
      static_cast<uint8_t>(Opcode::MOV) | kOpcodeEscape, 0x00, 0x03, 0xC8,
      static_cast<uint8_t>(Opcode::MOV) | kOpcodeEscape, 0xA0, 0x03,
      static_cast<uint8_t>(Opcode::RETURN), 0x03};
  const std::vector<size_t> offs = {0, 4, 7};

  const std::string image = writeImage(programWithCode(code, offs));
  check(!image.empty(), "v6-load: writeQlc accepted a hand-encoded stream");
  check(image.compare(0, 4, "QLC1") == 0, "v6-load: magic QLC1");
  const uint32_t written =
      static_cast<uint32_t>(static_cast<uint8_t>(image[4])) |
      (static_cast<uint32_t>(static_cast<uint8_t>(image[5])) << 8) |
      (static_cast<uint32_t>(static_cast<uint8_t>(image[6])) << 16) |
      (static_cast<uint32_t>(static_cast<uint8_t>(image[7])) << 24);
  check(kQlcVersion == 9 && written == 9,
        "v6-load: the image declares version 9");

  CompiledProgram back;
  std::string err;
  check(readImage(image, back, err), "v6-load: round-trip (" + err + ")");
  check(back.functions.size() == 1 && back.functions[0].code == code &&
            back.functions[0].instrOffsets == offs,
        "v6-load: the code bytes and the offset table survived");

  // The reader is single-version: v8 (the image Phase 3a extended with the
  // module identity and its tables) and every other number are refused with a
  // version error, no migration shim (decision D5: one case below the current
  // version, one above).
  for (uint32_t bad : {3u, 4u, 5u, 6u, 7u, 8u, 10u}) {
    std::string m = image;
    for (int i = 0; i < 4; i++)
      m[4 + i] = static_cast<char>((bad >> (8 * i)) & 0xFF);
    CompiledProgram p;
    std::string e;
    check(!readImage(m, p, e) && e.find("version") != std::string::npos,
          "v6-load: version " + std::to_string(bad) +
              " rejected with a version error");
  }

  // Pool operands are range-checked against the module pool (§4.2): the same
  // `mov r0, pool[0]` / `return r0` stream is accepted with a one-entry pool
  // and refused when the pool is empty (external-modules plan Phase 2 step 4).
  const std::vector<uint8_t> poolRead = {
      static_cast<uint8_t>(Opcode::MOV) | kOpcodeEscape,
      makeModifier(OperandWidth::Byte, RightKind::ConstPool, 0), 0x00, 0x00,
      static_cast<uint8_t>(Opcode::RETURN), 0x00};
  {
    CompiledProgram withPool = programWithCode(poolRead, {0, 4});
    withPool.constants.push_back(QLangValue::Int(7));
    CompiledProgram back2;
    std::string e2;
    check(readImage(writeImage(withPool), back2, e2),
          "v6-load: pool operand inside the module pool accepted (" + e2 +
              ")");
  }
  rejects("v6-load: pool index beyond the module pool rejected",
          writeImage(programWithCode(poolRead, {0, 4})));

  // `callfn` operands name a function record, so they are range-checked against
  // the function table once every record has been read (external-modules plan
  // v4 Phase 3b: the linker rewrites these operands when images merge, so an
  // image must not be able to carry a function index that is not there). The
  // base form of `callfn` is one byte each for a/b/c, so only `b` differs.
  auto callfnImage = [](uint8_t target) {
    return writeImage(programWithCode(
        {static_cast<uint8_t>(Opcode::CALLFN), 0x00, target, 0x00,
         static_cast<uint8_t>(Opcode::RETURN), 0x00},
        {0, 4}));
  };
  {
    CompiledProgram back3;
    std::string e3;
    check(readImage(callfnImage(0), back3, e3),
          "v6-load: callfn naming a function of the image accepted (" + e3 +
              ")");
    // The reason is pinned, not just the failure: the pool-index case above
    // would also reject this image if `callfn`'s `b` were misclassified.
    CompiledProgram bad3;
    std::string e4;
    check(!readImage(callfnImage(5), bad3, e4) &&
              e4.find("function index out of range") != std::string::npos,
          "v6-load: callfn index beyond the function table rejected (" + e4 +
              ")");
  }

  // `getimport`/`callimport` operands name an entry of the image's OWN import
  // table (§4.6), so they are range-checked once the tail has been read
  // (external-modules plan v4 Phase 3c). `getimport` is a 2-operand base form
  // (op, a, b), so only `b` differs between the two images below.
  auto getimportImage = [](uint8_t slot, uint32_t tableSize) {
    CompiledProgram p = programWithCode(
        {static_cast<uint8_t>(Opcode::GETIMPORT), 0x00, slot,
         static_cast<uint8_t>(Opcode::RETURN), 0x00},
        {0, 3});
    for (uint32_t i = 0; i < tableSize; i++)
      p.imports.push_back(ImportEntry{"m", "v", ExportKind::Var, 0});
    return writeImage(p);
  };
  {
    CompiledProgram back4;
    std::string e5;
    check(readImage(getimportImage(0, 1), back4, e5),
          "v6-load: getimport naming an entry of the image's table accepted (" +
              e5 + ")");
    CompiledProgram bad4;
    std::string e6;
    check(!readImage(getimportImage(5, 1), bad4, e6) &&
              e6.find("import index out of range") != std::string::npos,
          "v6-load: getimport index beyond the import table rejected (" + e6 +
              ")");
    // An empty table is the same refusal, so the check is the table's size and
    // not a coincidence of the operand's value.
    CompiledProgram bad5;
    std::string e7;
    check(!readImage(getimportImage(0, 0), bad5, e7) &&
              e7.find("import index out of range") != std::string::npos,
          "v6-load: getimport with no import table rejected (" + e7 + ")");
  }

  // An IMPORT entry's `ref`, unlike an EXPORT entry's, names the DEFINING
  // module's tables (its function index or its own cell), which this image does
  // not contain — so it is deliberately not range-checked here (Phase 3c): the
  // linker remaps it into a merged handle and the VM re-checks it at each use.
  // The export case above, refused for the same magnitude, is what makes this
  // check mean something rather than merely pass.
  {
    CompiledProgram p = programWithCode(code, offs);
    p.imports.push_back(ImportEntry{"other", "f", ExportKind::Func, 999});
    CompiledProgram back5;
    std::string e8;
    check(readImage(writeImage(p), back5, e8) && back5.imports.size() == 1 &&
              back5.imports[0].ref == 999,
          "v6-load: an import ref is a handle into the defining module (" + e8 +
              ")");
  }

  // One malformed modifier field per image. The first instruction's modifier
  // byte is at code[1]; every field below is reserved (decision D7).
  auto rejectsModifier = [&](uint8_t e, const std::string &name) {
    std::vector<uint8_t> c = code;
    c[1] = e;
    CompiledProgram p;
    std::string why;
    check(!readImage(writeImage(programWithCode(c, offs)), p, why), name);
  };
  rejectsModifier(0x0C, "v6-load: reserved right kind (3) rejected at load");
  rejectsModifier(0x07, "v6-load: reserved left addressing form (3) rejected");
  for (uint32_t form = 12; form <= 15; form++)
    rejectsModifier(static_cast<uint8_t>(form << 4),
                    "v6-load: reserved immediate form " + std::to_string(form) +
                        " rejected at load");

  // A kind the opcode does not permit (plan §2.5): ARRPUT writes through a
  // register, so an immediate right operand is outside its permitted set.
  {
    std::vector<uint8_t> c = {
        static_cast<uint8_t>(Opcode::ARRPUT) | kOpcodeEscape, 0x00, 0x01, 0x02};
    CompiledProgram p;
    std::string why;
    check(!readImage(writeImage(programWithCode(c, {0})), p, why),
          "v6-load: a kind the opcode does not permit is rejected");
  }

  // The flag-flow family (plan M3 S1, §11 item 7): `tst` has one register
  // operand and no right operand at all, so an immediate or a pool kind is
  // outside its permitted set; a flag compare naming the reserved kind 3 is
  // refused as well.
  {
    struct Case {
      Opcode op;
      uint8_t e;
      const char *why;
    };
    static const Case kRejected[] = {
        {Opcode::TST, 0x40, "v6-load: tst with an immediate kind is rejected"},
        {Opcode::TST, 0x08, "v6-load: tst with a pool kind is rejected"},
        {Opcode::TST, 0x0C, "v6-load: tst with the reserved kind is rejected"},
        {Opcode::EQF, 0x0C, "v6-load: eqf with the reserved kind is rejected"},
        {Opcode::GTEF, 0x0C,
         "v6-load: gtef with the reserved kind is rejected"},
    };
    for (const Case &c : kRejected) {
      std::vector<uint8_t> code = {
          static_cast<uint8_t>(static_cast<uint8_t>(c.op) | kOpcodeEscape),
          c.e, 0x01, 0x02};
      CompiledProgram p;
      std::string why;
      check(!readImage(writeImage(programWithCode(code, {0})), p, why), c.why);
    }
    // The legal spellings load: register-right base forms and the escaped
    // immediate / pool forms of a compare.
    static const std::vector<uint8_t> kAccepted[] = {
        {static_cast<uint8_t>(Opcode::EQF), 0x01, 0x02},
        {static_cast<uint8_t>(static_cast<uint8_t>(Opcode::LTF) |
                               kOpcodeEscape),
         0x40, 0x01, 0x02},
        {static_cast<uint8_t>(static_cast<uint8_t>(Opcode::GTEF) |
                               kOpcodeEscape),
         0x08, 0x01, 0x02},
        {static_cast<uint8_t>(Opcode::TST), 0x01},
    };
    for (size_t i = 0; i < 4; i++) {
      CompiledProgram prog = programWithCode(kAccepted[i], {0});
      // The pool-kind spelling names pool index 2; v8 range-checks pool
      // operands against the module pool, so it needs three entries.
      prog.constants.assign(3, QLangValue());
      CompiledProgram p;
      std::string why;
      check(readImage(writeImage(prog), p, why),
            "v6-load: legal flag-flow spelling " + std::to_string(i) + " loads");
    }
  }

  // CALL has no base form: its function id is int16, so the escape bit is part
  // of the opcode's definition (plan §2.8).
  {
    std::vector<uint8_t> c = {static_cast<uint8_t>(Opcode::CALL), 0x01, 0x02,
                              0x00};
    CompiledProgram p;
    std::string why;
    check(!readImage(writeImage(programWithCode(c, {0})), p, why),
          "v6-load: an unescaped CALL is rejected");
  }

  // The code-size guard (kMaxInstructions * 15, raised from v5's * 7 because a
  // v6 instruction is up to 15 bytes): a stream claiming more is refused before
  // any of it is read.
  {
    Bin f;
    f.b += "QLC1";
    f.u32(kQlcVersion);
    f.u32(1);
    f.str("main");
    f.i32(0);      // paramCount
    f.i32(1);      // registerCount
    f.u8(0);       // isGenerator
    f.u32(0);      // captures
    f.u32(0xFFFFFFFFu); // codeSize: larger than any legal stream
    f.u32(0);      // offCount
    f.u32(0);      // locCount
    f.u32(0);      // module pool
    f.u32(0);      // natives
    f.u32(0);      // vars
    f.u32(0);      // constVars
    f.i32(1);      // registerCount
    CompiledProgram p;
    std::string why;
    check(!readImage(f.b, p, why) && why.find("too large") != std::string::npos,
          "v6-load: an oversized code stream is refused");
  }

  // The guard is not too tight either: the widest legal instruction — a dword
  // left operand plus an 8-byte immediate plus a third operand — must load.
  {
    size_t best = 0;
    uint8_t bestE = 0;
    for (uint32_t e = 0; e <= 0xFF; e++) {
      size_t s = 0;
      try {
        s = escapedInstructionSize(Opcode::CONV, static_cast<uint8_t>(e));
      } catch (const QLangError &) {
        continue;
      }
      if (s > best) {
        best = s;
        bestE = static_cast<uint8_t>(e);
      }
    }
    check(best == 15, "v6-load: the widest instruction is 15 bytes");
    std::vector<uint8_t> c(best, 0);
    c[0] = static_cast<uint8_t>(Opcode::CONV) | kOpcodeEscape;
    c[1] = bestE;
    CompiledProgram p;
    std::string why;
    check(readImage(writeImage(programWithCode(c, {0})), p, why),
          "v6-load: a 15-byte instruction loads (" + why + ")");
  }
}

// --- script-corpus .qlc round-trip + execute (plan
// docs/plans/phase3-opcode-retirement-plan.md S4) ---
//
// Every script under tests/scripts is compiled exactly as `qlang_run` compiles
// it, written with writeQlc, read back with readQlc, and executed on a fresh
// context (the same isolation `qlang_run file.qlc` gets from a new process).
// The loaded run's stdout must be byte-identical to the freshly compiled
// program's: the v7 -> v8 bump moves the constant pool into a module-level
// section, so any behavioural difference here is a codec bug, not a format
// question.
//
// The corpus is CMake's QL_SCRIPTS list (23 scripts, perf_stress.ql included —
// the whole suite costs ~75 ms).
static const char *const kRoundTripScripts[] = {
    "cmp.ql",         "cast.ql",         "encode_repro.ql",
    "ext_consts.ql",  "ext_regs.ql",     "flags.ql",
    "hello.ql",       "helpers.ql",      "imm_kinds.ql",
    "jump_widths.ql", "len.ql",          "money.ql",
    "moneyv2.ql",     "perf_stress.ql",  "table_subscript.ql",
    "test_assert.ql", "test_assert2.ql", "test_cast.ql",
    "test_cast2.ql",  "test_minmax.ql",  "test_minmax2.ql",
    "type.ql",        "value_share.ql",
};

// The VM prints through std::cout (QLangVM.cpp print/println), so swapping its
// stream buffer captures a run without touching the VM. The buffer is restored
// on every exit path, including a thrown QLangError.
class StdoutCapture {
public:
  StdoutCapture() : m_saved(std::cout.rdbuf(m_ss.rdbuf())) {}
  ~StdoutCapture() { std::cout.rdbuf(m_saved); }
  StdoutCapture(const StdoutCapture &) = delete;
  StdoutCapture &operator=(const StdoutCapture &) = delete;
  std::string text() const { return m_ss.str(); }

private:
  std::ostringstream m_ss;
  std::streambuf *m_saved;
};

// Execute `prog` on `scope`, capturing everything it prints. Returns false and
// sets `err` when the VM threw (a failed assert or a runtime error), exactly
// the case `qlang_run` reports as exit 3.
static bool runCaptured(const CompiledProgram &prog, QLangContext &scope,
                        std::string &out, std::string &err) {
  QLangVM vm(prog, scope);
  StdoutCapture cap;
  try {
    vm.run();
  } catch (const QLangError &e) {
    err = e.what();
    return false;
  } catch (const std::exception &e) {
    err = e.what();
    return false;
  }
  out = cap.text();
  err.clear();
  return true;
}

static void testScriptQlcRoundTrip() {
  for (const char *script : kRoundTripScripts) {
    const std::string name(script);
    const std::string label = "qlc-exec: " + name;
    std::ifstream in(std::string(QLANG_TEST_SCRIPTS_DIR) + "/" + name,
                     std::ios::binary);
    if (!in) {
      check(false, label + " is readable");
      continue;
    }
    std::ostringstream src;
    src << in.rdbuf();

    // --- Compile, wire natives and collect nativeIds as qlang_run does ---
    CompiledProgram prog;
    std::map<std::string, int> nativeIds;
    QLangContext ctx;
    bool compiled = true;
    std::string why;
    try {
      std::vector<StmtPtr> program = ctx.parse(src.str());
      QLangCompiler compiler(std::move(program), &ctx);
      compiler.addExternalConsts({});
      std::unordered_set<std::string> nativeNames;
      for (const auto &[n, fn] : ctx.nativeFuncs()) nativeNames.insert(n);
      compiler.addExternalFuncs(nativeNames);
      compiler.addModules(ctx.modules());
      prog = compiler.compile();
      for (const auto &[n, fn] : ctx.nativeFuncs()) {
        auto it = compiler.nativeFuncIds().find(n);
        if (it == compiler.nativeFuncIds().end()) continue;
        int slot = -1 - it->second;
        while (prog.nativeFuncs.size() <= static_cast<size_t>(slot))
          prog.nativeFuncs.push_back(nullptr);
        prog.nativeFuncs[static_cast<size_t>(slot)] = fn;
      }
      const auto &resolved = compiler.nativeFuncsResolved();
      for (size_t i = 0; i < resolved.size(); i++) {
        while (prog.nativeFuncs.size() <= i)
          prog.nativeFuncs.push_back(nullptr);
        if (resolved[i]) prog.nativeFuncs[i] = resolved[i];
      }
      nativeIds.insert(compiler.nativeFuncIds().begin(),
                       compiler.nativeFuncIds().end());
    } catch (const QLangError &e) {
      compiled = false;
      why = e.what();
    }
    if (!compiled) {
      check(false, label + " compiles (" + why + ")");
      continue;
    }

    // --- writeQlc -> readQlc ---
    std::ostringstream os(std::ios::binary);
    if (!writeQlc(os, prog, nativeIds)) {
      check(false, label + " writeQlc accepted the program");
      continue;
    }
    CompiledProgram back;
    std::vector<std::pair<int, std::string>> slots;
    std::string err;
    std::istringstream is(os.str(), std::ios::binary);
    if (!readQlc(is, back, slots, err)) {
      check(false, label + " readQlc (" + err + ")");
      continue;
    }

    // The image declares the current format version.
    const uint32_t version =
        static_cast<uint32_t>(static_cast<uint8_t>(os.str()[4])) |
        (static_cast<uint32_t>(static_cast<uint8_t>(os.str()[5])) << 8) |
        (static_cast<uint32_t>(static_cast<uint8_t>(os.str()[6])) << 16) |
        (static_cast<uint32_t>(static_cast<uint8_t>(os.str()[7])) << 24);
    check(version == kQlcVersion,
          label + " image declares v" + std::to_string(version));

    // The code and the offset table survived byte for byte; the pool is the
    // program's (v8), so it is compared once, not per function.
    bool sameImage = back.functions.size() == prog.functions.size();
    for (size_t i = 0; sameImage && i < prog.functions.size(); i++) {
      const auto &a = prog.functions[i];
      const auto &b = back.functions[i];
      sameImage = a.code == b.code && a.instrOffsets == b.instrOffsets;
    }
    sameImage = sameImage && poolEq(prog.constants, back.constants);
    check(sameImage, label + " image survives the round-trip");

    // --- Re-resolve the native slots on a fresh context, then execute both ---
    QLangContext loadCtx;
    bool natives = true;
    std::string missing;
    for (const auto &[slot, n] : slots) {
      QLangContext::NativeFunc fn;
      const size_t dot = n.find('.');
      if (dot != std::string::npos) {
        // A `module.func` slot is what `import m` stores (SPEC §modules
        // "import statement"): only that module's function answers for it.
        for (const auto &mod : loadCtx.modules())
          if (mod.name == n.substr(0, dot)) {
            auto fit = mod.funcs.find(n.substr(dot + 1));
            if (fit != mod.funcs.end()) fn = fit->second;
            break;
          }
      } else {
        auto it = loadCtx.nativeFuncs().find(n);
        if (it != loadCtx.nativeFuncs().end())
          fn = it->second;
        else
          for (const auto &mod : loadCtx.modules()) {
            auto mit = mod.funcs.find(n);
            if (mit != mod.funcs.end()) {
              fn = mit->second;
              break;
            }
          }
      }
      if (!fn) {
        natives = false;
        missing = n;
        break;
      }
      while (back.nativeFuncs.size() <= static_cast<size_t>(slot))
        back.nativeFuncs.push_back(nullptr);
      back.nativeFuncs[static_cast<size_t>(slot)] = fn;
    }
    if (!natives) {
      check(false, label + " native slot '" + missing + "' re-resolves");
      continue;
    }

    std::string outA, errA, outB, errB;
    bool okA = runCaptured(prog, ctx, outA, errA);
    bool okB = runCaptured(back, loadCtx, outB, errB);
    bool same = okA == okB && outA == outB && (!okA || errA == errB);
    std::string detail;
    if (okA != okB)
      detail = okA ? " (the loaded run failed: " + errB + ")"
                   : " (the original run failed: " + errA + ")";
    else if (!okA && errA != errB)
      detail = " (both runs failed differently)";
    else if (outA != outB)
      detail = " (" + std::to_string(outA.size()) + " vs " +
               std::to_string(outB.size()) + " bytes)";
    check(same, label + " executes with identical stdout" + detail);
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
  testFlagFlowIdentities();
  testV6LoadValidation();
  testMalformed();
  testScriptQlcRoundTrip();
  std::cout << (fails == 0 ? "all codec checks passed\n"
                           : "codec checks FAILED\n");
  return fails == 0 ? 0 : 1;
}
