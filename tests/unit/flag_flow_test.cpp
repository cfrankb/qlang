// Flag-flow behaviour matrix (docs/plans/m3-flag-flow-plan.md S4; parent plan
// docs/plans/encoding-v6-and-indirect-plan.md §11 item 5): every comparator
// that writes Frame::flags against every flag jump, over each relation and each
// right-operand kind, plus the "no bit set" state, the general value domains,
// the error-text parity with the value-producing comparisons (decision D3), the
// alias guard (D6) and the reserved-slot load rejections (§11 item 7).
//
// The programs are hand-assembled with encodeInstruction and executed on a real
// QLangVM: nothing here goes through the compiler, because S5 (the emission
// switch) has not landed. This suite is the guard that makes S5 safe — if the
// negative-jump map is wrong it says so here, while no corpus script depends on
// it yet.
//
// The expectations come from the specification tables, re-implemented below as
// wantFlags() / jumpFires(). They deliberately do NOT call QLangVM::flagCompare
// or QLangVM::flagsSatisfied, so the suite cannot agree with a wrong VM by
// construction (docs/spec/13-bytecodes.md `## alternate conditional jump OpCode`,
// `### extended OpCode eq, lt, gt, lte, gte`).
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "qlang/QLangBytecode.h"
#include "qlang/QLangBytecodeIO.h"
#include "qlang/QLangCompiler.h"
#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"
#include "qlang/QLangVM.h"
#include "qlang/QLangValue.h"

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

namespace {

// The flag bits, spelled out locally so the oracle does not inherit them from
// the implementation under test (QLangVM::CMP_* are private anyway). Nothing
// below reads a flag directly: every verdict is observed through a register, so
// a renumbering of the VM's bits cannot be absorbed silently.
constexpr int kGR = 1, kEQ = 2, kLT = 4;

// The ordering relation a pair of values has (per SPEC `## operators < and >`),
// or `kUnordered` when the ordering comparators throw for that pair.
enum Relation {
  kLess,
  kEqual,
  kGreater,
  kUnordered
};

// How the comparator's right operand is spelled (plan §2.5 kinds).
enum OperandKind {
  kReg,
  kImm,
  kPool
};

const Opcode kCompares[] = {Opcode::CMP, Opcode::EQF,  Opcode::LTF,
                            Opcode::GTF, Opcode::LTEF, Opcode::GTEF};
const char *kCompareNames[] = {"cmp", "eqf", "ltf", "gtf", "ltef", "gtef"};
const Opcode kJumps[] = {Opcode::JE,  Opcode::JNE, Opcode::JG,
                         Opcode::JGE, Opcode::JLE, Opcode::JL};
const char *kJumpNames[] = {"je", "jne", "jg", "jge", "jle", "jl"};
const Relation kRels[] = {kLess, kEqual, kGreater};
const char *kRelNames[] = {"less", "equal", "greater"};
const OperandKind kKinds[] = {kReg, kImm, kPool};
const char *kKindNames[] = {"reg", "imm", "pool"};

// SPEC §bytecodes `### extended OpCode eq, lt, gt, lte, gte`: each `*f`
// comparator is a capability mask, so it sets only the bit of the relation it
// tests — the result is one-hot or zero. `cmp` is total over Int, so exactly
// one bit is always set (SPEC `## alternate conditional jump OpCode`).
int wantFlags(Opcode cmp, Relation rel) {
  switch (cmp) {
  case Opcode::CMP: return rel == kLess ? kLT : rel == kEqual ? kEQ : kGR;
  case Opcode::EQF: return rel == kEqual ? kEQ : 0;
  case Opcode::LTF: return rel == kLess ? kLT : 0;
  case Opcode::GTF: return rel == kGreater ? kGR : 0;
  case Opcode::LTEF: return rel == kLess ? kLT : rel == kEqual ? kEQ : 0;
  case Opcode::GTEF: return rel == kGreater ? kGR : rel == kEqual ? kEQ : 0;
  default: return -1;
  }
}

// SPEC §bytecodes `## alternate conditional jump OpCode`: the six predicates
// over the flag pattern. `je` tests `f == eq` — which is exactly why the
// two-bit `lte` state is rejected — and `jge`/`jle` test "that bit OR eq", so
// they fire for equality too.
bool jumpFires(Opcode jump, int f) {
  switch (jump) {
  case Opcode::JG: return f == kGR;
  case Opcode::JGE: return (f & (kGR | kEQ)) != 0;
  case Opcode::JLE: return (f & (kLT | kEQ)) != 0;
  case Opcode::JL: return f == kLT;
  case Opcode::JE: return f == kEQ;
  case Opcode::JNE: return (f & kEQ) == 0;
  default: return false;
  }
}

// Registers of every program built here. The main frame's register file ALIASES
// the context's global table (QLangVM::run), so a named variable is the
// cheapest observation channel: `res` carries the verdict, `a`/`b` the operands.
constexpr int kRes = 0, kLeft = 1, kRight = 2;
constexpr int kRegCount = 4;

// A hand-built function plus the pool entries its operands index. Since v8 a
// program carries ONE pool (SPEC §bytecodes "unified constant table"), so the
// pool travels beside the function until `runMain` merges the tables.
struct Built {
  Function fn;
  std::vector<QLangValue> pool;
};

// A tiny two-pass assembler. A jump's offset is relative to the END of the jump
// instruction (`off = target - (jump_pc + jump_size)`), so the whole layout has
// to be known before any offset can be written; the base form is 2 bytes for
// every offset in -128..127, so the layout itself does not depend on them.
class Asm {
public:
  int add(const DecodedInstruction &d) {
    m_ins.push_back(d);
    m_target.push_back(-1);
    return static_cast<int>(m_ins.size()) - 1;
  }
  // A base-form (1-byte signed offset) jump to instruction index `target`.
  int addJump(Opcode op, int target) {
    DecodedInstruction d;
    d.op = op;
    m_ins.push_back(d);
    m_target.push_back(target);
    return static_cast<int>(m_ins.size()) - 1;
  }
  int here() const { return static_cast<int>(m_ins.size()); }
  // Append a constant-pool entry, returning its index.
  int pool(const QLangValue &v) {
    m_pool.push_back(v);
    return static_cast<int>(m_pool.size()) - 1;
  }

  Built build(int registerCount) const {
    Function fn;
    fn.name = "main";
    fn.paramCount = 0;
    fn.registerCount = registerCount;
    std::vector<size_t> offs(m_ins.size());
    std::vector<uint8_t> scratch;
    for (size_t k = 0; k < m_ins.size(); k++) {
      offs[k] = scratch.size();
      encodeInstruction(scratch, m_ins[k]);
    }
    std::vector<DecodedInstruction> ins = m_ins;
    for (size_t k = 0; k < ins.size(); k++)
      if (m_target[k] >= 0)
        ins[k].jumpOffset = static_cast<int32_t>(offs[m_target[k]]) -
                            static_cast<int32_t>(offs[k] + 2);
    for (const DecodedInstruction &d : ins) {
      fn.instrOffsets.push_back(fn.code.size());
      fn.loc.emplace_back(1, 1);
      encodeInstruction(fn.code, d);
    }
    return {std::move(fn), m_pool};
  }

private:
  std::vector<DecodedInstruction> m_ins;
  std::vector<int> m_target;
  std::vector<QLangValue> m_pool;
};

// `mov r[a], <i8 immediate>`.
DecodedInstruction movImm(int a, int32_t value) {
  DecodedInstruction d;
  d.op = Opcode::MOV;
  d.escaped = true;
  d.modifier = makeModifier(OperandWidth::Byte, RightKind::Immediate,
                            static_cast<uint8_t>(ImmForm::I8));
  d.a = a;
  d.imm = value;
  return d;
}

// `mov r[a], pool[idx]`.
DecodedInstruction movPool(int a, int idx) {
  DecodedInstruction d;
  d.op = Opcode::MOV;
  d.escaped = true;
  d.modifier = makeModifier(OperandWidth::Byte, RightKind::ConstPool, 0);
  d.a = a;
  d.b = idx;
  return d;
}

DecodedInstruction ret(int a) {
  DecodedInstruction d;
  d.op = Opcode::RETURN;
  d.a = a;
  return d;
}

// The comparator's right operand, spelled per `kind` (plan §2.6: a right
// operand that is not a register means the in-place 2-operand form, so the
// stream carries no third operand and the VM must not read `c`). `kImm` spells
// an i8 integer, so callers using it must pass an Int `right`.
void setRight(DecodedInstruction &d, OperandKind kind, const QLangValue &right,
              Asm &asm_) {
  if (kind == kReg) {
    d.b = kRight;
    return;
  }
  d.escaped = true;
  if (kind == kImm) {
    d.modifier = makeModifier(OperandWidth::Byte, RightKind::Immediate,
                              static_cast<uint8_t>(ImmForm::I8));
    d.imm = right.asInt();
  } else {
    d.modifier = makeModifier(OperandWidth::Byte, RightKind::ConstPool, 0);
    d.b = asm_.pool(right);
  }
}

// One comparator + one flag jump:
//   0:  <cmp>   r1, <right>
//   1:  <jump>  -> 3          (the taken path)
//   2:  jmp     -> 4
//   3:  mov     r0, 1         (taken)
//   4:  return  r0
// `res` is seeded to 0, so the result is Int 1 when the jump fired and Int 0
// when it did not. Anything else (nil, a throw) means the stream desynchronised
// or the comparator raised.
Built compareProgram(Opcode cmpOp, Opcode jump, OperandKind kind,
                     const QLangValue &right) {
  Asm a;
  DecodedInstruction c;
  c.op = cmpOp;
  c.a = kLeft;
  setRight(c, kind, right, a);
  a.add(c);
  const int taken = a.here() + 2;
  a.addJump(jump, taken);
  a.addJump(Opcode::JMP, taken + 1);
  a.add(movImm(kRes, 1));
  a.add(ret(kRes));
  return a.build(kRegCount);
}

// `tst r1` + one flag jump (tst has one operand and no right operand at all).
Built tstProgram(Opcode jump) {
  Asm a;
  DecodedInstruction t;
  t.op = Opcode::TST;
  t.a = kLeft;
  a.add(t);
  const int taken = a.here() + 2;
  a.addJump(jump, taken);
  a.addJump(Opcode::JMP, taken + 1);
  a.add(movImm(kRes, 1));
  a.add(ret(kRes));
  return a.build(kRegCount);
}

// The value-producing comparison `r0 = (r1 <op> r2)` (the 3-operand register
// form), used as the reference for the error-text parity checks (D3).
Built valueCompareProgram(Opcode op) {
  Asm a;
  DecodedInstruction d;
  d.op = op;
  d.a = kRes;
  d.b = kLeft;
  d.c = kRight;
  a.add(d);
  a.add(ret(kRes));
  return a.build(kRegCount);
}

// A `__str__` body: `def __str__(self) { return "Point" }`, i.e.
// `mov r0, pool[0]` / `return r0`. Installed as function index 1 so a table
// value can carry a callable __str__ (SPEC `## operator == and !=`).
Built strBodyFunction(const std::string &text) {
  Asm a;
  const int idx = a.pool(QLangValue::String(text));
  a.add(movPool(0, idx));
  a.add(ret(0));
  Built b = a.build(2);
  b.fn.name = "__str__";
  b.fn.paramCount = 1;
  b.fn.registerCount = 2;
  return b;
}

// Shift every pool operand of `fn` by `delta`: v8 carries ONE pool per
// program, so an extra function's private indices move when its entries are
// appended after the main pool.
void shiftPoolOperands(Function &fn, int delta) {
  if (delta == 0) return;
  std::vector<uint8_t> code;
  for (size_t off : fn.instrOffsets) {
    DecodedInstruction d;
    decodeInstruction(fn.code.data(), off, fn.code.size(), d);
    if (d.rightKind == RightKind::ConstPool) d.b += delta;
    encodeInstruction(code, d);
  }
  fn.code = std::move(code);
}

// Runs one hand-built main program with `a`/`b` seeded into registers 1/2 and
// returns the value of `res` (register 0). `extra` adds functions (index 1, 2…)
// addressable by closure values seeded into the registers. Every pool merges
// into the program's single table (v8): main's first, each extra's entries
// appended with that function's pool operands shifted to match.
QLangValue runMain(const Built &main, const QLangValue &a, const QLangValue &b,
                   const std::vector<Built> &extra = {}) {
  CompiledProgram p;
  p.registerCount = kRegCount;
  p.vars["res"] = kRes;
  p.vars["a"] = kLeft;
  p.vars["b"] = kRight;
  p.constants = main.pool;
  p.functions.push_back(main.fn);
  for (const Built &e : extra) {
    Function f = e.fn;
    shiftPoolOperands(f, static_cast<int>(p.constants.size()));
    p.constants.insert(p.constants.end(), e.pool.begin(), e.pool.end());
    p.functions.push_back(std::move(f));
  }
  QLangContext ctx;
  QLangVM vm(p, ctx); // adoptGlobals: name -> register
  ctx.set("a", a);
  ctx.set("b", b);
  ctx.set("res", QLangValue::Int(0));
  vm.run();
  return ctx.get("res");
}

// The message a program throws, or "" when it runs to completion.
std::string runMessage(const Built &fn, const QLangValue &a,
                       const QLangValue &b,
                       const std::vector<Built> &extra = {}) {
  try {
    runMain(fn, a, b, extra);
    return {};
  } catch (const QLangError &e) {
    return e.message();
  }
}

// True when the run reported exactly `want` (taken / not taken).
bool ranAs(const Built &fn, const QLangValue &a, const QLangValue &b,
           bool want, std::string &why,
           const std::vector<Built> &extra = {}) {
  try {
    QLangValue res = runMain(fn, a, b, extra);
    if (res.type() != QLangValue::Type::Int) {
      why = "result is " + std::string(res.typeName()) + ", not 0/1";
      return false;
    }
    const int32_t v = res.asInt();
    if (v != 0 && v != 1) {
      why = "result is " + std::to_string(v) + ", not 0/1";
      return false;
    }
    if ((v == 1) != want) {
      why = want ? "jump not taken" : "jump taken";
      return false;
    }
    return true;
  } catch (const QLangError &e) {
    why = "threw " + e.message();
    return false;
  }
}

QLangValue tableOf(const std::vector<std::pair<std::string, QLangValue>> &kvs) {
  QLangValue t = QLangValue::Table();
  for (const auto &kv : kvs) t.asTable()->insert({kv.first, kv.second});
  return t;
}

// ---------------------------------------------------------------------------
// 1. The full matrix: 6 comparators x 6 jumps x 3 relations x 3 right kinds.
// ---------------------------------------------------------------------------
void testMatrix() {
  // Values per relation, small enough for the i8 immediate form.
  const int32_t kLeftOf[] = {1, 2, 3};
  const int32_t kRightOf[] = {2, 2, 2};
  for (size_t ci = 0; ci < 6; ci++) {
    for (size_t ji = 0; ji < 6; ji++) {
      for (size_t ri = 0; ri < 3; ri++) {
        for (size_t ki = 0; ki < 3; ki++) {
          const Opcode cmpOp = kCompares[ci], jump = kJumps[ji];
          const Relation rel = kRels[ri];
          const QLangValue left = QLangValue::Int(kLeftOf[ri]);
          const QLangValue right = QLangValue::Int(kRightOf[ri]);
          const bool want = jumpFires(jump, wantFlags(cmpOp, rel));
          std::string why;
          const bool ok = ranAs(compareProgram(cmpOp, jump, kKinds[ki], right),
                                left, right, want, why);
          const std::string name = std::string(kCompareNames[ci]) + "/" +
                                   kJumpNames[ji] + " " + kRelNames[ri] + "/" +
                                   kKindNames[ki] +
                                   (want ? ": taken" : ": not taken") +
                                   (why.empty() ? "" : " (" + why + ")");
          check(ok, name);
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// 2. The "no bit set" state, spelled out case by case.
// ---------------------------------------------------------------------------
void testNoneSetState() {
  // `ltf` with a > b: the tested relation does not hold, so NO flag is set.
  // Only `jne` fires (it tests `(f & eq) == 0`); je/jl/jg/jle/jge must not.
  // This is the state D1 is about: after a partial comparator the negative
  // branch has to be `jne`, because `jge`/`jle`/`jg`/`jl` all require a bit.
  const QLangValue big = QLangValue::Int(3), small = QLangValue::Int(2);
  const struct {
    Opcode jump;
    bool fires;
  } kNone[] = {{Opcode::JE, false},  {Opcode::JNE, true}, {Opcode::JL, false},
               {Opcode::JLE, false}, {Opcode::JG, false}, {Opcode::JGE, false}};
  for (const auto &k : kNone) {
    std::string why;
    std::string name = std::string("ltf a>b leaves no bit: ") +
                       (k.fires ? "" : "not ") + Bytecode::name(k.jump) +
                       " fires";
    check(ranAs(compareProgram(Opcode::LTF, k.jump, kReg, small), big, small,
                k.fires, why),
          name + (why.empty() ? "" : " (" + why + ")"));
  }

  // `ltef` with a == b leaves `eq` ALONE. `je` firing is the guard: it tests
  // `f == eq`, so it would stop firing if anyone reintroduced the rejected
  // two-bit `lte` state (SPEC `### extended OpCode …`: "it breaks the
  // implemented jump predicates — je tests f == eq"). `jle` and `jge` both fire
  // because each tests "that bit OR eq"; `jl` (f == lt) and `jg` (f == gt) do
  // not, which is what proves neither of those bits is set either.
  const QLangValue v = QLangValue::Int(2);
  const struct {
    Opcode jump;
    bool fires;
    const char *why;
  } kEq[] = {
      {Opcode::JE, true, "je fires (f is exactly eq, not eq|lt)"},
      {Opcode::JLE, true, "jle fires (lt or eq)"},
      {Opcode::JGE, true, "jge fires (gr or eq)"},
      {Opcode::JL, false, "jl does not fire (f is not lt)"},
      {Opcode::JG, false, "jg does not fire (f is not gt)"},
      {Opcode::JNE, false, "jne does not fire (eq is set)"},
  };
  for (const auto &k : kEq) {
    std::string why;
    std::string name = std::string("ltef a==b leaves eq only: ") + k.why;
    check(ranAs(compareProgram(Opcode::LTEF, k.jump, kReg, v), v, v, k.fires,
                why),
          name + (why.empty() ? "" : " (" + why + ")"));
  }

  // `gtef` with a < b: none set, same shape as `ltf` with a > b.
  for (const auto &k : kNone) {
    std::string why;
    std::string name = std::string("gtef a<b leaves no bit: ") +
                       (k.fires ? "" : "not ") + Bytecode::name(k.jump);
    check(ranAs(compareProgram(Opcode::GTEF, k.jump, kReg, big), small, big,
                k.fires, why),
          name + (why.empty() ? "" : " (" + why + ")"));
  }

  // `eqf` with a != b: none set (there is no `nef`; "not equal" is eqf + jne).
  for (const auto &k : kNone) {
    std::string why;
    std::string name = std::string("eqf a!=b leaves no bit: ") +
                       (k.fires ? "" : "not ") + Bytecode::name(k.jump);
    check(ranAs(compareProgram(Opcode::EQF, k.jump, kReg, small), big, small,
                k.fires, why),
          name + (why.empty() ? "" : " (" + why + ")"));
  }
}

// ---------------------------------------------------------------------------
// 3. `tst`: truthiness into the flags (SPEC `### OpCode tst`). Implemented but
//    not emitted in v6 (D4), so the matrix is the only thing exercising it.
// ---------------------------------------------------------------------------
void testTst() {
  const QLangValue kTruthy[] = {QLangValue::Int(1),
                                QLangValue::Int(-7),
                                QLangValue::Float(1.5),
                                QLangValue::String("x"),
                                QLangValue::Array({QLangValue::Int(1)}),
                                tableOf({{"k", QLangValue::Int(1)}})};
  const QLangValue kFalsy[] = {QLangValue(),           QLangValue::Int(0),
                               QLangValue::Float(0.0), QLangValue::String(""),
                               QLangValue::Array(),    tableOf({})};
  const char *kTruthyNames[] = {"int 1",        "int -7",    "float 1.5",
                                "string \"x\"", "array [1]", "table {k:1}"};
  const char *kFalsyNames[] = {"nil",         "int 0",    "float 0.0",
                               "string \"\"", "array []", "table {}"};
  for (size_t vi = 0; vi < 6; vi++) {
    for (size_t ji = 0; ji < 6; ji++) {
      const bool want = jumpFires(kJumps[ji], kEQ);
      std::string why;
      std::string name = std::string("tst truthy ") + kTruthyNames[vi] + "/" +
                         kJumpNames[ji] + (want ? ": taken" : ": not taken");
      check(ranAs(tstProgram(kJumps[ji]), kTruthy[vi], QLangValue(), want, why),
            name + (why.empty() ? "" : " (" + why + ")"));
      // Falsy leaves no bit set, so only `jne` fires.
      const bool wantF = jumpFires(kJumps[ji], 0);
      std::string whyF;
      std::string nameF = std::string("tst falsy ") + kFalsyNames[vi] + "/" +
                          kJumpNames[ji] + (wantF ? ": taken" : ": not taken");
      check(
          ranAs(tstProgram(kJumps[ji]), kFalsy[vi], QLangValue(), wantF, whyF),
          nameF + (whyF.empty() ? "" : " (" + whyF + ")"));
    }
  }
}

// ---------------------------------------------------------------------------
// 4. The general value domains — everything the compiler's `Int` gate rejects
//    today, which is the whole point of the `*f` family (§4.3).
// ---------------------------------------------------------------------------
struct DomainCase {
  const char *name;
  QLangValue a;
  QLangValue b;
  Relation rel; // the ordering relation, or kUnordered when it throws
  // What `==` answers, which eqf inherits verbatim (D3) — so it is NOT simply
  // `rel == kEqual`: `==` compares two numbers through int64, truncating a
  // float, while the orderings compare them as doubles.
  bool equal;
};

void testGeneralDomains() {
  const DomainCase kCases[] = {
      {"string < string", QLangValue::String("abc"), QLangValue::String("abd"),
       kLess, false},
      {"string == string", QLangValue::String("abc"), QLangValue::String("abc"),
       kEqual, true},
      {"string > string", QLangValue::String("abd"), QLangValue::String("abc"),
       kGreater, false},
      {"float < int", QLangValue::Float(1.5), QLangValue::Int(2), kLess, false},
      {"float == int", QLangValue::Float(2.0), QLangValue::Int(2), kEqual,
       true},
      // `==` truncates a float to int64, so 2.5 == 2 is TRUE while 2.5 > 2 is
      // also true (SPEC `## operator == and !=`: "only if type coercion is
      // possible and results in equivalency"). eqf inherits that from `==`
      // rather than deriving equality from the ordering, so both flags are set
      // by the two comparators. Pre-existing `==` behaviour, not an M3 change.
      {"float 2.5 vs int 2", QLangValue::Float(2.5), QLangValue::Int(2),
       kGreater, true},
      {"array < array",
       QLangValue::Array({QLangValue::Int(1), QLangValue::Int(2)}),
       QLangValue::Array({QLangValue::Int(1), QLangValue::Int(3)}), kLess,
       false},
      {"array == array", QLangValue::Array({QLangValue::Int(1)}),
       QLangValue::Array({QLangValue::Int(1)}), kEqual, true},
      {"array > array", QLangValue::Array({QLangValue::Int(2)}),
       QLangValue::Array({QLangValue::Int(1), QLangValue::Int(9)}), kGreater,
       false},
      {"table < table", tableOf({{"a", QLangValue::Int(1)}}),
       tableOf({{"a", QLangValue::Int(2)}}), kLess, false},
      {"table == table", tableOf({{"a", QLangValue::Int(1)}}),
       tableOf({{"a", QLangValue::Int(1)}}), kEqual, true},
      {"table > table", tableOf({{"a", QLangValue::Int(2)}}),
       tableOf({{"a", QLangValue::Int(1)}}), kGreater, false},
      // Mixed: equality is defined (never equal across types), ordering is not.
      {"string vs int (mixed)", QLangValue::String("abc"), QLangValue::Int(5),
       kUnordered, false},
      {"nil vs nil", QLangValue(), QLangValue(), kUnordered, true},
      // String vs array goes through the array's implicit __str__ ("[1]").
      {"string vs array (__str__)", QLangValue::String("[1]"),
       QLangValue::Array({QLangValue::Int(1)}), kEqual, false},
  };

  const Opcode kOrdering[] = {Opcode::LTF, Opcode::GTF, Opcode::LTEF,
                              Opcode::GTEF};
  const Opcode kValueTwin[] = {Opcode::LT, Opcode::GT, Opcode::LE, Opcode::GE};

  for (const DomainCase &c : kCases) {
    // Register and pool: the immediate form spells an i8 integer, and these
    // cases are the non-integer domains the `*f` family exists for.
    const OperandKind kDomainKinds[] = {kReg, kPool};
    for (const OperandKind kind : kDomainKinds) {
      const std::string kn = kKindNames[kind];
      // eqf: the equality rules, never an error.
      for (size_t ji = 0; ji < 6; ji++) {
        const bool want = jumpFires(kJumps[ji], c.equal ? kEQ : 0);
        std::string why;
        std::string name = std::string(c.name) + "/eqf/" + kJumpNames[ji] +
                           "/" + kn + (want ? ": taken" : ": not taken");
        check(ranAs(compareProgram(Opcode::EQF, kJumps[ji], kind, c.b), c.a,
                    c.b, want, why),
              name + (why.empty() ? "" : " (" + why + ")"));
      }
      for (size_t oi = 0; oi < 4; oi++) {
        if (c.rel == kUnordered) {
          // D3: the `*f` comparator raises the message the value-producing
          // comparison raises — same function, same op-name string.
          const std::string flagMsg = runMessage(
              compareProgram(kOrdering[oi], Opcode::JNE, kind, c.b), c.a, c.b);
          const std::string valueMsg =
              runMessage(valueCompareProgram(kValueTwin[oi]), c.a, c.b);
          check(!flagMsg.empty() && flagMsg == valueMsg,
                std::string(c.name) + "/" + Bytecode::name(kOrdering[oi]) +
                    " raises the " + Bytecode::name(kValueTwin[oi]) +
                    " text (got '" + flagMsg + "', want '" + valueMsg + "')");
          continue;
        }
        for (size_t ji = 0; ji < 6; ji++) {
          const bool want =
              jumpFires(kJumps[ji], wantFlags(kOrdering[oi], c.rel));
          std::string why;
          std::string name = std::string(c.name) + "/";
          name += Bytecode::name(kOrdering[oi]);
          name += "/";
          name += kJumpNames[ji];
          name += "/";
          name += kn;
          name += want ? ": taken" : ": not taken";
          check(ranAs(compareProgram(kOrdering[oi], kJumps[ji], kind, c.b), c.a,
                      c.b, want, why),
                name + (why.empty() ? "" : " (" + why + ")"));
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// 5. String vs an object carrying a real __str__ method: valueEqual and cmp
//    both route through displayString, which CALLS the method (SPEC
//    `## operator == and !=`, `## operators < and >`).
// ---------------------------------------------------------------------------
void testObjectStrDispatch() {
  const std::vector<Built> extra{strBodyFunction("Point")};
  QLangValue obj = tableOf(
      {{"__class__", QLangValue::String("Point")},
       {"__str__",
        QLangValue::Function(std::make_shared<Closure>(Closure{1, {}}))}});
  const QLangValue text = QLangValue::String("Point");
  // The relation is "equal": __str__ returns exactly the string.
  for (size_t ji = 0; ji < 6; ji++) {
    std::string why;
    std::string name = std::string("object __str__ == string/eqf/") +
                       kJumpNames[ji] +
                       (jumpFires(kJumps[ji], kEQ) ? ": taken" : ": not taken");
    check(ranAs(compareProgram(Opcode::EQF, kJumps[ji], kReg, obj), text, obj,
                jumpFires(kJumps[ji], kEQ), why, extra),
          name + (why.empty() ? "" : " (" + why + ")"));
    std::string why2;
    std::string name2 =
        std::string("object __str__ == string/ltef/") + kJumpNames[ji] +
        (jumpFires(kJumps[ji], kEQ) ? ": taken" : ": not taken");
    check(ranAs(compareProgram(Opcode::LTEF, kJumps[ji], kReg, obj), text, obj,
                jumpFires(kJumps[ji], kEQ), why2, extra),
          name2 + (why2.empty() ? "" : " (" + why2 + ")"));
  }
  // A different string is not equal and orders below "Point".
  const QLangValue other = QLangValue::String("Poink");
  for (size_t ji = 0; ji < 6; ji++) {
    std::string why;
    std::string name = std::string("object __str__ != string/eqf/") +
                       kJumpNames[ji] +
                       (jumpFires(kJumps[ji], 0) ? ": taken" : ": not taken");
    check(ranAs(compareProgram(Opcode::EQF, kJumps[ji], kReg, obj), other, obj,
                jumpFires(kJumps[ji], 0), why, extra),
          name + (why.empty() ? "" : " (" + why + ")"));
  }
  // The pool spelling of the same comparison (the object as a pool entry).
  for (size_t ji = 0; ji < 6; ji++) {
    std::string why;
    std::string name = std::string("object __str__ == string/eqf/pool/") +
                       kJumpNames[ji] +
                       (jumpFires(kJumps[ji], kEQ) ? ": taken" : ": not taken");
    check(ranAs(compareProgram(Opcode::EQF, kJumps[ji], kPool, obj), text, obj,
                jumpFires(kJumps[ji], kEQ), why, extra),
          name + (why.empty() ? "" : " (" + why + ")"));
  }
}

// ---------------------------------------------------------------------------
// 6. `cmp` keeps its Int-only contract (D2): the flag compares widened the
//    domain, `cmp` did not.
// ---------------------------------------------------------------------------
void testCmpStaysIntOnly() {
  const std::string kWant = "cmp operand must be an int";
  const QLangValue text = QLangValue::String("abc");
  const QLangValue num = QLangValue::Int(2);
  // A String on the left, an Int on the right, in every kind.
  for (size_t ki = 0; ki < 3; ki++) {
    const std::string msg = runMessage(
        compareProgram(Opcode::CMP, Opcode::JNE, kKinds[ki], num), text, num);
    check(msg == kWant, "cmp String/Int (" + std::string(kKindNames[ki]) +
                            ") throws '" + kWant + "' (got '" + msg + "')");
  }
  // A String on the right (register and pool: an immediate cannot be a string).
  const OperandKind kNonImmediate[] = {kReg, kPool};
  for (const OperandKind kind : kNonImmediate) {
    const std::string msg = runMessage(
        compareProgram(Opcode::CMP, Opcode::JNE, kind, text), num, text);
    check(msg == kWant, "cmp Int/String (" + std::string(kKindNames[kind]) +
                            ") throws '" + kWant + "' (got '" + msg + "')");
  }
  // The same operands DO work for the flag compares — that is the M3 change.
  std::string why;
  check(ranAs(compareProgram(Opcode::EQF, Opcode::JE, kReg, num), text, num,
              false, why),
        "eqf accepts what cmp rejects (string vs int)" +
            (why.empty() ? "" : " (" + why + ")"));
}

// ---------------------------------------------------------------------------
// 7. Error-text parity, asserted against the value-producing opcode's own
//    message rather than a literal (D3).
// ---------------------------------------------------------------------------
void testErrorTextParity() {
  struct Pair {
    Opcode flag;
    Opcode value;
    const char *op;
  };
  const Pair kPairs[] = {{Opcode::LTF, Opcode::LT, "<"},
                         {Opcode::GTF, Opcode::GT, ">"},
                         {Opcode::LTEF, Opcode::LE, "<="},
                         {Opcode::GTEF, Opcode::GE, ">="}};
  const QLangValue kOperands[][2] = {
      {QLangValue::String("abc"), QLangValue::Int(5)},
      {tableOf({{"a", QLangValue::Int(1)}}), QLangValue::Int(1)},
      {QLangValue(), QLangValue::String("x")},
      {QLangValue::Array({QLangValue::Int(1)}), QLangValue::Int(1)},
  };
  const char *kOperandNames[] = {"string/int", "table/int", "nil/string",
                                 "array/int"};
  // Register and pool only: the immediate form spells an Int, and three of the
  // four right operands here are not one.
  const OperandKind kParityKinds[] = {kReg, kPool};
  for (const Pair &p : kPairs) {
    for (size_t oi = 0; oi < 4; oi++) {
      for (const OperandKind kind : kParityKinds) {
        const std::string flagMsg = runMessage(
            compareProgram(p.flag, Opcode::JNE, kind, kOperands[oi][1]),
            kOperands[oi][0], kOperands[oi][1]);
        const std::string valueMsg = runMessage(
            valueCompareProgram(p.value), kOperands[oi][0], kOperands[oi][1]);
        check(!flagMsg.empty() && flagMsg == valueMsg,
              std::string(Bytecode::name(p.flag)) + " on " + kOperandNames[oi] +
                  "/" + kKindNames[kind] + " raises the '" + p.op +
                  "' text (got '" + flagMsg + "', want '" + valueMsg + "')");
      }
    }
  }
  // Spot-check the text itself so a shared-but-wrong message cannot pass.
  const std::string msg = runMessage(
      compareProgram(Opcode::LTEF, Opcode::JNE, kReg, QLangValue::Int(1)),
      tableOf({{"a", QLangValue::Int(1)}}), QLangValue::Int(1));
  check(msg == "operator '<=' cannot compare table and int",
        "ltef table/int message is \"operator '<=' cannot compare table and "
        "int\" (got '" +
            msg + "')");
}

// ---------------------------------------------------------------------------
// 8. The assembler aliases (D6): no identity, no mnemonic; the canonical names
//    stay. There is no assembler in the repo, so this name table IS the
//    contract a future one has to honour.
// ---------------------------------------------------------------------------
void testAliases() {
  const char *kAliases[] = {"ja", "jae", "jb", "jbe"};
  for (const char *alias : kAliases) {
    bool found = false;
    for (int id = 0; id < static_cast<int>(kOpcodeUsedCount); id++) {
      const char *n = Bytecode::name(static_cast<Opcode>(id));
      if (n && std::strcmp(n, alias) == 0) found = true;
    }
    check(!found, std::string("no opcode identity is named '") + alias +
                      "' (D6: alias of a canonical jump)");
  }
  const struct {
    Opcode op;
    const char *name;
  } kCanonical[] = {{Opcode::JG, "jg"},
                    {Opcode::JGE, "jge"},
                    {Opcode::JL, "jl"},
                    {Opcode::JLE, "jle"}};
  for (const auto &k : kCanonical)
    check(std::strcmp(Bytecode::name(k.op), k.name) == 0,
          std::string(Bytecode::name(k.op)) +
              " keeps its canonical mnemonic (D6)");
}

// ---------------------------------------------------------------------------
// 9. Reserved slots (§11 item 7): readQlc decodes every instruction, so a
//    modifier naming a kind the opcode does not permit is a LOAD error — the VM
//    is never handed a stream whose pc would desynchronise.
// ---------------------------------------------------------------------------
CompiledProgram programWithCode(std::vector<uint8_t> code,
                                std::vector<size_t> offsets) {
  CompiledProgram p;
  Function f;
  f.name = "main";
  f.registerCount = kRegCount;
  f.code = std::move(code);
  f.instrOffsets = std::move(offsets);
  f.loc.assign(f.instrOffsets.size(), {1, 0});
  p.functions.push_back(std::move(f));
  p.registerCount = kRegCount;
  return p;
}

std::string writeImage(const CompiledProgram &p) {
  std::ostringstream os(std::ios::binary);
  if (!writeQlc(os, p, {})) return {};
  return os.str();
}

bool readImage(const std::string &bytes, std::string &err) {
  CompiledProgram p;
  std::istringstream is(bytes, std::ios::binary);
  std::vector<std::pair<int, std::string>> slots;
  return readQlc(is, p, slots, err);
}

void testReservedSlots() {
  const uint8_t esc = kOpcodeEscape;
  struct Case {
    Opcode op;
    uint8_t e;
    const char *why;
  };
  const Case kRejected[] = {
      {Opcode::TST, 0x40, "tst with an immediate kind (0) is refused at load"},
      {Opcode::TST, 0x08, "tst with a pool kind (2) is refused at load"},
      {Opcode::TST, 0x0C, "tst with the reserved kind (3) is refused at load"},
      {Opcode::EQF, 0x0C, "eqf with the reserved kind (3) is refused at load"},
      {Opcode::LTF, 0x0C, "ltf with the reserved kind (3) is refused at load"},
      {Opcode::GTF, 0x0C, "gtf with the reserved kind (3) is refused at load"},
      {Opcode::LTEF, 0x0C,
       "ltef with the reserved kind (3) is refused at load"},
      {Opcode::GTEF, 0x0C,
       "gtef with the reserved kind (3) is refused at load"},
  };
  for (const Case &c : kRejected) {
    const std::vector<uint8_t> code = {
        static_cast<uint8_t>(static_cast<uint8_t>(c.op) | esc), c.e, 0x01,
        0x02};
    std::string err;
    check(!readImage(writeImage(programWithCode(code, {0})), err),
          std::string(c.why) + " (" + err + ")");
  }
  // The legal spellings load: the register base forms and the escaped
  // immediate / pool forms of a compare.
  const std::vector<uint8_t> kAccepted[] = {
      {static_cast<uint8_t>(Opcode::EQF), 0x01, 0x02},
      {static_cast<uint8_t>(static_cast<uint8_t>(Opcode::LTF) | esc), 0x40,
       0x01, 0x02},
      {static_cast<uint8_t>(static_cast<uint8_t>(Opcode::GTEF) | esc), 0x08,
       0x01, 0x02},
      {static_cast<uint8_t>(Opcode::TST), 0x01},
  };
  for (size_t i = 0; i < 4; i++) {
    CompiledProgram prog = programWithCode(kAccepted[i], {0});
    // The pool-kind spelling names pool index 2; v8 range-checks pool
    // operands against the module pool, so it needs three entries.
    prog.constants.assign(3, QLangValue());
    std::string err;
    check(readImage(writeImage(prog), err),
          "legal flag-flow spelling " + std::to_string(i) + " loads (" + err +
              ")");
  }
}

} // namespace

int main() {
  // The oracle's flag bits are deliberately its own: QLangVM::CMP_GR/CMP_EQ/
  // CMP_LT are private, and every check below observes the VM only through a
  // register (taken -> 1, not taken -> 0), so the suite cannot agree with a
  // wrong flag encoding by sharing constants with it.
  check(kOpcodeUsedCount == 74,
        "kOpcodeUsedCount is 74 (72 after the Phase 3 S3 compaction, +2 for "
        "getimport/callimport in Phase 3c)");

  testMatrix();
  testNoneSetState();
  testTst();
  testGeneralDomains();
  testObjectStrDispatch();
  testCmpStaysIntOnly();
  testErrorTextParity();
  testAliases();
  testReservedSlots();

  std::cout << (fails == 0 ? "ALL PASS" : "FAILURES") << ": " << passes
            << " passed, " << fails << " failed\n";
  return fails == 0 ? 0 : 1;
}
