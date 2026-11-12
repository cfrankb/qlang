#include "qlang/QLangCompiler.h"

#include <algorithm>
#include <climits>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <vector>

#include "qlang/QLangBytecodeIO.h"
#include "qlang/QLangErrors.h"

namespace qlang {

namespace {

// Addressing width of an operand index. The emitter never uses the 4-byte
// form: register and constant indices stay capped at 65 535.
OperandWidth indexWidth(int32_t index, const char *message) {
  if (index < 0 || index > 65535)
    throw QLangError(QLangErrorKind::Runtime, 0, 0, message);
  return (index <= 255) ? OperandWidth::Byte : OperandWidth::Word;
}

// Narrowest signed immediate form holding `v`. The emitter never produces a
// 64-bit literal (plan decision D3), so anything past the 4-byte form is an
// error rather than a widening.
ImmForm narrowestSignedForm(int64_t v) {
  if (v >= -128 && v <= 127) return ImmForm::I8;
  if (v >= -32768 && v <= 32767) return ImmForm::I16;
  if (v >= INT32_MIN && v <= INT32_MAX) return ImmForm::I32;
  throw QLangError(QLangErrorKind::Runtime, 0, 0,
                   "immediate out of int32 range");
}

} // namespace

// ---------------------------------------------------------------------------
// Raw instruction emitter: accumulates instructions in a vector, then
// encodes them into the flat byte stream at finish() time. (The struct
// itself lives in QLangCompiler.h so QLangCompiler can hold one by value.)
// ---------------------------------------------------------------------------

// Encode a raw instruction stream into a function body. Split out of
// FnEmitter::finish() so the linker can re-encode a stream it decoded out of a
// `.qlc` image and rewrote (external-modules plan v4 Phase 3b, Q32): operand
// widths are chosen once, here, so a merged index that needs a wider operand
// costs nothing extra and no encoded byte is ever patched in place.
void encodeRawStream(Function &fn, const std::vector<RawInstr> &raw) {
  // Validate jump targets.
  for (size_t i = 0; i < raw.size(); i++) {
    if (raw[i].jumpTarget >= 0 &&
        raw[i].jumpTarget >= static_cast<int32_t>(raw.size()))
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "invalid jump target in emitter");
  }

  // Initial sizes: jumps assume the widest form (jmp32); everything else uses
  // the size of the form its operands/immediate actually require (base or
  // escaped).
  std::vector<size_t> sizes(raw.size());
  for (size_t i = 0; i < raw.size(); i++) {
    if (raw[i].jumpTarget >= 0)
      sizes[i] = 6;
    else {
      auto [mod, escaped] = FnEmitter::computeModifier(raw[i]);
      sizes[i] = instructionSize(raw[i].op, escaped, mod);
    }
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
      size_t newSize = minJumpSize(offset);
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
    size_t offWidth = sizes[i];
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
      auto [mod, escaped] = FnEmitter::computeModifier(raw[i]);
      di.escaped = escaped;
      di.modifier = mod;
      // The encoder re-reads the kind and the form from the modifier; mirror
      // them back into the instruction so it describes the bytes it produced.
      di.rightKind =
          escaped ? modifierRightKind(mod) : defaultRightKind(raw[i].op);
      if (di.rightKind == RightKind::Immediate) {
        di.immPayload = raw[i].payload;
        di.immForm = escaped ? static_cast<ImmForm>(modifierRightForm(mod))
                             : ImmForm::I8;
      }
      di.immUnsigned = raw[i].immUnsigned;
      di.immFloat = raw[i].immFloat;
    }
    encodeInstruction(fn.code, di);
  }

  fn.instrOffsets = std::move(offsets);
  fn.loc.resize(raw.size());
  for (size_t i = 0; i < raw.size(); i++)
    fn.loc[i] = {raw[i].line, raw[i].col};
}

// The inverse of encodeRawStream(): decode a function body back into raw
// instructions. Jump offsets (relative to the end of the jump) become
// instruction indices again, and the line/col side table is folded back into
// each record, so a decoded stream round-trips through encodeRawStream() to
// the same bytes unless a rewrite changed an operand.
void decodeRawStream(const Function &fn, std::vector<RawInstr> &raw) {
  raw.clear();
  raw.reserve(fn.instrOffsets.size());
  // Byte offset -> instruction index, for resolving relative jump targets.
  std::unordered_map<size_t, int32_t> byOffset;
  byOffset.reserve(fn.instrOffsets.size() * 2);
  for (size_t i = 0; i < fn.instrOffsets.size(); i++)
    byOffset[fn.instrOffsets[i]] = static_cast<int32_t>(i);

  for (size_t i = 0; i < fn.instrOffsets.size(); i++) {
    DecodedInstruction di;
    decodeInstruction(fn.code.data(), fn.instrOffsets[i], fn.code.size(), di);
    RawInstr r;
    r.op = di.op;
    r.a = di.a;
    r.b = di.b;
    r.c = di.c;
    r.kind = di.rightKind;
    r.payload = di.immPayload;
    r.form = di.immForm;
    r.imm = di.imm;
    r.immUnsigned = di.immUnsigned;
    r.immFloat = di.immFloat;
    if (isJumpOpcode(di.op)) {
      const size_t target =
          fn.instrOffsets[i] + di.size +
          static_cast<size_t>(static_cast<int64_t>(di.jumpOffset));
      auto it = byOffset.find(target);
      if (it == byOffset.end())
        throw QLangError(QLangErrorKind::Runtime, 0, 0,
                         "jump target is not an instruction boundary");
      r.jumpTarget = it->second;
    }
    if (i < fn.loc.size()) {
      r.line = fn.loc[i].first;
      r.col = fn.loc[i].second;
    }
    raw.push_back(r);
  }
}

void FnEmitter::finish(Function &fn) { encodeRawStream(fn, raw); }

ImmForm FnEmitter::immFormOf(const RawInstr &r) {
  if (r.form != ImmForm::Reserved)
    return r.form;
  return narrowestSignedForm(r.imm);
}

std::pair<uint8_t, bool> FnEmitter::computeModifier(const RawInstr &r) {
  // CALL: b is a signed 16-bit function ID (negative = native, index =
  // -1 - b; non-negative = builtin), not a register. Always escaped: only a
  // modifier byte can say that the right operand is a signed 16-bit immediate
  // (plan §2.8).
  if (r.op == Opcode::CALL) {
    OperandWidth left = indexWidth(r.a, "register index out of range");
    if (r.b < INT16_MIN || r.b > INT16_MAX)
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "function ID out of range");
    if (r.c < 0 || r.c > 255)
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "argument count out of range");
    return {makeModifier(left, RightKind::Immediate,
                         static_cast<uint8_t>(ImmForm::I16)),
            true};
  }

  OperandWidth left = indexWidth(r.a, "register index out of range");
  size_t nOps = baseOperandCount(r.op);

  // `nop` has no operand at all, so it cannot carry a modifier either.
  if (nOps == 0)
    return {0, false};

  // A single-operand opcode has no right operand, so the modifier can only
  // widen the left index; the kind and form fields must stay at their base
  // values or the codec rejects the instruction.
  if (nOps == 1) {
    if (left == OperandWidth::Byte)
      return {0, false};
    return {makeModifier(left, RightKind::Register, 0), true};
  }

  // A base form carries no modifier, so it can only mean the opcode's own
  // right-operand kind with 1-byte operands and, for an immediate, the signed
  // 1-byte form. A kind the emitter chose explicitly (emitRight) is therefore
  // base only when it happens to be the opcode's own: a pool `mov` names a kind
  // the opcode would never mean on its own, so it needs a modifier to say so.
  // Anything else has to be announced by a modifier too, and a modifier of 0 is
  // a legal escaped modifier, hence the explicit flag.
  bool base = r.kind == defaultRightKind(r.op) && left == OperandWidth::Byte;
  if (r.kind == RightKind::Immediate) {
    ImmForm form = immFormOf(r);
    if (base && form == ImmForm::I8)
      return {0, false};
    return {makeModifier(left, r.kind, static_cast<uint8_t>(form)), true};
  }

  // Register / constant-pool right operand: the form field is its width.
  OperandWidth right = indexWidth(
      r.b, r.kind == RightKind::ConstPool ? "constant index out of range"
                                          : "register index out of range");
  // The third operand is in the stream only while the arity rule leaves the
  // 3-operand form in place (plan §2.6).
  if (nOps == 3 && !isInPlaceForm(r.op, r.kind) && (r.c < 0 || r.c > 255))
    throw QLangError(QLangErrorKind::Runtime, 0, 0,
                     "third operand must be a 1-byte register");
  if (base && right == OperandWidth::Byte)
    return {0, false};
  return {makeModifier(left, r.kind, static_cast<uint8_t>(right)), true};
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

// Forward declarations: defined further down, used by the encoding-guideline
// helpers below (inPlaceReg/foldSelfArith/compileArgsToSlots).
static std::optional<LiteralImm> literalImmOf(const QLangValue &v);
static bool isIntLiteral(const ExprPtr &e);
static std::optional<LiteralImm> foldNegLiteral(const ExprPtr &e);

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

void QLangCompiler::addExternalEnums(const std::unordered_map<std::string,
                                                          std::vector<std::pair<std::string, QLangValue>>> &enums) {
  // Host-injected enums (QLangContext::registerEnum): the runtime table is
  // already seeded as the hidden const var `__enum_<Name>`; register the
  // name -> member values so `E::M`/`E.M` fold and `E` resolves. Injected
  // enums are global (visible inside functions, like global consts).
  for (const auto &[name, members] : enums) {
    m_enums[name] = members;
    m_globalEnums.insert(name);
    m_injectedEnumNames.insert(name);
  }
}

namespace {

// Manifest lookups for the P4 compile-time checks (plan
// docs/plans/portable-module-plan.md Phase 4). Message style follows the
// runtime wording so compile-time and runtime report alike.
const ManifestMember *manifestMember(const ManifestType &t,
                                     const std::string &name, int kind) {
  for (const ManifestMember &m : t.members)
    if (m.name == name && (kind < 0 || m.kind == kind))
      return &m;
  return nullptr;
}

const ManifestSignal *manifestSignal(const ManifestType &t,
                                     const std::string &name) {
  for (const ManifestSignal &s : t.signals)
    if (s.name == name)
      return &s;
  return nullptr;
}

const char *qTagName(QTypeTag t) {
  switch (t) {
  case QTypeTag::Bool: return "Bool";
  case QTypeTag::Char: return "Char";
  case QTypeTag::Short: return "Short";
  case QTypeTag::Int: return "Int";
  case QTypeTag::Long: return "Long";
  case QTypeTag::Byte: return "Byte";
  case QTypeTag::Word: return "Word";
  case QTypeTag::Dword: return "Dword";
  case QTypeTag::Qword: return "Qword";
  case QTypeTag::Float: return "Float";
  case QTypeTag::Str: return "Str";
  default: return "?";
  }
}

// The inferred-literal side of the as<T>() acceptance matrix (QLangValue):
// only these four types ever come out of inferType on a literal.
std::optional<QTypeTag> qTagOfInferred(QLangValue::Type t) {
  switch (t) {
  case QLangValue::Type::Int: return QTypeTag::Int;
  case QLangValue::Type::Float: return QTypeTag::Float;
  case QLangValue::Type::String: return QTypeTag::Str;
  case QLangValue::Type::Bool: return QTypeTag::Bool;
  default: return std::nullopt;
  }
}

bool qTagAccepted(QTypeTag exp, QTypeTag got) {
  if (exp == got)
    return true;
  switch (exp) {
  case QTypeTag::Bool: // as<bool>: Bool/Char/Int
    return got == QTypeTag::Int;
  case QTypeTag::Char:
  case QTypeTag::Short:
  case QTypeTag::Int: // as<int32_t>: Int/Bool/Char storage
    return got == QTypeTag::Bool;
  case QTypeTag::Long: // as<int64_t>: any numeric
    return got == QTypeTag::Bool || got == QTypeTag::Int ||
           got == QTypeTag::Float;
  case QTypeTag::Float: // as<double>: any numeric
    return got == QTypeTag::Int || got == QTypeTag::Bool;
  default: // Str exact; Unknown/Void/External never reach here
    return false;
  }
}

// Single-write scan (plan P4 check 2): counts every plain-variable write in
// the module AST — declaration, plain assignment, delete, loop var — walking
// INTO def/arrow/class bodies, because a def may write a global through
// `global`. A name with exactly one write, whose initializer is a manifest
// constructor call, is a statically known receiver; anything else stays
// lenient.
struct ExtWriteScan {
  std::unordered_map<std::string, const Stmt *> single;
  std::unordered_set<std::string> poisoned;
  void write(const std::string &name, const Stmt *s) {
    if (name.empty() || poisoned.count(name))
      return;
    if (!single.emplace(name, s).second) {
      poisoned.insert(name);
      single.erase(name);
    }
  }
  void walk(const std::vector<StmtPtr> &stmts) {
    for (const StmtPtr &s : stmts) {
      if (!s)
        continue;
      switch (s->kind) {
      case StmtKind::Declaration:
      case StmtKind::Const:
      case StmtKind::Arrow:
      case StmtKind::Def:
        write(s->name, s.get());
        break;
      case StmtKind::Assignment:
        if (!s->target) // member/index targets write the base, not the name
          write(s->name, s.get());
        break;
      case StmtKind::Delete:
        write(s->delName, nullptr);
        break;
      case StmtKind::For:
      case StmtKind::ForIn:
        write(s->loopVar, nullptr);
        break;
      default:
        break;
      }
      walk(s->body);
      walk(s->whileBody);
      walk(s->ifBody);
      walk(s->elseBody);
      walk(s->classBody);
      walk(s->defaultBody);
      for (const auto &c : s->cases)
        walk(c.body);
    }
  }
};

} // namespace

void QLangCompiler::addExternalTypes(const std::vector<ManifestType> &types) {
  for (const ManifestType &t : types) {
    if (m_extTypes.count(t.name))
      throw QLangError(QLangErrorKind::Parse, 0, 0,
                       "duplicate external type '" + t.name + "' in manifest");
    m_extTypeStore.push_back(t);
    const ManifestType *rec = &m_extTypeStore.back();
    m_extTypes[t.name] = rec;
    // Bare `T` resolves through the host-injected-external-var path (declare
    // on first use); the VM seeds the TypeRef from ctx when the module runs
    // on a host with the bindings — codegen is unchanged.
    m_externalVars.insert(t.name);
    // `T(args)` compiles as the constructor native, and the slot is taken at
    // the call: compileCallExpr's native fallback consults `m_extTypes` (plan
    // native-slot-portability P2), so a type nothing constructs asks for
    // nothing — not in the image (P1) and not as a native this compile knows
    // about. A host compile already injected the name, so existing ids win.
    //
    // Enum tables fold exactly like registerEnum-injected ones (Round 41).
    // A name present in both sources must agree value-for-value, or the
    // host and its dump have drifted: compile fails (plan P4 check 4).
    for (const EnumMeta &em : t.enums) {
      std::vector<std::pair<std::string, QLangValue>> members;
      for (const EnumEntry &entry : em.entries)
        members.emplace_back(entry.name,
                             QLangValue::Int(static_cast<int32_t>(entry.value)));
      auto it = m_enums.find(em.typeName);
      if (it != m_enums.end()) {
        for (const auto &m : members) {
          bool agreed = false;
          for (const auto &h : it->second)
            if (h.first == m.first &&
                h.second.type() == QLangValue::Type::Int &&
                h.second.asInt() == m.second.asInt())
              agreed = true;
          if (!agreed)
            throw QLangError(
                QLangErrorKind::Parse, 0, 0,
                "enum '" + em.typeName + "' member '" + m.first +
                    "' disagrees with external type '" + t.name + "'");
        }
        continue;
      }
      m_enums[em.typeName] = std::move(members);
      m_globalEnums.insert(em.typeName);
      m_injectedEnumNames.insert(em.typeName);
    }
  }
}

void QLangCompiler::collectKnownReceivers() {
  m_knownReceivers.clear();
  if (m_extTypes.empty())
    return;
  ExtWriteScan scan;
  scan.walk(m_program);
  for (const auto &[name, s] : scan.single) {
    if (!s || s->kind == StmtKind::Arrow || s->kind == StmtKind::Def)
      continue;
    const ExprPtr &init =
        s->kind == StmtKind::Assignment ? s->assignValue : s->value;
    if (!init || init->kind != ExprKind::Call || init->calleeMember)
      continue;
    auto it = m_extTypes.find(init->callee);
    if (it == m_extTypes.end() || !it->second->hasCtor)
      continue;
    m_knownReceivers[name] = it->second;
  }
}

void QLangCompiler::checkExtArgTags(const std::vector<QTypeTag> &tags,
                                    const std::vector<ExprPtr> &args,
                                    const std::string &what) const {
  for (size_t i = 0; i < tags.size(); i++) {
    if (tags[i] == QTypeTag::Unknown || tags[i] == QTypeTag::Void ||
        tags[i] == QTypeTag::External)
      continue; // the binding accepts anything here — stay silent
    auto got = qTagOfInferred(inferType(args[i]));
    if (!got || qTagAccepted(tags[i], *got))
      continue; // dynamic argument: the runtime check remains authoritative
    throw QLangError(QLangErrorKind::Parse, m_curLine, m_curCol,
                     "type mismatch: " + what + " argument " +
                         std::to_string(i + 1) + " expects " +
                         qTagName(tags[i]) + ", got " + qTagName(*got));
  }
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

std::map<std::string, int> QLangCompiler::nativeSlotIds() const {
  // The `.qlc` native-slot table is the natives the program calls, not the ones the
  // compile was told about: `loadProgram` resolves every entry and refuses the image
  // for a name the context cannot serve, so an unused reservation would become a load
  // requirement for whoever loads it (docs/SERIALIZATION.md §5.2, plan
  // native-slot-portability P1). Ordered by name, as §4.3 asks.
  std::map<std::string, int> slots;
  for (const auto &[name, id] : m_nativeFuncIds)
    if (m_usedNativeIds.count(id))
      slots.emplace(name, id);
  return slots;
}

void QLangCompiler::addModules(const std::vector<QLangContext::Module> &modules) {
  // Records the exported names and the modules themselves: no native slot is
  // assigned here, because a module's functions are reachable only through the
  // module (or an import form that binds the name flatly), so an unused module
  // contributes nothing to the program's native-slot table.
  for (const auto &mod : modules)
    for (const auto &[name, _] : mod.funcs)
      m_moduleFuncNames.insert(name);
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
  emitImmNil(Opcode::MOV, r);
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
  // `x = x + k` / `x = x - k` -> inc/dec/add/sub in place (SPEC
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
      emitImmI(Opcode::ADD, r, static_cast<int32_t>(k));
  } else {
    if (k == 1)
      emit(Opcode::DEC, r, r);
    else
      emitImmI(Opcode::SUB, r, static_cast<int32_t>(k));
  }
  return true;
}

// True when compiling `e` never emits a CALL. A call reads its arguments from
// dest+1..dest+argCount, so a call-shaped expression must not be compiled to a
// destination inside an active call-arg slot range: its own arg slots would
// land on the enclosing call's remaining slots. Every other expression shape
// writes its destination register directly, so it can land in the slot itself.
static bool isCallFree(const ExprPtr &e) {
  if (!e)
    return true;
  switch (e->kind) {
  case ExprKind::Literal:
  case ExprKind::Variable:
    return true;
  case ExprKind::Binary:
    return isCallFree(e->left) && isCallFree(e->right);
  case ExprKind::Unary:
    return isCallFree(e->operand);
  case ExprKind::Array:
    for (const auto &el : e->elements)
      if (!isCallFree(el))
        return false;
    return true;
  case ExprKind::Table:
    for (const auto &[k, v] : e->items)
      if (!isCallFree(k) || !isCallFree(v))
        return false;
    return true;
  case ExprKind::Index:
    return isCallFree(e->indexBase) && isCallFree(e->indexExpr);
  case ExprKind::Member:
    return isCallFree(e->memberBase);
  case ExprKind::Cast:
    return isCallFree(e->castOperand);
  default:
    return false; // Call, Closure, FString, Ternary
  }
}

void QLangCompiler::compileArgsToSlots(const std::vector<ExprPtr> &args,
                                       int slotBase) {
  // Arguments land in their slot by increasing cost: a plain variable or a
  // literal is stored directly; any other expression that cannot emit a CALL
  // (isCallFree), or the last argument of the list, compiles straight into the
  // slot; everything else shares a single scratch register (allocated on first
  // use) so an N-arg call costs at most one temp instead of N (SPEC §bytecodes:
  // encoding guidelines — universal scratch pad register).
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
    // A literal lands in its slot as one inline immediate when its type has a
    // form for it (int/float/bool/nil), otherwise as one pool read.
    if (arg->kind == ExprKind::Literal) {
      emitLiteral(Opcode::MOV, slot, arg->literal);
      continue;
    }
    // `-(literal)` folds to one immediate straight into the slot — no temp, no
    // MOV (SPEC §bytecodes: encoding guidelines — avoid temporary registers).
    auto negFold = foldNegLiteral(arg);
    if (negFold) {
      emitLiteralImm(Opcode::MOV, slot, *negFold);
      continue;
    }
    // An argument that can be compiled straight into its slot needs neither a
    // scratch nor a copy: a call-free expression writes its destination
    // directly, and the last argument's nested call spills its own slots above
    // the range (every enclosing range sits below the call's dest, so nothing
    // of this call lives there). Landing in the slot also leaves no stale copy
    // behind, which is what Phase 3 H2 needs: `release(x)` must drop the last
    // reference (SPEC §bytecodes: encoding guidelines — avoid temporary
    // registers).
    int target = slot;
    if (!isCallFree(arg) && i + 1 < args.size()) {
      if (scratch < 0)
        scratch = tmpRegister();
      target = scratch;
    }
    compileExpr(arg, target);
    if (slot != target)
      emit(Opcode::MOV, slot, target, 0);
  }
  // Clear the shared scratch so it does not keep a reference alive past the
  // call (Phase 3 H2: release() must drop the last reference; a leaked temp
  // would retain the owned external).
  if (scratch >= 0)
    releaseTemp(scratch);
}

int QLangCompiler::emitConst(const QLangValue &value) {
  int idx = m_pool.add(value);
  std::string valStr;
  if (value.type() == QLangValue::Type::Int)
    valStr = std::to_string(value.asInt());
  else if (value.type() == QLangValue::Type::String)
    valStr = value.asString();
  else
    valStr = std::string("(type=") + std::to_string((int)value.type()) + ")";
  trace(TRACE_FULL, "[DBG-CONST] Adding constant type=%d value=%s -> index=%d (pool size=%zu)\n",
        (int)value.type(), valStr.c_str(), idx, m_pool.size());
  return idx;
}

void QLangCompiler::emit(Opcode op, int a, int b, int c) {
  uint16_t line = static_cast<uint16_t>(m_curLine);
  uint16_t col = static_cast<uint16_t>(m_curCol);
  if (isJumpOpcode(op)) {
    // Every jump the compiler emits carries its target in `a`. The jump class
    // has no condition-register form: a register test lowers to `tst` + a
    // flag jump (emitJumpIfTrue/emitJumpIfFalse), so no jump carries a second
    // operand (plan docs/plans/phase3-opcode-retirement-plan.md S1).
    m_emitter.emitJump(op, a, 0, line, col);
    return;
  }
  m_emitter.emit(op, a, b, c, 0, line, col);
}

void QLangCompiler::emitImmI(Opcode op, int a, int32_t imm) {
  RawInstr r;
  r.kind = RightKind::Immediate;
  r.payload = ImmPayload::Signed; // form picked by narrowestSignedForm
  r.imm = imm;
  m_emitter.emitRight(op, a, r, static_cast<uint16_t>(m_curLine),
                      static_cast<uint16_t>(m_curCol));
}

void QLangCompiler::emitImmF(Opcode op, int a, double value) {
  RawInstr r;
  r.kind = RightKind::Immediate;
  r.form = ImmForm::Float;
  r.payload = ImmPayload::Float;
  r.immFloat = value;
  m_emitter.emitRight(op, a, r, static_cast<uint16_t>(m_curLine),
                      static_cast<uint16_t>(m_curCol));
}

void QLangCompiler::emitImmB(Opcode op, int a, bool value) {
  RawInstr r;
  r.kind = RightKind::Immediate;
  r.form = value ? ImmForm::True : ImmForm::False;
  r.payload = value ? ImmPayload::True : ImmPayload::False;
  m_emitter.emitRight(op, a, r, static_cast<uint16_t>(m_curLine),
                      static_cast<uint16_t>(m_curCol));
}

void QLangCompiler::emitImmNil(Opcode op, int a) {
  RawInstr r;
  r.kind = RightKind::Immediate;
  r.form = ImmForm::Nil;
  r.payload = ImmPayload::Nil;
  m_emitter.emitRight(op, a, r, static_cast<uint16_t>(m_curLine),
                      static_cast<uint16_t>(m_curCol));
}

void QLangCompiler::emitPool(Opcode op, int a, int poolIdx) {
  RawInstr r;
  r.kind = RightKind::ConstPool;
  r.b = poolIdx;
  m_emitter.emitRight(op, a, r, static_cast<uint16_t>(m_curLine),
                      static_cast<uint16_t>(m_curCol));
}

void QLangCompiler::emitLiteralImm(Opcode op, int a, const LiteralImm &imm) {
  switch (imm.payload) {
  case ImmPayload::Float:
    emitImmF(op, a, imm.dbl);
    return;
  case ImmPayload::True:
    emitImmB(op, a, true);
    return;
  case ImmPayload::False:
    emitImmB(op, a, false);
    return;
  case ImmPayload::Nil:
    emitImmNil(op, a);
    return;
  case ImmPayload::None:
  case ImmPayload::Unsigned:
    break; // never produced by literalImmOf / foldNegLiteral
  }
  emitImmI(op, a, static_cast<int32_t>(imm.value)); // ImmPayload::Signed
}

void QLangCompiler::emitLiteral(Opcode op, int a, const QLangValue &value) {
  auto imm = literalImmOf(value);
  if (imm)
    return emitLiteralImm(op, a, *imm);
  emitPool(op, a, emitConst(value));
}

// The immediate a literal is encoded as, or nullopt when it has to stay in the
// constant pool (plan decision D3). The QLangValue type tag must survive the
// trip exactly, because the VM materializes the immediate from the form alone:
// `Int` stays Int for the signed 1/2/4-byte forms, `Float`/`Bool`/`Nil` map to
// their zero- or fixed-width forms, and everything else (String, Blob, Array,
// Table, Set, closures, and the 64-bit/unsigned tags whose type depends on how
// the value was produced) is a pool read.
static std::optional<LiteralImm> literalImmOf(const QLangValue &v) {
  using Type = QLangValue::Type;
  LiteralImm out;
  switch (v.type()) {
  case Type::Int: {
    int64_t n = v.asInt();
    if (n < INT32_MIN || n > INT32_MAX)
      return std::nullopt; // not reachable for an Int, kept as a guard
    out.form = narrowestSignedForm(n);
    out.payload = ImmPayload::Signed;
    out.value = n;
    return out;
  }
  case Type::Float:
    out.form = ImmForm::Float;
    out.payload = ImmPayload::Float;
    out.dbl = v.asFloat();
    return out;
  case Type::Bool:
    out.form = v.asInt() ? ImmForm::True : ImmForm::False;
    out.payload = v.asInt() ? ImmPayload::True : ImmPayload::False;
    return out;
  case Type::Nil:
    out.form = ImmForm::Nil;
    out.payload = ImmPayload::Nil;
    return out;
  default:
    return std::nullopt;
  }
}

// True when e is a Type::Int literal (the shape the arithmetic folds and the
// Int-only comparison fusion recognize).
static bool isIntLiteral(const ExprPtr &e) {
  return e && e->kind == ExprKind::Literal &&
         e->literal.type() == QLangValue::Type::Int;
}

// True when e is a numeric literal (Int or Float) that a unary minus can fold
// into at compile time.
static bool isNumericLiteral(const ExprPtr &e) {
  if (!e || e->kind != ExprKind::Literal) return false;
  auto t = e->literal.type();
  return t == QLangValue::Type::Int || t == QLangValue::Type::Float;
}

// Negate a numeric literal, keeping its type tag (Int stays Int, Float stays
// Float). Returns std::nullopt when an Int result leaves the int32 range the
// signed immediate forms cover.
static std::optional<QLangValue> negateLiteral(const QLangValue &v) {
  if (v.type() == QLangValue::Type::Float)
    return QLangValue::Float(-v.asFloat());
  int64_t n = -static_cast<int64_t>(v.asInt());
  if (n < INT32_MIN || n > INT32_MAX) return std::nullopt;
  return QLangValue::Int(static_cast<int32_t>(n));
}

// Fold a unary minus over a numeric literal to a single compile-time immediate
// so the compiler emits one store instead of a load plus NEG (SPEC §bytecodes:
// encoding guidelines — avoid temporary registers when possible). Float
// literals fold too: the v6 float immediate form carries the sign. Also folds
// one level of double negation (`-(-5)` -> 5). Returns std::nullopt for
// anything else (bool/nil literals, variables, other operators) or when the
// result overflows int32 (e.g. `-(-2147483648)`), in which case the caller
// falls back to the load + NEG sequence.
static std::optional<LiteralImm> foldNegLiteral(const ExprPtr &e) {
  if (!e || e->kind != ExprKind::Unary || e->uop.type != TokenType::Minus)
    return std::nullopt;
  const ExprPtr &o = e->operand;
  if (isNumericLiteral(o)) {
    auto neg = negateLiteral(o->literal);
    if (!neg) return std::nullopt;
    return literalImmOf(*neg);
  }
  if (o && o->kind == ExprKind::Unary && o->uop.type == TokenType::Minus &&
      isNumericLiteral(o->operand)) {
    return literalImmOf(o->operand->literal); // -(-x) == x
  }
  return std::nullopt;
}

// Map a comparison opcode to the conditional jump that fires when that
// comparison is FALSE (the negative branch every branch site takes).
//   == -> jne   != -> je   < -> jge   > -> jle   <= -> jg   >= -> jl
// Sound after `cmp` (a total order: exactly one bit set) AND after a flag
// compare, provided the compare is the *dual* relation — see
// flagCompareIfFalse below, which is why this map has only one entry per
// condition instead of a second one for the `*f` path.
static Opcode cmpJumpIfFalse(Opcode rel) {
  switch (rel) {
  case Opcode::EQ: return Opcode::JNE;
  case Opcode::NE: return Opcode::JE;
  case Opcode::LT: return Opcode::JGE;
  case Opcode::GT: return Opcode::JLE;
  case Opcode::LE: return Opcode::JG;
  case Opcode::GE: return Opcode::JL;
  default: return Opcode::Nop; // not a comparison; caller handles
  }
}

// The flag compare whose set bits are exactly the states in which `rel` is
// FALSE, so that cmpJumpIfFalse's jump fires precisely on the negative branch
// (SPEC §bytecodes "## alternate conditional jump OpCode", "### extended OpCode
// eq, lt, gt, lte, gte"). It is the *dual* relation, not the same one: a
// partial comparator leaves no bit set when its relation does not hold, and no
// jump predicate tests for "no bits set" — `jne` is `(f & eq) == 0`, so it
// fires for gr and lt as well. Pairing a compare with the *same* relation and
// `jne` (the only jump that fires on no bits set) therefore takes the branch
// when the condition HOLDS: `ltf` leaves lt, and `jne` jumps on lt. Working
// through the one-hot states gives the dual instead:
//   a == b false <=> a != b : eqf  f = eq | none     jne  (f & eq) == 0
//   a != b false <=> a == b : eqf  f = eq | none     je   f == eq
//   a <  b false <=> a >= b : gtef f = gr | eq | none  jge  (f & (gr|eq)) != 0
//   a >  b false <=> a <= b : ltef f = lt | eq | none  jle  (f & (lt|eq)) != 0
//   a <= b false <=> a >  b : gtf  f = gr | none     jg   f == gr
//   a >= b false <=> a <  b : ltf  f = lt | none     jl   f == lt
// Those are the same six jumps `cmp` needs, so one jump map serves both paths.
// `cmp`'s three-way result does not depend on the operator name (only its error
// text does, src/QLangVM.cpp `cmp`), so the dual is behaviour-identical to the
// value-producing compare it replaces apart from that name: a source `<` on
// incomparable operands now reports "operator '>=' cannot compare ...", which
// is the instruction the stream actually contains.
static Opcode flagCompareIfFalse(Opcode rel) {
  switch (rel) {
  case Opcode::EQ:
  case Opcode::NE: return Opcode::EQF;
  case Opcode::LT: return Opcode::GTEF;
  case Opcode::GT: return Opcode::LTEF;
  case Opcode::LE: return Opcode::GTF;
  case Opcode::GE: return Opcode::LTF;
  default: return Opcode::Nop; // not a comparison; caller handles
  }
}

int QLangCompiler::emitCmpJumpFalse(Opcode rel, int ra, int rb) {
  emit(Opcode::CMP, ra, rb);
  // Flag jump (JE/JNE/JG/JGE/JL/JLE): target in `a`, patched by caller.
  emit(cmpJumpIfFalse(rel), 0, 0, 0);
  return static_cast<int>(m_emitter.size()) - 1;
}

// Truthiness into the flags + the flag jump that takes the branch: `tst` sets
// eq from truth(r[a]) and clears the other two bits, so `je` fires exactly on
// the true side and `jne` — `(f & eq) == 0` — on the false one
// (SPEC §bytecodes "OpCode tst"; plan docs/plans/phase3-opcode-retirement-plan.md
// D1). The jump's target stays 0 for the caller to patch.
int QLangCompiler::emitJumpIfTrue(int condReg) {
  emit(Opcode::TST, condReg);
  emit(Opcode::JE, 0);
  return static_cast<int>(m_emitter.size()) - 1;
}

int QLangCompiler::emitJumpIfFalse(int condReg) {
  emit(Opcode::TST, condReg);
  emit(Opcode::JNE, 0);
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
  // Two lowerings of the same condition (plan M3 D2). Both operands statically
  // Int -> `cmp`: the bytes are unchanged from before M3 and cmp's total order
  // keeps the flags reusable for a second jump. Everything else -> a flag
  // compare over general values, picked by flagCompareIfFalse so that the one
  // jump map below fires exactly when the condition is false (SPEC §bytecodes
  // "extended OpCode eq, lt, gt, lte, gte"). The gate used to be "both Int or
  // no fusion at all", so every guard inferType cannot pin down — i.e. every
  // guard with a variable operand — paid for a boolean temp plus a truthiness
  // test; that domain is exactly what the flag compares cover.
  const bool intPath = inferType(cond->left) == QLangValue::Type::Int &&
                       inferType(cond->right) == QLangValue::Type::Int;
  const Opcode cmpOp = intPath ? Opcode::CMP : flagCompareIfFalse(rel);
  const Opcode jump = cmpJumpIfFalse(rel);
  // Plain-variable operands are compared in place (no mov into a temp).
  int ra = inPlaceReg(cond->left);
  bool raTmp = false;
  if (ra < 0) {
    ra = tmpRegister();
    raTmp = true;
    compileExpr(cond->left, ra);
  }
  // A literal right operand rides inside the compare: an inline immediate when
  // its type has a form for it, otherwise a constant-pool read (both escaped,
  // plan M3 D5). On the cmp path only an Int literal may be inlined, because
  // cmp is Int-only — the historical behaviour.
  const bool rightInline = cond->right &&
                           cond->right->kind == ExprKind::Literal &&
                           (!intPath || isIntLiteral(cond->right));
  if (rightInline) {
    emitLiteral(cmpOp, ra, cond->right->literal);
  } else {
    int rb = inPlaceReg(cond->right);
    bool rbTmp = false;
    if (rb < 0) {
      rb = tmpRegister();
      rbTmp = true;
      compileExpr(cond->right, rb);
    }
    emit(cmpOp, ra, rb);
    if (rbTmp)
      freeRegister(rb);
  }
  // The flag jump follows the compare immediately, so no instruction can
  // clobber the flags between them. Target in `a`, patched by the caller.
  emit(jump, 0, 0, 0);
  int jumpIdx = static_cast<int>(m_emitter.size()) - 1;
  if (raTmp)
    freeRegister(ra);
  return jumpIdx;
}

CompiledProgram QLangCompiler::compile() {
  // Module exports first (SPEC §bytecodes "keyword export"): the table must
  // exist before the body compiles, so an exported name is known (and, for a
  // function, un-rebindable) from the first statement on.
  collectExports();
  // Statically known external receivers for the manifest checks (plan P4
  // check 2): computed once from the full module AST, so a second write to
  // `p` anywhere — even in a def body — poisons the name for the whole
  // compile and the lenient path answers everywhere.
  collectKnownReceivers();
  // A library must be NAMABLE (SPEC §bytecodes "keyword export"): the name other
  // modules import by comes either from its own `export const MODULE_NAME` or
  // from the driver that named the file (plan Q37 — a discovered source with no
  // MODULE_NAME is named by its filename stem). Neither, and there is no way to
  // import the result.
  if (m_libraryMode && m_moduleNameFromSource.empty() && m_moduleName.empty())
    throw QLangError(QLangErrorKind::Parse, 0, 0,
                     "library module requires export const MODULE_NAME = \"<name>\"");
  // Every top-level `import` resolves here, BEFORE the program body compiles,
  // and the statements themselves emit no code (SPEC §modules "Where an import
  // may appear"): the names an import binds are module-wide no matter where the
  // statement sits, so a program that ends before reaching one must still see
  // them, and a loaded `.ql` module the program never reaches still takes its
  // place in the merged <init> order. collectExports(), above, has already
  // refused an import nested in a block or a function body.
  for (const StmtPtr &s : m_program) {
    if (s->kind == StmtKind::Import)
      resolveImport(s);
  }

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
  // Host-injected enums (addExternalEnums): bind each hidden table var to its
  // global slot BEFORE compiling the program, so member folding, bare-enum
  // reads, and function captures all resolve `__enum_<Name>` (a script
  // `enum <Name>` colliding with an injected one then hits the existing
  // "cannot redeclare enum" check — regOf finds the live binding).
  for (const auto &e : m_enums)
    declareVar("__enum_" + e.first);
  for (auto &s : m_program)
    compileStmt(s, nullptr);
  // Implicit `return nil` at the end of the main program.
  emit(Opcode::RETURN, 0);

  // Wrap the main stream into functions[0]. A library module's top level is
  // its initializer `<init>`, not a runnable `main` (SPEC §bytecodes "keyword
  // export", external-modules plan Phase 3a).
  Function mainFn;
  mainFn.name = m_libraryMode ? "<init>" : "main";
  mainFn.paramCount = 0;
  // Register 0 is always live (the implicit final `return regs[0]`).
  mainFn.registerCount = std::max(m_nextReg, 1);
  m_emitter.finish(mainFn);

  CompiledProgram out;
  out.functions.push_back(std::move(mainFn));
  for (auto &fn : m_functions)
    out.functions.push_back(std::move(fn));

  // The one pool every function's operands index into (SPEC §bytecodes
  // "unified constant table"); the dedup map dies with the compile.
  out.constants = m_pool.take();
  // Module identity: the module's own `export const MODULE_NAME` wins over the
  // name the driver supplied, which in turn wins over the default (SPEC
  // §bytecodes "keyword export").
  if (!m_moduleNameFromSource.empty())
    out.moduleName = m_moduleNameFromSource;
  else if (!m_moduleName.empty())
    out.moduleName = m_moduleName;
  // The import table (SPEC §bytecodes "import table (lookup)"): one entry per
  // name bound from a loaded `.ql` module, `ref` still the DEFINING module's
  // handle until the linker rewrites it. C++ module imports stay compile-time
  // natives in the negative-slot space of `CALL` operand `b` (resolveImport)
  // and are not table entries.
  checkImportCycles();
  out.imports = std::move(m_imports);
  finishExports(out); // reads m_userFuncs / m_regs, so before they move out
  buildRegisterReport(); // before m_regs moves out (names are read from it)
  out.vars = std::move(m_regs);
  out.constVars = std::move(m_constVars);
  // Register 0 is always live (see mainFn.registerCount above).
  out.registerCount = std::max(m_nextReg, 1);
  return out;
}

// The exportable identity of a top-level statement (SPEC §bytecodes "export
// table (lookup)": the global functions, variables and constants a module
// exposes): the global name it binds and the kind the export table records it
// as. False when the statement binds no exportable global.
//
// A top-level arrow closure is a Var, not a Func: unlike `def` it has no entry
// in the function table — the register holds the closure value, and that
// register is the cell an importer shares (SPEC §bytecodes "technical
// specifications" 3).
static bool exportableDecl(const StmtPtr &s, std::string &name,
                           ExportKind &kind) {
  switch (s->kind) {
  case StmtKind::Def:
    name = s->funcName;
    kind = ExportKind::Func;
    return !name.empty();
  case StmtKind::Const:
    name = s->name;
    kind = ExportKind::Const;
    return !name.empty();
  case StmtKind::Arrow:
    name = s->name;
    kind = ExportKind::Var;
    return !name.empty();
  case StmtKind::Declaration:
  case StmtKind::Assignment:
    if (s->target) return false; // `a.b = 1` binds no global name
    name = s->name;
    kind = ExportKind::Var;
    return !name.empty();
  default:
    return false;
  }
}

// The initializer expression of an exportable declaration (`const`/`local` use
// `value`, a bare assignment uses `assignValue`); null when there is none.
static ExprPtr declInitValue(const StmtPtr &s) {
  if (!s) return nullptr;
  return s->kind == StmtKind::Assignment ? s->assignValue : s->value;
}

// `export` inside a control-flow block (SPEC §bytecodes "keyword export": the
// keyword is global-scope only). The parser rejects it inside a function,
// method or class body, but an `if`/`for`/`while`/`switch` body at global scope
// is still a nested statement list, and the export pre-pass only walks the
// top level — so an export there would be parsed, compiled and silently
// dropped from the table. Rejecting it here is the only way it cannot be lost.
// rejectNestedExports scans the statements NESTED inside `s`; the top-level
// list is never passed to it, because there the two forms are legal.
static void rejectNestedExports(const StmtPtr &s);

static void rejectNestedExportsIn(const std::vector<StmtPtr> &stmts) {
  for (const StmtPtr &s : stmts) {
    if (!s) continue;
    if (s->kind == StmtKind::ExportRef || s->isExported)
      throw QLangError(QLangErrorKind::Parse, s->line, s->col,
                       "export is only valid at global scope");
    // An `import` binds module-wide names, and the import pre-pass walks only
    // the top level, so nested in a block or a def body it would bind names the
    // block looks like it scopes (SPEC §modules "Where an import may appear").
    if (s->kind == StmtKind::Import)
      throw QLangError(QLangErrorKind::Parse, s->line, s->col,
                       "import of module '" + s->importModule +
                           "' must appear at module top level");
    rejectNestedExports(s);
  }
}

static void rejectNestedExports(const StmtPtr &s) {
  if (!s) return;
  switch (s->kind) {
  case StmtKind::For:
  case StmtKind::Def:
    rejectNestedExportsIn(s->body);
    break;
  case StmtKind::While:
    rejectNestedExportsIn(s->whileBody);
    break;
  case StmtKind::If:
    rejectNestedExportsIn(s->ifBody);
    rejectNestedExportsIn(s->elseBody);
    break;
  case StmtKind::Switch:
    for (const auto &c : s->cases) rejectNestedExportsIn(c.body);
    rejectNestedExportsIn(s->defaultBody);
    break;
  default:
    break;
  }
}

// Pre-pass over the top-level statements (SPEC §bytecodes "keyword export").
// It runs BEFORE compilation for two reasons: a standalone `export NAME;` may
// name a declaration that appears later in the file, and the export table has
// to exist while the body compiles so an exported function's name is already
// un-rebindable.
void QLangCompiler::collectExports() {
  // An `export` is a statement of the module, never of a block inside it.
  for (const StmtPtr &s : m_program) rejectNestedExports(s);
  // Pass 1: every top-level declaration, so a standalone export resolves
  // forward. The first binding of a name wins — a later bare assignment to it
  // is a write, not a new declaration.
  for (const StmtPtr &s : m_program) {
    std::string name;
    ExportKind kind;
    if (!exportableDecl(s, name, kind)) continue;
    m_globalDecls.emplace(name, s);
  }
  // Pass 2: the exports themselves, kept in source order.
  for (const StmtPtr &s : m_program) {
    std::string name;
    ExportKind kind;
    const StmtPtr *decl = nullptr;
    if (s->kind == StmtKind::ExportRef) {
      name = s->name;
      auto it = m_globalDecls.find(name);
      if (it == m_globalDecls.end())
        throw QLangError(QLangErrorKind::Parse, s->line, s->col,
                         "export of undeclared name '" + name + "'");
      decl = &it->second;
      if (!exportableDecl(*decl, name, kind))
        throw QLangError(QLangErrorKind::Parse, s->line, s->col,
                         "export of undeclared name '" + name + "'");
    } else if (s->isExported) {
      if (!exportableDecl(s, name, kind)) // the parser already rejects this
        throw QLangError(QLangErrorKind::Parse, s->line, s->col,
                         "export can only prefix def, const or a variable "
                         "assignment");
      decl = &s;
    } else {
      continue;
    }
    if (!m_exportedKinds.emplace(name, kind).second)
      throw QLangError(QLangErrorKind::Parse, s->line, s->col,
                       "'" + name + "' is already exported");
    m_exports.push_back(ExportEntry{kind, name, -1});
    // The module's own name (SPEC §bytecodes "keyword export"): a `const`
    // holding a constant string, because a loader has to read the identity out
    // of the image without running the module. Any other shape — a variable, a
    // missing initializer, a non-string or non-constant value — names nothing.
    if (name == "MODULE_NAME") {
      ExprPtr init = kind == ExportKind::Const ? declInitValue(*decl) : nullptr;
      if (!init)
        throw QLangError(
            QLangErrorKind::Parse, s->line, s->col,
            "MODULE_NAME must be `export const MODULE_NAME = \"<name>\"`");
      QLangValue v = evalConstExpr(init); // throws when not a constant
      if (v.type() != QLangValue::Type::String)
        throw QLangError(QLangErrorKind::Parse, s->line, s->col,
                         "MODULE_NAME must be a string");
      m_moduleNameFromSource = v.asString();
    }
  }
}

// Resolves the collected export names to what a loader dereferences (SPEC
// §bytecodes "export table (lookup)"): the function index for a `def`, the
// register (global slot) for a variable or constant. Runs after the program is
// compiled, when both exist.
void QLangCompiler::finishExports(CompiledProgram &out) {
  for (ExportEntry &e : m_exports) {
    if (e.kind == ExportKind::Func) {
      auto it = m_userFuncs.find(e.name);
      if (it == m_userFuncs.end())
        throw QLangError(QLangErrorKind::Parse, 0, 0,
                         "exported function '" + e.name + "' is not defined");
      e.ref = it->second;
    } else {
      auto it = m_regs.find(e.name);
      if (it == m_regs.end())
        throw QLangError(QLangErrorKind::Parse, 0, 0,
                         "exported name '" + e.name + "' is not bound");
      e.ref = it->second;
    }
  }
  out.exports = std::move(m_exports);
}

void QLangCompiler::guardNotExportedFunc(const std::string &name,
                                         const char *op) const {
  auto it = m_exportedKinds.find(name);
  if (it != m_exportedKinds.end() && it->second == ExportKind::Func)
    throw QLangError(QLangErrorKind::Parse, m_curLine, m_curCol,
                     std::string("cannot ") + op + " exported function '" +
                         name + "'");
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
  // Receiver checks from a manifest apply at top level only (plan P4 v1):
  // inside a function/closure body a parameter or local could shadow a known
  // receiver name, so the body stays fully lenient.
  const bool savedInFnBody = m_inFnBody;
  m_inFnBody = true;
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
    if (findBuiltin(id) >= 0) continue; // builtin
    if (m_nativeFuncIds.count(id)) continue; // native (C++) function
    // A name imported from a loaded `.ql` module is not a variable of this
    // module either: it resolves through the import table (plan Phase 3b), so
    // capturing it as a free variable would be wrong.
    if (m_qlImports.count(id)) continue;
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
  emitImmNil(Opcode::MOV, 0); // reg 0 = nil
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
    // A global capture stores `-1 - <absolute global-table slot>` instead of a
    // register: the slot is the capture's home in the shared table, and naming
    // it absolutely (rather than letting the VM re-derive it from the name) is
    // what lets a linked program resolve a library function's globals, where
    // several modules share one table and the names are not in the image at all
    // (external-modules plan v4 Phase 3b).
    int callerReg = 0;
    // A capture whose home is a module-level IMMUTABLE cell — a top-level const
    // or an enum's hidden table var referenced by a top-level `def` — is named
    // by its absolute table slot too. At module top level the register IS the
    // slot (declareVar), so the unlinked program aliases the very cell it used
    // to copy, and a linked program reaches it from another module's frame,
    // where a frame-relative register would name the CALLER's storage instead
    // (external-modules plan v4 Phase 3b: an imported `def` is called with
    // CALLFN, so its capture spec cannot depend on who calls it). A mutable
    // top-level capture keeps the frame-relative copy; `global` already routes
    // through the slot map above.
    const bool immutableCell =
        !savedInFunction && m_scope &&
        (m_constVars.count(var) ||
         (var.compare(0, 7, "__enum_") == 0 && var.size() > 7));
    if (m_globalRegMap.count(var))
      callerReg = -1 - m_globalRegMap[var];
    else if (immutableCell && savedRegs.count(var))
      callerReg = -1 - savedRegs[var];
    else if (savedRegs.count(var))
      callerReg = savedRegs[var];
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
  // The generator's closure spec is read HERE, before `fn` moves into the
  // function table. The constant itself cannot be emitted until the enclosing
  // function's emitter state is restored (below), and reading `fn.captures`
  // after the move yields a moved-from vector: the spec lands in the pool with
  // NO captures, which turns the VM's MAKEGEN capture-binding fallback into a
  // silent no-op and stops a linked program's synthesized `makegen` closure
  // from folding with this one (external-modules plan v4 Q33).
  const bool isGenerator = fn.isGenerator;
  std::vector<CaptureRef> genCaptures;
  if (isGenerator) {
    m_generatorFuncIndices.insert(funcIndex);
    genCaptures = fn.captures;
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
  if (isGenerator) {
    auto closure = std::make_shared<Closure>();
    closure->funcIndex = funcIndex;
    closure->captures = std::move(genCaptures);
    int genConstIdx = emitConst(QLangValue::Function(closure));
    m_generatorConstIndices[funcIndex] = genConstIdx;
  }

  m_inFnBody = savedInFnBody;
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
  emitPool(Opcode::MOV, keyR, emitConst(QLangValue::String("__statics__")));
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
  // Bound by the pre-pass at top level; resolved here only for a class body,
  // which that pass does not walk (SPEC §modules "Where an import may appear").
  case StmtKind::Import: resolveImport(s); break;
  // `export NAME;` emits no code: the export table is built by the passes
  // around compile() (SPEC §bytecodes "keyword export").
  case StmtKind::ExportRef: break;
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
  if (s->value && s->value->kind == ExprKind::Literal) {
    // `local a = 0` -> one store of the literal into the target register
    // (SPEC §bytecodes: encoding guidelines) instead of MOV tmp + MOV.
    emitLiteral(Opcode::MOV, r, s->value->literal);
  } else if (!s->value) {
    // `local z` -> implicit int, default 0 (SPEC §declarations).
    // A typed valueless decl (`local b:byte`) gets zero of that type via
    // the CONV emitted below.
    emitImmI(Opcode::MOV, r, 0);
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
    // An imported generator counts too: the call yields a generator value, so
    // resuming the variable has to dispatch through CALLCLOS like a local one
    // (plan Phase 3c — without this the name is unbound as a callee and the
    // resume is refused as `unknown function`).
    if ((m_userFuncs.count(e->callee) && m_generatorVars.count(e->callee)) ||
        importedIsGenerator(callKey(s->value)))
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
    emitImmNil(Opcode::MOV, r);
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
    emitPool(Opcode::MOV, keyR, emitConst(QLangValue::String(s->name)));
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
        // `cap += 12` -> `add cur, i8 12` in place (SPEC §2.6 in-place form).
        emitImmI(opCode == Opcode::ADD ? Opcode::ADD : Opcode::SUB, cur,
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
  guardNotExportedFunc(s->name, "assign to");
  guardNotImported(s->name, "assign to");
  const std::string &op = s->assignOp.text;
  // Array append: `name += value` where name is an array.
  if (op == "+=" && m_varTypes[s->name] == QLangValue::Type::Array) {
    // SPEC §operators "operator +=" decides by the RIGHT-HAND type:
    // `[1,2,3] += [5,6]` extends ([1,2,3,5,6]), `[1,2,3] += 4` appends one
    // element ([1,2,3,4]). A literal RHS is known here; a variable RHS is
    // known through the inferred-type table (`x += x`). Anything else
    // (unknown/static-typed) keeps the single-element append.
    const bool extend = inferType(s->assignValue) == QLangValue::Type::Array;
    int t = tmpRegister();
    compileExpr(s->assignValue, t);
    emit(extend ? Opcode::ARRAPPV : Opcode::ARRAPP, r, t, 0);
    releaseTemp(t); // dead after the append
    return;
  }
  if (op == "=") {
    if (s->assignValue && s->assignValue->kind == ExprKind::Literal) {
      // `a = 0` -> one store of the literal into the target register
      // (SPEC §bytecodes: encoding guidelines).
      emitLiteral(Opcode::MOV, r, s->assignValue->literal);
    } else if (foldSelfArith(s->assignValue, s->name, r)) {
      // `a = a + 1` -> inc a / `a = a - 4` -> `add a, i8 -4` (in place).
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
      // `a += 12` -> `add r, i8 12` in place (SPEC §2.6 in-place form).
      emitImmI(opCode == Opcode::ADD ? Opcode::ADD : Opcode::SUB, r,
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
  guardNotImported(s->delName, "delete");
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
      emitImmI(Opcode::MOV, stepReg, 1);
    }
    int zeroReg = tmpRegister();
    emitImmI(Opcode::MOV, zeroReg, 0);

    // direction = (end > start) ? 1 : -1
    int subReg = tmpRegister();
    emit(Opcode::SUB, subReg, endReg, startReg);
    int posReg = tmpRegister();
    emit(Opcode::GT, posReg, subReg, zeroReg);
    int dirReg = tmpRegister();
    emitImmI(Opcode::MOV, dirReg, -1);
    int dTrueIdx = emitJumpIfTrue(posReg); // ascending bounds -> dir = 1
    int dDoneLabel = static_cast<int>(m_emitter.size());
    emit(Opcode::JMP, 0);
    int dSetOne = static_cast<int>(m_emitter.size());
    emitImmI(Opcode::MOV, dirReg, 1);
    m_emitter.patchJump(dTrueIdx, dSetOne);
    m_emitter.patchJump(dDoneLabel, static_cast<int>(m_emitter.size()));

    // No explicit step: advance in the loop's natural direction (unit step).
    // Without this a backward no-step loop would get step +1 vs dir -1 and
    // take the run-once path.
    if (!hasStep)
      emit(Opcode::MOV, stepReg, dirReg);

    // step == 0 -> natural-direction unit step (so a `step 0` loop advances).
    int zeroCmp = tmpRegister();
    emit(Opcode::EQ, zeroCmp, stepReg, zeroReg);
    int zeroStepLabel =
        emitJumpIfFalse(zeroCmp);           // step != 0 -> skip normalization
    emit(Opcode::MOV, stepReg, dirReg);     // step == 0 -> step = dir (+1 / -1)
    m_emitter.patchJump(zeroStepLabel, static_cast<int>(m_emitter.size()));

    // runtime conditions
    int posStep = tmpRegister();
    emit(Opcode::GT, posStep, stepReg, zeroReg);
    int posDir = tmpRegister();
    emit(Opcode::GT, posDir, dirReg, zeroReg);
    int sameSign = tmpRegister();
    emit(Opcode::EQ, sameSign, posStep, posDir);

    // Decision: sameSign -> guard loop; else fall through to run-once.
    int guardStepIdx = emitJumpIfTrue(sameSign);

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
    // `guardOnceLabel` is the `tst` itself: the loop back edge below points at
    // it so every pass re-evaluates the direction.
    int guardOnceLabel = static_cast<int>(m_emitter.size());
    int guardOnceJeIdx = emitJumpIfTrue(posDir); // forward -> LT guard path
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
    m_emitter.patchJump(guardOnceJeIdx, guardFwdIdx);   // forward -> guard
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
    m_emitter.patchJump(guardStepIdx, guardLoopStart);  // sameSign -> guard loop
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
        emitImmI(Opcode::MOV, stepReg, 1);
      }
      int zeroReg = tmpRegister();
      emitImmI(Opcode::MOV, zeroReg, 0);
      // Natural direction from the (runtime) bounds.
      int subReg = tmpRegister();
      emit(Opcode::SUB, subReg, endReg, startReg);
      int posReg = tmpRegister();
      emit(Opcode::GT, posReg, subReg, zeroReg);
      int dirReg = tmpRegister();
      emitImmI(Opcode::MOV, dirReg, -1);
      int dTrueIdx = emitJumpIfTrue(posReg); // ascending bounds -> dir = 1
      int dDoneLabel = static_cast<int>(m_emitter.size());
      emit(Opcode::JMP, 0);
      int dSetOne = static_cast<int>(m_emitter.size());
      emitImmI(Opcode::MOV, dirReg, 1);
      m_emitter.patchJump(dTrueIdx, dSetOne);
      m_emitter.patchJump(dDoneLabel, static_cast<int>(m_emitter.size()));

      // step == 0 -> infinite loop: body + increment forever, no guard.
      int zeroCmp = tmpRegister();
      emit(Opcode::EQ, zeroCmp, stepReg, zeroReg);
      int zeroJumpIdx = emitJumpIfTrue(zeroCmp);
      int infExitLabel = static_cast<int>(m_emitter.size());
      int posStep = tmpRegister();
      emit(Opcode::GT, posStep, stepReg, zeroReg);
      int posDir = tmpRegister();
      emit(Opcode::GT, posDir, dirReg, zeroReg);
      int sameSign = tmpRegister();
      emit(Opcode::EQ, sameSign, posStep, posDir);
      int ssJumpIdx = emitJumpIfTrue(sameSign);
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
      int fwdGuardIdx = emitJumpIfTrue(posDir2);
      // Int-vs-Int range guards: fused to cmp+j* (SPEC §bytecodes). The j*
      // is the last instruction emitted, so backExitIdx/fwdExitIdx point at
      // it (patched to guardExit below).
      emitCmpJumpFalse(Opcode::GE, x, endReg); // backward guard (x >= end)
      int backExitIdx = static_cast<int>(m_emitter.size()) - 1;
      // Defect (pre-existing, unchanged here): this skip JMP is never
      // patched, so its target stays instruction 0 — a descending loop with
      // non-constant bounds spins there. Tracked in docs/plans/todo.md.
      emit(Opcode::JMP, 0);
      // The direction test above jumps here, to the forward guard's CMP.
      m_emitter.patchJump(fwdGuardIdx, static_cast<int>(m_emitter.size()));
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
  emitImmI(Opcode::MOV, idx, 0);
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
  // Fused compare+j* for any comparison condition (SPEC §bytecodes: alternate
  // conditional jump): cmp+j* when both operands are statically Int, otherwise
  // the dual flag compare + the same j*. Everything that is not a comparison
  // (truthiness, && / ||, !, calls) lowers to `tst` + `jne`.
  int jumpFalseIdx = emitCmpJumpFalseExpr(s->cond);
  if (jumpFalseIdx < 0) {
    int condReg = tmpRegister();
    compileExpr(s->cond, condReg);
    jumpFalseIdx = emitJumpIfFalse(condReg);
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
  // Fused compare+j* for any comparison condition (SPEC §bytecodes: alternate
  // conditional jump): cmp+j* when both operands are statically Int, otherwise
  // the dual flag compare + the same j*. Everything that is not a comparison
  // (truthiness, && / ||, !, calls) lowers to `tst` + `jne`.
  int jumpFalseIdx = emitCmpJumpFalseExpr(s->ifCond);
  if (jumpFalseIdx < 0) {
    int condReg = tmpRegister();
    compileExpr(s->ifCond, condReg);
    jumpFalseIdx = emitJumpIfFalse(condReg);
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
  int jumpTrueIdx = emitJumpIfTrue(condReg);
  m_emitter.patchJump(jumpTrueIdx, lit->second);
}

void QLangCompiler::compileExprStmt(const StmtPtr &s) {
  // Statement-form prefix/postfix inc/dec on a variable must write back to
  // that variable's register (e.g. --a, a++). Otherwise the inc/dec result
  // would land in a throwaway temp and the variable would be unchanged.
  if (s->value && s->value->kind == ExprKind::Unary &&
      (s->value->uop.text == "++" || s->value->uop.text == "--")) {
    const ExprPtr &opnd = s->value->operand;
    if (opnd && opnd->kind == ExprKind::Variable) {
      // An imported name has no register (it is read through the import table),
      // so this guard has to come before the register lookup: otherwise the
      // undeclared-variable error fires first and says the opposite of the
      // truth about a name the module did bind.
      guardNotImported(opnd->name, "increment/decrement");
      int r = regOf(opnd->name);
      if (r < 0)
        throw QLangError(QLangErrorKind::Parse, 0, 0,
                         "use of undeclared variable '" + opnd->name + "'");
      guardNotConst(opnd->name, "increment/decrement");
      guardNotExportedFunc(opnd->name, "increment/decrement");
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
    for (size_t ci = 0; ci < built.captureNames.size(); ci++) {
      const std::string &cap = built.captureNames[ci];
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
      // A capture buildFunction already named by its absolute global cell
      // (`-1 - slot`, the value in built.captures) keeps that operand: the
      // def-time MAKEFUNC runs in a frame aliasing the shared table (the
      // entry or module-init frame), and a frame-relative register there
      // binds a frame-local env cell whose release MOVES the table cell out
      // of the table when the frame pops — importers then read an emptied
      // cell (F1). The sentinel names the one merged cell, and the linker
      // rebases negative capture registers, so it survives linking
      // (external-modules plan v4 Phase 3b).
      const bool absCell =
          ci < built.captures.size() && built.captures[ci].reg < 0;
      if (absCell) reg = built.captures[ci].reg;
      if (!absCell && reg < 0) {
        // A module-level `def` must reach its free variables through a cell.
        // In library mode there is no caller frame to copy from, so a name
        // that resolves to no cell is a compile error, never a silent nil
        // (module-export/import plan P0.3, Q43).
        if (m_libraryMode)
          throw QLangError(QLangErrorKind::Parse, 0, 0,
                           "def '" + s->funcName + "' captures '" + cap +
                               "', which has no cell in module '" +
                               m_moduleName + "'");
        reg = 0; // preserve legacy binding for unresolvable names
      }
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
    emitImmNil(Opcode::MOV, 0); // nil
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
    emitImmNil(Opcode::MOV, 0); // nil
  }
  emit(Opcode::YIELD, 0);
}

void QLangCompiler::compileEmit(const StmtPtr &s) {
  // Signal check against a known receiver (plan P4 check 3): `emit p.sig(...)`
  // where `let p = T(...)` gave p a manifest type. Existence and arity use
  // the VM's wording; argument tags stay with the runtime signal check.
  if (!m_inFnBody && s->emitTarget->kind == ExprKind::Variable) {
    auto rit = m_knownReceivers.find(s->emitTarget->name);
    if (rit != m_knownReceivers.end()) {
      const ManifestSignal *ms = manifestSignal(*rit->second, s->name);
      if (!ms)
        throw QLangError(QLangErrorKind::Parse, m_curLine, m_curCol,
                         "unknown signal '" + s->name + "' on type '" +
                             rit->second->name + "'");
      if (static_cast<int>(ms->argTags.size()) !=
          static_cast<int>(s->emitArgs.size()))
        throw QLangError(
            QLangErrorKind::Parse, m_curLine, m_curCol,
            "signal '" + s->name + "' on type '" + rit->second->name +
                "' expects " + std::to_string(ms->argTags.size()) +
                " argument(s), got " + std::to_string(s->emitArgs.size()));
    }
  }
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
  for (int j = 0; j < argCount; j++)
    emitImmNil(Opcode::MOV, senderReg + 1 + j);
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

// Depth-first walk of the import graph looking for a path from `node` back to
// `entry`; on success `path` holds the cycle (`a -> b -> a`). A cycle that does
// not pass through the entry module is not this program's problem — the two
// libraries involved could not have been loaded in this order in the first
// place — so only a path back to the entry is reported.
static bool importCyclePath(
    const std::unordered_map<std::string, std::vector<std::string>> &edges,
    const std::string &entry, const std::string &node,
    std::vector<std::string> &path, std::unordered_set<std::string> &onPath) {
  path.push_back(node);
  onPath.insert(node);
  auto it = edges.find(node);
  if (it != edges.end()) {
    for (const std::string &dep : it->second) {
      if (dep == entry) {
        path.push_back(dep);
        return true;
      }
      if (onPath.count(dep))
        continue;
      if (importCyclePath(edges, entry, dep, path, onPath))
        return true;
    }
  }
  onPath.erase(node);
  path.pop_back();
  return false;
}

// SPEC §bytecodes "technical specifications" 4 / plan Phase 3b: an import cycle
// is a compile error, raised here rather than at link time. The graph is the
// entry module's import table plus every loaded image's, so the check stays
// correct once a library can import a library (Phase 3c); in Phase 3b a library
// cannot import one at all (compileQlImport), which makes it defensive.
void QLangCompiler::checkImportCycles() const {
  if (m_imports.empty() || !m_scope)
    return;
  const std::string entry = !m_moduleNameFromSource.empty()
                                ? m_moduleNameFromSource
                                : (m_moduleName.empty() ? std::string("main")
                                                        : m_moduleName);
  std::unordered_map<std::string, std::vector<std::string>> edges;
  auto addEdge = [&edges](const std::string &from, const std::string &to) {
    std::vector<std::string> &v = edges[from];
    if (std::find(v.begin(), v.end(), to) == v.end())
      v.push_back(to);
  };
  for (const ImportEntry &im : m_imports)
    addEdge(entry, im.module);
  for (const auto &lm : m_scope->loadedModules()) {
    if (!lm)
      continue;
    for (const ImportEntry &im : lm->program.imports)
      addEdge(lm->program.moduleName, im.module);
  }
  if (edges.count(entry) == 0)
    return;
  std::vector<std::string> path;
  std::unordered_set<std::string> onPath;
  if (!importCyclePath(edges, entry, entry, path, onPath))
    return;
  std::string msg = "import cycle: ";
  for (size_t i = 0; i < path.size(); i++)
    msg += (i ? " -> " : "") + path[i];
  throw QLangError(QLangErrorKind::Parse, 0, 0, msg);
}

// The loaded `.ql` module an `import` names, honouring an `import m as a`
// alias; nullptr when the name is not a loaded image (external-modules plan v4
// Phase 3b). A name that is not loaded is not yet a failure: resolveImport
// hands it to QLangContext::findModule, which resolves it through the search
// callback and the search paths (SPEC §api "module search paths"; plan C8a) —
// and a module discovered that way is registered under its own declared name,
// which is what the alias map also carries.
const LoadedModule *QLangCompiler::qlModule(const std::string &name) const {
  if (!m_scope)
    return nullptr;
  auto ait = m_qlAliases.find(name);
  return m_scope->loadedModule(ait == m_qlAliases.end() ? name : ait->second);
}

// Whether a call of the imported name yields a generator (plan Phase 3c). The
// importing module has no function record of its own for the name — the import
// entry points at the DEFINING image's — so `isGenerator` has to be read out of
// that image. Anything the table cannot answer (no such import, a value export,
// a module that is not loaded, a ref outside the function table) is answered
// "not a generator": the paths that have to REFUSE such a name check the table
// themselves and say why.
bool QLangCompiler::importedIsGenerator(const std::string &name) const {
  auto it = m_qlImports.find(name);
  if (it == m_qlImports.end())
    return false;
  const ImportEntry &im = m_imports[static_cast<size_t>(it->second)];
  if (im.kind != ExportKind::Func)
    return false;
  const LoadedModule *dm = m_scope ? m_scope->loadedModule(im.module) : nullptr;
  if (!dm || im.ref < 0 ||
      im.ref >= static_cast<int>(dm->program.functions.size()))
    return false;
  return dm->program.functions[static_cast<size_t>(im.ref)].isGenerator;
}

void QLangCompiler::guardNotImported(const std::string &name,
                                     const char *op) const {
  auto it = m_qlImports.find(name);
  if (it == m_qlImports.end())
    return;
  const ImportEntry &im = m_imports[static_cast<size_t>(it->second)];
  throw QLangError(QLangErrorKind::Parse, m_curLine, m_curCol,
                   std::string("cannot ") + op + " imported name '" + name +
                       "' (from module '" + im.module +
                       "'): the defining module owns it");
}

// The registered C++ module named `name`, or nullptr.
const QLangContext::Module *
QLangCompiler::cppModule(const std::string &name) const {
  for (const auto &m : m_moduleFuncs)
    if (m.name == name)
      return &m;
  return nullptr;
}

// Whether `base` names a MODULE the program imported rather than a value (SPEC
// §modules "import statement"). A variable, class or enum of the same name
// wins: the qualifier is what the source spelled (`import m as a` binds `a`),
// and nothing here can shadow it before it is declared.
bool QLangCompiler::moduleQualified(const ExprPtr &base) const {
  if (!base || base->kind != ExprKind::Variable || base->scopeStatic)
    return false;
  const std::string &q = base->name;
  if (!m_moduleNs.count(q))
    return false;
  return regOf(q) < 0 && m_captureRegs.count(q) == 0 &&
         m_classes.count(q) == 0 && m_enums.count(q) == 0 &&
         m_userFuncs.count(q) == 0;
}

// The name a call resolves through: the qualified key when the callee's base is
// a module qualifier, the callee itself otherwise (SPEC §modules "import
// statement" — `import m` binds the module, `from m import a` the name). A
// method call on a real table keeps the bare name: its member is read at
// runtime.
std::string QLangCompiler::callKey(const ExprPtr &e) const {
  const ExprPtr &base = e->calleeMember ? e->calleeMember->memberBase
                                        : nullptr;
  return moduleQualified(base) ? base->name + "." + e->callee : e->callee;
}

// Binds `spelling` to a native slot (the negative `CALL` operand `b`) and
// records the function for it. Assigning the id and resolving the pointer in
// one step is what keeps a module registered after the compile started from
// leaving a slot that never resolves: the ids taken at the start of a compile
// are only a starting point, not the last word on which modules exist.
int QLangCompiler::bindNative(const std::string &spelling,
                              const QLangContext::NativeFunc &fn) {
  if (!m_nativeFuncIds.count(spelling))
    addExternalFuncs({spelling});
  const int id = m_nativeFuncIds[spelling];
  const size_t slot = static_cast<size_t>(-1 - id);
  if (m_nativeFuncsResolved.size() <= slot)
    m_nativeFuncsResolved.resize(slot + 1);
  m_nativeFuncsResolved[slot] = fn;
  return id;
}

// `q.name` where `q` is a module qualifier (SPEC §modules "import statement").
// A loaded `.ql` module's entities are already bound under the qualified key by
// resolveQlImport, so a miss there means the module does not export the name; a
// C++ module binds nothing at import time, so its lookup happens here.
int QLangCompiler::moduleMember(const std::string &q, const std::string &name,
                                int line, int col) {
  auto it = m_qlImports.find(q + "." + name);
  if (it != m_qlImports.end())
    return it->second;
  auto nit = m_moduleNs.find(q);
  const QLangContext::Module *mod =
      nit == m_moduleNs.end() ? nullptr : cppModule(nit->second);
  if (!mod)
    throw QLangError(QLangErrorKind::Parse, line, col,
                     "module '" + q + "' does not export '" + name + "'");
  auto fit = mod->funcs.find(name);
  if (fit == mod->funcs.end())
    throw QLangError(QLangErrorKind::Parse, line, col,
                     "module '" + q + "' has no function '" + name + "'");
  // The slot is named by the qualified spelling, so the image says which
  // module's function it wants and a same-named function of another module
  // cannot take the slot.
  return bindNative(q + "." + name, fit->second);
}

// Runs one call of an import-table entry (external-modules plan v4 Phase 3c):
// the callee is named through the table, so the instruction carries no handle
// into the defining module — the linker resolves the entry when the images
// merge, which is why a `callimport` never survives a link. Shared by a flat
// imported name and the qualified spelling of a module import (`m.f`), whose
// args are staged the same way.
void QLangCompiler::emitImportCall(int importIdx, const std::string &name,
                                   int argBase, int dest, int argCount,
                                   int line, int col) {
  const ImportEntry &im = m_imports[static_cast<size_t>(importIdx)];
  const LoadedModule *dm =
      m_scope ? m_scope->loadedModule(im.module) : nullptr;
  if (!dm || im.ref < 0)
    throw QLangError(QLangErrorKind::Parse, line, col,
                     "imported function '" + name +
                         "' is not available: module '" + im.module +
                         "' is not loaded");
  // `ref` is a function index for a function export and a cell for a value
  // export (SPEC §bytecodes "import table (lookup)"), so each kind is
  // checked against the table it names — the same split the reader makes
  // for an export entry. Checking a cell against the function table would
  // refuse an exported closure whose register happens to sit past the last
  // function.
  if (im.kind == ExportKind::Func &&
      im.ref >= static_cast<int>(dm->program.functions.size()))
    throw QLangError(QLangErrorKind::Parse, line, col,
                     "imported function '" + name +
                         "' names a function index outside module '" +
                         im.module + "'");
  if (im.kind != ExportKind::Func &&
      im.ref >= dm->program.registerCount)
    throw QLangError(QLangErrorKind::Parse, line, col,
                     "imported name '" + name +
                         "' names a cell outside module '" + im.module +
                         "'");
  if (im.kind != ExportKind::Func) {
    // The name holds a closure value (an exported arrow or closure-typed
    // variable): read the defining module's cell, then call what it holds.
    // `getimport` names a table entry rather than a register, so this works
    // at any depth (the Phase 3b fallback was top-level only).
    emit(Opcode::GETIMPORT, dest, importIdx);
    emit(Opcode::CALLCLOS, dest, dest, argCount);
    return;
  }
  const Function &df = dm->program.functions[static_cast<size_t>(im.ref)];
  // A capture named by a register is seeded from the CALLER's frame, so a
  // function with one still cannot be called across a module boundary: the
  // import table says WHICH function, not which frame seeds its captures.
  // A module-level `def`'s captures of its own module's consts and `global`s
  // are absolute table slots (see the capture lowering in compileFunction),
  // so this fires on the one case that is not: a def that calls a sibling
  // def which itself has captures — that callee is materialized as a closure
  // value in a top-level register, and the caller captures the register
  // (frame-relative), which only the defining module's frame can seed.
  for (const CaptureRef &cr : df.captures)
    if (cr.reg >= 0)
      throw QLangError(
          QLangErrorKind::Parse, line, col,
          "imported function '" + name +
              "' captures a local of its own module and cannot be called "
              "across a module boundary: its captures are seeded from the "
              "defining module's frame");
  emit(Opcode::CALLIMPORT, dest, importIdx, argCount);
  if (df.isGenerator) {
    // The VM builds the generator from the imported function (the local
    // MAKEGEN path's arg-register clearing applies for the same reason:
    // the args must not leak into the next instruction).
    for (int j = 0; j < argCount; j++)
      emitImmNil(Opcode::MOV, argBase + j);
  }
}

// The export names to bind for one `import` form, and whether they are bound
// under the module's qualifier (`import m`) or flat (`from m import …`,
// `import m.f`). The pair's second member is the per-name alias of
// `from m import a as b` — a compile-time binding name, empty otherwise.
// SPEC §modules "import statement".
static std::vector<std::pair<std::string, std::string>> importNames(
    const StmtPtr &s, const std::vector<ExportEntry> &exports,
    bool &qualified) {
  qualified = false;
  using Binding = std::pair<std::string, std::string>;
  if (!s->importFunc.empty())
    return {{s->importFunc, ""}}; // `import m.f` — one explicitly named entity
  if (s->importAll) {
    std::vector<Binding> names; // `from m import *`
    for (const auto &e : exports)
      // MODULE_NAME is identity, not an entity: every module exports it, so
      // binding it flat would make two star-imports collide. It stays
      // reachable as `m.MODULE_NAME` and by explicit `from m import
      // MODULE_NAME`. SPEC §modules "import statement".
      if (e.name != "MODULE_NAME") names.push_back({e.name, ""});
    return names;
  }
  if (!s->importFuncs.empty())
    return s->importFuncs; // `from m import a, b as c`
  qualified = true; // `import m`, `import m as a` — the module, not its names
  std::vector<Binding> names;
  for (const auto &e : exports) names.push_back({e.name, ""});
  return names;
}

// Rejects a write through a module qualifier: the module owns its entities, and
// `m.x = 1` on a C++ module would otherwise create a variable named `m` and
// write into nil (SPEC §modules "import statement").
void QLangCompiler::guardNotModuleMember(const ExprPtr &target,
                                         const char *op) const {
  if (!moduleQualified(target->memberBase))
    return;
  throw QLangError(QLangErrorKind::Parse, m_curLine, m_curCol,
                   std::string("cannot ") + op + " module member '" +
                       target->memberBase->name + "." + target->memberName +
                       "'");
}

// Records one entity bound from a loaded module and returns its import index
// (SPEC §bytecodes "import table (lookup)"). `ref` stays the DEFINING module's
// handle — the entry program has no merged index yet, which is exactly what the
// linker resolves (plan Phase 3b).
int QLangCompiler::bindQlImport(const LoadedModule &mod, const ExportEntry &exp,
                                const std::string &localName,
                                const std::string &moduleAs,
                                const StmtPtr &s) {
  const std::string &modName = mod.program.moduleName;
  // A name two sources both claim has no defined meaning, so it is a compile
  // error rather than a precedence rule (plan Phase 3b: "a name in both a C++
  // module and a .ql module = compile error").
  if (m_nativeFuncIds.count(localName) || m_moduleFuncNames.count(localName))
    throw QLangError(
        QLangErrorKind::Parse, s->line, s->col,
        "'" + localName + "' is exported by module '" + modName +
            "' and is also a C++ module or native function name");
  if (findBuiltin(localName) >= 0)
    throw QLangError(
        QLangErrorKind::Parse, s->line, s->col,
        "'" + localName + "' is a built-in function name and cannot be "
                          "imported from module '" +
            modName + "'");
  auto uit = m_userFuncs.find(localName);
  if (uit != m_userFuncs.end())
    throw QLangError(QLangErrorKind::Parse, s->line, s->col,
                     "'" + localName + "' is imported from module '" + modName +
                         "' and is also defined in this module");
  auto it = m_qlImports.find(localName);
  if (it != m_qlImports.end()) {
    const ImportEntry &old = m_imports[static_cast<size_t>(it->second)];
    // Re-importing the very same entity is a no-op, not a collision (the flat
    // forms can reach the same export twice: `import m`, `from m import f`).
    if (old.module == modName && old.name == exp.name)
      return it->second;
    throw QLangError(QLangErrorKind::Parse, s->line, s->col,
                     "'" + localName + "' is already imported from module '" +
                         old.module + "'");
  }
  m_imports.push_back(ImportEntry{modName, exp.name, exp.kind, exp.ref});
  int idx = static_cast<int>(m_imports.size()) - 1;
  m_qlImports[localName] = idx;
  // Provenance for diagnostics and the disassembler, same shape a C++ module
  // import records (the alias is what the source spelled).
  ImportedModuleFunc imp{moduleAs.empty() ? modName : moduleAs, exp.name, false};
  bool seen = false;
  for (const auto &e : m_importedModuleFuncs)
    if (!e.fromClass && e.func == exp.name && e.module == imp.module) {
      seen = true;
      break;
    }
  if (!seen)
    m_importedModuleFuncs.push_back(std::move(imp));
  return idx;
}

// `import` resolved against a loaded image's export table (plan Phases 3b/3c).
// The five forms are the C++ resolver's; only the source of the names differs.
void QLangCompiler::compileQlImport(const StmtPtr &s, const LoadedModule &mod) {
  // A library may import another module: since Phase 3c the references become
  // `getimport`/`callimport` operands into the module's own import table, both
  // of which `.qlc` v9 carries, so the image stays serializable and the linker
  // resolves the chain transitively.
  // `import m as a`: the alias names the module for a later `a.f`.
  if (!s->importAlias.empty())
    m_qlAliases[s->importAlias] = mod.program.moduleName;
  const std::string qualifier =
      s->importAlias.empty() ? s->importModule : s->importAlias;

  bool qualified = false;
  const std::vector<std::pair<std::string, std::string>> names =
      importNames(s, mod.program.exports, qualified);
  if (qualified)
    m_moduleNs[qualifier] = mod.program.moduleName;

  for (const auto &[fname, alias] : names) {
    const ExportEntry *exp = nullptr;
    for (const auto &e : mod.program.exports)
      if (e.name == fname) { exp = &e; break; }
    if (!exp)
      throw QLangError(
          QLangErrorKind::Parse, s->line, s->col,
          "module '" + mod.program.moduleName + "' does not export '" + fname +
              "'");
    // Bound by this very form already (a repeated `import m`, or the statement
    // resolving again where it sits): the second binding would collide with the
    // first, so it is a no-op.
    // A `import m` entity is bound under the qualifier (`m.f`), so the bare
    // name stays invisible and two modules can each contribute their own `f`;
    // a per-name alias is bound under the alias — collisions and their
    // diagnostics speak the alias, the table keeps the exported name.
    const std::string key =
        qualified ? qualifier + "." + fname : (alias.empty() ? fname : alias);
    if (auto it = m_qlImports.find(key); it != m_qlImports.end()) {
      const ImportEntry &old = m_imports[static_cast<size_t>(it->second)];
      if (old.module == mod.program.moduleName && old.name == fname)
        continue;
    }
    bindQlImport(mod, *exp, key, s->importAlias, s);
  }
}

// Resolves one `import` statement and binds what its form says it binds; the
// statement itself emits no code. Called from the pre-pass for the top level
// and from compileStmt for the class bodies the pre-pass does not walk.
void QLangCompiler::resolveImport(const StmtPtr &s) {
  // Resolves the module and binds what the form says it binds (SPEC §modules
  // "import statement"):
  //   import math                          — binds the MODULE: math.min, math.max
  //   import math as m                     — the same under the alias: m.min
  //   import math.min                      — binds the one name flatly
  //   from math import *                   — binds every name flatly
  //   from math import min, max            — binds those names flatly
  // The same five forms resolve against a loaded `.ql` module's export table
  // when the name is one (external-modules plan v4 Phase 3b); being both is an
  // error, never a precedence. A class body compiles with `m_inFunction` clear,
  // so a class may import; a `def` body may not (see the `else` below).
  if (!m_inFunction) {
    // Find the module.
    const QLangContext::Module *mod = cppModule(s->importModule);
    const LoadedModule *ql = qlModule(s->importModule);
    if (ql && mod)
      throw QLangError(
          QLangErrorKind::Parse, s->line, s->col,
          "module '" + s->importModule +
              "' is registered both as a C++ module and as a loaded .ql module");
    // Nobody loaded it: ask the context to find it (SPEC §api "module search
    // paths"; plan C8a). The C++ module lookup comes first precisely because a
    // native module outranks a file, so discovery runs only when neither
    // lookup above matched. A candidate that was found but could not be taken
    // in (a malformed image, a callback refusal, a missing dependency) throws
    // with the reason; re-thrown as a compile error so the report points at
    // the `import`.
    if (!ql && !mod && m_scope) {
      std::shared_ptr<LoadedModule> found;
      try {
        found = m_scope->findModule(s->importModule);
      } catch (const QLangError &e) {
        throw QLangError(QLangErrorKind::Parse, s->line, s->col, e.what());
      }
      if (found) {
        // The image is registered under its own declared name, which may
        // disagree with the name the import asked for (`foo.qlc` declaring
        // `MODULE_NAME = "bar"`). Record the mapping — the same shape an
        // `import m as a` alias uses — so this import and every later
        // reference to the requested name resolve (Q36: key on MODULE_NAME).
        if (found->program.moduleName != s->importModule)
          m_qlAliases[s->importModule] = found->program.moduleName;
        ql = qlModule(s->importModule);
      }
    }
    if (ql) {
      compileQlImport(s, *ql);
      return;
    }
    if (!mod) {
      // Say what was searched: "undefined module" alone cannot tell a host
      // whether the name was wrong or the path list was.
      std::string msg = "undefined module '" + s->importModule + "'";
      if (m_scope) {
        std::string searched;
        for (const std::string &p : m_scope->moduleSearchPaths()) {
          if (!searched.empty())
            searched += ", ";
          searched += p.empty() ? std::string("<working directory>") : p;
        }
        if (!searched.empty())
          msg += " (searched " + searched + ")";
      }
      throw QLangError(QLangErrorKind::Parse, 0, 0, msg);
    }

    // The five forms, C++ modules (SPEC §modules "import statement"): the flat
    // ones bind a name into THIS module; `import m` / `import m as a` binds the
    // MODULE, whose functions are then reached as `m.name` and never by the
    // bare name. Nothing is pre-bound: a module the program never reaches
    // contributes no native slot.
    if (s->importFunc.empty() && !s->importAll && s->importFuncs.empty()) {
      m_moduleNs[s->importAlias.empty() ? s->importModule : s->importAlias] =
          mod->name;
      return;
    }

    // Collect function names to import, with any per-name alias.
    std::vector<std::pair<std::string, std::string>> names;
    if (!s->importFunc.empty())
      names.push_back({s->importFunc, ""}); // `import m.f` — one explicit name
    else if (s->importAll) // `from m import *`
      for (const auto &[fname, func] : mod->funcs) names.push_back({fname, ""});
    else // `from m import min, max as mm`
      names = s->importFuncs;

    // Bind each function to its native slot under the flat name — under the
    // alias when one was given; the provenance keeps the exported name, so
    // the listing still says `module.name`.
    for (const auto &[fname, alias] : names) {
      auto it = mod->funcs.find(fname);
      if (it == mod->funcs.end())
        throw QLangError(QLangErrorKind::Parse, s->line, s->col,
                         "undefined function '" + fname + "' in module '" +
                             mod->name + "'");
      const std::string localName = alias.empty() ? fname : alias;
      // The other direction of the collision check in bindQlImport: a name a
      // loaded `.ql` module already bound cannot also arrive from a C++ module.
      auto qit = m_qlImports.find(localName);
      if (qit != m_qlImports.end())
        throw QLangError(
            QLangErrorKind::Parse, s->line, s->col,
            "'" + localName + "' is imported from module '" +
                m_imports[static_cast<size_t>(qit->second)].module +
                "' and is also exported by C++ module '" + mod->name + "'");
      bindNative(localName, it->second);
      // Record the provenance (module -> func) once per name; the first
      // binding wins, mirroring the slot above (a re-import of the same name
      // resolves to the same native slot).
      ImportedModuleFunc imp{mod->name, fname, false};
      bool seen = false;
      for (const auto &e : m_importedModuleFuncs)
        if (!e.fromClass && e.func == fname) { seen = true; break; }
      if (!seen)
        m_importedModuleFuncs.push_back(std::move(imp));
    }
  } else {
    // `import` is a module-level statement: it binds names into the module's
    // scope, and a function body has private registers and no module scope to
    // bind into. Dropping it silently left the name unbound, so the call that
    // should have resolved at compile time failed at run time instead
    // (`unknown native function`).
    throw QLangError(QLangErrorKind::Parse, s->line, s->col,
                     "import of module '" + s->importModule +
                         "' must appear at module top level, not inside a "
                         "function body");
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
      emitPool(Opcode::MOV, keyR, emitConst(QLangValue::String(st->name)));
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
      // Record the class method's provenance so tools never list it as a
      // main-module export.
      m_importedModuleFuncs.push_back({s->className, st->name, true});
      auto closure = std::make_shared<Closure>();
      closure->funcIndex = funcIndex;
      int constIdx = emitConst(QLangValue::Function(closure));
      int keyR = tmpRegister();
      emitPool(Opcode::MOV, keyR, emitConst(QLangValue::String(st->name)));
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
      emitPool(Opcode::MOV, keyR, emitConst(QLangValue::String(sf.first)));
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
      // Record the static method's provenance (see the instance-method note).
      m_importedModuleFuncs.push_back({s->className, sm.name, true});
      sm.funcIndex = funcIndex;
      auto closure = std::make_shared<Closure>();
      closure->funcIndex = funcIndex;
      int constIdx = emitConst(QLangValue::Function(closure));
      int keyR = tmpRegister();
      emitPool(Opcode::MOV, keyR, emitConst(QLangValue::String(sm.name)));
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
        emitPool(Opcode::MOV, keyR, emitConst(QLangValue::String(cn)));
        int valR = tmpRegister();
        emitImmI(Opcode::MOV, valR, 1);
        emit(Opcode::TABPUT, sconstReg, keyR, valR);
        freeRegister(keyR);
        freeRegister(valR);
      }
      int sckeyR = tmpRegister();
      emitPool(Opcode::MOV, sckeyR,
               emitConst(QLangValue::String("__static_const__")));
      emit(Opcode::TABPUT, staticsReg, sckeyR, sconstReg);
      freeRegister(sckeyR);
      freeRegister(sconstReg);
    }
    int skeyR = tmpRegister();
    emitPool(Opcode::MOV, skeyR, emitConst(QLangValue::String("__statics__")));
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
      emitPool(Opcode::MOV, keyR, emitConst(QLangValue::String(cn)));
      int valR = tmpRegister();
      emitImmI(Opcode::MOV, valR, 1);
      emit(Opcode::TABPUT, cfieldsReg, keyR, valR);
      freeRegister(keyR);
      freeRegister(valR);
    }
    int cfkeyR = tmpRegister();
    emitPool(Opcode::MOV, cfkeyR,
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
      emitPool(Opcode::MOV, keyR, emitConst(QLangValue::String(ft.first)));
      int valR = tmpRegister();
      QLangValue tv(ft.second);
      emitPool(Opcode::MOV, valR, emitConst(QLangValue::String(tv.typeName())));
      emit(Opcode::TABPUT, ftypesReg, keyR, valR);
      freeRegister(keyR);
      freeRegister(valR);
    }
    int ftkeyR = tmpRegister();
    emitPool(Opcode::MOV, ftkeyR,
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
      emitPool(Opcode::MOV, keyR, emitConst(QLangValue::String(fieldNames[i])));
      int valR = tmpRegister();
      emitImmI(Opcode::MOV, valR, static_cast<int32_t>(i));
      emit(Opcode::TABPUT, fieldsReg, keyR, valR);
      freeRegister(keyR);
      freeRegister(valR);
    }
    int fkeyR = tmpRegister();
    emitPool(Opcode::MOV, fkeyR, emitConst(QLangValue::String(kFieldMetaKey)));
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
  // Record the prototype function's provenance (see the method notes).
  m_importedModuleFuncs.push_back(
      {s->className, s->className + "_prototype", true});
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
  emitPool(Opcode::MOV, r, emitConst(QLangValue::Table(kv)));
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
  //   [match check: EQ/IN] -> tst + je -> [JMP to skip] -> [case body]
  // After all cases, compile default, then emit JMP to switch end.
  // The JMP after each case jumps to the next case's check (or switchEnd for last).
  std::vector<int> caseMatchStarts; // instruction index of each case's match start (const emit)
  std::vector<int> caseJeIdx;       // `je` of each case's truthiness test
  std::vector<int> caseSkipJmpIdx;    // JMP after the test (skip to next case)
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
    // tst + je; the target is patched below to the case body start.
    caseJeIdx.push_back(emitJumpIfTrue(cmpReg));
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

  // Patch all truthiness-test jumps to point at their case body start,
  // skip-JMP targets to next case (or default body for last),
  // and exit-JMP targets to switchEnd.
  for (size_t i = 0; i < s->cases.size(); i++) {
    m_emitter.patchJump(caseJeIdx[i], caseBodyStarts[i]);
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
  trace(TRACE_FULL, "[DBG-SW] constants.size()=%zu\n", m_pool.size());
  for (size_t i = 0; i < m_pool.size(); i++) {
    const auto &c = m_pool[i];
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
    // One instruction: an inline immediate when the literal's type has a v6
    // form for it (int/float/bool/nil), otherwise a constant-pool read.
    emitLiteral(Opcode::MOV, dest, e->literal);
    break;
  case ExprKind::Variable: {
    // Bare static member name inside a method body of a class with statics:
    // read from the class-level `__statics__` table (shared across instances).
    if (!m_classStaticNames.empty() && m_classStaticNames.count(e->name)) {
      int staticsR = staticsTableReg();
      int keyR = tmpRegister();
      emitPool(Opcode::MOV, keyR, emitConst(QLangValue::String(e->name)));
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
    // Imported from a loaded `.ql` module (external-modules plan v4 Phase 3c):
    // the value lives in the DEFINING module's shared cell, so it is read
    // through the import table. An operand that names a table entry instead of
    // a register is what makes the cell reachable from any depth — Phase 3b
    // could only express it as a register move, which a function's private
    // registers cannot name.
    auto iit = m_qlImports.find(e->name);
    if (iit != m_qlImports.end()) {
      const ImportEntry &im = m_imports[static_cast<size_t>(iit->second)];
      if (im.kind == ExportKind::Func)
        throw QLangError(
            QLangErrorKind::Parse, 0, 0,
            "'" + e->name + "' is a function imported from module '" +
                im.module + "': call it instead of reading it");
      // An exported `const` is not folded to a literal here: its value exists
      // only once the defining module's top level has run, which the compiler
      // cannot know from an image.
      emit(Opcode::GETIMPORT, dest, iit->second);
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
    // A MODULE, imported or not: the qualifier reaches the entities, the name
    // on its own is no value (SPEC §modules "import statement"). A module
    // that is only loaded — pulled in transitively, never bound here — gets
    // its own report: the caller is often already reading `m.<name>`, and
    // advising exactly that was the audit F5 complaint.
    if (cppModule(e->name) || qlModule(e->name)) {
      if (!m_moduleNs.count(e->name))
        throw QLangError(
            QLangErrorKind::Parse, 0, 0,
            "module '" + e->name +
                "' is not imported in this module: bind it with 'import " +
                e->name + "' or 'from " + e->name + " import <name>'");
      throw QLangError(
          QLangErrorKind::Parse, 0, 0,
          "'" + e->name +
              "' is a module, not a value: reach what it offers as '" +
                  e->name + ".<name>' (or bind a name into this module"
                  " with 'from " + e->name + " import <name>')");
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
      // Constant folding: `-(literal)` -> one store of a negative immediate
      // instead of a load plus NEG (and a MOV) (SPEC §bytecodes: encoding
      // guidelines — avoid temporary registers when possible).
      if (e->uop.type == TokenType::Minus) {
        auto folded = foldNegLiteral(e);
        if (folded) {
          emitLiteralImm(Opcode::MOV, dest, *folded);
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
    // deepcopy(x) lowers directly to DEEPCOPY dest, src (SPEC §bytecodes
    // "OpCode deepcopy"): the argument is compiled into a temp and the copy
    // lands straight in dest — no CALL, no arg staging in dest+1.. (encoding
    // guidelines: copy directly to the final register, avoid temporaries).
    // Arity is checked here: the spec makes deepcopy() / deepcopy(a, b)
    // compile errors (SPEC §functions "deepcopy"), and the compiler sees the
    // argument count on this path.
    if (!e->calleeMember &&
        findBuiltin(e->callee) == static_cast<int>(Builtin::Deepcopy)) {
      if (e->args.size() != 1)
        throw QLangError(QLangErrorKind::Parse, e->callLine, e->callCol,
                         "deepcopy expects exactly one argument, got " +
                             std::to_string(e->args.size()));
      int srcR = tmpRegister();
      compileExpr(e->args[0], srcR);
      emit(Opcode::DEEPCOPY, dest, srcR, 0);
      m_emitter.setLastLoc(static_cast<uint16_t>(e->callLine),
                           static_cast<uint16_t>(e->callCol));
      releaseTemp(srcR);
      break;
    }
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

    // `m.f(...)` where `m` is a module imported as a module (SPEC §modules
    // "import statement"): the entity is named by its qualified spelling, so a
    // same-named entity of another module cannot answer instead, and the table
    // member path below does not apply — `m` is not a value.
    if (e->calleeMember && moduleQualified(e->calleeMember->memberBase)) {
      const std::string &q = e->calleeMember->memberBase->name;
      const int mb = moduleMember(q, e->callee, e->callLine, e->callCol);
      if (mb < 0) {
        emit(Opcode::CALL, dest, mb, argCount);  // C++ module: negative slot
        m_usedNativeIds.insert(mb);  // this call is what puts the slot in the image
        m_emitter.setLastLoc(static_cast<uint16_t>(e->callLine),
                             static_cast<uint16_t>(e->callCol));
      } else {
        emitImportCall(mb, q + "." + e->callee, argBase, dest, argCount,
                       e->callLine, e->callCol);
      }
      break;
    }
    // Method call: obj.method(args) -> load the member (closure) first.
    if (e->calleeMember) {
      // Manifest method check (plan P4 check 1/2): `T.m(...)` / `T::m(...)`
      // on a manifest type name checks membership only (a static call has no
      // self — the runtime stays authoritative); `p.m(...)` on a known
      // receiver also checks arity and inferable argument tags, with the
      // runtime binding's wording.
      if (e->calleeMember->memberBase->kind == ExprKind::Variable) {
        const std::string &mbn = e->calleeMember->memberBase->name;
        auto eit = m_extTypes.find(mbn);
        const ManifestType *smt =
            eit != m_extTypes.end() ? eit->second : nullptr;
        bool smtIsTypeName = smt != nullptr;
        if (!smt && !m_inFnBody) {
          auto rit = m_knownReceivers.find(mbn);
          if (rit != m_knownReceivers.end())
            smt = rit->second;
        }
        if (smt) {
          const ManifestMember *mm = manifestMember(*smt, e->callee, 2);
          if (!mm)
            throw QLangError(QLangErrorKind::Parse, e->callLine, e->callCol,
                             "type '" + smt->name + "' has no method '" +
                                 e->callee + "'");
          if (!smtIsTypeName) {
            if (static_cast<int>(mm->argTags.size()) != argCount)
              throw QLangError(
                  QLangErrorKind::Parse, e->callLine, e->callCol,
                  "method expects " + std::to_string(mm->argTags.size()) +
                      " argument(s), got " + std::to_string(argCount));
            checkExtArgTags(mm->argTags, e->args,
                            "method '" + e->callee + "' on type '" +
                                smt->name + "'");
          }
        }
      }
      // A manifest type name on the left of '::' is not a script class: the
      // membership check above has run, and the call falls through to the
      // dynamic path (same codegen as T.m(...)).
      const bool extCallStatic =
          e->calleeMember->scopeStatic &&
          e->calleeMember->memberBase->kind == ExprKind::Variable &&
          m_extTypes.count(e->calleeMember->memberBase->name);
      // Static method call: C::name(args). The closure lives in the class-level
      // `__statics__` table and is called with that table as a hidden first
      // argument (the anchor) — no self.
      if (e->calleeMember->scopeStatic && !extCallStatic) {
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
        emitPool(Opcode::MOV, skR,
                 emitConst(QLangValue::String("__statics__")));
        // __proto_<class> is a named register: read it in place.
        emit(Opcode::TABGET, staticsR, regOf("__proto_" + cname), skR);
        int keyR = tmpRegister();
        emitPool(Opcode::MOV, keyR, emitConst(QLangValue::String(e->callee)));
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
      emitPool(Opcode::MOV, keyR,
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
        emitPool(Opcode::MOV, keyR, emitConst(QLangValue::String(mname)));
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
        emitPool(Opcode::MOV, keyR, emitConst(QLangValue::String(fname)));
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
        emitPool(Opcode::MOV, skeyR,
                 emitConst(QLangValue::String("__statics__")));
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
        emitPool(Opcode::MOV, ckeyR,
                 emitConst(QLangValue::String(kClassMetaKey)));
        int cvalR = tmpRegister();
        emitPool(Opcode::MOV, cvalR, emitConst(QLangValue::String(e->callee)));
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
        emitPool(Opcode::MOV, cfkeyR,
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
        emitPool(Opcode::MOV, ftkeyR,
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
        emitPool(Opcode::MOV, fkeyR,
                 emitConst(QLangValue::String(kFieldMetaKey)));
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
        for (int j = 0; j < newArgCount; j++)
          emitImmNil(Opcode::MOV, newArgBase + j);
      }
      break;
    }
    // Called from a loaded `.ql` module (external-modules plan v4 Phase 3c):
    // the key is the flat name a `from` form bound or the qualified name a
    // module import bound (SPEC §modules "import statement").
    const std::string key = callKey(e);
    auto iit = m_qlImports.find(key);
    if (iit != m_qlImports.end()) {
      emitImportCall(iit->second, key, argBase, dest, argCount, e->callLine,
                     e->callCol);
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
          emitImmNil(Opcode::MOV, argBase + j);
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
    // External type construction from a manifest (plan P4 check 1): `T(args)`
    // where T names a manifest type. No constructor -> refused; a typed
    // QL_CTOR gets the full arity + inferable tag checks (runtime wording);
    // the context-taking form is presence-only — its lambda owns validation.
    // Codegen continues below: addExternalTypes ensured a native id (host's
    // or a reserved negative slot re-resolved by the .qlc native table).
    auto extct = m_extTypes.find(e->callee);
    if (extct != m_extTypes.end()) {
      const ManifestType &ext = *extct->second;
      if (!ext.hasCtor)
        throw QLangError(QLangErrorKind::Parse, e->callLine, e->callCol,
                         "type '" + ext.name + "' has no constructor");
      if (!ext.ctorArgTags.empty()) {
        if (static_cast<int>(ext.ctorArgTags.size()) != argCount)
          throw QLangError(
              QLangErrorKind::Parse, e->callLine, e->callCol,
              "constructor expects " +
                  std::to_string(ext.ctorArgTags.size()) +
                  " argument(s), got " + std::to_string(argCount));
        checkExtArgTags(ext.ctorArgTags, e->args,
                        "constructor of type '" + ext.name + "'");
      }
    }
    // Built-in function. The classification API (`findBuiltin`), not the
    // throwing `builtinId`: a name that is not a builtin is a normal answer on
    // this path — a native (C++) function takes the negative-slot branch below
    // and an unknown name raises its own compile error there.
    int funcId = findBuiltin(e->callee);
    // Native (C++) function — use negative index.
    if (funcId < 0) {
      // `connect(o, "sig", ...)` / `disconnect(o, "sig")` with a statically
      // known signal name on a known receiver: the signal must exist (plan
      // P4 check 3). A dynamic name or receiver stays runtime-checked.
      if ((e->callee == "connect" || e->callee == "disconnect") &&
          !m_inFnBody && e->args.size() >= 2 &&
          e->args[0]->kind == ExprKind::Variable &&
          e->args[1]->kind == ExprKind::Literal &&
          e->args[1]->literal.type() == QLangValue::Type::String) {
        auto rit = m_knownReceivers.find(e->args[0]->name);
        if (rit != m_knownReceivers.end() &&
            !manifestSignal(*rit->second, e->args[1]->literal.asString()))
          throw QLangError(
              QLangErrorKind::Parse, e->callLine, e->callCol,
              "unknown signal '" + e->args[1]->literal.asString() +
                  "' on type '" + rit->second->name + "'");
      }
      auto nit = m_nativeFuncIds.find(e->callee);
      if (nit == m_nativeFuncIds.end() && extct != m_extTypes.end()) {
        // A manifest type's constructor, and this compile was never told the
        // name: the contract is all the type knowledge it has, so the slot is
        // taken here, at the call (plan native-slot-portability P2). `extct`
        // above already refused a type with no constructor; an engine-agnostic
        // compile has no pointer to bind, and `loadProgram` re-resolves the
        // name in the host that loads the image.
        bindNative(e->callee, nullptr);
        nit = m_nativeFuncIds.find(e->callee);
      }
      if (nit != m_nativeFuncIds.end()) {
        emit(Opcode::CALL, dest, nit->second, argCount);
        m_usedNativeIds.insert(nit->second);  // the image names the calls it makes
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
    // Constant elements: one pool load (mov kind 2) of a pool payload. The
    // load hands out a fresh copy of it (SPEC §types "value semantics"), so
    // mutating the result never corrupts the pool entry this site (or a
    // deduped twin) shares.
    // Non-constant elements (`local outer = [inner]`): the array is built at
    // runtime — load an EMPTY array into the destination, then ARRAPP each
    // element into it (SPEC §types "array: contain list of mixed variable").
    // The destination payload is freshly created here, so appending in place
    // cannot reach any other name.
    bool allConst = true;
    for (auto &el : e->elements) {
      try {
        evalConstExpr(el);
      } catch (...) {
        allConst = false;
        break;
      }
    }
    if (allConst) {
      std::vector<QLangValue> elems;
      elems.reserve(e->elements.size());
      for (auto &el : e->elements)
        elems.push_back(evalConstExpr(el));
      emitPool(Opcode::MOV, dest,
               emitConst(QLangValue::Array(std::move(elems))));
      break;
    }
    emitPool(Opcode::MOV, dest, emitConst(QLangValue::Array()));
    for (auto &el : e->elements) {
      int t = tmpRegister();
      compileExpr(el, t);
      emit(Opcode::ARRAPP, dest, t, 0);
      releaseTemp(t); // dead after the append
    }
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
      emitPool(Opcode::MOV, dest, emitConst(QLangValue::Table(kv)));
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
        emitPool(Opcode::MOV, keyR,
                 emitConst(QLangValue::String(item.first->name)));
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
    // base[index] read -> INDEXGET dest, baseR, idxR. The base kind is not
    // known statically, so the polymorphic opcode is used: it dispatches to an
    // array index or to ObjectOps field access (SPEC §declarations:
    // `t["age"]` is equivalent to `t.age`).
    int baseR = tmpRegister();
    compileExpr(e->indexBase, baseR);
    int idxR = tmpRegister();
    compileExpr(e->indexExpr, idxR);
    emit(Opcode::INDEXGET, dest, baseR, idxR);
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
        // A host-injected enum has no hidden-var register inside a function
        // body until captured — the member value folds, so no load is
        // needed. Anything else must have a live binding (register or
        // capture), which keeps a block-scoped `local enum` invisible
        // outside its block.
        if (regOf(hvar) < 0 && !m_captureRegs.count(hvar) &&
            !m_injectedEnumNames.count(e->memberBase->name))
          throw QLangError(QLangErrorKind::Parse, 0, 0,
                           "unknown enum '" + e->memberBase->name + "'");
        bool found = false;
        for (const auto &m : eit->second) {
          if (m.first == e->memberName) {
            emitPool(Opcode::MOV, dest, emitConst(m.second));
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
    // A module qualifier on the left (`math.min`, SPEC §modules "import
    // statement"): an exported VALUE of a `.ql` module lives in an import-table
    // cell; a function has no value under this form, only a call (`m.f(...)`).
    if (moduleQualified(e->memberBase)) {
      const std::string &q = e->memberBase->name;
      const int mb = moduleMember(q, e->memberName, m_curLine, m_curCol);
      if (mb >= 0 &&
          m_imports[static_cast<size_t>(mb)].kind != ExportKind::Func) {
        emit(Opcode::GETIMPORT, dest, mb);
        break;
      }
      throw QLangError(
          QLangErrorKind::Parse, m_curLine, m_curCol,
          mb < 0 ? "module function '" + q + "." + e->memberName +
                       "' is not a value: call it as '" + q + "." +
                       e->memberName + "(...)'"
                 : "imported function '" + q + "." + e->memberName +
                       "' is not a first-class function value: call it as '" +
                       q + "." + e->memberName + "(...)'");
    }
    // Static access on a manifest type name (`T::M`) falls through to the
    // dynamic path below, where the manifest check runs — enum members have
    // already folded above, and with bindings T and T:: reach the same
    // TypeRef value at runtime.
    const bool extStaticBase =
        e->memberBase->kind == ExprKind::Variable &&
        m_extTypes.count(e->memberBase->name);
    if (e->scopeStatic && !extStaticBase) {
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
      emitPool(Opcode::MOV, skR, emitConst(QLangValue::String("__statics__")));
      emit(Opcode::TABGET, staticsR, baseR, skR);
      int keyR = tmpRegister();
      emitPool(Opcode::MOV, keyR, emitConst(QLangValue::String(e->memberName)));
      emit(Opcode::TABGET, dest, staticsR, keyR);
      break;
    }
    // Manifest shape check (plan P4 check 1/2): the base names a manifest
    // type, or a statically known receiver `let p = T(...)` at top level.
    // Unknown name -> has no field; a method read bare off a TYPE name is
    // refused (mirrors the module-entity rule — TypeRef carries no methods);
    // an instance's method reads as a BoundMethod and stays legal.
    if (e->memberBase->kind == ExprKind::Variable) {
      const std::string &mbn = e->memberBase->name;
      auto eit = m_extTypes.find(mbn);
      const ManifestType *smt = eit != m_extTypes.end() ? eit->second : nullptr;
      bool smtIsTypeName = smt != nullptr;
      if (!smt && !m_inFnBody) {
        auto rit = m_knownReceivers.find(mbn);
        if (rit != m_knownReceivers.end())
          smt = rit->second;
      }
      bool extFolded = false;
      if (smt) {
        if (smtIsTypeName)
        {
          // `T.M` / `T::M` for an enum entry declared on the type: fold to
          // the Int value from the manifest (mirrors the runtime TypeRef
          // constant lookup; plan P4 check 1).
          for (const EnumMeta &em : smt->enums)
            for (const EnumEntry &en : em.entries)
              if (en.name == e->memberName)
              {
                emitPool(Opcode::MOV, dest,
                         emitConst(QLangValue::Int(
                             static_cast<int32_t>(en.value))));
                extFolded = true;
                break;
              }
        }
        if (extFolded)
          break;
        const ManifestMember *mm = manifestMember(*smt, e->memberName, -1);
        if (!mm)
          throw QLangError(QLangErrorKind::Parse, 0, 0,
                           "type '" + smt->name + "' has no field '" +
                               e->memberName + "'");
        if (mm->kind == 2 && smtIsTypeName)
          throw QLangError(
              QLangErrorKind::Parse, 0, 0,
              "'" + mbn + "." + e->memberName +
                  "' is a method, not a value: call it as '" + mbn + "." +
                  e->memberName + "(...)'");
      }
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
    emitPool(Opcode::MOV, keyR, emitConst(QLangValue::String(e->memberName)));
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
    // The condition goes to a temp; `tst` + `jne` branches to the false side.
    // Block branches lower to a closure call whose `return` yields the
    // ternary's value.
    int condReg = tmpRegister();
    compileExpr(e->cond, condReg);
    size_t jumpFalseIdx = static_cast<size_t>(emitJumpIfFalse(condReg));
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

    emitPool(Opcode::MOV, templateReg, emitConst(QLangValue::String(fmt)));

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
    if (findBuiltin(id) >= 0) continue;
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

QLangValue QLangCompiler::evalConstExpr(const ExprPtr &e) {
  if (!e)
    throw QLangError(QLangErrorKind::Parse, 0, 0, "null array/table element");
  switch (e->kind) {
  case ExprKind::Literal:
    return e->literal;
  case ExprKind::Array: {
    std::vector<QLangValue> elems;
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
      emitImmI(Opcode::MOV, idxR, 0); // ab[] -> append
    int valR = tmpRegister();
    if (target->indexExpr && isCompound) {
      // read-modify-write for compound ops (e.g. a[i] += v, t["n"] += v)
      int tmpR = tmpRegister();
      emit(Opcode::INDEXGET, tmpR, baseR, idxR);
      Opcode opCode = (op.text == "++") ? Opcode::INC
                      : (op.text == "--") ? Opcode::DEC
                                          : binOp(op.text.substr(0, op.text.size() - 1));
      if ((opCode == Opcode::ADD || opCode == Opcode::SUB) && isIntLiteral(value)) {
        // `a[i] += 12` -> `add tmpR, i8 12` in place (SPEC §2.6 in-place form).
        emitImmI(opCode == Opcode::ADD ? Opcode::ADD : Opcode::SUB, tmpR,
                 value->literal.asInt());
      } else {
        compileExpr(value, valR);
        emit(opCode, tmpR, tmpR, valR);
      }
      emit(Opcode::INDEXPUT, baseR, idxR, tmpR);
    } else if (target->indexExpr) {
      // Plain index assignment ab[i] = v / t["k"] = v (SPEC §declarations):
      // direct put — the compound path above would call binOp with the empty
      // operator text left by stripping the '='. INDEXPUT dispatches on the
      // runtime base type (array element or ObjectOps field).
      compileExpr(value, valR);
      emit(Opcode::INDEXPUT, baseR, idxR, valR);
    } else {
      compileExpr(value, valR);
      // empty subscript ab[] = v -> append
      emit(Opcode::ARRAPP, baseR, valR, 0);
    }
  } else if (target->kind == ExprKind::Member) {
    guardNotModuleMember(target, "assign to");
    const std::string &baseName = target->memberBase->name;
    // Manifest write check (plan P4): the field must exist and be writable;
    // the read-only rejection mirrors the VM wording.
    if (!m_inFnBody) {
      auto eit = m_extTypes.find(baseName);
      const ManifestType *smt =
          eit != m_extTypes.end() ? eit->second : nullptr;
      if (!smt) {
        auto rit = m_knownReceivers.find(baseName);
        if (rit != m_knownReceivers.end())
          smt = rit->second;
      }
      if (smt) {
        const ManifestMember *mm = manifestMember(*smt, target->memberName, -1);
        if (!mm)
          throw QLangError(QLangErrorKind::Parse, m_curLine, m_curCol,
                           "type '" + smt->name + "' has no field '" +
                               target->memberName + "'");
        if (mm->kind == 1)
          throw QLangError(
              QLangErrorKind::Parse, m_curLine, m_curCol,
              "cannot assign to const field '" + target->memberName + "'");
        if (mm->kind == 2)
          throw QLangError(
              QLangErrorKind::Parse, m_curLine, m_curCol,
              "field '" + target->memberName + "' on type '" + smt->name +
                  "' is a method");
      }
    }
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
    emitPool(Opcode::MOV, keyR,
             emitConst(QLangValue::String(target->memberName)));
    int valR = tmpRegister();
    if (isCompound) {
      int tmpR = tmpRegister();
      emit(Opcode::TABGET, tmpR, baseR, keyR);
      Opcode opCode = (op.text == "++") ? Opcode::INC
                      : (op.text == "--") ? Opcode::DEC
                                          : binOp(op.text.substr(0, op.text.size() - 1));
      if ((opCode == Opcode::ADD || opCode == Opcode::SUB) && isIntLiteral(value)) {
        // `t.k += 12` -> `add tmpR, i8 12` in place (SPEC §2.6 in-place form).
        emitImmI(opCode == Opcode::ADD ? Opcode::ADD : Opcode::SUB, tmpR,
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
