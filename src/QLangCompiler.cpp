#include "qlang/QLangCompiler.h"

#include <algorithm>
#include <climits>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <vector>

#include "qlang/QLangErrors.h"

namespace qlang {

// ---------------------------------------------------------------------------
// Raw instruction emitter: accumulates instructions in a vector, then
// encodes them into the flat byte stream at finish() time. (The struct
// itself lives in QLangCompiler.h so QLangCompiler can hold one by value.)
// ---------------------------------------------------------------------------
void FnEmitter::finish(Function &fn) {
  // Validate jump targets.
  for (size_t i = 0; i < raw.size(); i++) {
    if (raw[i].jumpTarget >= 0 &&
        raw[i].jumpTarget >= static_cast<int32_t>(raw.size()))
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "invalid jump target in emitter");
  }

  // Initial sizes: jumps assume the widest form (jmp32, plus a condition
  // register for JMPIFFALSE/JMPIFTRUE); everything else uses the size of
  // the form its operands/immediate actually require (base or escaped).
  std::vector<size_t> sizes(raw.size());
  for (size_t i = 0; i < raw.size(); i++) {
    if (raw[i].jumpTarget >= 0)
      sizes[i] = isConditionalJumpOpcode(raw[i].op) ? 7 : 6;
    else
      sizes[i] = instructionSize(raw[i].op, computeModifier(raw[i]));
  }

  auto recomputeOffsets = [&]() {
    std::vector<size_t> off(raw.size());
    size_t pos = 0;
    for (size_t i = 0; i < raw.size(); i++) {
      off[i] = pos;
      pos += sizes[i];
    }
    return off;
  };

  // Fixpoint: set each jump to the narrowest form that fits the current
  // offsets; shrinking/growing shifts later offsets, so iterate until
  // stable.
  std::vector<size_t> offsets = recomputeOffsets();
  for (int iter = 0; iter < 32; iter++) {
    bool changed = false;
    for (size_t i = 0; i < raw.size(); i++) {
      if (raw[i].jumpTarget < 0) continue;
      int32_t offset = static_cast<int32_t>(offsets[raw[i].jumpTarget]) -
                       static_cast<int32_t>(offsets[i] + sizes[i]);
      size_t newSize = minJumpSize(offset) +
                       (isConditionalJumpOpcode(raw[i].op) ? 1 : 0);
      if (newSize != sizes[i]) {
        sizes[i] = newSize;
        changed = true;
      }
    }
    if (!changed) break;
    offsets = recomputeOffsets();
  }

  // Post-loop validation: every jump offset must fit in its chosen width
  // (a safety net for a non-converging fixpoint).
  for (size_t i = 0; i < raw.size(); i++) {
    if (raw[i].jumpTarget < 0) continue;
    int32_t offset = static_cast<int32_t>(offsets[raw[i].jumpTarget]) -
                     static_cast<int32_t>(offsets[i] + sizes[i]);
    size_t offWidth = sizes[i] - (isConditionalJumpOpcode(raw[i].op) ? 1 : 0);
    if (offWidth == 1 && (offset < -127 || offset > 127))
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "jump offset does not fit (emitter fixpoint failed)");
    if (offWidth == 2 && (offset < -32767 || offset > 32767))
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "jump offset does not fit (emitter fixpoint failed)");
  }

  // Encode in order.
  fn.code.clear();
  for (size_t i = 0; i < raw.size(); i++) {
    DecodedInstruction di;
    di.op = raw[i].op;
    di.a = raw[i].a;
    di.b = raw[i].b;
    di.c = raw[i].c;
    di.imm = raw[i].imm;
    di.size = sizes[i];
    if (raw[i].jumpTarget >= 0) {
      di.jumpOffset = static_cast<int32_t>(offsets[raw[i].jumpTarget]) -
                      static_cast<int32_t>(offsets[i] + sizes[i]);
      di.escaped = sizes[i] > baseInstructionSize(raw[i].op);
      di.modifier = (sizes[i] >= 6) ? 1 : 0;
    } else {
      di.modifier = computeModifier(raw[i]);
      di.escaped = di.modifier != 0;
    }
    encodeInstruction(fn.code, di);
  }

  fn.instrOffsets = std::move(offsets);
  fn.loc.resize(raw.size());
  for (size_t i = 0; i < raw.size(); i++)
    fn.loc[i] = {raw[i].line, raw[i].col};
  fn.constants = std::move(consts);
}

uint8_t FnEmitter::computeModifier(const RawInstr &r) {
  // CALL: b is a signed 16-bit function ID (negative = native, index =
  // -1 - b; non-negative = builtin), not a register. Always escaped with the
  // right field fixed to 2 (signed 16-bit form).
  if (r.op == Opcode::CALL) {
    if (r.a < 0 || r.a > 65535)
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "register index out of range");
    if (r.b < INT16_MIN || r.b > INT16_MAX)
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "function ID out of range");
    if (r.c < 0 || r.c > 255)
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "argument count out of range");
    uint8_t leftForm = (r.a > 255) ? 1 : 0;
    return static_cast<uint8_t>((2 << 4) | leftForm);
  }
  if (isImmediateOpcode(r.op)) {
    if (r.a < 0 || r.a > 65535)
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "register index out of range");
    if (r.imm < INT32_MIN || r.imm > INT32_MAX)
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "immediate out of int32 range");
    uint8_t leftForm = (r.a > 255) ? 1 : 0;
    uint8_t immForm;
    if (r.imm >= -128 && r.imm <= 127)
      immForm = 4;
    else if (r.imm >= -32768 && r.imm <= 32767)
      immForm = 5;
    else
      immForm = 6;
    if (leftForm == 0 && immForm == 4)
      return 0; // base form
    return static_cast<uint8_t>((immForm << 4) | leftForm);
  }
  // RegConst
  if (r.a < 0 || r.a > 65535)
    throw QLangError(QLangErrorKind::Runtime, 0, 0,
                     "register index out of range");
  if (r.b < 0 || r.b > 65535)
    throw QLangError(QLangErrorKind::Runtime, 0, 0,
                     "register index out of range");
  if (r.c < 0 || r.c > 255)
    throw QLangError(QLangErrorKind::Runtime, 0, 0,
                     "third operand must be a 1-byte register");
  uint8_t leftForm = (r.a > 255) ? 1 : 0;
  uint8_t rightForm = (r.b > 255) ? 1 : 0;
  if (leftForm == 0 && rightForm == 0)
    return 0; // base form
  return static_cast<uint8_t>((rightForm << 4) | leftForm);
}

// Reserved prototype/instance key holding the closed-field schema: a table
// mapping each declared field name to its index (Phase 1). The prototype
// function emits it and the constructor copies it onto each instance so the
// VM can enforce the closed field set in GETFIELD/SETFIELD.
static const char *kFieldMetaKey = "__fields__";
// Reserved instance key holding the class name, stamped by the constructor for
// closed-field-set error messages (Phase 1.3).
static const char *kClassMetaKey = "__class__";
// Reserved prototype/instance key holding the const-field set: a table mapping
// each `const` field name to 1 (Phase 2). Emitted into the prototype and copied
// onto each instance so SETFIELD can reject assignment to read-only fields.
static const char *kConstFieldMetaKey = "__const_fields__";
// Reserved prototype/instance key holding the inferred field types: a table
// mapping each type-constrained field name to its type-name string (Phase 5).
// Emitted into the prototype and copied onto each instance so SETFIELD can
// reject values that violate the type inferred from the field's default.
static const char *kFieldTypeMetaKey = "__field_types__";

// True when an expression is a compile-time array literal (used to decide
// array-append vs. array-extend for `arr += rhs`).
static bool isArrayExpr(const ExprPtr &e) {
  return e && e->kind == ExprKind::Array;
}

// Forward declaration: defined further down, used by the encoding-guideline
// helpers below (inPlaceReg/foldSelfArith/compileArgsToSlots).
static bool isIntLiteral(const ExprPtr &e);
static std::optional<int32_t> foldNegLiteral(const ExprPtr &e);

// True when e is a call to the builtin range(a, b) with exactly two arguments.
// On success, outStart/outEnd are set to the two argument expressions so the
// compiler can emit a counter loop or bounds check without materializing an
// array (spec §range).
static bool isRangeCall(const ExprPtr &e, ExprPtr &outStart, ExprPtr &outEnd) {
  if (!e || e->kind != ExprKind::Call || e->callee != "range" ||
      e->args.size() != 2)
    return false;
  outStart = e->args[0];
  outEnd = e->args[1];
  return true;
}

// Fold an expression to a compile-time integer when it is a numeric literal
// (Int/Float) or a unary-minus of such a literal. Returns std::nullopt for
// anything else (variable, binary op, call, ...). Used to decide at compile
// time whether a `step` value and a range's bounds are constants, which
// determines whether the loop can be emitted with a fixed comparison (the
// common, tight case) or needs a runtime direction/path selection.
static std::optional<int64_t> foldConst(const ExprPtr &e) {
  if (!e) return std::nullopt;
  if (e->kind == ExprKind::Literal) {
    const QLangValue &v = e->literal;
    if (v.type() == QLangValue::Type::Int)
      return static_cast<int64_t>(v.asInt());
    if (v.type() == QLangValue::Type::Float)
      return static_cast<int64_t>(v.asFloat());
    return std::nullopt;
  }
  if (e->kind == ExprKind::Unary && e->uop.type == TokenType::Minus) {
    auto inner = foldConst(e->operand);
    if (inner) return -*inner;
  }
  return std::nullopt;
}

// Evaluate a table key expression to a string.
// Static identifier keys are Variable expressions; dynamic keys ($expr) are
// other expression forms that must be constant-evaluable at compile time.
static std::string evalConstKey(const ExprPtr &key) {
  if (!key)
    throw QLangError(QLangErrorKind::Parse, 0, 0, "null table key");
  if (key->kind == ExprKind::Variable)
    return key->name;
  if (key->kind == ExprKind::Literal)
    return key->literal.asString();
  throw QLangError(QLangErrorKind::Parse, 0, 0,
                   "non-constant table key");
}

QLangCompiler::QLangCompiler(std::vector<StmtPtr> program, QLangContext *scope)
    : m_program(std::move(program)), m_scope(scope) {}

void QLangCompiler::addExternalConsts(const std::unordered_set<std::string> &names) {
  m_constVars.insert(names.begin(), names.end());
}

void QLangCompiler::addExternalVars(const std::unordered_set<std::string> &names) {
  m_externalVars.insert(names.begin(), names.end());
}

void QLangCompiler::addExternalFuncs(const std::unordered_set<std::string> &names) {
  // Ids are negative and unique across all calls: the .qlc native-slot table
  // (slot = -1 - id) requires unique ids, and a second call must not reuse
  // ids assigned by an earlier one.
  for (const auto &name : names) {
    if (m_nativeFuncIds.count(name))
      continue;
    int next = -1;
    for (const auto &[_, id] : m_nativeFuncIds)
      next = std::min(next, id - 1);
    m_nativeFuncIds[name] = next;
  }
}

void QLangCompiler::addModules(const std::vector<QLangContext::Module> &modules) {
  std::unordered_set<std::string> allNames;
  for (const auto &mod : modules) {
    for (const auto &[name, _] : mod.funcs) {
      allNames.insert(name);
      m_moduleFuncNames.insert(name);
    }
  }
  addExternalFuncs(allNames);
  m_moduleFuncs = modules;
}

int QLangCompiler::newRegister() { return m_nextReg++; }
bool QLangCompiler::inSlotRange(int r) const
{
  for (auto &slot : m_slotRanges)
    if (r >= slot.first && r < slot.second)
      return true;
  return false;
}
int QLangCompiler::maxLiveTmp() const
{
  int m = -1;
  for (int t : m_liveTmps)
    if (t > m)
      m = t;
  return m;
}
// Move deferred temps that are now above the highest live temp into the
// pool (they became reusable once the high temp died).
void QLangCompiler::promoteDeferred()
{
  int hi = maxLiveTmp();
  auto it = m_deferred.begin();
  while (it != m_deferred.end())
  {
    // Deferred regs inside an active slot range stay out: promoting them
    // would defeat beginArgSlots' quarantine (they re-enter the pool while
    // the call's slot values are live, and tmpRegister() can alias them).
    if (*it > hi && !inSlotRange(*it))
    {
      m_freeRegs.push_back(*it);
      it = m_deferred.erase(it);
    }
    else
      ++it;
  }
}
// Pool entry point: keep the pool strictly above the highest live temp.
void QLangCompiler::returnToPool(int r)
{
  if (r > maxLiveTmp())
    m_freeRegs.push_back(r);
  else
    m_deferred.insert(r);
}
int QLangCompiler::tmpRegister()
{
  // Reuse dead temps first so registerCount is the high-water mark of
  // simultaneously-live temps, not the total number of statements. The pool
  // must stay above every live temp (a call's arg slots dest+1.. must never
  // alias one), so entries at or below the highest live temp are demoted to
  // the deferred set. Take the SMALLEST eligible temp: an LIFO top would
  // hand out the largest pooled register first, forcing the rest of the
  // statement's temps onto fresh registers and ratcheting m_nextReg up every
  // statement. Ascending consumption packs a statement's temps into the
  // lowest free block, so the block is stable and gets reused by later
  // statements.
  promoteDeferred();
  int hi = maxLiveTmp();
  int r = -1;
  for (int c : m_freeRegs)
    if (c > hi && !inSlotRange(c) && (r < 0 || c < r))
      r = c;
  if (r >= 0)
    m_freeRegs.erase(std::find(m_freeRegs.begin(), m_freeRegs.end(), r));
  else
  {
    // Demote the sub-high and in-slot-range entries so the next allocation
    // re-checks them (they become eligible once the high temp dies / the
    // slot range closes).
    for (auto it = m_freeRegs.begin(); it != m_freeRegs.end();)
      if (*it <= hi || inSlotRange(*it))
      {
        m_deferred.insert(*it);
        it = m_freeRegs.erase(it);
      }
      else
        ++it;
    r = newRegister();
  }
  m_liveTmps.push_back(r);
  return r;
}
// Idempotent: a temp is only freed while live (endOfStatement and explicit
// frees can both target the same temp). A freed register inside an active
// call-arg slot range goes to the quarantine, not the pool: handing it back
// out would let a later argReg alias a slot the slot-MOVs are about to
// clobber.
void QLangCompiler::freeRegister(int r)
{
  auto it = std::find(m_liveTmps.begin(), m_liveTmps.end(), r);
  if (it == m_liveTmps.end())
    return;
  m_liveTmps.erase(it);
  m_protectedTmps.erase(r);
  for (auto &slot : m_slotRanges)
    if (r >= slot.first && r < slot.second)
    {
      m_quarantined.insert(r);
      return;
    }
  returnToPool(r);
}
// Statement boundary: free every live temp that wasn't allocated before this
// statement (those are protected), then restore the protection set to its
// pre-statement state. Restoring (not clearing) matters: loop-header temps
// were protected by the enclosing loop's statement and must stay protected
// for the remaining body statements.
void QLangCompiler::endOfStatement()
{
  // Free against THIS statement's entry snapshot, not the current set
  // (nested statements restore their own, larger sets).
  auto pre = m_protectStack.back();
  m_protectStack.pop_back();
  auto live = m_liveTmps; // copy: freeRegister erases from m_liveTmps
  for (int r : live)
    if (!pre.count(r))
      freeRegister(r);
  m_protectedTmps = pre;
  // All call-arg slot ranges are stale now: restore their quarantined
  // registers to the pool (innermost first; a register still inside an
  // enclosing active range stays quarantined).
  while (!m_slotRanges.empty())
    endArgSlots();
}
// Release a statement-level temp: clear it (so it can't keep a reference
// alive past the statement — Phase 7 finalizers need the true final
// release) and return it to the pool.
void QLangCompiler::releaseTemp(int r)
{
  emit(Opcode::MOVC, r, emitConst(QLangValue()), 0);
  freeRegister(r);
}
// Drop freed registers in [lo, hi): those slots are about to be reserved for
// call-arg/template slots, so tmpRegister() must not hand one back out.
// Covers the deferred set too: a deferred register below a named register
// would clobber it via call arg slots once promoted.
void QLangCompiler::dropFreeRegs(int lo, int hi)
{
  m_freeRegs.erase(
      std::remove_if(m_freeRegs.begin(), m_freeRegs.end(),
                     [lo, hi](int r) { return r >= lo && r < hi; }),
      m_freeRegs.end());
  auto it = m_deferred.begin();
  while (it != m_deferred.end())
    if (*it >= lo && *it < hi)
      it = m_deferred.erase(it);
    else
      ++it;
}
// Reserve dest+1 .. dest+argCount as live call-arg slots: grow m_nextReg
// past them (the frame must cover the slots) and keep freed registers out
// of the range so tmpRegister() can't hand a slot back out as a temp (the
// slot MOVs would then alias live values, and slots beyond the old
// high-water mark would read past the frame).
void QLangCompiler::reserveArgSlots(int dest, int nSlots)
{
  beginArgSlots(dest, nSlots);
}
// Grow the frame past the slot region and quarantine pool registers in it
// for the duration of the call (see endArgSlots / endOfStatement).
void QLangCompiler::beginArgSlots(int dest, int nSlots)
{
  int lo = dest + 1, hi = dest + 1 + nSlots;
  while (m_nextReg < hi)
    m_nextReg++;
  m_slotRanges.push_back({lo, hi});
  auto it = m_freeRegs.begin();
  while (it != m_freeRegs.end())
  {
    if (*it >= lo && *it < hi)
    {
      m_quarantined.insert(*it);
      it = m_freeRegs.erase(it);
    }
    else
      ++it;
  }
}
void QLangCompiler::endArgSlots()
{
  auto rng = m_slotRanges.back();
  m_slotRanges.pop_back();
  auto it = m_quarantined.begin();
  while (it != m_quarantined.end())
  {
    int r = *it;
    bool inEnclosing = false;
    for (auto &slot : m_slotRanges)
      if (r >= slot.first && r < slot.second)
      {
        inEnclosing = true;
        break;
      }
    if (r >= rng.first && r < rng.second && !inEnclosing)
    {
      returnToPool(r);
      it = m_quarantined.erase(it);
    }
    else
      ++it;
  }
}

void QLangCompiler::buildRegisterReport()
{
  m_report = RegisterReport{};
  m_report.registerCount = std::max(m_nextReg, 1);
  m_report.nextNamedReg = m_nextNamedReg;
  for (const auto &[name, r] : m_regs)
    m_report.named[r] = name;
  // Block-scoped names too (normally none survive to end of compile, since
  // every block is closed; innermost scope wins if one does).
  for (const auto &scope : m_blockScopes)
    for (const auto &[name, r] : scope)
      m_report.named[r] = name;
  m_report.namedRegs = m_namedRegs;
  m_report.freeNamedRegs = m_freeNamedRegs;
  m_report.freeRegs = m_freeRegs;
  m_report.liveTmps = m_liveTmps;
  m_report.protectedTmps = m_protectedTmps;
  m_report.quarantined = m_quarantined;
  m_report.deferred = m_deferred;
  m_report.slotRanges = m_slotRanges;
}

int QLangCompiler::declareVar(const std::string &name, bool blockScoped) {
  // A named register must never alias an active call-arg slot region:
  // declare-on-first-use (host-injected external vars, register-less consts)
  // runs mid-statement while an enclosing call's slots are live, and the
  // slot MOVs would clobber the freshly declared register.
  auto inSlotRange = [this](int r) {
    for (auto &slot : m_slotRanges)
      if (r >= slot.first && r < slot.second)
        return true;
    return false;
  };
  if (blockScoped && !m_blockScopes.empty()) {
    auto &top = m_blockScopes.back();
    auto it = top.find(name);
    if (it != top.end())
      return it->second;
    int r;
    // Reuse a register freed by a dead block scope first (keeps
    // registerCount from growing with the number of loop bodies); fall back
    // to the dense counter.
    if (!m_freeNamedRegs.empty())
    {
      r = m_freeNamedRegs.back();
      m_freeNamedRegs.pop_back();
    }
    else
      r = m_nextNamedReg++;
    // Skip live temps (e.g. an enclosing loop's header temps, protected
    // across statements), already-allocated named registers, and active
    // call-arg slots: a named register must never alias any of them.
    while (std::find(m_liveTmps.begin(), m_liveTmps.end(), r) !=
             m_liveTmps.end() ||
           m_namedRegs.count(r) || inSlotRange(r))
      r++;
    // Temps must stay ABOVE named registers: a call's arg slots are
    // dest+1..dest+argCount, and dest is a temp — a low temp would put the
    // slots on top of live named variables. Push the temp counter past the
    // new named register and drop pool registers below it; pool registers
    // above it (the common case) are kept for reuse.
    while (m_nextReg <= r)
      m_nextReg++;
    dropFreeRegs(0, r + 1);
    m_namedRegs.insert(r);
    top[name] = r;
    trace(TRACE_FULL, "[DECL] line=%d '%s' -> r=%d\n", m_curLine, name.c_str(), r);
    return r;
  }
  auto it = m_regs.find(name);
  if (it != m_regs.end())
    return it->second;
  if (m_scope && !m_inFunction) {
    // Phase 2: top-level names live in the context's global table, and the
    // table slot IS the main-frame register — the main frame aliases the
    // table's slots directly, so there is no scope sync in or out. Ask the
    // table for the slot: existing names (host-injected externals/consts)
    // keep their slot, new names append. Inside a function this path is
    // skipped: locals are frame registers, and globals are reached only via
    // an explicit `global` (capture), never through the table here.
    int r = static_cast<int>(m_scope->globalSlotFor(name));
    while (m_nextReg <= r)
      m_nextReg++;
    if (r + 1 > m_nextNamedReg)
      m_nextNamedReg = r + 1;
    // The slot may reuse a register the free lists handed out earlier (a
    // released block-local below it): reclaim it from both lists.
    m_freeNamedRegs.erase(
        std::remove(m_freeNamedRegs.begin(), m_freeNamedRegs.end(), r),
        m_freeNamedRegs.end());
    dropFreeRegs(0, r + 1);
    m_namedRegs.insert(r);
    m_regs[name] = r;
    trace(TRACE_FULL, "[DECL] line=%d '%s' -> r=%d (table)\n", m_curLine,
          name.c_str(), r);
    return r;
  }
  int r;
  // Same free-list reuse and constraints as the block-scoped path above.
  if (!m_freeNamedRegs.empty())
  {
    r = m_freeNamedRegs.back();
    m_freeNamedRegs.pop_back();
  }
  else
    r = m_nextNamedReg++;
  while (std::find(m_liveTmps.begin(), m_liveTmps.end(), r) !=
           m_liveTmps.end() ||
         m_namedRegs.count(r) || inSlotRange(r))
    r++;
  while (m_nextReg <= r)
    m_nextReg++;
  dropFreeRegs(0, r + 1);
  m_namedRegs.insert(r);
  m_regs[name] = r;
  trace(TRACE_FULL, "[DECL] line=%d '%s' -> r=%d\n", m_curLine, name.c_str(), r);
  return r;
}

void QLangCompiler::popBlockScope()
{
  if (m_blockScopes.empty())
    return;
  auto &top = m_blockScopes.back();
  for (auto &kv : top)
  {
    trace(TRACE_FULL, "[POP] line=%d '%s' r=%d\n", m_curLine, kv.first.c_str(),
          kv.second);
    // The scope is dead: its named registers return to the free list so a
    // later declaration can reuse them.
    m_namedRegs.erase(kv.second);
    m_freeNamedRegs.push_back(kv.second);
  }
  m_blockScopes.pop_back();
}

int QLangCompiler::regOf(const std::string &name) const {
  for (auto it = m_blockScopes.rbegin(); it != m_blockScopes.rend(); ++it) {
    auto f = it->find(name);
    if (f != it->end())
      return f->second;
  }
  auto it = m_regs.find(name);
  return it == m_regs.end() ? -1 : it->second;
}

int QLangCompiler::inPlaceReg(const ExprPtr &e) const {
  // Only plain local/parameter variables can be read directly: their
  // register is stable for the rest of the statement (named registers are
  // never handed back to the temp pool). Captures (REFLOAD), enum tables,
  // externals, literals and compound expressions all need a register.
  if (e && e->kind == ExprKind::Variable) {
    int r = regOf(e->name);
    if (r >= 0)
      return r;
  }
  return -1;
}

bool QLangCompiler::foldSelfArith(const ExprPtr &e, const std::string &name,
                                  int r) {
  // `x = x + k` / `x = x - k` -> inc/dec/addi/subi in place (SPEC
  // §bytecodes: encoding guidelines — favor int immediates over
  // temporaries). `+` is commutative so the variable may be on either side;
  // `-` only folds when the variable is the left operand.
  if (!e || e->kind != ExprKind::Binary)
    return false;
  const std::string &t = e->op.text;
  if (t != "+" && t != "-")
    return false;
  bool leftIsSelf = e->left && e->left->kind == ExprKind::Variable &&
                    e->left->name == name;
  bool rightIsSelf = e->right && e->right->kind == ExprKind::Variable &&
                     e->right->name == name;
  if (!leftIsSelf && !rightIsSelf)
    return false;
  const ExprPtr &lit = (t == "+" && leftIsSelf) ? e->right
                          : (t == "+")           ? e->left
                                                 : e->right;
  if (!isIntLiteral(lit))
    return false;
  int64_t k = lit->literal.asInt();
  if (t == "+") {
    if (k == 1)
      emit(Opcode::INC, r, r);
    else
      emitImm(Opcode::ADDI, r, k);
  } else {
    if (k == 1)
      emit(Opcode::DEC, r, r);
    else
      emitImm(Opcode::SUBI, r, k);
  }
  return true;
}

void QLangCompiler::compileArgsToSlots(const std::vector<ExprPtr> &args,
                                       int slotBase) {
  // Simple operands land directly in their slot; complex ones share a single
  // scratch register (allocated on first use) so an N-arg call costs at most
  // one temp instead of N (SPEC §bytecodes: encoding guidelines — universal
  // scratch pad register).
  int scratch = -1;
  for (size_t i = 0; i < args.size(); i++) {
    const ExprPtr &arg = args[i];
    int slot = slotBase + static_cast<int>(i);
    int direct = inPlaceReg(arg);
    if (direct >= 0) {
      if (slot != direct)
        emit(Opcode::MOV, slot, direct, 0);
      continue;
    }
    if (isIntLiteral(arg)) {
      emitImm(Opcode::MOVI, slot, arg->literal.asInt());
      continue;
    }
    // `-(literal)` folds to one MOVI straight into the slot — no temp, no
    // MOV (SPEC §bytecodes: encoding guidelines — avoid temporary registers).
    auto negFold = foldNegLiteral(arg);
    if (negFold) {
      emitImm(Opcode::MOVI, slot, *negFold);
      continue;
    }
    if (arg->kind == ExprKind::Literal) {
      emit(Opcode::MOVC, slot, emitConst(arg->literal), 0);
      continue;
    }
    if (scratch < 0)
      scratch = tmpRegister();
    compileExpr(arg, scratch);
    if (slot != scratch)
      emit(Opcode::MOV, slot, scratch, 0);
  }
  // Clear the shared scratch so it does not keep a reference alive past the
  // call (Phase 3 H2: release() must drop the last reference; a leaked temp
  // would retain the owned external).
  if (scratch >= 0)
    releaseTemp(scratch);
}

int QLangCompiler::emitConst(const QLangValue &value) {
  int idx = m_emitter.addConst(value);
  std::string valStr;
  if (value.type() == QLangValue::Type::Int)
    valStr = std::to_string(value.asInt());
  else if (value.type() == QLangValue::Type::String)
    valStr = value.asString();
  else
    valStr = std::string("(type=") + std::to_string((int)value.type()) + ")";
  trace(TRACE_FULL, "[DBG-CONST] Adding constant type=%d value=%s -> index=%d (pool size=%zu)\n",
        (int)value.type(), valStr.c_str(), idx, m_emitter.consts.size());
  return idx;
}

void QLangCompiler::emit(Opcode op, int a, int b, int c) {
  uint16_t line = static_cast<uint16_t>(m_curLine);
  uint16_t col = static_cast<uint16_t>(m_curCol);
  if (isJumpOpcode(op)) {
    // JMPIFFALSE/JMPIFTRUE carry the condition register in `a` and the
    // target in `b`; every other jump carries the target in `a`.
    int32_t target = a;
    int32_t cond = 0;
    if (op == Opcode::JMPIFFALSE || op == Opcode::JMPIFTRUE) {
      cond = a;
      target = b;
    }
    m_emitter.emitJump(op, target, cond, line, col);
    return;
  }
  m_emitter.emit(op, a, b, c, 0, line, col);
}

void QLangCompiler::emitImm(Opcode op, int a, int32_t imm) {
  m_emitter.emit(op, a, 0, 0, imm,
                 static_cast<uint16_t>(m_curLine),
                 static_cast<uint16_t>(m_curCol));
}

// True when e is a Type::Int literal: the only literals eligible for MOVI
// (the value tag must match the constant-pool path — a `true` literal is
// Bool, not Int, and 64-bit literals are Qword/Long, not Int).
static bool isIntLiteral(const ExprPtr &e) {
  return e && e->kind == ExprKind::Literal &&
         e->literal.type() == QLangValue::Type::Int;
}

// Fold a unary-minus over an Int literal to a compile-time int32 so the
// compiler can emit one MOVI with a negative immediate instead of
// MOVI + NEG (+ MOV). Also folds one level of double negation (`-(-5)`
// -> 5). Returns std::nullopt for anything else (float literals,
// variables, other operators) or when the result overflows int32
// (e.g. `-(-2147483648)`), in which case the caller falls back to the
// MOVI + NEG sequence.
static std::optional<int32_t> foldNegLiteral(const ExprPtr &e) {
  if (!e || e->kind != ExprKind::Unary || e->uop.type != TokenType::Minus)
    return std::nullopt;
  const ExprPtr &o = e->operand;
  if (isIntLiteral(o)) {
    int64_t v = -static_cast<int64_t>(o->literal.asInt());
    if (v < INT32_MIN || v > INT32_MAX) return std::nullopt;
    return static_cast<int32_t>(v);
  }
  if (o && o->kind == ExprKind::Unary &&
      o->uop.type == TokenType::Minus && isIntLiteral(o->operand)) {
    return o->operand->literal.asInt(); // -(-x) == x, always in range
  }
  return std::nullopt;
}

// Map a comparison opcode to the conditional jump that fires when that
// comparison is FALSE (the JMPIFFALSE semantics the branch sites use).
//   == -> jne   != -> je   < -> jge   > -> jle   <= -> jg   >= -> jl
static Opcode cmpJumpIfFalse(Opcode rel) {
  switch (rel) {
  case Opcode::EQ: return Opcode::JNE;
  case Opcode::NE: return Opcode::JE;
  case Opcode::LT: return Opcode::JGE;
  case Opcode::GT: return Opcode::JLE;
  case Opcode::LE: return Opcode::JG;
  case Opcode::GE: return Opcode::JL;
  default: return Opcode::JMPIFFALSE; // not a comparison; caller handles
  }
}

int QLangCompiler::emitCmpJumpFalse(Opcode rel, int ra, int rb) {
  emit(Opcode::CMP, ra, rb);
  // Flag jump (JE/JNE/JG/JGE/JL/JLE): target in `a`, patched by caller.
  emit(cmpJumpIfFalse(rel), 0, 0, 0);
  return static_cast<int>(m_emitter.size()) - 1;
}

int QLangCompiler::emitCmpJumpFalseExpr(const ExprPtr &cond) {
  if (!cond || cond->kind != ExprKind::Binary)
    return -1;
  const std::string &t = cond->op.text;
  Opcode rel;
  if (t == "==") rel = Opcode::EQ;
  else if (t == "!=") rel = Opcode::NE;
  else if (t == "<") rel = Opcode::LT;
  else if (t == ">") rel = Opcode::GT;
  else if (t == "<=") rel = Opcode::LE;
  else if (t == ">=") rel = Opcode::GE;
  else return -1;
  // Both operands must be compile-time Int (SPEC: cmp/cmpi are Int-only).
  if (inferType(cond->left) != QLangValue::Type::Int) return -1;
  if (inferType(cond->right) != QLangValue::Type::Int) return -1;
  // Plain-variable operands are compared in place (no mov into a temp).
  int ra = inPlaceReg(cond->left);
  bool raTmp = false;
  if (ra < 0) {
    ra = tmpRegister();
    raTmp = true;
    compileExpr(cond->left, ra);
  }
  int jumpIdx;
  if (isIntLiteral(cond->right)) {
    emitImm(Opcode::CMPI, ra, cond->right->literal.asInt());
    // Flag jump (JE/JNE/JG/JGE/JL/JLE): target in `a`, patched by caller.
    emit(cmpJumpIfFalse(rel), 0, 0, 0);
    jumpIdx = static_cast<int>(m_emitter.size()) - 1;
  } else {
    int rb = inPlaceReg(cond->right);
    bool rbTmp = false;
    if (rb < 0) {
      rb = tmpRegister();
      rbTmp = true;
      compileExpr(cond->right, rb);
    }
    jumpIdx = emitCmpJumpFalse(rel, ra, rb);
    if (rbTmp)
      freeRegister(rb);
  }
  if (raTmp)
    freeRegister(ra);
  return jumpIdx;
}

CompiledProgram QLangCompiler::compile() {
  // Compile the top-level program into the main (scratch) function context.
  if (m_scope) {
    // Phase 2: the main program's non-global register space (locals, temps)
    // starts ABOVE the context's global table — the table's slots are the
    // top-level names' registers (see declareVar), so nothing else may
    // claim them.
    int base = static_cast<int>(m_scope->globalCount());
    if (m_nextReg < base)
      m_nextReg = base;
    if (m_nextNamedReg < base)
      m_nextNamedReg = base;
  }
  for (auto &s : m_program)
    compileStmt(s, nullptr);
  // Implicit `return nil` at the end of the main program.
  emit(Opcode::RETURN, 0);

  // Wrap the main stream into functions[0].
  Function mainFn;
  mainFn.name = "main";
  mainFn.paramCount = 0;
  // Register 0 is always live (the implicit final `return regs[0]`).
  mainFn.registerCount = std::max(m_nextReg, 1);
  m_emitter.finish(mainFn);

  CompiledProgram out;
  out.functions.push_back(std::move(mainFn));
  for (auto &fn : m_functions)
    out.functions.push_back(std::move(fn));

  Bytecode outCode;
  outCode.setCode(out.functions[0].code);
  outCode.setConstants(out.functions[0].constants);
  // Copy (not move) so out.functions retains its instruction streams for the VM.
  for (auto &fn : out.functions)
    outCode.addFunction(fn.name, fn.paramCount, fn.registerCount,
                        fn.code, fn.instrOffsets, fn.constants);
  out.code = std::move(outCode);
  buildRegisterReport(); // before m_regs moves out (names are read from it)
  out.vars = std::move(m_regs);
  out.constVars = std::move(m_constVars);
  // Register 0 is always live (see mainFn.registerCount above).
  out.registerCount = std::max(m_nextReg, 1);
  return out;
}

// Compile a function body (def body or arrow-closure body) into its own
// instruction stream and constant pool, append it as a new function, and return
// its function index. The enclosing function's compilation state is saved and
// restored so nested definitions work.
// Collect all variable names referenced in an expression.
static void collectLocalVars(const StmtPtr &s, std::unordered_set<std::string> &locals) {
  if (!s) return;
  if (s->kind == StmtKind::Declaration || s->kind == StmtKind::Const) {
    locals.insert(s->name);
  } else if (s->kind == StmtKind::For) {
    locals.insert(s->loopVar);
    for (auto &b : s->body) collectLocalVars(b, locals);
  } else if (s->kind == StmtKind::ForIn) {
    locals.insert(s->loopVar);
    for (auto &b : s->body) collectLocalVars(b, locals);
  } else if (s->kind == StmtKind::If) {
    for (auto &b : s->ifBody) collectLocalVars(b, locals);
    for (auto &b : s->elseBody) collectLocalVars(b, locals);
  } else if (s->kind == StmtKind::While) {
    for (auto &b : s->whileBody) collectLocalVars(b, locals);
  } else if (s->kind == StmtKind::Def) {
    // A nested `def` name is a local of THIS function (declared via
    // declareVar in the Def handler when it has captures). Without this it
    // is auto-captured as a "free variable", shifting every capture index
    // and turning references to the def into REFLOADs of the wrong slot.
    locals.insert(s->funcName);
    for (auto &b : s->body) collectLocalVars(b, locals);
  } else if (s->kind == StmtKind::Arrow) {
    // `local f = (p)[c] { ... }`: the lambda's name is a local of THIS
    // function. Without this it is auto-captured as a "free variable",
    // shifting every capture index (incl. `global` captures) and turning
    // references to `f` into REFLOADs of the wrong capture slot.
    locals.insert(s->name);
  }
}

static void collectStmtIds(const StmtPtr &s, std::unordered_set<std::string> &ids,
                           const std::unordered_set<std::string> *skip);
static void collectIds(const ExprPtr &e, std::unordered_set<std::string> &ids,
                       const std::unordered_set<std::string> *skip = nullptr) {
  if (!e) return;
  if (e->kind == ExprKind::Variable) {
    if (!skip || !skip->count(e->name))
      ids.insert(e->name);
    return;
  }
  if (e->kind == ExprKind::Binary) {
    collectIds(e->left, ids, skip);
    collectIds(e->right, ids, skip);
  } else if (e->kind == ExprKind::Unary) {
    collectIds(e->operand, ids, skip);
  } else if (e->kind == ExprKind::Call) {
    // A simple (non-method) call references the callee name as a value: a
    // captured def-fn called from a nested scope must be a free variable of
    // this function (S2). Method calls (obj.m()) carry a method name, not a
    // free variable, so they are excluded.
    if (!e->calleeMember)
      ids.insert(e->callee);
    for (auto &a : e->args) collectIds(a, ids, skip);
  } else if (e->kind == ExprKind::Index) {
    collectIds(e->indexBase, ids, skip);
    if (e->indexExpr) collectIds(e->indexExpr, ids, skip);
  } else if (e->kind == ExprKind::Member) {
    collectIds(e->memberBase, ids, skip);
  } else if (e->kind == ExprKind::Array) {
    for (auto &el : e->elements) collectIds(el, ids, skip);
  } else if (e->kind == ExprKind::Table) {
    for (auto &item : e->items) {
      collectIds(item.first, ids, skip);
      collectIds(item.second, ids, skip);
    }
  } else if (e->kind == ExprKind::Closure) {
    for (auto &st : e->body) collectStmtIds(st, ids, skip);
  } else if (e->kind == ExprKind::Ternary) {
    collectIds(e->cond, ids, skip);
    collectIds(e->trueExpr, ids, skip);
    collectIds(e->falseExpr, ids, skip);
    for (auto &st : e->trueBlock) collectStmtIds(st, ids, skip);
    for (auto &st : e->falseBlock) collectStmtIds(st, ids, skip);
  }
}

static void collectStmtIds(const StmtPtr &s, std::unordered_set<std::string> &ids,
                           const std::unordered_set<std::string> *skip = nullptr) {
  if (!s) return;
  if (s->kind == StmtKind::Declaration || s->kind == StmtKind::Const) {
    if (s->value) collectIds(s->value, ids, skip);
  } else if (s->kind == StmtKind::Assignment) {
    if (s->assignValue) collectIds(s->assignValue, ids, skip);
    if (s->target) collectIds(s->target, ids, skip);
  } else if (s->kind == StmtKind::If) {
    collectIds(s->ifCond, ids, skip);
    for (auto &b : s->ifBody) collectStmtIds(b, ids, skip);
    for (auto &b : s->elseBody) collectStmtIds(b, ids, skip);
  } else if (s->kind == StmtKind::For) {
    if (s->forStart) collectIds(s->forStart, ids, skip);
    if (s->forEnd) collectIds(s->forEnd, ids, skip);
    for (auto &b : s->body) collectStmtIds(b, ids, skip);
  } else if (s->kind == StmtKind::ForIn) {
    if (s->forEnd) collectIds(s->forEnd, ids, skip);
    for (auto &b : s->body) collectStmtIds(b, ids, skip);
  } else if (s->kind == StmtKind::While) {
    collectIds(s->cond, ids, skip);
    for (auto &b : s->whileBody) collectStmtIds(b, ids, skip);
  } else if (s->kind == StmtKind::Return) {
    if (s->returnValue) collectIds(s->returnValue, ids, skip);
  } else if (s->kind == StmtKind::Yield) {
    if (s->yieldValue) collectIds(s->yieldValue, ids, skip);
  } else if (s->kind == StmtKind::ExprStmt) {
    if (s->value) collectIds(s->value, ids, skip);
  } else if (s->kind == StmtKind::Def) {
    // Skip params of this nested def (and recursively deeper nested defs).
    std::unordered_set<std::string> nestedParams(s->params.begin(), s->params.end());
    for (auto &b : s->body) collectStmtIds(b, ids, &nestedParams);
  }
}

int QLangCompiler::buildFunction(const std::string &name,
                                 const std::vector<std::string> &params,
                                 std::vector<std::string> captures,
                                 const std::vector<StmtPtr> &body) {
  // Auto-detect free variables: variables referenced in the body that are
  // not parameters and not yet declared in this function's scope.
  std::unordered_set<std::string> allIds;
  for (auto &s : body)
    collectStmtIds(s, allIds);
  // Also collect all locally-declared variables (params + local decls + loop vars)
  // so we don't mistakenly capture them as free variables.
  std::unordered_set<std::string> paramSet(params.begin(), params.end());
  std::unordered_set<std::string> localVars = paramSet;
  for (auto &s : body)
    collectLocalVars(s, localVars);

  // Save the enclosing function's compilation state.
  FnEmitter savedEmitter = std::move(m_emitter);
  auto savedRegs = m_regs;
  // Save caller's register mapping for global keyword use.
  m_savedRegs = m_regs;
  auto savedConstVars = m_constVars;
  auto savedEnums = m_enums;
  auto savedVarTypes = m_varTypes;
  auto savedUserFuncs = m_userFuncs;
  auto savedClosureVars = m_closureVars;
  auto savedCaptureRegs = m_captureRegs;
  auto savedGlobalVars = m_globalVars;
  auto savedCapturedVars = m_capturedVars;
  auto savedGeneratorVars = m_generatorVars;
  int savedNextReg = m_nextReg;
  auto savedFreeRegs = std::move(m_freeRegs);
  m_freeRegs.clear(); // fresh register space: no outer-frame temps in here
  auto savedLiveTmps = std::move(m_liveTmps);
  m_liveTmps.clear();
  auto savedProtectedTmps = std::move(m_protectedTmps);
  m_protectedTmps.clear();
  auto savedProtectStack = std::move(m_protectStack);
  m_protectStack.clear();
  auto savedSlotRanges = std::move(m_slotRanges);
  m_slotRanges.clear();
  auto savedQuarantined = std::move(m_quarantined);
  m_quarantined.clear();
  auto savedDeferred = std::move(m_deferred);
  m_deferred.clear();
  auto savedNamedRegs = std::move(m_namedRegs);
  m_namedRegs.clear();
  auto savedFreeNamedRegs = std::move(m_freeNamedRegs);
  m_freeNamedRegs.clear();
  int savedNamedReg = m_nextNamedReg;
  bool savedInFunction = m_inFunction;

  // Fresh function context: params occupy registers 0..paramCount-1 and must
  // reserve those registers so temporaries never collide with a parameter.
  trace(TRACE_FULL, "[DBG-BF] Resetting emitter (buildFunction for '%s') this=%p\n", name.c_str(), (void*)this);
  m_emitter = FnEmitter();
  m_regs.clear();
  // Keep m_constVars and m_userFuncs populated so global consts and
  // user-defined functions remain visible inside function bodies.
  // m_varTypes is cleared to avoid stale type info from the outer scope.
  m_varTypes.clear();
  m_closureVars.clear();
  m_captureRegs.clear();
  m_generatorVars.clear();
  m_labels.clear();
  m_nextReg = 0;
  m_nextNamedReg = static_cast<int>(params.size());
  // Register 0 is the frame's return register: a parameterless function's
  // first local must not claim it, or a capture seeded from reg 0 would read
  // the return value (S1: closure outlives its creator frame).
  if (m_nextNamedReg < 1)
    m_nextNamedReg = 1;
  m_inFunction = true;
  for (size_t i = 0; i < params.size(); i++) {
    int r = static_cast<int>(i);
    m_regs[params[i]] = r;
    m_namedRegs.insert(r);
    if (r >= m_nextReg)
      m_nextReg = r + 1;
  }
  // Register 0 is always live (the frame's return register, read by the
  // implicit final `return regs[0]`): even a parameterless function must
  // not hand it out as a temp, or the frame would be sized too small.
  if (m_nextReg < 1)
    m_nextReg = 1;

  // Now detect free variables in the fresh context.
  std::vector<std::string> updatedCaptures = captures;
  for (auto &id : allIds) {
    if (paramSet.count(id)) continue;
    if (localVars.count(id)) continue; // locally declared (loop var, local, etc.)
    if (m_regs.count(id)) continue; // already declared in this function
    if (m_userFuncs.count(id)) {
      // A def function with captures is materialized as a closure value at
      // def time; a reference from a nested scope must capture the closure
      // itself so the call goes through its def-time cells. Capture-free defs
      // are still called statically (CALLFN).
      int fi = m_userFuncs[id];
      // fi is the final program index (main is prepended at 0); m_functions
      // holds user functions only, so it sits at fi-1.
      if (m_functions[static_cast<size_t>(fi) - 1].captures.empty())
        continue;
      if (std::find(updatedCaptures.begin(), updatedCaptures.end(), id) ==
          updatedCaptures.end())
        updatedCaptures.push_back(id);
      continue;
    }
    if (builtinId(id) >= 0) continue; // builtin
    if (m_nativeFuncIds.count(id)) continue; // native (C++) function
    // Global consts are visible everywhere — capture them like free variables.
    if (m_constVars.count(id)) {
      if (std::find(updatedCaptures.begin(), updatedCaptures.end(), id) == updatedCaptures.end())
        updatedCaptures.push_back(id);
      continue;
    }
    // Enum names resolve to hidden table variables: capture the hidden
    // variable (live binding to the declaring scope), not the enum name.
    if (m_enums.count(id)) {
      const std::string hvar = "__enum_" + id;
      if (std::find(updatedCaptures.begin(), updatedCaptures.end(), hvar) ==
          updatedCaptures.end())
        updatedCaptures.push_back(hvar);
      continue;
    }
    // Class names (e.g. C::staticMember bases) are compile-time, not values.
    if (m_classes.count(id)) continue;
    // Skip top-level globals: they are invisible in function scope unless
    // explicitly imported via `global`.
    if (m_globalVars.count(id)) continue;
    // F3: implicit capture of an enclosing function's local is removed. The
    // spec mandates explicit capture lists (arrow closures); a reference to a
    // name that is not a parameter, local, global const/func, enum, or an
    // explicitly captured variable is a compile error (raised by regOf in
    // compileExpr).
  }

  // Register the explicit capture list (name -> capture index). Phase 2:
  // REFLOAD/REFSTORE index frame.captures by capture position, so this map
  // must carry the capture index, not the enclosing frame's register (the
  // pre-cell model). Global captures bind to table slots at MAKEFUNC time
  // (bindCaptures), not to frame registers.
  for (size_t i = 0; i < updatedCaptures.size(); i++) {
    m_captureRegs[updatedCaptures[i]] = static_cast<int>(i);
  }

  // Also populate m_regs for captured globals so assignments work correctly.
  for (auto &name : updatedCaptures) {
    if (m_capturedVars.count(name) && regOf(name) < 0)
      declareVar(name);
  }

  // Compile the body.
  for (auto &s : body)
    compileStmt(s, &updatedCaptures);

  // Sync captures (modified by global statements) back to updatedCaptures.
  // This ensures fn.captures is populated correctly.

  // Implicit `return nil`. The RETURN opcode returns reg 0, but a prior
  // statement may have left a value there; force reg 0 to nil so a function
  // with no return statement returns nil (spec: return value defaults to nil).
  emit(Opcode::MOVC, 0, emitConst(QLangValue())); // reg 0 = nil
  emit(Opcode::RETURN, 0);

  Function fn;
  fn.name = name;
  fn.paramCount = static_cast<int>(params.size());
  fn.registerCount = m_nextReg;
  // Populate the capture spec so the VM can bind enclosing-frame registers
  // when this function is called.
  fn.captureNames = updatedCaptures;
  fn.captures.resize(updatedCaptures.size());
  for (size_t i = 0; i < updatedCaptures.size(); i++) {
    const std::string &var = updatedCaptures[i];
    // The capture's register in the CREATING frame: a local capture sits in
    // the enclosing function's register map (savedRegs); a global capture is
    // tracked in m_globalRegMap. bindCaptures seeds the cell from this
    // register, so it must be the capture's real home register, not 0 (S1:
    // a closure outliving its creator frame must read the captured value,
    // not register 0).
    int callerReg = 0;
    if (savedRegs.count(var))
      callerReg = savedRegs[var];
    else if (m_globalRegMap.count(var))
      callerReg = m_globalRegMap[var];
    fn.captures[i].reg = callerReg; // bound at call time by the VM
  }

  int funcIndex = static_cast<int>(m_functions.size()) + 1; // +1 for prepended mainFn
  m_emitter.finish(fn);
  // Detect generator: any `yield` statement (at any nesting depth) makes this
  // a generator function.
  for (auto &s : body) {
    if (s && containsYield(s)) {
      fn.isGenerator = true;
      break;
    }
  }
  if (fn.isGenerator) {
    m_generatorFuncIndices.insert(funcIndex);
  }
  m_functions.push_back(std::move(fn));

  // Restore the enclosing function's compilation state.
  m_emitter = std::move(savedEmitter);
  m_regs = std::move(savedRegs);
  m_constVars = std::move(savedConstVars);
  m_enums = std::move(savedEnums);
  m_varTypes = std::move(savedVarTypes);
  m_userFuncs = std::move(savedUserFuncs);
  m_closureVars = std::move(savedClosureVars);
  m_captureRegs = std::move(savedCaptureRegs);
  m_savedRegs.clear();
  m_globalRegMap.clear();
  m_globalVars = std::move(savedGlobalVars);
  m_capturedVars = std::move(savedCapturedVars);
  m_generatorVars = std::move(savedGeneratorVars);
  m_nextReg = savedNextReg;
  m_nextNamedReg = savedNamedReg;
  m_namedRegs = std::move(savedNamedRegs);
  m_freeNamedRegs = std::move(savedFreeNamedRegs);
  m_freeRegs = std::move(savedFreeRegs);
  m_liveTmps = std::move(savedLiveTmps);
  m_protectedTmps = std::move(savedProtectedTmps);
  m_slotRanges = std::move(savedSlotRanges);
  m_quarantined = std::move(savedQuarantined);
  m_deferred = std::move(savedDeferred);
  m_protectStack = std::move(savedProtectStack);
  m_inFunction = savedInFunction;

  // Emit generator closure constant into the MAIN bytecode (not the temporary
  // function bytecode that was just moved into fn). MAKEGEN in compileExpr
  // references constants in the current emitter.
  if (fn.isGenerator) {
    auto closure = std::make_shared<Closure>();
    closure->funcIndex = funcIndex;
    closure->captures = fn.captures;
    int genConstIdx = emitConst(QLangValue::Function(closure));
    m_generatorConstIndices[funcIndex] = genConstIdx;
  }

  return funcIndex;
}

// Register holding the current method's `__statics__` table. In a static
// method the hidden anchor parameter (register 0) IS the table. In an
// instance method register 0 is `self`, so emit a TABGET of `self.__statics__`.
int QLangCompiler::staticsTableReg() {
  if (m_inStaticMethod)
    return 0;
  int r = tmpRegister();
  int keyR = tmpRegister();
  emit(Opcode::MOVC, keyR, emitConst(QLangValue::String("__statics__")));
  emit(Opcode::TABGET, r, 0, keyR);
  return r;
}

void QLangCompiler::compileStmt(const StmtPtr &s,
                                std::vector<std::string> *captures) {
  if (!s)
    return;
  // Track the statement's source position for emit() stamping (SPEC #
  // error reporting).
  m_curLine = s->line;
  m_curCol = s->col;
  // Protect temps live before this statement (e.g. loop-header temps): only
  // temps allocated within this statement are freed at its end. The entry
  // snapshot goes on the protect stack; endOfStatement pops it and frees
  // against it (nested statements restore their own, larger sets). The
  // snapshot is taken AFTER adding the currently-live temps: temps allocated
  // outside any statement (a ForIn range setup) are live but unrecorded, and
  // a snapshot taken before the add would let the first body statement free
  // them, aliasing their values.
  for (int r : m_liveTmps)
    m_protectedTmps.insert(r);
  m_protectStack.push_back(m_protectedTmps);
  switch (s->kind) {
  case StmtKind::Declaration: compileDeclaration(s); break;
  case StmtKind::Const: compileConst(s); break;
  case StmtKind::Assignment: compileAssignment(s); break;
  case StmtKind::Delete: compileDelete(s); break;
  case StmtKind::For: compileFor(s); break;
  case StmtKind::ForIn: compileForIn(s); break;
  case StmtKind::While: compileWhile(s); break;
  case StmtKind::If: compileIf(s); break;
  case StmtKind::GotoIf: compileGotoIf(s); break;
  case StmtKind::ExprStmt: compileExprStmt(s); break;
  case StmtKind::Def: compileDef(s); break;
  case StmtKind::Arrow: compileArrow(s); break;
  case StmtKind::Return: compileReturn(s); break;
  case StmtKind::Yield: compileYield(s); break;
  case StmtKind::Emit: compileEmit(s); break;
  case StmtKind::Continue: compileContinue(s); break;
  case StmtKind::Break: compileBreak(s); break;
  case StmtKind::Label: compileLabel(s); break;
  case StmtKind::Goto: compileGoto(s); break;
  case StmtKind::Import: compileImport(s); break;
  case StmtKind::Class: compileClass(s); break;
  case StmtKind::Enum: compileEnum(s); break;
  case StmtKind::Global: compileGlobal(s, captures); break;
  case StmtKind::Switch: compileSwitch(s); break;
  }
  // Statement boundary: free temps allocated within this statement.
  endOfStatement();
}

void QLangCompiler::compileDeclaration(const StmtPtr &s) {
  int r = declareVar(s->name, true);
  if (isIntLiteral(s->value)) {
    // `local a = 0` -> one MOVI into the target register (SPEC
    // §bytecodes: MOVI) instead of MOV tmp + MOV.
    emitImm(Opcode::MOVI, r, s->value->literal.asInt());
  } else if (!s->value) {
    // `local z` -> implicit int, default 0 (SPEC §declarations).
    // A typed valueless decl (`local b:byte`) gets zero of that type via
    // the CONV emitted below.
    emitImm(Opcode::MOVI, r, 0);
  } else {
    // Compile the RHS directly into the declared register (SPEC
    // §bytecodes: encoding guidelines — always copy directly to the
    // final register): no temp, no MOV, no dead nil-clear.
    compileExpr(s->value, r);
  }
  updateVarType(s->name, s->value);
  // Track generator variables: if RHS calls a generator function, mark LHS.
  if (s->value && s->value->kind == ExprKind::Call) {
    auto *e = s->value.get();
    trace(TRACE_FULL, "[DBG-GENVAR] Tracking gen var '%s' from call to '%s': userFuncs=%d genVars=%d\n",
          s->name.c_str(), e->callee.c_str(),
          (int)m_userFuncs.count(e->callee), (int)m_generatorVars.count(e->callee));
    if (m_userFuncs.count(e->callee) && m_generatorVars.count(e->callee))
      m_generatorVars.insert(s->name);
    trace(TRACE_FULL, "[DBG-GENVAR] m_generatorVars after insert: {");
    for (auto &g : m_generatorVars) trace(TRACE_FULL, " %s", g.c_str());
    trace(TRACE_FULL, " }\n");
  }
  // Enforce a typed declaration (e.g. local b:byte = 32).
  if (s->declaredType)
    emit(Opcode::CONV, r, r, static_cast<int>(*s->declaredType));
  // Track top-level variable declarations as globals.
  if (!m_inFunction)
    m_globalVars.insert(s->name);
}

void QLangCompiler::compileConst(const StmtPtr &s) {
  int r = declareVar(s->name, true);
  m_constVars.insert(s->name);
  // Compile the RHS directly into the declared register (SPEC
  // §bytecodes: encoding guidelines — always copy directly to the
  // final register): no temp, no MOV, no dead nil-clear.
  if (s->value)
    compileExpr(s->value, r);
  else
    emit(Opcode::MOVC, r, emitConst(QLangValue()));
  updateVarType(s->name, s->value);
  // Enforce a typed declaration (e.g. local b:byte = 32).
  if (s->declaredType)
    emit(Opcode::CONV, r, r, static_cast<int>(*s->declaredType));
  // Global consts are visible everywhere.
  if (!m_inFunction)
    m_globalVars.insert(s->name);
}

void QLangCompiler::compileAssignment(const StmtPtr &s) {
  // Bare static member name in a method body of a class with statics:
  // write through the shared `__statics__` table.
  if (!s->target && !m_classStaticNames.empty() &&
      m_classStaticNames.count(s->name)) {
    int staticsR = staticsTableReg();
    int keyR = tmpRegister();
    emit(Opcode::MOVC, keyR, emitConst(QLangValue::String(s->name)));
    int t = tmpRegister();
    compileExpr(s->assignValue, t);
    const std::string &op = s->assignOp.text;
    if (op == "=") {
      emit(Opcode::TABPUT, staticsR, keyR, t);
    } else {
      int cur = tmpRegister();
      emit(Opcode::TABGET, cur, staticsR, keyR);
      Opcode opCode = (op == "++") ? Opcode::INC
                                   : (op == "--") ? Opcode::DEC
                                                  : binOp(op.substr(0, op.size() - 1));
      emit(opCode, cur, cur, t);
      emit(Opcode::TABPUT, staticsR, keyR, cur);
    }
    return;
  }
  // Direct assignment to a captured variable (write-back / reference).
  auto cit = m_captureRegs.find(s->name);
  if (cit != m_captureRegs.end() && !s->target) {
    const std::string &op = s->assignOp.text;
    if (op == "=") {
      int t = tmpRegister();
      compileExpr(s->assignValue, t);
      emit(Opcode::REFSTORE, t, cit->second, 0);
    } else {
      int cur = tmpRegister();
      emit(Opcode::REFLOAD, cur, cit->second, 0);
      Opcode opCode = (op == "++") ? Opcode::INC
                                   : (op == "--") ? Opcode::DEC
                                                  : binOp(op.substr(0, op.size() - 1));
      if ((opCode == Opcode::ADD || opCode == Opcode::SUB) &&
          isIntLiteral(s->assignValue)) {
        // `cap += 12` / `cap -= 12` -> ADDI/SUBI in-place (SPEC §bytecodes:
        // new OpCode addi/subi).
        emitImm(opCode == Opcode::ADD ? Opcode::ADDI : Opcode::SUBI, cur,
                s->assignValue->literal.asInt());
      } else {
        int t = tmpRegister();
        compileExpr(s->assignValue, t);
        emit(opCode, cur, cur, t);
      }
      emit(Opcode::REFSTORE, cur, cit->second, 0);
    }
    return;
  }
  // Index/member assignment target (e.g. a[i] = v, t.name = v).
  if (s->target) {
    compileAssignTarget(s->target, s->assignOp, s->assignValue);
    return;
  }
  int r = regOf(s->name);
  if (r < 0) {
    // Bare assignment auto-declares an implicit local, unless it's a captured
    // global (which should have been declared via `global` first).
    if (!m_capturedVars.count(s->name))
      r = declareVar(s->name); // bare assignment auto-declares an implicit local
  }
  // Track top-level bare assignments as globals.
  if (!m_inFunction)
    m_globalVars.insert(s->name);
  guardNotConst(s->name, "assignment");
  const std::string &op = s->assignOp.text;
  // Array append: `name += value` where name is an array.
  if (op == "+=" && m_varTypes[s->name] == QLangValue::Type::Array) {
    int t = tmpRegister();
    compileExpr(s->assignValue, t);
    emit(isArrayExpr(s->assignValue) ? Opcode::ARRAPPV : Opcode::ARRAPP,
         r, t, 0);
    releaseTemp(t); // dead after the append
    return;
  }
  if (op == "=") {
    if (isIntLiteral(s->assignValue)) {
      // `a = 0` -> one MOVI into the target register (SPEC §bytecodes: MOVI).
      emitImm(Opcode::MOVI, r, s->assignValue->literal.asInt());
    } else if (foldSelfArith(s->assignValue, s->name, r)) {
      // `a = a + 1` -> inc a / `a = a - 4` -> subi a, 4 (in place).
    } else {
      // Compile the RHS directly into the target register (SPEC
      // §bytecodes: encoding guidelines — always copy directly to the
      // final register): no temp, no MOV, no dead nil-clear.
      compileExpr(s->assignValue, r);
    }
    updateVarType(s->name, s->assignValue);
  } else {
    Opcode opCode = (op == "++") ? Opcode::INC
                                 : (op == "--") ? Opcode::DEC
                                                : binOp(op.substr(0, op.size() - 1));
    if ((opCode == Opcode::ADD || opCode == Opcode::SUB) &&
        isIntLiteral(s->assignValue)) {
      // `a += 12` / `a -= 12` -> ADDI/SUBI in-place (SPEC §bytecodes:
      // new OpCode addi/subi).
      emitImm(opCode == Opcode::ADD ? Opcode::ADDI : Opcode::SUBI, r,
              s->assignValue->literal.asInt());
    } else {
      int t = tmpRegister();
      compileExpr(s->assignValue, t);
      emit(opCode, r, r, t);
      releaseTemp(t); // dead after the compound op
    }
  }
}

void QLangCompiler::compileDelete(const StmtPtr &s) {
  int r = regOf(s->delName);
  if (r < 0) {
    if (m_externalVars.count(s->delName))
      throw QLangError(QLangErrorKind::Parse, 0, 0,
                       "cannot delete external variable '" + s->delName + "'");
    throw QLangError(QLangErrorKind::Parse, 0, 0,
                     "delete of undeclared variable '" + s->delName + "'");
  }
  emit(Opcode::DELETEx, r);
}

void QLangCompiler::compileFor(const StmtPtr &s) {
  int x = declareVar(s->loopVar);
  m_blockScopes.emplace_back(); // body is a block scope (SPEC # block scope)
  int startReg = tmpRegister();
  compileExpr(s->forStart, startReg);
  emit(Opcode::MOV, x, startReg);
  int endReg = tmpRegister();
  compileExpr(s->forEnd, endReg);

  bool hasStep = s->stepExpr != nullptr;
  auto stepVal = hasStep ? foldConst(s->stepExpr) : std::nullopt;
  auto startFold = foldConst(s->forStart);
  auto endFold = foldConst(s->forEnd);
  bool boundsFoldable = startFold && endFold;
  int64_t correctDir = boundsFoldable
                           ? ((endFold.value() > startFold.value()) ? 1 : -1)
                           : 0;

  if (hasStep && stepVal && boundsFoldable) {
    // Constant step + constant bounds: fully resolved at compile time.
    int64_t sv = *stepVal;
    if (sv == 0) {
      // step 0 -> infinite loop, x frozen at start.
      m_loops.push_back({0, 0});
      int bodyStart = static_cast<int>(m_emitter.size());
      for (auto &body : s->body)
        compileStmt(body);
      int bodyEnd = static_cast<int>(m_emitter.size());
      int incLabel = bodyEnd;
      emit(Opcode::JMP, bodyStart);
      int exitLabel = static_cast<int>(m_emitter.size());
      for (int j = bodyStart; j < bodyEnd; j++) {
        auto &ci = m_emitter.raw[j];
        if (ci.op == Opcode::CONTINUE) m_emitter.patchJump(j, incLabel);
        else if (ci.op == Opcode::BREAK) m_emitter.patchJump(j, exitLabel);
      }
      m_loops.pop_back();
    } else if ((sv > 0) != (correctDir > 0)) {
      // User step points the wrong way: run the body exactly once.
      m_loops.push_back({0, 0});
      int bodyStart = static_cast<int>(m_emitter.size());
      for (auto &body : s->body)
        compileStmt(body);
      int bodyEnd = static_cast<int>(m_emitter.size());
      int exitLabel = static_cast<int>(m_emitter.size());
      for (int j = bodyStart; j < bodyEnd; j++) {
        auto &ci = m_emitter.raw[j];
        if (ci.op == Opcode::CONTINUE) m_emitter.patchJump(j, exitLabel);
        else if (ci.op == Opcode::BREAK) m_emitter.patchJump(j, exitLabel);
      }
      m_loops.pop_back();
    } else {
      // Guard loop in the natural direction with the user's step.
      m_loops.push_back({0, 0});
      int stepReg = tmpRegister();
      compileExpr(s->stepExpr, stepReg);
      int guardLabel = static_cast<int>(m_emitter.size());
      // QLang ranges are always inclusive: forward LE (x <= end),
      // backward GE (x >= end). Int-vs-Int, so fused to cmp+j* (SPEC
      // §bytecodes: alternate conditional jump).
      Opcode fwdCmp = (correctDir > 0) ? Opcode::LE : Opcode::GE;
      int jumpFalseIdx = emitCmpJumpFalse(fwdCmp, x, endReg);
      int bodyStart = static_cast<int>(m_emitter.size());
      for (auto &body : s->body)
        compileStmt(body);
      int bodyEnd = static_cast<int>(m_emitter.size());
      int incLabel = bodyEnd;
      emit(Opcode::ADD, x, x, stepReg);
      emit(Opcode::JMP, guardLabel);
      int exitLabel = static_cast<int>(m_emitter.size());
      for (int j = bodyStart; j < bodyEnd; j++) {
        auto &ci = m_emitter.raw[j];
        if (ci.op == Opcode::CONTINUE) m_emitter.patchJump(j, incLabel);
        else if (ci.op == Opcode::BREAK) m_emitter.patchJump(j, exitLabel);
      }
      m_emitter.patchJump(jumpFalseIdx, exitLabel);
      m_loops.pop_back();
    }
  } else {
    // Runtime path: variable step and/or variable bounds. The loop's natural
    // direction is sign(end - start), computed at runtime. A zero step is
    // normalized to that direction's unit step (so a `step 0` loop advances,
    // matching the spec's for-step semantics). The user step's sign then
    // selects one of two bodies:
    //   1. sign(step) == sign(dir)  -> bounded guard loop
    //   2. otherwise (wrong direction) -> run the body exactly once
    m_loops.push_back({0, 0});

    int stepReg = tmpRegister();
    if (hasStep) {
      compileExpr(s->stepExpr, stepReg);
    } else {
      emitImm(Opcode::MOVI, stepReg, 1);
    }
    int zeroReg = tmpRegister();
    emitImm(Opcode::MOVI, zeroReg, 0);

    // direction = (end > start) ? 1 : -1
    int subReg = tmpRegister();
    emit(Opcode::SUB, subReg, endReg, startReg);
    int posReg = tmpRegister();
    emit(Opcode::GT, posReg, subReg, zeroReg);
    int dirReg = tmpRegister();
    emitImm(Opcode::MOVI, dirReg, -1);
    int dPosLabel = static_cast<int>(m_emitter.size());
    emit(Opcode::JMPIFTRUE, posReg, 0);
    int dDoneLabel = static_cast<int>(m_emitter.size());
    emit(Opcode::JMP, 0);
    m_emitter.patchJump(dPosLabel, dPosLabel + 2);
    emitImm(Opcode::MOVI, dirReg, 1);
    m_emitter.patchJump(dDoneLabel, dPosLabel + 3);

    // No explicit step: advance in the loop's natural direction (unit step).
    // Without this a backward no-step loop would get step +1 vs dir -1 and
    // take the run-once path.
    if (!hasStep)
      emit(Opcode::MOV, stepReg, dirReg);

    // step == 0 -> natural-direction unit step (so a `step 0` loop advances).
    int zeroCmp = tmpRegister();
    emit(Opcode::EQ, zeroCmp, stepReg, zeroReg);
    int zeroStepLabel = static_cast<int>(m_emitter.size());
    emit(Opcode::JMPIFFALSE, zeroCmp, 0);      // step != 0 -> skip normalization
    emit(Opcode::MOV, stepReg, dirReg);        // step == 0 -> step = dir (+1 / -1)
    m_emitter.patchJump(zeroStepLabel, zeroStepLabel + 2);

    // runtime conditions
    int posStep = tmpRegister();
    emit(Opcode::GT, posStep, stepReg, zeroReg);
    int posDir = tmpRegister();
    emit(Opcode::GT, posDir, dirReg, zeroReg);
    int sameSign = tmpRegister();
    emit(Opcode::EQ, sameSign, posStep, posDir);

    // Decision: sameSign -> guard loop; else fall through to run-once.
    int guardLabel = static_cast<int>(m_emitter.size());
    emit(Opcode::JMPIFTRUE, sameSign, 0);

    // Run-once body (wrong direction): execute exactly once, then exit.
    emit(Opcode::MOV, x, startReg);
    int runOnceBodyStart = static_cast<int>(m_emitter.size());
    for (auto &body : s->body)
      compileStmt(body);
    int runOnceBodyEnd = static_cast<int>(m_emitter.size());
    int runOnceJmpIdx = static_cast<int>(m_emitter.size());
    emit(Opcode::JMP, 0);                 // patched to loopExit below
    for (int j = runOnceBodyStart; j < runOnceBodyEnd; j++) {
      auto &ci = m_emitter.raw[j];
      if (ci.op == Opcode::CONTINUE) m_emitter.patchJump(j, 0);   // patched below
      else if (ci.op == Opcode::BREAK) m_emitter.patchJump(j, 0); // patched below
    }

    // Guard loop (natural direction, bounded by a guard check). Direction is
    // fixed for the whole loop, so the direction branch is a one-time decision
    // that is cheaply re-evaluated each iteration.
    //   forward (dir > 0):  `to` bound inclusive -> LE guard (x <= end continues)
    //   backward (dir <= 0): `to` bound inclusive -> GE guard (x >= end continues)
    int guardLoopStart = static_cast<int>(m_emitter.size());
    emit(Opcode::MOV, x, startReg);
    int guardOnceLabel = static_cast<int>(m_emitter.size());
    emit(Opcode::JMPIFTRUE, posDir, 0);          // forward -> LT guard path
    // Guards are Int-vs-Int: fused to cmp+j* (SPEC §bytecodes: alternate
    // conditional jump). guardGeIdx/guardFwdIdx point at the CMP; the j* is
    // at +1 (patched below).
    int guardGeIdx = static_cast<int>(m_emitter.size());
    emitCmpJumpFalse(Opcode::GE, x, endReg);     // backward guard (x >= end)
    int guardSkipIdx = static_cast<int>(m_emitter.size());
    emit(Opcode::JMP, 0);                        // backward -> loop body
    int guardFwdIdx = static_cast<int>(m_emitter.size());
    emitCmpJumpFalse(Opcode::LE, x, endReg);     // forward guard (x <= end)
    int guardLoopEntry = static_cast<int>(m_emitter.size());
    for (auto &body : s->body)
      compileStmt(body);
    int guardBodyEnd = static_cast<int>(m_emitter.size());
    emit(Opcode::ADD, x, x, stepReg);
    emit(Opcode::JMP, guardOnceLabel);           // loop back to direction branch
    int loopExit = static_cast<int>(m_emitter.size());

    // Patch the guard loop's branches.
    m_emitter.patchJump(guardOnceLabel, guardFwdIdx);   // forward -> guard
    m_emitter.patchJump(guardGeIdx + 1, loopExit);     // backward exit
    m_emitter.patchJump(guardFwdIdx + 1, loopExit);    // forward exit
    m_emitter.patchJump(guardSkipIdx, guardLoopEntry); // backward -> body (JMP reads a)
    for (int j = guardLoopEntry; j < guardBodyEnd; j++) {
      auto &ci = m_emitter.raw[j];
      if (ci.op == Opcode::CONTINUE) m_emitter.patchJump(j, guardBodyEnd);   // -> ADD (still increment)
      else if (ci.op == Opcode::BREAK) m_emitter.patchJump(j, loopExit);
    }
    // Patch the run-once body to exit (continue/break both leave the loop).
    m_emitter.patchJump(runOnceJmpIdx, loopExit);
    for (int j = runOnceBodyStart; j < runOnceBodyEnd; j++) {
      auto &ci = m_emitter.raw[j];
      if (ci.op == Opcode::CONTINUE) m_emitter.patchJump(j, loopExit);
      else if (ci.op == Opcode::BREAK) m_emitter.patchJump(j, loopExit);
    }
    // Patch the decision jump.
    m_emitter.patchJump(guardLabel, guardLoopStart);  // sameSign -> guard loop
    m_loops.pop_back();
  }
  popBlockScope();
}

void QLangCompiler::compileForIn(const StmtPtr &s) {
  // for i in range(a, b) -> counter loop: i = a; while (i <= b) { ...; ++i }
  // No array is materialized (spec §range).
  m_blockScopes.emplace_back(); // body is a block scope (SPEC # block scope)
  ExprPtr ra, rb;
  if (isRangeCall(s->forEnd, ra, rb)) {
    int x = declareVar(s->loopVar);
    int startReg = tmpRegister();
    compileExpr(ra, startReg);
    emit(Opcode::MOV, x, startReg);
    int endReg = tmpRegister();
    compileExpr(rb, endReg);

    bool hasStep = s->stepExpr != nullptr;
    auto stepVal = hasStep ? foldConst(s->stepExpr) : std::nullopt;
    auto startFold = foldConst(ra);
    auto endFold = foldConst(rb);
    bool boundsFoldable = startFold && endFold;
    int64_t correctDir = boundsFoldable
                             ? ((endFold.value() > startFold.value()) ? 1 : -1)
                             : 0;

    if (hasStep && stepVal && boundsFoldable) {
      int64_t sv = *stepVal;
      if (sv == 0) {
        m_loops.push_back({0, 0});
        int bodyStart = static_cast<int>(m_emitter.size());
        for (auto &body : s->body)
          compileStmt(body);
        int bodyEnd = static_cast<int>(m_emitter.size());
        int incLabel = bodyEnd;
        emit(Opcode::JMP, bodyStart);
        int exitLabel = static_cast<int>(m_emitter.size());
        for (int j = bodyStart; j < bodyEnd; j++) {
          auto &ci = m_emitter.raw[j];
          if (ci.op == Opcode::CONTINUE) m_emitter.patchJump(j, incLabel);
          else if (ci.op == Opcode::BREAK) m_emitter.patchJump(j, exitLabel);
        }
        m_loops.pop_back();
      } else if ((sv > 0) != (correctDir > 0)) {
        m_loops.push_back({0, 0});
        int bodyStart = static_cast<int>(m_emitter.size());
        for (auto &body : s->body)
          compileStmt(body);
        int bodyEnd = static_cast<int>(m_emitter.size());
        int exitLabel = static_cast<int>(m_emitter.size());
        for (int j = bodyStart; j < bodyEnd; j++) {
          auto &ci = m_emitter.raw[j];
          if (ci.op == Opcode::CONTINUE) m_emitter.patchJump(j, exitLabel);
          else if (ci.op == Opcode::BREAK) m_emitter.patchJump(j, exitLabel);
        }
        m_loops.pop_back();
      } else {
        m_loops.push_back({0, 0});
        int stepReg = tmpRegister();
        compileExpr(s->stepExpr, stepReg);
        int guardLabel = static_cast<int>(m_emitter.size());
        // Int-vs-Int range guard: fused to cmp+j* (SPEC §bytecodes).
        int jumpFalseIdx =
            emitCmpJumpFalse(correctDir > 0 ? Opcode::LE : Opcode::GE, x,
                             endReg);
        int bodyStart = static_cast<int>(m_emitter.size());
        for (auto &body : s->body)
          compileStmt(body);
        int bodyEnd = static_cast<int>(m_emitter.size());
        int incLabel = bodyEnd;
        emit(Opcode::ADD, x, x, stepReg);
        emit(Opcode::JMP, guardLabel);           // loop back to the guard
        int exitLabel = static_cast<int>(m_emitter.size());
        for (int j = bodyStart; j < bodyEnd; j++) {
          auto &ci = m_emitter.raw[j];
          if (ci.op == Opcode::CONTINUE) m_emitter.patchJump(j, incLabel);
          else if (ci.op == Opcode::BREAK) m_emitter.patchJump(j, exitLabel);
        }
        m_emitter.patchJump(jumpFalseIdx, exitLabel);
        m_loops.pop_back();
      }
    } else {
      m_loops.push_back({0, 0});
      int stepReg = tmpRegister();
      if (hasStep) {
        compileExpr(s->stepExpr, stepReg);
      } else {
        emitImm(Opcode::MOVI, stepReg, 1);
      }
      int zeroReg = tmpRegister();
      emitImm(Opcode::MOVI, zeroReg, 0);
      // Natural direction from the (runtime) bounds.
      int subReg = tmpRegister();
      emit(Opcode::SUB, subReg, endReg, startReg);
      int posReg = tmpRegister();
      emit(Opcode::GT, posReg, subReg, zeroReg);
      int dirReg = tmpRegister();
      emitImm(Opcode::MOVI, dirReg, -1);
      int dPosLabel = static_cast<int>(m_emitter.size());
      emit(Opcode::JMPIFTRUE, posReg, 0);
      int dDoneLabel = static_cast<int>(m_emitter.size());
      emit(Opcode::JMP, 0);
      m_emitter.patchJump(dPosLabel, dPosLabel + 2);
      emitImm(Opcode::MOVI, dirReg, 1);
      m_emitter.patchJump(dDoneLabel, dPosLabel + 3);

      // step == 0 -> infinite loop: body + increment forever, no guard.
      int zeroCmp = tmpRegister();
      emit(Opcode::EQ, zeroCmp, stepReg, zeroReg);
      int zeroJumpIdx = static_cast<int>(m_emitter.size());
      emit(Opcode::JMPIFTRUE, zeroCmp, 0);
      int infExitLabel = static_cast<int>(m_emitter.size());
      int posStep = tmpRegister();
      emit(Opcode::GT, posStep, stepReg, zeroReg);
      int posDir = tmpRegister();
      emit(Opcode::GT, posDir, dirReg, zeroReg);
      int sameSign = tmpRegister();
      emit(Opcode::EQ, sameSign, posStep, posDir);
      int ssJumpIdx = static_cast<int>(m_emitter.size());
      emit(Opcode::JMPIFTRUE, sameSign, 0);
      // Run-once: step and direction disagree -> body executes a single time.
      emit(Opcode::MOV, x, startReg);
      int runOnceBodyStart = static_cast<int>(m_emitter.size());
      for (auto &body : s->body)
        compileStmt(body);
      int runOnceBodyEnd = static_cast<int>(m_emitter.size());
      int runOnceExitJmpIdx = static_cast<int>(m_emitter.size());
      emit(Opcode::JMP, 0); // placeholder, backpatched to guardExit below
      // Infinite-loop (step == 0) body: x advances forever, no guard.
      // Emitted before the guarded loop so the guarded loop's exit
      // (guardExit) falls past it.
      emit(Opcode::MOV, x, startReg);
      int infBodyStart = static_cast<int>(m_emitter.size());
      for (auto &body : s->body)
        compileStmt(body);
      int infBodyEnd = static_cast<int>(m_emitter.size());
      emit(Opcode::ADD, x, x, stepReg);
      emit(Opcode::JMP, infBodyStart);
      for (int j = infBodyStart; j < infBodyEnd; j++) {
        auto &ci = m_emitter.raw[j];
        if (ci.op == Opcode::CONTINUE) m_emitter.patchJump(j, infBodyEnd);
        else if (ci.op == Opcode::BREAK) m_emitter.patchJump(j, infExitLabel);
      }
      // Guarded loop: init x once, then re-check the direction-selected guard
      // every pass (forward: x <= end; backward: x >= end).
      int guardInit = static_cast<int>(m_emitter.size());
      emit(Opcode::MOV, x, startReg);
      int guardLabel = static_cast<int>(m_emitter.size());
      int posDir2 = tmpRegister();
      emit(Opcode::GT, posDir2, dirReg, zeroReg);
      int fwdGuardIdx = static_cast<int>(m_emitter.size());
      emit(Opcode::JMPIFTRUE, posDir2, 0);
      // Int-vs-Int range guards: fused to cmp+j* (SPEC §bytecodes). The j*
      // is the last instruction emitted, so backExitIdx/fwdExitIdx point at
      // it (patched to guardExit below).
      emitCmpJumpFalse(Opcode::GE, x, endReg); // backward guard (x >= end)
      int backExitIdx = static_cast<int>(m_emitter.size()) - 1;
      int skipFwdIdx = static_cast<int>(m_emitter.size());
      emit(Opcode::JMP, 0);
      m_emitter.patchJump(fwdGuardIdx, skipFwdIdx + 1);
      emitCmpJumpFalse(Opcode::LE, x, endReg); // forward guard (x <= end)
      int fwdExitIdx = static_cast<int>(m_emitter.size()) - 1;
      int bodyStart = static_cast<int>(m_emitter.size());
      for (auto &body : s->body)
        compileStmt(body);
      int bodyEnd = static_cast<int>(m_emitter.size());
      emit(Opcode::ADD, x, x, stepReg);
      emit(Opcode::JMP, guardLabel);
      int guardExit = static_cast<int>(m_emitter.size());
      for (int j = bodyStart; j < bodyEnd; j++) {
        auto &ci = m_emitter.raw[j];
        if (ci.op == Opcode::CONTINUE) m_emitter.patchJump(j, bodyEnd); // jump to increment
        else if (ci.op == Opcode::BREAK) m_emitter.patchJump(j, guardExit);
      }
      m_emitter.patchJump(backExitIdx, guardExit);
      m_emitter.patchJump(fwdExitIdx, guardExit);
      m_emitter.patchJump(zeroJumpIdx, infBodyStart);
      m_emitter.patchJump(ssJumpIdx, guardInit);
      m_emitter.patchJump(runOnceExitJmpIdx, guardExit);
      for (int j = runOnceBodyStart; j < runOnceBodyEnd; j++) {
        auto &ci = m_emitter.raw[j];
        if (ci.op == Opcode::CONTINUE) m_emitter.patchJump(j, guardExit);
        else if (ci.op == Opcode::BREAK) m_emitter.patchJump(j, guardExit);
      }
      m_loops.pop_back();
    }
    popBlockScope();
    return;
  }
  // `step` is only meaningful for numeric ranges, not array/table
  // iteration (whose stride is fixed at 1). Reject it here.
  if (s->stepExpr != nullptr) {
    throw QLangError(
        QLangErrorKind::Type, 0, 0,
        "step is not supported over array literals in for-in loops");
  }
  // for i in <collection> ... next i : iterate array elements / table keys.
  int x = declareVar(s->loopVar); // loopVar holds each element/key
  int idx = tmpRegister();        // index counter
  emitImm(Opcode::MOVI, idx, 0);
  int colReg = tmpRegister();
  compileExpr(s->forEnd, colReg); // load the collection once
  int lenReg = tmpRegister();
  emit(Opcode::LEN, lenReg, colReg, 0);

  int testLabel = static_cast<int>(m_emitter.size());
  // idx < len(collection): Int-vs-Int, fused to cmp+j* (SPEC §bytecodes).
  int jumpFalseIdx = emitCmpJumpFalse(Opcode::LT, idx, lenReg);
  // Assign loopVar = collection[idx] at the top of each iteration.
  emit(Opcode::SEQGET, x, colReg, idx);

  m_loops.push_back({0, 0});
  int bodyStart = static_cast<int>(m_emitter.size());
  for (auto &body : s->body)
    compileStmt(body);
  int bodyEnd = static_cast<int>(m_emitter.size());

  int incLabel = bodyEnd;
  emit(Opcode::INC, idx, idx);
  emit(Opcode::JMP, testLabel);
  int exitLabel = static_cast<int>(m_emitter.size());
  m_emitter.patchJump(jumpFalseIdx, exitLabel);
  for (int j = bodyStart; j < bodyEnd; j++) {
    auto &ci = m_emitter.raw[j];
    if (ci.op == Opcode::CONTINUE) m_emitter.patchJump(j, incLabel);
    else if (ci.op == Opcode::BREAK) m_emitter.patchJump(j, exitLabel);
  }
  m_loops.pop_back();
  popBlockScope();
}

void QLangCompiler::compileWhile(const StmtPtr &s) {
  int testLabel = static_cast<int>(m_emitter.size());
  // Fused cmp+j* when the condition is an Int-vs-Int comparison (SPEC
  // §bytecodes: alternate conditional jump); legacy EQ/GT + JMPIFFALSE
  // otherwise.
  int jumpFalseIdx = emitCmpJumpFalseExpr(s->cond);
  if (jumpFalseIdx < 0) {
    int condReg = tmpRegister();
    compileExpr(s->cond, condReg);
    emit(Opcode::JMPIFFALSE, condReg, 0);
    jumpFalseIdx = static_cast<int>(m_emitter.size()) - 1;
  }

  // continue -> re-evaluate the condition (testLabel is known before the body);
  // break -> exit (post-body), so patch BREAK instructions after the body.
  m_loops.push_back({testLabel, 0});
  int bodyStart = static_cast<int>(m_emitter.size());
  m_blockScopes.emplace_back(); // body is a block scope (SPEC # block scope)
  for (auto &body : s->whileBody)
    compileStmt(body);
  popBlockScope();
  int bodyEnd = static_cast<int>(m_emitter.size());

  emit(Opcode::JMP, testLabel);
  int exitLabel = static_cast<int>(m_emitter.size());
  m_emitter.patchJump(jumpFalseIdx, exitLabel);
  for (int j = bodyStart; j < bodyEnd; j++)
    if (m_emitter.raw[j].op == Opcode::BREAK)
      m_emitter.patchJump(j, exitLabel);
  m_loops.pop_back();
}

void QLangCompiler::compileIf(const StmtPtr &s) {
  // Fused cmp+j* when the condition is an Int-vs-Int comparison (SPEC
  // §bytecodes: alternate conditional jump); legacy EQ/GT + JMPIFFALSE
  // otherwise.
  int jumpFalseIdx = emitCmpJumpFalseExpr(s->ifCond);
  if (jumpFalseIdx < 0) {
    int condReg = tmpRegister();
    compileExpr(s->ifCond, condReg);
    emit(Opcode::JMPIFFALSE, condReg, 0);
    jumpFalseIdx = static_cast<int>(m_emitter.size()) - 1;
  }

  m_blockScopes.emplace_back(); // if body is a block scope (SPEC # block scope)
  for (auto &body : s->ifBody)
    compileStmt(body);
  popBlockScope();

  if (!s->elseBody.empty()) {
    // Emit JMP after if body to skip else body; patch target after else body compiles.
    emit(Opcode::JMP, 0);
    int jumpPastIdx = static_cast<int>(m_emitter.size()) - 1;
    int elseBodyStart = static_cast<int>(m_emitter.size());
    m_blockScopes.emplace_back(); // else body is a separate block scope
    for (auto &body : s->elseBody)
      compileStmt(body);
    popBlockScope();
    m_emitter.patchJump(jumpPastIdx, static_cast<int>(m_emitter.size()));
    m_emitter.patchJump(jumpFalseIdx, elseBodyStart);
  } else {
    m_emitter.patchJump(jumpFalseIdx, static_cast<int>(m_emitter.size()));
  }
}

void QLangCompiler::compileGotoIf(const StmtPtr &s) {
  int condReg = tmpRegister();
  compileExpr(s->gotoIfCond, condReg);
  auto lit = m_labels.find(s->gotoIfLabel);
  if (lit == m_labels.end())
    throw QLangError(QLangErrorKind::Parse, 0, 0,
                     "undefined label '" + s->gotoIfLabel + "'");
  emit(Opcode::JMPIFTRUE, condReg, lit->second);
}

void QLangCompiler::compileExprStmt(const StmtPtr &s) {
  // Statement-form prefix/postfix inc/dec on a variable must write back to
  // that variable's register (e.g. --a, a++). Otherwise the inc/dec result
  // would land in a throwaway temp and the variable would be unchanged.
  if (s->value && s->value->kind == ExprKind::Unary &&
      (s->value->uop.text == "++" || s->value->uop.text == "--")) {
    const ExprPtr &opnd = s->value->operand;
    if (opnd && opnd->kind == ExprKind::Variable) {
      int r = regOf(opnd->name);
      if (r < 0)
        throw QLangError(QLangErrorKind::Parse, 0, 0,
                         "use of undeclared variable '" + opnd->name + "'");
      guardNotConst(opnd->name, "increment/decrement");
      emit(s->value->uop.text == "++" ? Opcode::INC : Opcode::DEC, r, r);
      return;
    }
  }
  int t = tmpRegister();
  compileExpr(s->value, t);
}

void QLangCompiler::compileDef(const StmtPtr &s) {
  // `def name(params) ... end` (or brace form): a global function.
  trace(TRACE_FULL, "[DBG-GENVAR] Def stmt for '%s', m_generatorVars={", s->funcName.c_str());
  for (auto &g : m_generatorVars) trace(TRACE_FULL, " %s", g.c_str());
  trace(TRACE_FULL, " }, m_functions.size=%d\n", (int)m_functions.size());
  int funcIndex = buildFunction(s->funcName, s->params, {}, s->body);
  m_userFuncs[s->funcName] = funcIndex;
  // Track generator functions at the outer scope so assignments from them
  // are recognized as producing generator values.
  trace(TRACE_FULL, "[DBG-GENVAR] After buildFunction: m_functions.size=%d, back.isGenerator=%d\n",
        (int)m_functions.size(), m_functions.empty() ? -1 : (int)m_functions.back().isGenerator);
  if (!m_functions.empty() && m_functions.back().isGenerator)
    m_generatorVars.insert(s->funcName);
  trace(TRACE_FULL, "[DBG-GENVAR] After insert: m_generatorVars={");
  for (auto &g : m_generatorVars) trace(TRACE_FULL, " %s", g.c_str());
  trace(TRACE_FULL, " }\n");
  // Phase 1: a def function with captures is materialized as a closure
  // value at def time (MAKEFUNC): its cells bind to THIS scope's
  // registers, so calls from any depth reach the lexically correct
  // variables (fixes phase0 S2). Calls go through CALLCLOS.
  // buildFunction just pushed it: it is the last element of m_functions
  // (the returned funcIndex is the final program index, main prepended at
  // 0, so it is NOT a valid m_functions index).
  const Function &built = m_functions.back();
  if (!built.captures.empty()) {
    int r = declareVar(s->funcName);
    auto closure = std::make_shared<Closure>();
    closure->funcIndex = funcIndex;
    for (const std::string &cap : built.captureNames) {
      int reg = regOf(cap);
      if (reg < 0) {
        // The capture is itself a free variable of this scope (transitive
        // capture): load it into a hidden named register so the cell can
        // alias it for the frame's lifetime.
        auto cit = m_captureRegs.find(cap);
        if (cit != m_captureRegs.end()) {
          reg = declareVar(std::string("__cap_") + cap);
          emit(Opcode::REFLOAD, reg, cit->second, 0);
        }
      }
      if (reg < 0)
        reg = 0; // preserve legacy binding for unresolvable names
      CaptureRef cr;
      cr.reg = reg;
      closure->captures.push_back(cr);
    }
    int constIdx = emitConst(QLangValue::Function(closure));
    emit(Opcode::MAKEFUNC, r, constIdx);
  }
}

void QLangCompiler::compileArrow(const StmtPtr &s) {
  // `local name = (params)[captures] { ... }`: an arrow-closure value.
  int funcIndex = buildFunction(s->name, s->params, s->captures, s->body);
  m_closureVars.insert(s->name);
  m_varTypes[s->name] = QLangValue::Type::Function;
  // If the closure contains yield (at any nesting depth), mark it as a
  // generator variable so nested functions emit CALLCLOS for calls to it.
  for (auto &st : s->body) {
    if (st && containsYield(st)) {
      m_generatorVars.insert(s->name);
      break;
    }
  }
  int r = declareVar(s->name);
  // Constant describing the closure: funcIndex + enclosing capture registers.
  auto closure = std::make_shared<Closure>();
  closure->funcIndex = funcIndex;
  for (auto &cap : s->captures) {
    int reg = regOf(cap);
    if (reg < 0)
      throw QLangError(QLangErrorKind::Parse, 0, 0,
                       "capture of undeclared variable '" + cap + "'");
    CaptureRef cr;
    cr.reg = reg; // env/cell filled in by MAKEFUNC at runtime
    closure->captures.push_back(cr);
  }
  int constIdx = emitConst(QLangValue::Function(closure));
  // Build the closure value directly in the declared register (SPEC
  // §bytecodes: encoding guidelines — always copy directly to the
  // final register).
  emit(Opcode::MAKEFUNC, r, constIdx);
}

void QLangCompiler::compileReturn(const StmtPtr &s) {
  // Return value goes into register 0 (the frame's return register).
  // Compile the expression directly into it (SPEC §bytecodes: encoding
  // guidelines — always copy directly to the final register).
  if (s->returnValue) {
    compileExpr(s->returnValue, 0);
  } else {
    emit(Opcode::MOVC, 0, emitConst(QLangValue())); // nil
  }
  emit(Opcode::RETURN, 0);
}

void QLangCompiler::compileYield(const StmtPtr &s) {
  // Yield value goes into register 0 (the generator's yield register).
  // Compile the expression directly into it (SPEC §bytecodes: encoding
  // guidelines — always copy directly to the final register).
  if (s->yieldValue) {
    compileExpr(s->yieldValue, 0);
  } else {
    emit(Opcode::MOVC, 0, emitConst(QLangValue())); // nil
  }
  emit(Opcode::YIELD, 0);
}

void QLangCompiler::compileEmit(const StmtPtr &s) {
  // emit obj.signal(args): the sender object lands in a temp, the signal
  // name is a string constant, and the args occupy the consecutive slots
  // sender+1..sender+argCount (the same layout CALL uses for its args).
  int senderReg = tmpRegister();
  compileExpr(s->emitTarget, senderReg);
  int argCount = static_cast<int>(s->emitArgs.size());
  reserveArgSlots(senderReg, argCount);
  compileArgsToSlots(s->emitArgs, senderReg + 1);
  int sigConstIdx = emitConst(QLangValue::String(s->name));
  emit(Opcode::EMIT, senderReg, sigConstIdx, argCount);
  // Clear the arg slots so they do not keep a reference alive past the
  // statement (Phase 7 finalizers need the true final release).
  int nilC = m_emitter.addConst(QLangValue());
  for (int j = 0; j < argCount; j++)
    emit(Opcode::MOVC, senderReg + 1 + j, nilC, 0);
  releaseTemp(senderReg);
}

void QLangCompiler::compileContinue(const StmtPtr &s) {
  if (m_loops.empty())
    throw QLangError(QLangErrorKind::Parse, 0, 0,
                     "'continue' outside of a loop");
  emit(Opcode::CONTINUE, m_loops.back().contAddr);
}

void QLangCompiler::compileBreak(const StmtPtr &s) {
  if (!m_loops.empty()) {
    emit(Opcode::BREAK, m_loops.back().breakAddr);
    return;
  }
  if (!m_switches.empty()) {
    // Record this break JMP for later patching once we know the switch end.
    m_breakJmps.push_back(static_cast<int>(m_emitter.size()));
    emit(Opcode::JMP, 0); // placeholder, patched in switch compilation
    return;
  }
  throw QLangError(QLangErrorKind::Parse, 0, 0,
                   "'break' outside of a loop or switch");
}

void QLangCompiler::compileLabel(const StmtPtr &s) {
  // Record the current instruction index as the target for this label.
  m_labels[s->labelName] = static_cast<int>(m_emitter.size());
}

void QLangCompiler::compileGoto(const StmtPtr &s) {
  auto lit = m_labels.find(s->gotoLabel);
  if (lit == m_labels.end())
    throw QLangError(QLangErrorKind::Parse, 0, 0,
                     "undefined label '" + s->gotoLabel + "'");
  emit(Opcode::JMP, lit->second);
}

void QLangCompiler::compileImport(const StmtPtr &s) {
  // Resolve module functions at compile time and register them as external
  // funcs in the current scope. Supports:
  //   import math                          — flat import all
  //   import math.min                      — flat import single func (legacy)
  //   import math as m                     — register under alias, flat import all
  //   from math import *                   — flat import all
  //   from math import min, max            — flat import specific funcs
  if (!m_inFunction) {
    // Find the module.
    const QLangContext::Module *mod = nullptr;
    for (const auto &m : m_moduleFuncs) {
      if (m.name == s->importModule) { mod = &m; break; }
    }
    if (!mod)
      throw QLangError(QLangErrorKind::Parse, 0, 0,
                       "undefined module '" + s->importModule + "'");

    // If alias specified, register the module under the alias name.
    if (!s->importAlias.empty()) {
      QLangContext::Module aliased;
      aliased.name = s->importAlias;
      aliased.funcs = mod->funcs;
      m_moduleFuncs.push_back(aliased);
      // Now import from the alias.
      mod = &m_moduleFuncs.back();
    }

    // Collect function names to import.
    std::vector<std::string> names;
    if (!s->importFunc.empty()) {
      // Legacy single-function import.
      auto fit = mod->funcs.find(s->importFunc);
      if (fit == mod->funcs.end())
        throw QLangError(QLangErrorKind::Parse, 0, 0,
                         "undefined function '" + s->importFunc + "' in module '" + s->importModule + "'");
      names.push_back(s->importFunc);
    } else if (s->importAll) {
      // `from math import *` — import all.
      for (const auto &[fname, func] : mod->funcs)
        names.push_back(fname);
    } else if (!s->importFuncs.empty()) {
      // `from math import min, max` — import specific functions.
      for (const auto &fname : s->importFuncs) {
        if (mod->funcs.find(fname) == mod->funcs.end())
          throw QLangError(QLangErrorKind::Parse, 0, 0,
                           "undefined function '" + fname + "' in module '" + s->importModule + "'");
        names.push_back(fname);
      }
    } else {
      // `import math` — flat import all (v1).
      for (const auto &[fname, func] : mod->funcs)
        names.push_back(fname);
    }

    // Register each function as an external func in the current scope.
    for (const auto &fname : names) {
      int idx = -1 - m_nativeFuncIds[fname];
      while (m_nativeFuncsResolved.size() <= static_cast<size_t>(idx))
        m_nativeFuncsResolved.push_back(nullptr);
      auto it = mod->funcs.find(fname);
      if (it != mod->funcs.end())
        m_nativeFuncsResolved[static_cast<size_t>(idx)] = it->second;
    }
  }
}

void QLangCompiler::compileClass(const StmtPtr &s) {
  // class Name { fields + methods end }
  // Compile body into a function that creates the prototype table and returns it.
  // Store the function index; at construction time we CALLFN it to get the prototype.
  FnEmitter savedEmitter = std::move(m_emitter);
  auto savedRegs = m_regs;
  auto savedConstVars = m_constVars;
  auto savedVarTypes = m_varTypes;
  auto savedUserFuncs = m_userFuncs;
  auto savedClosureVars = m_closureVars;
  auto savedCaptureRegs = m_captureRegs;
  auto savedGlobalVars = m_globalVars;
  auto savedCapturedVars = m_capturedVars;
  auto savedGeneratorVars = m_generatorVars;
  int savedNextReg = m_nextReg;
  auto savedFreeRegs = std::move(m_freeRegs);
  m_freeRegs.clear(); // fresh register space for the prototype function
  auto savedLiveTmps = std::move(m_liveTmps);
  m_liveTmps.clear();
  auto savedProtectedTmps = std::move(m_protectedTmps);
  m_protectedTmps.clear();
  auto savedProtectStack = std::move(m_protectStack);
  m_protectStack.clear();
  auto savedSlotRanges = std::move(m_slotRanges);
  m_slotRanges.clear();
  auto savedQuarantined = std::move(m_quarantined);
  m_quarantined.clear();
  auto savedDeferred = std::move(m_deferred);
  m_deferred.clear();
  auto savedNamedRegs = std::move(m_namedRegs);
  m_namedRegs.clear();
  auto savedFreeNamedRegs = std::move(m_freeNamedRegs);
  m_freeNamedRegs.clear();
  int savedNamedReg = m_nextNamedReg;
  bool savedInFunction = m_inFunction;

  m_emitter = FnEmitter();
  m_regs.clear();
  m_constVars.clear();
  m_varTypes.clear();
  m_userFuncs.clear();
  m_closureVars.clear();
  m_captureRegs.clear();
  m_generatorVars.clear();
  m_globalVars.clear();
  m_capturedVars.clear();
  m_labels.clear();
  m_nextReg = 0;
  m_inFunction = false;

  // Emit: TABCREATE proto, then compile body, then RETURN proto.
  emit(Opcode::TABCREATE, 0);
  m_nextReg = 1; // temporaries must not collide with prototype at reg 0
  m_nextNamedReg = 1;

  std::vector<std::string> methodNames;
  std::unordered_map<std::string, int> methodFuncIndices;
  // Fixed arity (Phase 4): method name -> declared param count (incl. self).
  std::unordered_map<std::string, int> methodArities;
  // Closed field set (Phase 1): declared fields in declaration order.
  std::vector<std::string> fieldNames;
  std::unordered_map<std::string, int> fieldIndices;
  // Const fields (Phase 2): declared fields marked `const` (read-only).
  std::vector<std::string> constNames;
  // Field type inference (Phase 5): field name -> type inferred from the
  // field's default value (name -> type-name string in `__field_types__`).
  std::unordered_map<std::string, QLangValue::Type> fieldTypes;
  // Static members (new extension beyond v1). Collected in the loop and
  // compiled into the shared `__statics__` table AFTER the loop: method
  // bodies must see the full static name set, so static methods are deferred.
  std::unordered_set<std::string> allStatics;
  std::vector<std::pair<std::string, ExprPtr>> staticFieldDecls; // (name, value)
  std::vector<std::string> staticConstNames;
  struct StaticMethodDecl {
    std::string name;
    std::vector<std::string> params;
    std::vector<std::string> captures;
    std::vector<StmtPtr> body;
    int funcIndex = -1; // filled in the post-loop statics build
  };
  std::vector<StaticMethodDecl> staticMethodDecls;
  for (auto &st : s->classBody) {
    if (!st->isStatic)
      continue;
    if (st->kind == StmtKind::Arrow || st->kind == StmtKind::Assignment ||
        st->kind == StmtKind::Const)
      allStatics.insert(st->name);
  }
  for (auto &st : s->classBody) {
    if (st->kind == StmtKind::Assignment || st->kind == StmtKind::Const) {
      if (st->isStatic) {
        // Static field: stored in the `__statics__` table, NOT the instance
        // field schema. Value is compiled in the post-loop statics build.
        staticFieldDecls.push_back(
            {st->name, st->kind == StmtKind::Assignment ? st->assignValue
                                                        : st->value});
        if (st->kind == StmtKind::Const)
          staticConstNames.push_back(st->name);
        continue;
      }
      // A field: `name = value` or `const name = value`.
      int keyR = tmpRegister();
      emit(Opcode::MOVC, keyR, emitConst(QLangValue::String(st->name)));
      int valR = tmpRegister();
      compileExpr(st->kind == StmtKind::Assignment ? st->assignValue
                                                   : st->value,
                  valR);
      emit(Opcode::TABPUT, 0, keyR, valR);
      freeRegister(keyR);
      freeRegister(valR);
      // Record the declared field (name -> index) for the closed field set.
      fieldIndices[st->name] = static_cast<int>(fieldNames.size());
      fieldNames.push_back(st->name);
      if (st->kind == StmtKind::Const)
        constNames.push_back(st->name);
      // Field type inference (Phase 5): infer the field's type from its
      // default value. Only constant defaults yield a constraint; a
      // non-constant (or `nil`) default leaves the field unconstrained.
      try {
        QLangValue def =
            evalConstExpr(st->kind == StmtKind::Assignment ? st->assignValue
                                                           : st->value);
        if (def.type() != QLangValue::Type::Nil)
          fieldTypes[st->name] = def.type();
      } catch (...) {
        // Non-constant default: no compile-time type constraint.
      }
    } else if (st->kind == StmtKind::Arrow) {
      if (st->isStatic) {
        // Static method: compiled (with a hidden anchor param) in the
        // post-loop statics build, after all static names are known.
        staticMethodDecls.push_back(
            {st->name, st->params, st->captures, st->body});
        continue;
      }
      // Instance method: compile now, with the class's static names visible
      // so the body can reference statics via `self.__statics__`.
      m_classStaticNames = allStatics;
      m_inStaticMethod = false;
      int funcIndex = buildFunction(st->name, st->params, st->captures, st->body);
      m_classStaticNames.clear();
      auto closure = std::make_shared<Closure>();
      closure->funcIndex = funcIndex;
      int constIdx = emitConst(QLangValue::Function(closure));
      int keyR = tmpRegister();
      emit(Opcode::MOVC, keyR, emitConst(QLangValue::String(st->name)));
      int funcR = tmpRegister();
      emit(Opcode::MAKEFUNC, funcR, constIdx);
      emit(Opcode::TABPUT, 0, keyR, funcR);
      freeRegister(keyR);
      freeRegister(funcR);
      methodNames.push_back(st->name);
      methodFuncIndices[st->name] = funcIndex;
      // Fixed arity (Phase 4): record the declared parameter count
      // (includes `self`) for call-site checking.
      methodArities[st->name] = static_cast<int>(st->params.size());
    } else {
      compileStmt(st);
    }
  }

  // Static members (new extension): build the shared `__statics__` table and
  // install it into the prototype BEFORE `__fields__` (same metadata-ordering
  // constraint as `__const_fields__` above). Every instance copies this table
  // by reference, so instance writes mutate class-level state.
  if (!staticFieldDecls.empty() || !staticMethodDecls.empty()) {
    int staticsReg = tmpRegister();
    emit(Opcode::TABCREATE, staticsReg);
    for (auto &sf : staticFieldDecls) {
      int keyR = tmpRegister();
      emit(Opcode::MOVC, keyR, emitConst(QLangValue::String(sf.first)));
      int valR = tmpRegister();
      compileExpr(sf.second, valR);
      emit(Opcode::TABPUT, staticsReg, keyR, valR);
      freeRegister(keyR);
      freeRegister(valR);
    }
    for (auto &sm : staticMethodDecls) {
      // Hidden anchor parameter (the `__statics__` table) as param 0, so a
      // static method needs no self and can read/write statics directly.
      std::vector<std::string> params = {"__anchor__"};
      for (auto &p : sm.params)
        params.push_back(p);
      m_classStaticNames = allStatics;
      m_inStaticMethod = true;
      int funcIndex = buildFunction(sm.name, params, sm.captures, sm.body);
      m_inStaticMethod = false;
      m_classStaticNames.clear();
      sm.funcIndex = funcIndex;
      auto closure = std::make_shared<Closure>();
      closure->funcIndex = funcIndex;
      int constIdx = emitConst(QLangValue::Function(closure));
      int keyR = tmpRegister();
      emit(Opcode::MOVC, keyR, emitConst(QLangValue::String(sm.name)));
      int funcR = tmpRegister();
      emit(Opcode::MAKEFUNC, funcR, constIdx);
      emit(Opcode::TABPUT, staticsReg, keyR, funcR);
      freeRegister(keyR);
      freeRegister(funcR);
    }
    // `__static_const__` set (name -> 1) for static const fields, so
    // SETFIELD can reject assignment to them.
    if (!staticConstNames.empty()) {
      int sconstReg = tmpRegister();
      emit(Opcode::TABCREATE, sconstReg);
      for (const auto &cn : staticConstNames) {
        int keyR = tmpRegister();
        emit(Opcode::MOVC, keyR, emitConst(QLangValue::String(cn)));
        int valR = tmpRegister();
        emitImm(Opcode::MOVI, valR, 1);
        emit(Opcode::TABPUT, sconstReg, keyR, valR);
        freeRegister(keyR);
        freeRegister(valR);
      }
      int sckeyR = tmpRegister();
      emit(Opcode::MOVC, sckeyR,
           emitConst(QLangValue::String("__static_const__")));
      emit(Opcode::TABPUT, staticsReg, sckeyR, sconstReg);
      freeRegister(sckeyR);
      freeRegister(sconstReg);
    }
    int skeyR = tmpRegister();
    emit(Opcode::MOVC, skeyR, emitConst(QLangValue::String("__statics__")));
    emit(Opcode::TABPUT, 0, skeyR, staticsReg);
    freeRegister(skeyR);
    freeRegister(staticsReg);
  }

  // Const fields (Phase 2): emit `__const_fields__` metadata (name -> 1) into
  // the prototype BEFORE installing `__fields__`, so the prototype is not yet
  // a "class instance" (enforcement key present) while metadata is written —
  // setField would otherwise reject the non-declared metadata key.
  if (!constNames.empty()) {
    int cfieldsReg = tmpRegister();
    emit(Opcode::TABCREATE, cfieldsReg);
    for (const auto &cn : constNames) {
      int keyR = tmpRegister();
      emit(Opcode::MOVC, keyR, emitConst(QLangValue::String(cn)));
      int valR = tmpRegister();
      emitImm(Opcode::MOVI, valR, 1);
      emit(Opcode::TABPUT, cfieldsReg, keyR, valR);
      freeRegister(keyR);
      freeRegister(valR);
    }
    int cfkeyR = tmpRegister();
    emit(Opcode::MOVC, cfkeyR,
         emitConst(QLangValue::String(kConstFieldMetaKey)));
    emit(Opcode::TABPUT, 0, cfkeyR, cfieldsReg);
    freeRegister(cfkeyR);
    freeRegister(cfieldsReg);
  }

  // Field types (Phase 5): emit `__field_types__` metadata (name ->
  // type-name string) into the prototype BEFORE installing `__fields__` for
  // the same reason as the const-field metadata above.
  if (!fieldTypes.empty()) {
    int ftypesReg = tmpRegister();
    emit(Opcode::TABCREATE, ftypesReg);
    for (const auto &ft : fieldTypes) {
      int keyR = tmpRegister();
      emit(Opcode::MOVC, keyR, emitConst(QLangValue::String(ft.first)));
      int valR = tmpRegister();
      QLangValue tv(ft.second);
      emit(Opcode::MOVC, valR, emitConst(QLangValue::String(tv.typeName())));
      emit(Opcode::TABPUT, ftypesReg, keyR, valR);
      freeRegister(keyR);
      freeRegister(valR);
    }
    int ftkeyR = tmpRegister();
    emit(Opcode::MOVC, ftkeyR,
         emitConst(QLangValue::String(kFieldTypeMetaKey)));
    emit(Opcode::TABPUT, 0, ftkeyR, ftypesReg);
    freeRegister(ftkeyR);
    freeRegister(ftypesReg);
  }

  // Closed field set (Phase 1): emit `__fields__` metadata (name -> index)
  // into the prototype table LAST so instances inherit the closed field
  // schema. Installing `__fields__` is what makes the prototype a "class
  // instance" for setField/getField enforcement.
  if (!fieldNames.empty()) {
    int fieldsReg = tmpRegister();
    emit(Opcode::TABCREATE, fieldsReg);
    for (size_t i = 0; i < fieldNames.size(); i++) {
      int keyR = tmpRegister();
      emit(Opcode::MOVC, keyR, emitConst(QLangValue::String(fieldNames[i])));
      int valR = tmpRegister();
      emitImm(Opcode::MOVI, valR, static_cast<int32_t>(i));
      emit(Opcode::TABPUT, fieldsReg, keyR, valR);
      freeRegister(keyR);
      freeRegister(valR);
    }
    int fkeyR = tmpRegister();
    emit(Opcode::MOVC, fkeyR, emitConst(QLangValue::String(kFieldMetaKey)));
    emit(Opcode::TABPUT, 0, fkeyR, fieldsReg);
    freeRegister(fkeyR);
    freeRegister(fieldsReg);
  }

  emit(Opcode::RETURN, 0);

  int prototypeFuncIdx = static_cast<int>(m_functions.size()) + 1; // +1 for mainFn prepend
  Function protoFn;
  protoFn.name = s->className + "_prototype";
  protoFn.paramCount = 0;
  protoFn.registerCount = m_nextReg;
  m_emitter.finish(protoFn);
  m_functions.push_back(std::move(protoFn));
  m_emitter = std::move(savedEmitter);
  m_regs = std::move(savedRegs);
  m_constVars = std::move(savedConstVars);
  m_varTypes = std::move(savedVarTypes);
  m_userFuncs = std::move(savedUserFuncs);
  m_closureVars = std::move(savedClosureVars);
  m_captureRegs = std::move(savedCaptureRegs);
  m_globalVars = std::move(savedGlobalVars);
  m_capturedVars = std::move(savedCapturedVars);
  m_generatorVars = std::move(savedGeneratorVars);
  m_nextReg = savedNextReg;
  m_nextNamedReg = savedNamedReg;
  m_namedRegs = std::move(savedNamedRegs);
  m_freeNamedRegs = std::move(savedFreeNamedRegs);
  m_freeRegs = std::move(savedFreeRegs);
  m_liveTmps = std::move(savedLiveTmps);
  m_protectedTmps = std::move(savedProtectedTmps);
  m_slotRanges = std::move(savedSlotRanges);
  m_quarantined = std::move(savedQuarantined);
  m_deferred = std::move(savedDeferred);
  m_protectStack = std::move(savedProtectStack);
  m_inFunction = savedInFunction;

  ClassRecord rec;
  rec.prototypeFunc = prototypeFuncIdx;
  rec.methodNames = std::move(methodNames);
  rec.methodFuncIndices = std::move(methodFuncIndices);
  rec.methodArities = std::move(methodArities);
  rec.fieldNames = std::move(fieldNames);
  rec.fieldIndices = std::move(fieldIndices);
  for (const auto &cn : constNames)
    rec.constFields.insert(cn);
  rec.fieldTypes = std::move(fieldTypes);
  // Static members (new extension): recorded so `C::name` codegen and call
  // sites can resolve them and check arity.
  for (auto &sf : staticFieldDecls)
    rec.staticFields.insert(sf.first);
  for (const auto &cn : staticConstNames)
    rec.staticConstFields.insert(cn);
  for (auto &sm : staticMethodDecls) {
    rec.staticMethodFuncIndices[sm.name] = sm.funcIndex;
    rec.staticMethodArities[sm.name] = static_cast<int>(sm.params.size());
  }
  m_classes[s->className] = std::move(rec);
  // Cache the prototype table in a hidden variable (`__proto_<Name>`) so
  // every instance and `C::name` access shares ONE prototype — and with it
  // the one shared `__statics__` map.
  int protoVar = declareVar("__proto_" + s->className);
  emit(Opcode::CALLFN, protoVar, prototypeFuncIdx, 0);
}

void QLangCompiler::compileEnum(const StmtPtr &s) {
  // enum Name[:type] { A, B, C = 100, D } — immutable name -> value table
  // stored in a hidden variable (like a class prototype). Implied values
  // are sequential from 0; an explicit `= N` resumes the sequence at N+1.
  QLangValue::Type baseType = s->enumType.value_or(QLangValue::Type::Int);
  auto makeValue = [baseType](int64_t v) -> QLangValue {
    switch (baseType) {
    case QLangValue::Type::Char: return QLangValue::Char(static_cast<int8_t>(v));
    case QLangValue::Type::Byte: return QLangValue::Byte(static_cast<uint8_t>(v));
    case QLangValue::Type::Word: return QLangValue::Word(static_cast<uint16_t>(v));
    case QLangValue::Type::Short: return QLangValue::Short(static_cast<int16_t>(v));
    case QLangValue::Type::Unsigned: return QLangValue::Unsigned(static_cast<uint32_t>(v));
    case QLangValue::Type::Qword: return QLangValue::Qword(static_cast<uint64_t>(v));
    case QLangValue::Type::Long: return QLangValue::Long(v);
    default: return QLangValue::Int(static_cast<int32_t>(v));
    }
  };
  std::vector<std::pair<std::string, QLangValue>> members;
  int64_t nextVal = 0;
  for (const auto &m : s->enumMembers) {
    int64_t v = nextVal++;
    if (m.value) {
      QLangValue cv = evalConstExpr(m.value);
      if (cv.type() != QLangValue::Type::Int)
        throw QLangError(QLangErrorKind::Parse, 0, 0,
                         "enum member '" + m.name +
                         "' value must be an integer literal");
      v = cv.asInt();
      nextVal = v + 1; // explicit value resumes the implied sequence
    }
    members.push_back({m.name, makeValue(v)});
  }
  // Redeclaration: a visible same-name enum (any enclosing scope, or a
  // global enum inside a function) is an error — enums cannot be redefined.
  const std::string hvar = "__enum_" + s->name;
  if (regOf(hvar) >= 0 ||
      (!s->isLocalEnum && m_globalEnums.count(s->name)))
    throw QLangError(QLangErrorKind::Parse, 0, 0,
                     "cannot redeclare enum '" + s->name + "'");
  int r = declareVar(hvar, s->isLocalEnum);
  TableValue kv = std::make_shared<std::map<std::string, QLangValue>>();
  for (const auto &m : members)
    (*kv)[m.first] = m.second;
  emit(Opcode::MOVC, r, emitConst(QLangValue::Table(kv)));
  m_enums[s->name] = std::move(members);
  if (!m_inFunction)
    m_globalEnums.insert(s->name);
}

void QLangCompiler::compileGlobal(const StmtPtr &s, std::vector<std::string> *captures) {
  if (!m_inFunction)
    throw QLangError(QLangErrorKind::Parse, 0, 0,
                     "'global' is only valid inside a function");
  for (const auto &name : s->globalNames) {
    // Allow global to override local declarations — global wins.
    if (regOf(name) >= 0) {
      m_regs.erase(name);
    }
    if (!m_globalVars.count(name)) {
      throw QLangError(QLangErrorKind::Parse, 0, 0,
                       "global variable '" + name + "' is not declared");
    }
    // Add to capture list so the VM binds it to the enclosing frame.
    if (captures && std::find(captures->begin(), captures->end(), name) == captures->end()) {
      captures->push_back(name);
    }
    // Mark as captured so compileExpr routes through m_captureRegs.
    m_capturedVars.insert(name);
    // Set up the capture register immediately so subsequent references
    // compile as REFLOAD/REFSTORE instead of MOV/MOV.
    // m_captureRegs stores the capture INDEX (0, 1, 2...) for the VM.
    if (captures) {
      int captureIdx = static_cast<int>(captures->size()) - 1;
      m_captureRegs[name] = captureIdx;
      // F3: resolve the global by name against the context's global table
      // (Phase 2) — no register-0 fallback. The VM's bindCaptures aliases the
      // table slot directly for globals (env==null), so the capture's `reg`
      // is the table slot.
      size_t slot = m_scope ? m_scope->globalSlotOf(name) : SIZE_MAX;
      if (slot == SIZE_MAX)
        throw QLangError(QLangErrorKind::Parse, 0, 0,
                         "global variable '" + name +
                         "' is not in the global table");
      m_globalRegMap[name] = static_cast<int>(slot);
    }
  }
}

void QLangCompiler::compileSwitch(const StmtPtr &s) {
  // Compile the switch expression into a register.
  int valReg = tmpRegister();
  compileExpr(s->switchExpr, valReg);

  // Push switch context so `break` inside case bodies jumps here.
  // We'll patch the exit target after we know where switchEnd is.
  m_switches.push_back(0); // placeholder, patched later

  // Emit a comparison + conditional-jump chain for each case.
  // For each case we emit:
  //   [match check: EQ/IN] -> JMPIFTRUE -> [JMP to skip] -> [case body]
  // After all cases, compile default, then emit JMP to switch end.
  // The JMP after each case jumps to the next case's check (or switchEnd for last).
  std::vector<int> caseMatchStarts; // instruction index of each case's match start (const emit)
  std::vector<int> caseJmpiIfTrueIdx; // instruction index of each JMPIFTRUE
  std::vector<int> caseSkipJmpIdx;    // JMP after JMPIFTRUE (skip to next case)
  std::vector<int> caseExitJmpIdx;    // implicit JMP after case body (exit to switchEnd)
  std::vector<int> caseBodyStarts;    // instruction index of each case body
  for (size_t i = 0; i < s->cases.size(); i++) {
    const auto &sc = s->cases[i];
    int matchCheckIdx = static_cast<int>(m_emitter.size());
    caseMatchStarts.push_back(matchCheckIdx);
    // Emit the match check for this case.
    int cmpReg = tmpRegister();
    if (sc.isIn) {
      int exprReg = tmpRegister();
      compileExpr(sc.value, exprReg);
      emit(Opcode::INx, cmpReg, valReg, exprReg);
    } else {
      int exprReg = tmpRegister();
      compileExpr(sc.value, exprReg);
      emit(Opcode::EQ, cmpReg, valReg, exprReg);
    }
    emit(Opcode::JMPIFTRUE, cmpReg, 0); // target to patch: jump to case body
    caseJmpiIfTrueIdx.push_back(static_cast<int>(m_emitter.size()) - 1);
    // Emit JMP to skip this case body (patched to next case or switchEnd).
    int skipJmpIdx = static_cast<int>(m_emitter.size());
    emit(Opcode::JMP, 0);
    caseSkipJmpIdx.push_back(skipJmpIdx);
    // Compile the case body, then emit an implicit JMP to skip remaining cases.
    caseBodyStarts.push_back(static_cast<int>(m_emitter.size()));
    m_blockScopes.emplace_back(); // each case body is a block scope
    for (auto &body : sc.body) {
      trace(TRACE_FULL, "[DBG-CASE] Case %zu body stmt kind=%d\n", i, (int)body->kind);
      if (body->kind == StmtKind::Assignment) {
        trace(TRACE_FULL, "[DBG-CASE]   Assign op=%s value=", body->assignOp.text.c_str());
        if (body->assignValue && body->assignValue->kind == ExprKind::Literal)
          trace(TRACE_FULL, "Literal:%d (type=%d)\n", (int)body->assignValue->literal.asInt(), (int)body->assignValue->literal.type());
        else
          trace(TRACE_FULL, "non-literal\n");
      }
      compileStmt(body);
    }
    popBlockScope();
    emit(Opcode::JMP, 0); // implicit exit: patched after default compiles
    caseExitJmpIdx.push_back(static_cast<int>(m_emitter.size()) - 1);
  }

  // Compile the default body (if any).
  int defaultBodyStart = static_cast<int>(m_emitter.size());
  m_blockScopes.emplace_back(); // default body is a block scope
  for (auto &db : s->defaultBody)
    compileStmt(db);
  popBlockScope();
  int defaultBodyEnd = static_cast<int>(m_emitter.size());

  // Now we know where switchEnd is (after all cases and default).
  int switchEnd = static_cast<int>(m_emitter.size());
  // Update the switch context with the real exit target.
  m_switches.back() = switchEnd;
  // Patch any break JMPs that were emitted with placeholder target 0.
  for (int idx : m_breakJmps) {
    m_emitter.patchJump(idx, switchEnd);
  }
  m_breakJmps.clear();

  // Patch all JMPIFTRUE targets to point at their case body start,
  // skip-JMP targets to next case (or default body for last),
  // and exit-JMP targets to switchEnd.
  for (size_t i = 0; i < s->cases.size(); i++) {
    m_emitter.patchJump(caseJmpiIfTrueIdx[i], caseBodyStarts[i]);
    if (i + 1 < s->cases.size())
      m_emitter.patchJump(caseSkipJmpIdx[i], caseMatchStarts[i + 1]);
    else
      m_emitter.patchJump(caseSkipJmpIdx[i], defaultBodyStart);
    m_emitter.patchJump(caseExitJmpIdx[i], switchEnd);
  }

  // Pop switch context after patching is complete.
  m_switches.pop_back();

  // Debug: dump switch bytecode
  trace(TRACE_FULL, "[DBG-SW] switchEnd=%d instructions.size()=%zu\n", switchEnd, m_emitter.size());
  for (size_t i = 0; i < m_emitter.size(); i++) {
    const auto &inst = m_emitter.raw[i];
    trace(TRACE_FULL, "[DBG-SW]   [%zu] op=%d a=%d b=%d c=%d\n", i, (int)inst.op, inst.a, inst.b, inst.c);
  }
  trace(TRACE_FULL, "[DBG-SW] constants.size()=%zu\n", m_emitter.consts.size());
  for (size_t i = 0; i < m_emitter.consts.size(); i++) {
    const auto &c = m_emitter.consts[i];
    std::string cv;
    if (c.type() == QLangValue::Type::Int)
      cv = std::to_string(c.asInt());
    else if (c.type() == QLangValue::Type::String)
      cv = c.asString();
    else
      cv = std::string("(type=") + std::to_string((int)c.type()) + ")";
    trace(TRACE_FULL, "[DBG-SW]   const[%zu] type=%d str=%s\n", i, (int)c.type(), cv.c_str());
  }
}


void QLangCompiler::compileExpr(const ExprPtr &e, int dest) {
  if (!e)
    throw QLangError(QLangErrorKind::Parse, 0, 0, "null expression");

  switch (e->kind) {
  case ExprKind::Literal:
    if (isIntLiteral(e))
      emitImm(Opcode::MOVI, dest, e->literal.asInt()); // no pool slot needed
    else
      emit(Opcode::MOVC, dest, emitConst(e->literal));
    break;
  case ExprKind::Variable: {
    // Bare static member name inside a method body of a class with statics:
    // read from the class-level `__statics__` table (shared across instances).
    if (!m_classStaticNames.empty() && m_classStaticNames.count(e->name)) {
      int staticsR = staticsTableReg();
      int keyR = tmpRegister();
      emit(Opcode::MOVC, keyR, emitConst(QLangValue::String(e->name)));
      emit(Opcode::TABGET, dest, staticsR, keyR);
      break;
    }
    // Captured variable: load through the closure reference (live binding).
    auto cit = m_captureRegs.find(e->name);
    if (cit != m_captureRegs.end()) {
      emit(Opcode::REFLOAD, dest, cit->second, 0);
      break;
    }
    // Explicitly captured global var (declared with `global`): route through
    // m_captureRegs so it uses REFLOAD instead of MOV.
    if (m_capturedVars.count(e->name) && m_captureRegs.count(e->name)) {
      emit(Opcode::REFLOAD, dest, m_captureRegs[e->name], 0);
      break;
    }
    int r = regOf(e->name);
    if (r >= 0) {
      // Value already in dest: skip the self-MOV (SPEC §bytecodes:
      // encoding guidelines — avoid redundant copies / aliasing).
      if (dest != r)
        emit(Opcode::MOV, dest, r);
      break;
    }
    // Host-injected external variable (seeded via QLangContext::set, e.g.
    // a TypeRef from import<T>()): declare a register on first use so it
    // can be loaded; the VM seeds it from the scope at run start.
    if (m_externalVars.count(e->name)) {
      r = declareVar(e->name);
      emit(Opcode::MOV, dest, r);
      break;
    }
    // Enum name: resolves to the hidden table variable holding its values
    // (captured across function boundaries like a free variable).
    if (m_enums.count(e->name)) {
      const std::string hvar = "__enum_" + e->name;
      auto cit = m_captureRegs.find(hvar);
      if (cit != m_captureRegs.end())
        emit(Opcode::REFLOAD, dest, cit->second, 0);
      else
        emit(Opcode::MOV, dest, regOf(hvar));
      break;
    }
    // Global const: script-level consts already have a register (declared
    // by the `const` statement); a register-less const is a host-injected
    // external const (e.g. a TypeRef seeded by import<T>()) — declare a
    // register on first use so it can be loaded.
    if (m_constVars.count(e->name)) {
      r = declareVar(e->name);
      emit(Opcode::MOV, dest, r);
      break;
    }
    throw QLangError(QLangErrorKind::Parse, 0, 0,
                     "use of undeclared variable '" + e->name + "'");
    break;
  }
  case ExprKind::Binary:
  case ExprKind::Unary: {
    if (e->kind == ExprKind::Binary) {
      // `l in range(a, b)` -> (l >= a) && (l <= b): inclusive bounds check,
      // no array materialized (spec §range).
      if (e->op.text == "in") {
        ExprPtr ra, rb;
        if (isRangeCall(e->right, ra, rb)) {
          int l = inPlaceReg(e->left);
          bool lTmp = false;
          if (l < 0) {
            l = tmpRegister();
            lTmp = true;
            compileExpr(e->left, l);
          }
          int aReg = tmpRegister();
          compileExpr(ra, aReg);
          int geReg = tmpRegister();
          emit(Opcode::GE, geReg, l, aReg);
          int bReg = tmpRegister();
          compileExpr(rb, bReg);
          int leReg = tmpRegister();
          emit(Opcode::LE, leReg, l, bReg);
          emit(Opcode::LAND, dest, geReg, leReg);
          if (lTmp)
            freeRegister(l);
          freeRegister(aReg);
          freeRegister(geReg);
          freeRegister(bReg);
          freeRegister(leReg);
          break;
        }
      }
      // Plain-variable operands are used in place (no mov); anything else
      // is compiled into a temp (SPEC §bytecodes: encoding guidelines —
      // avoid temporary registers when possible).
      int l = inPlaceReg(e->left);
      bool lTmp = false;
      if (l < 0) {
        l = tmpRegister();
        lTmp = true;
        compileExpr(e->left, l);
      }
      int rr = inPlaceReg(e->right);
      bool rrTmp = false;
      if (rr < 0) {
        rr = tmpRegister();
        rrTmp = true;
        compileExpr(e->right, rr);
      }
      emit(binOp(e->op.text), dest, l, rr);
      if (lTmp)
        freeRegister(l);
      if (rrTmp)
        freeRegister(rr);
    } else {
      // Constant folding: `-(literal)` -> one MOVI with a negative
      // immediate instead of MOVI + NEG (+ MOV) (SPEC §bytecodes:
      // encoding guidelines — avoid temporary registers when possible).
      if (e->uop.type == TokenType::Minus) {
        auto folded = foldNegLiteral(e);
        if (folded) {
          emitImm(Opcode::MOVI, dest, *folded);
          break;
        }
      }
      int o = inPlaceReg(e->operand);
      bool oTmp = false;
      if (o < 0) {
        o = tmpRegister();
        oTmp = true;
        compileExpr(e->operand, o);
      }
      emit(unOp(e->uop.text), dest, o);
      if (oTmp)
        freeRegister(o);
    }
    break;
  }
  case ExprKind::Call: {
    int argCount = static_cast<int>(e->args.size());
    trace(TRACE_FRAME, "[DBG-CALL] Entering call expr for '%s' with %d args\n", e->callee.c_str(), argCount);
    trace(TRACE_FULL, "[DBG-CALL] &m_emitter=%p m_emitter.size=%zu\n", (void*)&m_emitter, m_emitter.size());
    // Reserve one extra slot: method/constructor calls shift args up by one
    // (self/anchor), so their live slots reach dest+argCount+1. Must happen
    // BEFORE any arg register is allocated, or it would take a slot register.
    reserveArgSlots(dest, argCount + 1);
    int argBase = dest + 1;
    // Two clobber hazards for register-based callee loads (nested def-fns,
    // arrow-closure vars, generator vars — e.g. `def outer(a) def inner(b)
    // ... end; return inner(10); end`):
    //  1. cr in the arg-slot range [argBase, argBase+argCount-1]:
    //     compileArgsToSlots overwrites the closure before the call.
    //  2. dest is a register the callee's capture cells live-alias (e.g.
    //     `return inner(10)` compiles into reg 0, which is also outer's
    //     param `a` that inner's cells alias): writing the callee into dest
    //     BEFORE the callee runs corrupts the captured value.
    // Both are fixed by hoisting the callee into a temp (never an arg slot)
    // and calling with i.a = temp, i.b = dest: the VM reads the callee from
    // the temp, reads args from the original slots, and writes the result
    // into dest only at RETURN (after the callee has run).
    int calleeTmpR = -1;
    {
      auto uit = m_userFuncs.find(e->callee);
      if (uit != m_userFuncs.end()
          && !m_generatorFuncIndices.count(uit->second)) {
        int fi = uit->second;
        // Only the register-based sub-path is exposed; REFLOAD-based loads
        // (name is a free var in a capture cell) are immune — cells can't
        // be clobbered by register writes.
        const Function &fn = m_functions[static_cast<size_t>(fi) - 1];
        if (!fn.captures.empty()
            && m_captureRegs.find(e->callee) == m_captureRegs.end()) {
          int cr = regOf(e->callee);
          bool crInSlots =
              cr >= 0 && cr >= argBase && cr <= argBase + argCount - 1;
          bool destIsSource = false;
          for (const std::string &cap : fn.captureNames)
            if (regOf(cap) == dest)
              destIsSource = true;
          if (crInSlots || destIsSource) {
            calleeTmpR = tmpRegister();  // never in the active slot range
            emit(Opcode::MOV, calleeTmpR, cr);
          }
        }
      } else if (m_closureVars.count(e->callee)
                 || m_generatorVars.count(e->callee)) {
        // Arrow-closure / generator vars: source registers are baked into
        // the runtime closure, so be conservative — always call via a
        // temp and let the result land in dest only at RETURN.
        int cr = regOf(e->callee);
        if (cr >= 0) {
          calleeTmpR = tmpRegister();
          emit(Opcode::MOV, calleeTmpR, cr);
        }
      }
    }
    // Compile each arg directly into the VM's slot (dest+1 ..
    // dest+argCount): simple operands land in the slot without a temp,
    // complex ones share ONE scratch register (SPEC §bytecodes: encoding
    // guidelines — universal scratch pad).
    compileArgsToSlots(e->args, argBase);

    // Method call: obj.method(args) -> load the member (closure) first.
    if (e->calleeMember) {
      // Static method call: C::name(args). The closure lives in the class-level
      // `__statics__` table and is called with that table as a hidden first
      // argument (the anchor) — no self.
      if (e->calleeMember->scopeStatic) {
        if (e->calleeMember->memberBase->kind != ExprKind::Variable)
          throw QLangError(QLangErrorKind::Parse, e->callLine, e->callCol,
                           "'::' requires a class name on the left");
        const std::string &cname = e->calleeMember->memberBase->name;
        auto cit = m_classes.find(cname);
        if (cit == m_classes.end())
          throw QLangError(QLangErrorKind::Parse, e->callLine, e->callCol,
                           "'" + cname + "' is not a class");
        const ClassRecord &rec = cit->second;
        if (!rec.staticMethodFuncIndices.count(e->callee))
          throw QLangError(QLangErrorKind::Parse, e->callLine, e->callCol,
                           "'" + cname + "' has no static method '" +
                               e->callee + "'");
        int declaredArity = rec.staticMethodArities.at(e->callee);
        if (argCount != declaredArity)
          throw QLangError(
              QLangErrorKind::Parse, e->callLine, e->callCol,
              "static method '" + e->callee + "' expects " +
                  std::to_string(declaredArity) + " argument" +
                  (declaredArity == 1 ? "" : "s") + ", got " +
                  std::to_string(argCount));
        int staticsR = tmpRegister();
        int skR = tmpRegister();
        emit(Opcode::MOVC, skR, emitConst(QLangValue::String("__statics__")));
        // __proto_<class> is a named register: read it in place.
        emit(Opcode::TABGET, staticsR, regOf("__proto_" + cname), skR);
        int keyR = tmpRegister();
        emit(Opcode::MOVC, keyR, emitConst(QLangValue::String(e->callee)));
        int funcReg = tmpRegister();
        emit(Opcode::TABGET, funcReg, staticsR, keyR);
        // Shift user args down by one to make room for the anchor at argBase.
        for (int i = argCount - 1; i >= 0; i--)
          emit(Opcode::MOV, argBase + i + 1, argBase + i, 0);
        emit(Opcode::MOV, argBase, staticsR, 0); // anchor = statics table
        emit(Opcode::MOV, dest, funcReg);
        emit(Opcode::CALLCLOS, dest, dest, argCount + 1);
        break;
      }
      // Prepend self only for known class methods (whose signature includes self).
      // Table-stored closures are called as-is.
      bool prependSelf = false;
      int declaredArity = -1; // param count incl. self; -1 = unknown
      for (const auto &kv : m_classes) {
        if (kv.second.methodFuncIndices.count(e->callee)) {
          prependSelf = true;
          auto ait = kv.second.methodArities.find(e->callee);
          if (ait != kv.second.methodArities.end())
            declaredArity = ait->second;
          break;
        }
      }
      // Fixed arity (Phase 4.2): a known class method must be called with
      // exactly (declared params - 1) arguments.
      if (prependSelf && declaredArity > 0 &&
          argCount + 1 != declaredArity) {
        int expected = declaredArity - 1;
        throw QLangError(
            QLangErrorKind::Parse, e->callLine, e->callCol,
            "method '" + e->callee + "' expects " +
            std::to_string(expected) + " argument" +
            (expected == 1 ? "" : "s") + ", got " +
            std::to_string(argCount));
      }
      // Resolve the callee's base once: a plain-variable base (obj.name)
      // reads in place, only compound bases take a temp.
      int baseR = inPlaceReg(e->calleeMember->memberBase);
      if (baseR < 0)
      {
        baseR = tmpRegister();
        compileExpr(e->calleeMember->memberBase, baseR);
      }
      int keyR = tmpRegister();
      emit(Opcode::MOVC, keyR,
           emitConst(QLangValue::String(e->calleeMember->memberName)));
      int funcReg = tmpRegister();
      emit(Opcode::TABGET, funcReg, baseR, keyR);
      if (prependSelf) {
        // Shift user args down by one, then store the base (self) directly
        // into the vacated argBase slot.
        for (int i = argCount - 1; i >= 0; i--)
          emit(Opcode::MOV, argBase + i + 1, argBase + i, 0);
        emit(Opcode::MOV, argBase, baseR, 0);
        emit(Opcode::MOV, dest, funcReg);
        emit(Opcode::CALLCLOS, dest, dest, argCount + 1);
      } else {
        emit(Opcode::MOV, dest, funcReg);
        emit(Opcode::CALLCLOS, dest, dest, argCount);
      }
      break;
    }

    // Class constructor call: ClassName(args) -> create instance, copy methods,
    // call __init__(self, args...).
    auto cit = m_classes.find(e->callee);
    if (cit != m_classes.end()) {
      const ClassRecord &rec = cit->second;
      // Create instance table.
      int instanceReg = tmpRegister();
      emit(Opcode::TABCREATE, instanceReg);
      // Load the cached prototype table (created once at class definition).
      int protoReg = tmpRegister();
      emit(Opcode::MOV, protoReg, regOf("__proto_" + e->callee));
      // Copy methods from prototype to instance.
      for (const auto &mname : rec.methodNames) {
        int keyR = tmpRegister();
        emit(Opcode::MOVC, keyR, emitConst(QLangValue::String(mname)));
        int valR = tmpRegister();
        emit(Opcode::TABGET, valR, protoReg, keyR);
        emit(Opcode::TABPUT, instanceReg, keyR, valR);
        freeRegister(keyR);
        freeRegister(valR);
      }
      // Copy field defaults from prototype to instance (Phase 6): a
      // `v = 7` in the class body is an initial value applied at
      // construction, not merely a schema entry. Done before the metadata
      // writes below, like the method copies.
      for (const auto &fname : rec.fieldNames) {
        int keyR = tmpRegister();
        emit(Opcode::MOVC, keyR, emitConst(QLangValue::String(fname)));
        int valR = tmpRegister();
        emit(Opcode::TABGET, valR, protoReg, keyR);
        emit(Opcode::TABPUT, instanceReg, keyR, valR);
        freeRegister(keyR);
        freeRegister(valR);
      }
      // Static members (new extension): copy the shared `__statics__` table
      // from the prototype BY REFERENCE (it is a table value), so the instance
      // and every sibling mutate the same class-level map.
      if (!rec.staticFields.empty() || !rec.staticMethodFuncIndices.empty()) {
        int skeyR = tmpRegister();
        emit(Opcode::MOVC, skeyR, emitConst(QLangValue::String("__statics__")));
        int svalR = tmpRegister();
        emit(Opcode::TABGET, svalR, protoReg, skeyR);
        emit(Opcode::TABPUT, instanceReg, skeyR, svalR);
        freeRegister(skeyR);
        freeRegister(svalR);
      }
      // Closed field set (Phase 1.3): stamp the class name onto the instance
      // BEFORE installing the `__fields__` schema, so the instance is not yet
      // a "class instance" (enforcement key present) while metadata is written.
      {
        int ckeyR = tmpRegister();
        emit(Opcode::MOVC, ckeyR, emitConst(QLangValue::String(kClassMetaKey)));
        int cvalR = tmpRegister();
        emit(Opcode::MOVC, cvalR, emitConst(QLangValue::String(e->callee)));
        emit(Opcode::TABPUT, instanceReg, ckeyR, cvalR);
        freeRegister(ckeyR);
        freeRegister(cvalR);
      }
      // Const fields (Phase 2): copy the `__const_fields__` set from the
      // prototype BEFORE installing `__fields__`, so the instance is not yet a
      // "class instance" (enforcement key present) while metadata is written —
      // setField would otherwise reject the non-declared metadata key.
      if (!rec.constFields.empty()) {
        int cfkeyR = tmpRegister();
        emit(Opcode::MOVC, cfkeyR,
             emitConst(QLangValue::String(kConstFieldMetaKey)));
        int cfvalR = tmpRegister();
        emit(Opcode::TABGET, cfvalR, protoReg, cfkeyR);
        emit(Opcode::TABPUT, instanceReg, cfkeyR, cfvalR);
        freeRegister(cfkeyR);
        freeRegister(cfvalR);
      }
      // Field types (Phase 5): copy the `__field_types__` table from the
      // prototype BEFORE installing `__fields__` (same ordering constraint as
      // the const-field metadata above).
      if (!rec.fieldTypes.empty()) {
        int ftkeyR = tmpRegister();
        emit(Opcode::MOVC, ftkeyR,
             emitConst(QLangValue::String(kFieldTypeMetaKey)));
        int ftvalR = tmpRegister();
        emit(Opcode::TABGET, ftvalR, protoReg, ftkeyR);
        emit(Opcode::TABPUT, instanceReg, ftkeyR, ftvalR);
        freeRegister(ftkeyR);
        freeRegister(ftvalR);
      }
      // Closed field set (Phase 1): copy the `__fields__` schema from the
      // prototype onto the instance LAST so GETFIELD/SETFIELD can enforce the
      // closed set. Installing `__fields__` is what makes the instance a
      // "class instance".
      if (!rec.fieldNames.empty()) {
        int fkeyR = tmpRegister();
        emit(Opcode::MOVC, fkeyR, emitConst(QLangValue::String(kFieldMetaKey)));
        int fvalR = tmpRegister();
        emit(Opcode::TABGET, fvalR, protoReg, fkeyR);
        emit(Opcode::TABPUT, instanceReg, fkeyR, fvalR);
        freeRegister(fkeyR);
        freeRegister(fvalR);
      }
      // Save instance to dest before overwriting instanceReg with args.
      emit(Opcode::MOV, dest, instanceReg);
      // Instance temp is dead: clear + return to the pool (the second
      // reference must go for Phase 7 finalizer timing).
      releaseTemp(instanceReg);
      // Phase 7: attach the `__destroy__` finalizer (no-op if the class has
      // none). The deleter keeps the closure and owning program alive so the
      // method can run at final release, even after this run()'s VM is gone.
      emit(Opcode::TABFINALIZE, dest);
      // Call __init__ (fixed arity, Phase 4.3/4.4).
      auto initIt = rec.methodFuncIndices.find("__init__");
      if (initIt == rec.methodFuncIndices.end()) {
        // Phase 4.4: no __init__ -> the constructor takes zero arguments.
        if (argCount != 0) {
          throw QLangError(
              QLangErrorKind::Parse, e->callLine, e->callCol,
              "class '" + e->callee + "' has no constructor; expected 0 "
              "argument" + (argCount == 1 ? "" : "s") + ", got " +
              std::to_string(argCount));
        }
        // No __init__: dest already holds the finalized instance and no
        // self-arg copy was made, so there is nothing to release (Phase 7
        // finalizers need the true final release — a stray self-arg copy
        // would extend the instance's lifetime past its last named ref).
        break;
      }
      // Prepend instance as first arg (self): move args into place,
      // then move saved instance as arg[0].
      int newArgCount = argCount + 1;
      int newArgBase = dest + 1;
      for (int i = argCount - 1; i >= 0; i--) {
        emit(Opcode::MOV, newArgBase + i + 1, newArgBase + i, 0);
      }
      emit(Opcode::MOV, newArgBase, dest, 0);
      // Phase 4.3: __init__ exists -> check arg count against its declared
      // arity (params include `self`, so expected = arity - 1).
      auto ait = rec.methodArities.find("__init__");
      if (ait != rec.methodArities.end()) {
        int expected = ait->second - 1;
        if (argCount != expected) {
          throw QLangError(
              QLangErrorKind::Parse, e->callLine, e->callCol,
              "constructor of '" + e->callee + "' expects " +
              std::to_string(expected) + " argument" +
              (expected == 1 ? "" : "s") + ", got " +
              std::to_string(argCount));
        }
      }
      int funcIdx = initIt->second;
      emit(Opcode::CALLFN, dest, funcIdx, newArgCount);
      // __init__ may return nil; instance was passed as self at newArgBase
      // and survives there (callee frame is separate).
      emit(Opcode::MOV, dest, newArgBase);
      // Release the caller-frame copies of the call args (self + ctor args)
      // so they do not extend the instance's lifetime to frame end (Phase 7
      // finalizers need the true final release; same pattern as the MAKEGEN
      // arg-register clearing above).
      {
        int nilC = m_emitter.addConst(QLangValue());
        for (int j = 0; j < newArgCount; j++)
          emit(Opcode::MOVC, newArgBase + j, nilC, 0);
      }
      break;
    }
    // User `def` function: funcIndex known at compile time.
    auto uit = m_userFuncs.find(e->callee);
    if (uit != m_userFuncs.end()) {
      int fi = uit->second;
      // Generator functions: emit MAKEGEN to create a generator object,
      // not CALLFN which would run the generator inline.
      if (m_generatorFuncIndices.count(fi)) {
        int constIdx = m_generatorConstIndices[fi];
        emit(Opcode::MAKEGEN, dest, constIdx, argCount);
        // Clear arg regs so they don't leak into the next instruction.
        for (int j = 0; j < argCount; j++)
          emit(Opcode::MOVC, argBase + j, m_emitter.addConst(QLangValue()), 0);
      } else if (!m_functions[static_cast<size_t>(fi) - 1].captures.empty()) {
        // Phase 1: def function with captures — the closure value (cells
        // bound at def time) is the name's value in this scope; dispatch
        // through CALLCLOS.
        auto cit = m_captureRegs.find(e->callee);
        if (cit != m_captureRegs.end()) {
          // The name is a free variable of this function: its value comes
          // from the capture cell, not a local register.
          emit(Opcode::REFLOAD, dest, cit->second, 0);
          emit(Opcode::CALLCLOS, dest, dest, argCount);
        } else {
          int cr = regOf(e->callee);
          if (cr >= 0) {
            if (calleeTmpR >= 0) {
              // Callee hoisted to a temp: the VM reads the callee from
              // the temp (i.a), reads args from the original slots, and
              // writes the result into dest (i.b) at RETURN.
              emit(Opcode::CALLCLOS, calleeTmpR, dest, argCount);
            } else {
              if (dest != cr)
                emit(Opcode::MOV, dest, cr);
              emit(Opcode::CALLCLOS, dest, dest, argCount);
            }
          } else {
            throw QLangError(QLangErrorKind::Parse, 0, 0,
                             "cannot call captured function '" + e->callee +
                             "'");
          }
        }
      } else {
        emit(Opcode::CALLFN, dest, fi, argCount);
      }
      break;
    }
    // Arrow-closure variable: the closure value (with its funcIndex) is loaded
    // from the variable register and dispatched at runtime.
    auto cif = m_closureVars.find(e->callee);
    if (cif != m_closureVars.end()) {
      int cr = regOf(e->callee);
      if (calleeTmpR >= 0) {
        // Conservative: source registers are baked into the runtime
        // closure, so dest is written only at RETURN.
        emit(Opcode::CALLCLOS, calleeTmpR, dest, argCount);
      } else {
        emit(Opcode::MOV, dest, cr);
        emit(Opcode::CALLCLOS, dest, dest, argCount);
      }
      break;
    }
    // Generator variable: use CALLCLOS (runtime dispatch to resume generator).
    trace(TRACE_FULL, "[DBG-GENVAR] Call to '%s': in m_generatorVars? %d\n",
          e->callee.c_str(), (int)m_generatorVars.count(e->callee));
    if (m_generatorVars.count(e->callee)) {
      int cr = regOf(e->callee);
      if (calleeTmpR >= 0) {
        // Conservative: source registers are baked into the runtime
        // closure, so dest is written only at RETURN.
        emit(Opcode::CALLCLOS, calleeTmpR, dest, argCount);
      } else {
        emit(Opcode::MOV, dest, cr);
        emit(Opcode::CALLCLOS, dest, dest, argCount);
      }
      break;
    }
    // Built-in function.
    int funcId = builtinId(e->callee);
    // Native (C++) function — use negative index.
    if (funcId < 0) {
      auto nit = m_nativeFuncIds.find(e->callee);
      if (nit != m_nativeFuncIds.end()) {
        emit(Opcode::CALL, dest, nit->second, argCount);
        break;
      }
      // Not a user def, closure, generator var, builtin, or native/module
      // function: an unknown call must be a compile error, not a silent
      // negative-index CALL (which would dispatch into nativeFuncs[0]).
      throw QLangError(QLangErrorKind::Parse, e->callLine, e->callCol,
                       "unknown function '" + e->callee + "'");
    }
    emit(Opcode::CALL, dest, funcId, argCount);
    // Propagate source position for runtime error reporting (on the CALL).
    m_emitter.setLastLoc(static_cast<uint16_t>(e->callLine),
                         static_cast<uint16_t>(e->callCol));
    // release(x) mutates its argument in place (drops the owned shared_ptr,
    // Phase 3 H2). The argument was compiled into the slot dest+1; write it
    // back to the source variable so the mutation is visible (the slot is a
    // copy — without this, the original variable keeps its reference and the
    // object is never freed).
    if (e->callee == "release") {
      for (size_t i = 0; i < e->args.size(); i++) {
        int argReg = inPlaceReg(e->args[i]);
        if (argReg >= 0)
          emit(Opcode::MOV, argReg, dest + 1 + i, 0);
      }
    }
    if (e->callee == "range") trace(TRACE_FULL, "[DBG-RANGE] emitted CALL dest=%d b=%d c=%d\n", dest, funcId, argCount);
    break;
  }
  case ExprKind::Array: {
    ArrayValue elems;
    elems.reserve(e->elements.size());
    for (auto &el : e->elements)
      elems.push_back(evalConstExpr(el));
    emit(Opcode::MOVC, dest, emitConst(QLangValue::Array(std::move(elems))));
    break;
  }
  case ExprKind::Table: {
    // Check if all elements and keys are constant-evaluable.
    bool allConst = true;
    for (auto &item : e->items) {
      try {
        evalConstKey(item.first);
        evalConstExpr(item.second);
      } catch (...) {
        allConst = false;
        break;
      }
    }
    if (allConst) {
      TableValue kv = std::make_shared<std::map<std::string, QLangValue>>();
      for (auto &item : e->items)
        kv->operator[](evalConstKey(item.first)) = evalConstExpr(item.second);
      emit(Opcode::MOVC, dest, emitConst(QLangValue::Table(kv)));
      break;
    }
    // Runtime table construction.
    int baseR = tmpRegister();
    emit(Opcode::TABCREATE, baseR);
    for (auto &item : e->items) {
      int keyR = tmpRegister();
      // Static identifier keys are Variable expressions - emit as string constant.
      // Dynamic keys ($expr) are compiled as expressions.
      if (item.first->kind == ExprKind::Variable)
        emit(Opcode::MOVC, keyR, emitConst(QLangValue::String(item.first->name)));
      else
        compileExpr(item.first, keyR);
      int valR = tmpRegister();
      compileExpr(item.second, valR);
      emit(Opcode::TABPUT, baseR, keyR, valR);
    }
    emit(Opcode::MOV, dest, baseR);
    break;
  }
  case ExprKind::Closure: {
    // Anonymous arrow closure: build the function and emit MAKEFUNC.
    int funcIndex = buildFunction("closure", e->params, e->captures, e->body);
    auto closure = std::make_shared<Closure>();
    closure->funcIndex = funcIndex;
    for (auto &cap : e->captures) {
      int reg = regOf(cap);
      if (reg < 0)
        throw QLangError(QLangErrorKind::Parse, 0, 0,
                         "capture of undeclared variable '" + cap + "'");
      CaptureRef cr;
      cr.reg = reg;
      closure->captures.push_back(cr);
    }
    int constIdx = emitConst(QLangValue::Function(closure));
    emit(Opcode::MAKEFUNC, dest, constIdx);
    break;
  }
  case ExprKind::Index: {
    // base[index] read -> ARRGET dest, baseR, idxR
    int baseR = tmpRegister();
    compileExpr(e->indexBase, baseR);
    int idxR = tmpRegister();
    compileExpr(e->indexExpr, idxR);
    emit(Opcode::ARRGET, dest, baseR, idxR);
    break;
  }
  case ExprKind::Member: {
    // Enum member access (both notations): Enum::name and Enum.name fold to
    // the immutable value at compile time. Unknown enum or member is a
    // compile error (SPEC §enum).
    if (e->memberBase->kind == ExprKind::Variable) {
      auto eit = m_enums.find(e->memberBase->name);
      if (eit != m_enums.end()) {
        const std::string hvar = "__enum_" + e->memberBase->name;
        if (regOf(hvar) < 0 && !m_captureRegs.count(hvar))
          throw QLangError(QLangErrorKind::Parse, 0, 0,
                           "unknown enum '" + e->memberBase->name + "'");
        bool found = false;
        for (const auto &m : eit->second) {
          if (m.first == e->memberName) {
            emit(Opcode::MOVC, dest, emitConst(m.second));
            found = true;
            break;
          }
        }
        if (!found)
          throw QLangError(QLangErrorKind::Parse, 0, 0,
                           "enum '" + e->memberBase->name +
                           "' has no member '" + e->memberName + "'");
        break; // value folded — skip class/table member paths
      }
    }
    if (e->scopeStatic) {
      // C::name -> TABGET staticsR, C, "__statics__"; TABGET dest, staticsR,
      // name. The base must be a known class and the name one of its static
      // members (compile-time check).
      if (e->memberBase->kind != ExprKind::Variable)
        throw QLangError(QLangErrorKind::Parse, 0, 0,
                         "'::' requires a class name on the left");
      const std::string &cname = e->memberBase->name;
      auto cit = m_classes.find(cname);
      if (cit == m_classes.end())
        throw QLangError(QLangErrorKind::Parse, 0, 0,
                         "'" + cname + "' is not a class");
      const ClassRecord &rec = cit->second;
      if (!rec.staticFields.count(e->memberName) &&
          !rec.staticMethodFuncIndices.count(e->memberName))
        throw QLangError(QLangErrorKind::Parse, 0, 0,
                         "'" + cname + "' has no static member '" +
                             e->memberName + "'");
      int baseR = tmpRegister();
      emit(Opcode::MOV, baseR, regOf("__proto_" + cname));
      int staticsR = tmpRegister();
      int skR = tmpRegister();
      emit(Opcode::MOVC, skR, emitConst(QLangValue::String("__statics__")));
      emit(Opcode::TABGET, staticsR, baseR, skR);
      int keyR = tmpRegister();
      emit(Opcode::MOVC, keyR, emitConst(QLangValue::String(e->memberName)));
      emit(Opcode::TABGET, dest, staticsR, keyR);
      break;
    }
    // base.name read -> TABGET dest, baseR, keyR. A plain-variable base is
    // read in place (its named register is stable for the statement); only
    // compound bases need a temp (SPEC §bytecodes: avoid temporaries).
    int baseR = inPlaceReg(e->memberBase);
    if (baseR < 0)
    {
      baseR = tmpRegister();
      compileExpr(e->memberBase, baseR);
    }
    int keyR = tmpRegister();
    emit(Opcode::MOVC, keyR, emitConst(QLangValue::String(e->memberName)));
    emit(Opcode::TABGET, dest, baseR, keyR);
    break;
  }
  case ExprKind::Cast: {
    int src = tmpRegister();
    compileExpr(e->castOperand, src);
    emit(Opcode::CAST, dest, src, static_cast<int>(e->targetType));
    break;
  }
  case ExprKind::Ternary: {
    // cond ? trueBranch : falseBranch  (SPEC §operators "operator ?").
    // The condition goes to a temp; JMPIFFALSE branches to the false side.
    // Block branches lower to a closure call whose `return` yields the
    // ternary's value.
    int condReg = tmpRegister();
    compileExpr(e->cond, condReg);
    emit(Opcode::JMPIFFALSE, condReg, 0);
    size_t jumpFalseIdx = m_emitter.size() - 1;
    if (e->trueExpr)
      compileExpr(e->trueExpr, dest);
    else
      compileTernaryBlock(e->trueBlock, dest);
    emit(Opcode::JMP, 0);
    size_t jumpEndIdx = m_emitter.size() - 1;
    m_emitter.patchJump(jumpFalseIdx, static_cast<int32_t>(m_emitter.size()));
    if (e->falseExpr)
      compileExpr(e->falseExpr, dest);
    else
      compileTernaryBlock(e->falseBlock, dest);
    m_emitter.patchJump(jumpEndIdx, static_cast<int32_t>(m_emitter.size()));
    freeRegister(condReg);
    break;
  }
  case ExprKind::FString: {
    // f"literal ${expr} more" -> call format(template, expr1, expr2, ...)
    // CALL convention: args at dest+1..dest+argCount, result at dest.
    // So: reg[dest+1] = template, reg[dest+2..] = expressions.
    // Format specs are encoded into the template placeholder:
    //   no spec     -> %s
    //   ".2f"       -> %.2f
    //   "d"         -> %d
    std::string fmt;
    int exprCount = static_cast<int>(e->exprParts.size());
    for (int i = 0; i < exprCount; i++) {
      if (i < static_cast<int>(e->strParts.size()))
        fmt += e->strParts[i];

      // Build the printf placeholder from the format spec.
      std::string placeholder;
      if (i < static_cast<int>(e->formatSpecs.size())) {
        if (!e->formatSpecs[i].empty()) {
          // Python format spec -> printf spec.
          // ":.2f" -> "%.2f", ":d" -> "%d", ":f" -> "%f", ":10s" -> "%10s"
          const std::string &spec = e->formatSpecs[i];
          // Find where the conversion specifier starts (first alphabetic char).
          // e.g. ".2f" -> convStart=2, conversion="f", modifiers=".2"
          size_t convStart = 0;
          while (convStart < spec.size() && !std::isalpha(static_cast<unsigned char>(spec[convStart])))
            convStart++;
          if (convStart == spec.size()) {
            // No conversion specifier — treat as string.
            placeholder = "%s";
          } else {
            std::string conversion = spec.substr(convStart);
            std::string modifiers = spec.substr(0, convStart);
            // Handle width: ":10s" -> "%10s", ":>10" -> "%10s" (center-align not supported)
            int width = 0;
            bool leftAlign = false;
            bool zeroPad = false;
            bool altForm = false;
            size_t mi = 0;
            // Parse flags: #, 0, -, +, space
            while (mi < modifiers.size()) {
              char flag = modifiers[mi];
              if (flag == '#') {
                altForm = true;
                mi++;
              } else if (flag == '0' && width == 0) {
                // '0' is a zero-pad flag only when it appears before width digits
                zeroPad = true;
                mi++;
              } else if (flag == '-') {
                leftAlign = true;
                mi++;
              } else if (flag == '+' || flag == ' ') {
                // Sign flags — not used in placeholder generation but consume them
                mi++;
              } else {
                break;
              }
            }
            // Parse width digits
            while (mi < modifiers.size() && std::isdigit(static_cast<unsigned char>(modifiers[mi]))) {
              width = width * 10 + (modifiers[mi] - '0');
              mi++;
            }
            // Check for dot+precision (only for float/double)
            int precision = -1;
            if (mi < modifiers.size() && modifiers[mi] == '.') {
              mi++;
              precision = 0;
              while (mi < modifiers.size() && std::isdigit(static_cast<unsigned char>(modifiers[mi]))) {
                precision = precision * 10 + (modifiers[mi] - '0');
                mi++;
              }
            }
            // Build printf placeholder
            if (conversion == "f" || conversion == "e" || conversion == "g" ||
                conversion == "a" || conversion == "F" || conversion == "E" ||
                conversion == "G" || conversion == "A") {
              // Float: %[flags][width][.precision]f
              std::string flags;
              if (zeroPad) flags += '0';
              else if (!leftAlign && width > 0) flags += ' ';
              if (width > 0) {
                char wb[16];
                std::snprintf(wb, sizeof(wb), "%d", width);
                placeholder = "%" + flags + wb;
              } else {
                placeholder = "%" + flags;
              }
              if (precision >= 0) {
                char pb[16];
                std::snprintf(pb, sizeof(pb), ".%d", precision);
                placeholder += pb;
              }
              placeholder += conversion;
            } else if (conversion == "d" || conversion == "i" || conversion == "x" ||
                       conversion == "X" || conversion == "o" || conversion == "u") {
              // Integer: %[flags][width]d
              std::string flags;
              if (altForm) flags += '#';
              if (zeroPad && width > 0) flags += '0';
              else if (!leftAlign && width > 0) flags += ' ';
              if (width > 0) {
                char wb[16];
                std::snprintf(wb, sizeof(wb), "%d", width);
                placeholder = "%" + flags + wb + conversion;
              } else {
                placeholder = "%" + flags + conversion;
              }
            } else if (conversion == "s") {
              // String: %[flags][width]s
              std::string flags;
              if (!leftAlign && width > 0) flags += ' ';
              if (width > 0) {
                char wb[16];
                std::snprintf(wb, sizeof(wb), "%d", width);
                placeholder = "%" + flags + wb + "s";
              } else {
                placeholder = "%" + flags + "s";
              }
            } else {
              // Unknown conversion — fall back to %s
              placeholder = "%s";
            }
          }
        } else {
          // Empty format spec — default to %s.
          placeholder = "%s";
        }
      } else {
        placeholder = "%s";
      }
      fmt += placeholder;
    }
    if (static_cast<int>(e->strParts.size()) > exprCount)
      fmt += e->strParts.back();

    // Ensure enough registers: CALL reads dest+1..dest+argCount. Quarantine
    // the slot region (template + expr args) for the duration of the call.
    int totalArgs = exprCount + 1;
    beginArgSlots(dest, totalArgs);

    // Layout: dest+1=template, dest+2..dest+totalArgs=expr args.
    int templateReg = dest + 1;
    int exprStartReg = dest + 2;

    emit(Opcode::MOVC, templateReg, emitConst(QLangValue::String(fmt)));

    // Each interpolated expression lands directly in its slot (simple
    // operands without a temp; complex ones share one scratch register).
    compileArgsToSlots(e->exprParts, exprStartReg);

    emit(Opcode::CALL, dest, static_cast<int>(Builtin::Format), totalArgs);
    break;
  }
  }
}

void QLangCompiler::compileTernaryBlock(const std::vector<StmtPtr> &block,
                                        int dest) {
  // The block is a zero-arg closure: its `return` yields the ternary's value
  // (SPEC §operators "operator ?"). F3 removed implicit capture of enclosing
  // locals, so the block's free variables are passed as an explicit capture
  // list (the arrow-closure pattern); buildFunction augments it with global
  // consts, enums, and capturing def functions.
  std::unordered_set<std::string> allIds;
  for (auto &s : block)
    collectStmtIds(s, allIds);
  std::unordered_set<std::string> localVars;
  for (auto &s : block)
    collectLocalVars(s, localVars);
  std::vector<std::string> captures;
  for (const std::string &id : allIds) {
    if (localVars.count(id)) continue; // declared inside the block
    if (m_userFuncs.count(id)) continue; // buildFunction captures it if needed
    if (builtinId(id) >= 0) continue;
    if (m_nativeFuncIds.count(id)) continue;
    if (m_constVars.count(id)) continue; // visible everywhere
    if (m_enums.count(id)) continue; // resolves to a hidden table var
    if (m_classes.count(id)) continue; // compile-time, not a value
    // Top-level globals are invisible inside a function scope unless
    // imported via `global`; the fallback below skips them there (no
    // register, no capture) and the body compile raises the clean error.
    // At script level they have live registers and are captured like any
    // local: the block is inline in the current scope.
    if (regOf(id) < 0 && m_captureRegs.find(id) == m_captureRegs.end())
      continue; // unresolvable here: the body compile raises the clean error
    captures.push_back(id);
  }
  int funcIndex = buildFunction("ternary", {}, captures, block);
  // buildFunction just pushed it: it is the last element of m_functions
  // (the returned funcIndex is the final program index, main prepended at
  // 0, so it is NOT a valid m_functions index).
  const Function &built = m_functions.back();
  auto closure = std::make_shared<Closure>();
  closure->funcIndex = funcIndex;
  for (const std::string &cap : built.captureNames) {
    int reg = regOf(cap);
    if (reg < 0) {
      // The capture is itself a free variable of this scope (transitive
      // capture): load it into a hidden named register so the cell can
      // alias it for the frame's lifetime.
      auto cit = m_captureRegs.find(cap);
      if (cit != m_captureRegs.end()) {
        reg = declareVar(std::string("__cap_") + cap);
        emit(Opcode::REFLOAD, reg, cit->second, 0);
      }
    }
    if (reg < 0)
      reg = 0; // preserve legacy binding for unresolvable names
    CaptureRef cr;
    cr.reg = reg;
    closure->captures.push_back(cr);
  }
  int closureReg = tmpRegister();
  int constIdx = emitConst(QLangValue::Function(closure));
  emit(Opcode::MAKEFUNC, closureReg, constIdx);
  emit(Opcode::CALLCLOS, closureReg, dest, 0);
  freeRegister(closureReg);
}

int QLangCompiler::builtinId(const std::string &name) const {
  if (name == "toStr" || name == "str") return static_cast<int>(Builtin::ToStr);
  if (name == "toInt" || name == "int") return static_cast<int>(Builtin::ToInt);
  if (name == "type") return static_cast<int>(Builtin::Type);
  if (name == "print") return static_cast<int>(Builtin::Print);
  if (name == "println") return static_cast<int>(Builtin::Println);
  if (name == "printf") return static_cast<int>(Builtin::Printf);
  if (name == "len") return static_cast<int>(Builtin::Len);
  if (name == "blob") return static_cast<int>(Builtin::Blob);
  if (name == "substr") return static_cast<int>(Builtin::Substr);
  if (name == "slice") return static_cast<int>(Builtin::Slice);
  if (name == "range") return static_cast<int>(Builtin::Range);
  if (name == "join") return static_cast<int>(Builtin::Join);
  if (name == "isset") return static_cast<int>(Builtin::IsSet);
  if (name == "sprintf") return static_cast<int>(Builtin::Sprintf);
  if (name == "format") return static_cast<int>(Builtin::Format);
  if (name == "assert") return static_cast<int>(Builtin::Assert);
  if (name == "next") return static_cast<int>(Builtin::Next);
  if (name == "set") return static_cast<int>(Builtin::Set);
  if (name == "set_max_steps") return static_cast<int>(Builtin::SetMaxSteps);
  if (name == "get_max_steps") return static_cast<int>(Builtin::GetMaxSteps);
  if (name == "release") return static_cast<int>(Builtin::Release);
  return -1;
}

QLangValue QLangCompiler::evalConstExpr(const ExprPtr &e) {
  if (!e)
    throw QLangError(QLangErrorKind::Parse, 0, 0, "null array/table element");
  switch (e->kind) {
  case ExprKind::Literal:
    return e->literal;
  case ExprKind::Array: {
    ArrayValue elems;
    for (auto &el : e->elements)
      elems.push_back(evalConstExpr(el));
    return QLangValue::Array(std::move(elems));
  }
  case ExprKind::Table: {
    TableValue kv = std::make_shared<std::map<std::string, QLangValue>>();
    for (auto &item : e->items)
      kv->operator[](evalConstKey(item.first)) = evalConstExpr(item.second);
    return QLangValue::Table(kv);
  }
  default:
    throw QLangError(QLangErrorKind::Parse, 0, 0,
                     "non-constant element in array/table literal");
  }
}

QLangValue::Type QLangCompiler::inferType(const ExprPtr &e) const {
  if (!e)
    return QLangValue::Type::Nil;
  switch (e->kind) {
  case ExprKind::Array: return QLangValue::Type::Array;
  case ExprKind::Table: return QLangValue::Type::Table;
  case ExprKind::Literal: return e->literal.type();
  case ExprKind::Variable: {
    auto it = m_varTypes.find(e->name);
    return it == m_varTypes.end() ? QLangValue::Type::Nil : it->second;
  }
  default: return QLangValue::Type::Nil;
  }
}

void QLangCompiler::updateVarType(const std::string &name, const ExprPtr &e) {
  if (!e)
    return;
  QLangValue::Type t = inferType(e);
  if (t == QLangValue::Type::Array || t == QLangValue::Type::Table)
    m_varTypes[name] = t;
}

void QLangCompiler::compileAssignTarget(const ExprPtr &target, const Token &op,
                                        const ExprPtr &value) {
  const bool isCompound = (op.text != "=");
  if (target->kind == ExprKind::Index) {
    // base is a variable; use its register directly so the mutation persists.
    const std::string &baseName = target->indexBase->name;
    int baseR;
    // Base may be a captured variable: load it through the reference so the
    // read-modify-write mutates the shared (enclosing) array.
    auto cit = m_captureRegs.find(baseName);
    if (cit != m_captureRegs.end()) {
      baseR = tmpRegister();
      emit(Opcode::REFLOAD, baseR, cit->second, 0);
    } else {
      baseR = regOf(baseName);
      if (baseR < 0)
        baseR = declareVar(baseName); // bare `ab[1]=5` auto-declares ab
    }
    int idxR = tmpRegister();
    if (target->indexExpr)
      compileExpr(target->indexExpr, idxR);
    else
      emitImm(Opcode::MOVI, idxR, 0); // ab[] -> append
    int valR = tmpRegister();
    if (target->indexExpr) {
      // read-modify-write for compound ops (e.g. a[i] += v)
      int tmpR = tmpRegister();
      emit(Opcode::ARRGET, tmpR, baseR, idxR);
      Opcode opCode = (op.text == "++") ? Opcode::INC
                      : (op.text == "--") ? Opcode::DEC
                                          : binOp(op.text.substr(0, op.text.size() - 1));
      if ((opCode == Opcode::ADD || opCode == Opcode::SUB) && isIntLiteral(value)) {
        // `a[i] += 12` / `a[i] -= 12` -> ADDI/SUBI in-place (SPEC §bytecodes:
        // new OpCode addi/subi).
        emitImm(opCode == Opcode::ADD ? Opcode::ADDI : Opcode::SUBI, tmpR,
                value->literal.asInt());
      } else {
        compileExpr(value, valR);
        emit(opCode, tmpR, tmpR, valR);
      }
      emit(Opcode::ARRPUT, baseR, idxR, tmpR);
    } else {
      compileExpr(value, valR);
      // empty subscript ab[] = v -> append
      emit(Opcode::ARRAPP, baseR, valR, 0);
    }
  } else if (target->kind == ExprKind::Member) {
    const std::string &baseName = target->memberBase->name;
    int baseR;
    auto cit = m_captureRegs.find(baseName);
    if (cit != m_captureRegs.end()) {
      baseR = tmpRegister();
      emit(Opcode::REFLOAD, baseR, cit->second, 0);
    } else {
      baseR = regOf(baseName);
      if (baseR < 0)
        baseR = declareVar(baseName);
    }
    int keyR = tmpRegister();
    emit(Opcode::MOVC, keyR, emitConst(QLangValue::String(target->memberName)));
    int valR = tmpRegister();
    if (isCompound) {
      int tmpR = tmpRegister();
      emit(Opcode::TABGET, tmpR, baseR, keyR);
      Opcode opCode = (op.text == "++") ? Opcode::INC
                      : (op.text == "--") ? Opcode::DEC
                                          : binOp(op.text.substr(0, op.text.size() - 1));
      if ((opCode == Opcode::ADD || opCode == Opcode::SUB) && isIntLiteral(value)) {
        // `t.k += 12` / `t.k -= 12` -> ADDI/SUBI in-place (SPEC §bytecodes:
        // new OpCode addi/subi).
        emitImm(opCode == Opcode::ADD ? Opcode::ADDI : Opcode::SUBI, tmpR,
                value->literal.asInt());
      } else {
        compileExpr(value, valR);
        emit(opCode, tmpR, tmpR, valR);
      }
      emit(Opcode::TABPUT, baseR, keyR, tmpR);
    } else {
      compileExpr(value, valR);
      emit(Opcode::TABPUT, baseR, keyR, valR);
    }
  }
}

Opcode QLangCompiler::binOp(const std::string &text) const {
  if (text == "==") return Opcode::EQ;
  if (text == "!=") return Opcode::NE;
  if (text == "<") return Opcode::LT;
  if (text == ">") return Opcode::GT;
  if (text == "<=") return Opcode::LE;
  if (text == ">=") return Opcode::GE;
  if (text == "+") return Opcode::ADD;
  if (text == "-") return Opcode::SUB;
  if (text == "*") return Opcode::MUL;
  if (text == "/") return Opcode::DIV;
  if (text == "&") return Opcode::AND;
  if (text == "|") return Opcode::OR;
  if (text == "^") return Opcode::XOR;
  if (text == "<<") return Opcode::SHL;
  if (text == ">>") return Opcode::SHR;
  if (text == "&&") return Opcode::LAND;
  if (text == "||") return Opcode::LOR;
  if (text == "**") return Opcode::POW;
  if (text == "%") return Opcode::MOD;
  if (text == "in") return Opcode::INx;
  throw QLangError(QLangErrorKind::Parse, 0, 0, "unsupported operator '" + text + "'");
}

Opcode QLangCompiler::unOp(const std::string &text) const {
  if (text == "--") return Opcode::DEC;
  if (text == "++") return Opcode::INC;
  if (text == "-") return Opcode::NEG;
  if (text == "!") return Opcode::NOT;
  if (text == "$") return Opcode::EVALKEY;
  throw QLangError(QLangErrorKind::Parse, 0, 0, "unsupported unary operator '" + text + "'");
}

void QLangCompiler::guardNotConst(const std::string &name,
                                  const char *op) const {
  if (m_constVars.count(name))
    throw QLangError(QLangErrorKind::Parse, 0, 0,
                     std::string("cannot ") + op + " to constant '" + name + "'");
}

bool QLangCompiler::containsYield(const StmtPtr &s) {
  if (!s)
    return false;
  if (s->kind == StmtKind::Yield)
    return true;
  // Recurse into block-bearing statement types.
  switch (s->kind) {
  case StmtKind::For:
    for (auto &st : s->body)
      if (containsYield(st))
        return true;
    return false;
  case StmtKind::ForIn:
    for (auto &st : s->body)
      if (containsYield(st))
        return true;
    return false;
  case StmtKind::While:
    for (auto &st : s->whileBody)
      if (containsYield(st))
        return true;
    return false;
  case StmtKind::If:
    for (auto &st : s->ifBody)
      if (containsYield(st))
        return true;
    for (auto &st : s->elseBody)
      if (containsYield(st))
        return true;
    return false;
  case StmtKind::Def:
    for (auto &st : s->body)
      if (containsYield(st))
        return true;
    return false;
  case StmtKind::Arrow:
    for (auto &st : s->body)
      if (containsYield(st))
        return true;
    return false;
  case StmtKind::Switch:
    for (auto &c : s->cases)
      for (auto &st : c.body)
        if (containsYield(st))
          return true;
    for (auto &st : s->defaultBody)
      if (containsYield(st))
        return true;
    return false;
  default:
    return false;
  }
}

} // namespace qlang
