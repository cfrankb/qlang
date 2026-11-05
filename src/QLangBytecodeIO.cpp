// .qlc program-image codec (docs/SERIALIZATION.md).
//
// Values: 1-byte tag (QLangValue::Type in declaration order, fixed) plus a
// little-endian payload. Non-durable types (Generator, Object, External,
// BoundMethod, TypeRef) are rejected by writeQlc (F1): a program whose
// constant pool holds them cannot be durably encoded, so the writer returns
// false rather than silently remapping them to Nil. Programs: header +
// function records + native-slot table + variable map. The reader treats its
// input as untrusted: every read is bounds-checked, structural invariants are
// validated, and any failure leaves the out-parameters untouched.

#include "qlang/QLangBytecodeIO.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "qlang/QLangErrors.h"

namespace qlang {
namespace {

// Robustness limits (docs/SERIALIZATION.md §8).
constexpr int kMaxDepth = 64;
constexpr std::size_t kMaxValues = 64 * 1024 * 1024;       // 64 Mi values
constexpr std::size_t kMaxInstructions = 64 * 1024 * 1024; // 64 Mi instructions

// ---------------------------------------------------------------------------
// Little-endian writer helpers. Every multi-byte field is emitted byte by
// byte in explicit little-endian order (F1): no struct memcpy, no
// host-endian assumption.
// ---------------------------------------------------------------------------
void putBytes(std::ostream &os, const void *p, size_t n) {
  os.write(static_cast<const char *>(p), static_cast<std::streamsize>(n));
}
void putU8(std::ostream &os, uint8_t v) { os.put(static_cast<char>(v)); }
void putU16(std::ostream &os, uint16_t v) {
  os.put(static_cast<char>(v & 0xFF));
  os.put(static_cast<char>((v >> 8) & 0xFF));
}
void putU32(std::ostream &os, uint32_t v) {
  os.put(static_cast<char>(v & 0xFF));
  os.put(static_cast<char>((v >> 8) & 0xFF));
  os.put(static_cast<char>((v >> 16) & 0xFF));
  os.put(static_cast<char>((v >> 24) & 0xFF));
}
void putI32(std::ostream &os, int32_t v) { putU32(os, static_cast<uint32_t>(v)); }
void putU64(std::ostream &os, uint64_t v) {
  for (int i = 0; i < 8; i++)
    os.put(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void putI64(std::ostream &os, int64_t v) { putU64(os, static_cast<uint64_t>(v)); }
void putF64(std::ostream &os, double v) {
  uint64_t bits;
  std::memcpy(&bits, &v, sizeof bits);
  putU64(os, bits);
}
void putStr(std::ostream &os, const std::string &s) {
  putU32(os, static_cast<uint32_t>(s.size()));
  putBytes(os, s.data(), s.size());
}

// ---------------------------------------------------------------------------
// Bounds-checked reader over an in-memory buffer. Every failure throws
// QLangError(Io) naming the 0-based byte offset where the read failed.
// ---------------------------------------------------------------------------
struct Reader {
  const std::string &buf;
  size_t pos = 0;

  [[noreturn]] void fail(const std::string &reason) const {
    throw QLangError(QLangErrorKind::Io, 0, 0, "qlc: " + reason +
                               " (byte offset " + std::to_string(pos) + ")");
  }
  void need(size_t n) const {
    if (pos + n > buf.size())
      fail("truncated read");
  }
  uint8_t u8() {
    need(1);
    return static_cast<uint8_t>(buf[pos++]);
  }
  // Explicit little-endian reads (F1): byte by byte, no host-endian memcpy.
  uint16_t u16() {
    need(2);
    uint16_t v = static_cast<uint16_t>(static_cast<uint8_t>(buf[pos]) |
                                       (static_cast<uint8_t>(buf[pos + 1]) << 8));
    pos += 2;
    return v;
  }
  int32_t i32() {
    need(4);
    uint32_t v = static_cast<uint32_t>(static_cast<uint8_t>(buf[pos])) |
                 (static_cast<uint32_t>(static_cast<uint8_t>(buf[pos + 1])) << 8) |
                 (static_cast<uint32_t>(static_cast<uint8_t>(buf[pos + 2])) << 16) |
                 (static_cast<uint32_t>(static_cast<uint8_t>(buf[pos + 3])) << 24);
    pos += 4;
    return static_cast<int32_t>(v);
  }
  uint32_t u32() { return static_cast<uint32_t>(i32()); }
  uint64_t u64() {
    need(8);
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
      v = (v << 8) | static_cast<uint8_t>(buf[pos + i]);
    pos += 8;
    return v;
  }
  int64_t i64() { return static_cast<int64_t>(u64()); }
  double f64() {
    uint64_t bits = u64();
    double d;
    std::memcpy(&d, &bits, 8);
    return d;
  }
  std::string str() {
    uint32_t len = u32();
    need(len);
    std::string s(buf.data() + pos, len);
    pos += len;
    return s;
  }
};

std::vector<uint8_t> buf_copy(Reader &r, uint32_t len) {
  const uint8_t *p = reinterpret_cast<const uint8_t *>(r.buf.data() + r.pos);
  return std::vector<uint8_t>(p, p + len);
}

// Decode one value. Enforces the §8 depth/size limits and rejects the
// non-durable tags (15 Generator, 16 Object), which a writer must never emit.
QLangValue decodeValue(Reader &r, int depth, size_t &valueCount) {
  if (depth > kMaxDepth)
    r.fail("value nesting too deep");
  if (valueCount >= kMaxValues)
    r.fail("too many values");
  valueCount++;
  uint8_t tag = r.u8();
  using T = QLangValue::Type;
  if (tag > static_cast<uint8_t>(T::Set))
    r.fail("invalid value tag");
  switch (static_cast<T>(tag)) {
  case T::Nil:
    return QLangValue();
  case T::Bool:
    return QLangValue::Bool(r.i32() ? 1 : 0);
  case T::Char: {
    int32_t v = r.i32();
    if (v < INT8_MIN || v > INT8_MAX)
      r.fail("char out of range");
    return QLangValue::Char(static_cast<int8_t>(v));
  }
  case T::Byte:
    return QLangValue::Byte(r.u8());
  case T::Word: {
    uint32_t v = r.u32();
    if (v > 0xFFFF)
      r.fail("word out of range");
    return QLangValue::Word(static_cast<uint16_t>(v));
  }
  case T::Short: {
    int32_t v = r.i32();
    if (v < INT16_MIN || v > INT16_MAX)
      r.fail("short out of range");
    return QLangValue::Short(static_cast<int16_t>(v));
  }
  case T::Int:
    return QLangValue::Int(r.i32());
  case T::Unsigned:
    return QLangValue::Unsigned(r.u32());
  case T::Qword:
    return QLangValue::Qword(r.u64());
  case T::String:
    return QLangValue::String(r.str());
  case T::Blob: {
    uint32_t len = r.u32();
    r.need(len);
    std::vector<uint8_t> bytes = buf_copy(r, len);
    r.pos += len;
    return QLangValue::Blob(std::move(bytes));
  }
  case T::Array: {
    uint32_t count = r.u32();
    auto a = std::make_shared<std::vector<QLangValue>>();
    a->reserve(count);
    for (uint32_t i = 0; i < count; i++)
      a->push_back(decodeValue(r, depth + 1, valueCount));
    return QLangValue::Array(std::move(a));
  }
  case T::Float:
    return QLangValue::Float(r.f64());
  case T::Table: {
    uint32_t count = r.u32();
    auto m = std::make_shared<std::map<std::string, QLangValue>>();
    for (uint32_t i = 0; i < count; i++) {
      // Read the key before decoding the value: in `m[k] = v` the right
      // operand is evaluated first (C++17), which would decode the value
      // before the key is read.
      std::string key = r.str();
      (*m)[std::move(key)] = decodeValue(r, depth + 1, valueCount);
    }
    return QLangValue::Table(std::move(m));
  }
  case T::Function: {
    int32_t funcIndex = r.i32();
    uint32_t capCount = r.u32();
    auto c = std::make_shared<Closure>();
    c->funcIndex = funcIndex;
    for (uint32_t i = 0; i < capCount; i++) {
      CaptureRef ref;
      ref.reg = r.i32();
      c->captures.push_back(ref);
    }
    return QLangValue::Function(std::move(c));
  }
  case T::Generator: // non-durable: a file carrying tag 15 is malformed
  case T::Object:    // non-durable: a file carrying tag 16 is malformed
  case T::External:  // non-durable: a file carrying tag 20 is malformed
  case T::BoundMethod: // non-durable: a file carrying tag 21 is malformed
  case T::TypeRef:  // non-durable: a file carrying tag 22 is malformed
    r.fail("invalid value tag");
  case T::Eof:
    return QLangValue(T::Eof);
  case T::Long:
    return QLangValue::Long(r.i64());
  case T::Set: {
    uint32_t count = r.u32();
    auto a = std::make_shared<std::vector<QLangValue>>();
    a->reserve(count);
    for (uint32_t i = 0; i < count; i++)
      a->push_back(decodeValue(r, depth + 1, valueCount));
    return QLangValue::Set(std::move(a));
  }
  }
  r.fail("invalid value tag");
}

} // namespace

// True when the value (and every value nested inside arrays/sets/tables) is
// durable — i.e. carries no non-durable type (Generator, Object, External,
// BoundMethod, TypeRef). writeQlc rejects programs whose constant pools hold
// non-durable values (F1): serializeValue would silently remap them to Nil,
// corrupting a program that depends on them.
bool isDurable(const QLangValue &v) {
  using T = QLangValue::Type;
  T t = v.type();
  if (t == T::Generator || t == T::Object || t == T::External ||
      t == T::BoundMethod || t == T::TypeRef)
    return false;
  switch (t) {
  case T::Array:
  case T::Set: {
    const auto &a = (t == T::Array) ? v.asArray() : v.asSet();
    for (const auto &e : *a)
      if (!isDurable(e))
        return false;
    break;
  }
  case T::Table: {
    const auto &m = v.asTable();
    for (const auto &[k, val] : *m)
      if (!isDurable(val))
        return false;
    break;
  }
  default:
    break;
  }
  return true;
}

void serializeValue(std::ostream &os, const QLangValue &v) {
  using T = QLangValue::Type;
  T t = v.type();
  if (t == T::Generator || t == T::Object || t == T::External ||
      t == T::BoundMethod || t == T::TypeRef)
    t = T::Nil; // not durable (R2); writeQlc rejects these up front (F1)
  putU8(os, static_cast<uint8_t>(t));
  switch (t) {
  case T::Nil:
  case T::Eof:
    break;
  case T::Bool:
  case T::Int:
    putI32(os, v.asInt());
    break;
  case T::Char:
    putI32(os, v.asChar());
    break;
  case T::Short:
    putI32(os, v.asShort());
    break;
  case T::Byte:
    putU8(os, v.asByte());
    break;
  case T::Word:
    putU32(os, v.asWord());
    break;
  case T::Unsigned:
    putU32(os, v.asUnsigned());
    break;
  case T::Qword:
    putU64(os, v.asQword());
    break;
  case T::Long:
    putI64(os, v.asLong());
    break;
  case T::Float:
    putF64(os, v.asFloat());
    break;
  case T::String:
    putStr(os, v.asString());
    break;
  // The shared_ptr is an in-memory detail: the durable form stores the
  // PAYLOAD (bytes / element list), so the .qlc layout is unchanged by
  // Phase 2 pointerization (SPEC §bytecodes "constant pool", SERIALIZATION §4).
  case T::Blob: {
    const auto &b = v.asBlob();
    putU32(os, static_cast<uint32_t>(b->size()));
    putBytes(os, b->data(), b->size());
    break;
  }
  case T::Array:
  case T::Set: {
    const auto &a = (t == T::Array) ? v.asArray() : v.asSet();
    putU32(os, static_cast<uint32_t>(a->size()));
    for (const auto &e : *a)
      serializeValue(os, e);
    break;
  }
  case T::Table: {
    const auto &m = v.asTable();
    putU32(os, static_cast<uint32_t>(m->size()));
    for (const auto &[k, val] : *m) { // std::map: lexicographic (R3)
      putStr(os, k);
      serializeValue(os, val);
    }
    break;
  }
  case T::Function: {
    const auto &c = v.asFunction();
    putI32(os, c.funcIndex);
    putU32(os, static_cast<uint32_t>(c.captures.size()));
    for (const auto &cap : c.captures)
      putI32(os, cap.reg);
    break;
  }
  case T::Generator:
  case T::Object:
  case T::External:
  case T::BoundMethod:
  case T::TypeRef:
    break; // unreachable: remapped to Nil above
  }
}

QLangValue deserializeValue(std::istream &is) {
  std::string buf((std::istreambuf_iterator<char>(is)),
                  std::istreambuf_iterator<char>());
  Reader r{buf};
  size_t valueCount = 0;
  return decodeValue(r, 0, valueCount);
}

bool writeQlc(std::ostream &os, const CompiledProgram &prog,
              const std::map<std::string, int> &nativeIds) {
  // F1: reject programs whose constant pools hold non-durable values
  // (Generator, Object, External, BoundMethod, TypeRef). serializeValue would
  // silently remap them to Nil, corrupting a program that depends on them.
  for (const auto &f : prog.functions)
    for (const auto &c : f.constants)
      if (!isDurable(c))
        return false;
  // Header.
  putBytes(os, "QLC1", 4);
  // Format version (F1): written from the single shared constant
  // kQlcVersion (docs/SERIALIZATION.md §4). v5 is the escape-byte
  // instruction format (flat per-function code stream + offset table);
  // the reader accepts v5 only.
  putU32(os, kQlcVersion); // format version

  // Functions (functions[0] is the implicit "main"; the compiler emits it
  // first, so the §4.1 invariant holds by construction).
  const auto &fns = prog.functions;
  putU32(os, static_cast<uint32_t>(fns.size()));
  for (const auto &f : fns) {
    putStr(os, f.name);
    putI32(os, f.paramCount);
    putI32(os, f.registerCount);
    putU8(os, f.isGenerator ? 1 : 0);
    putU32(os, static_cast<uint32_t>(f.captures.size()));
    for (const auto &cap : f.captures)
      putI32(os, cap.reg);
    // Instruction byte stream (escape-byte format, docs/SERIALIZATION.md
    // §4.1): the raw per-function code blob, verbatim.
    putU32(os, static_cast<uint32_t>(f.code.size()));
    putBytes(os, f.code.data(), f.code.size());
    // Instruction offset table: byte offset of each instruction in the
    // stream (strictly increasing).
    putU32(os, static_cast<uint32_t>(f.instrOffsets.size()));
    for (size_t off : f.instrOffsets)
      putU32(os, static_cast<uint32_t>(off));
    // Location side-table: one (u16 line, u16 col) per instruction,
    // parallel to the offset table.
    putU32(os, static_cast<uint32_t>(f.loc.size()));
    for (const auto &lc : f.loc) {
      putU16(os, lc.first);
      putU16(os, lc.second);
    }
    putU32(os, static_cast<uint32_t>(f.constants.size()));
    for (const auto &c : f.constants)
      serializeValue(os, c);
  }

  // §4.2 native-slot table: the name per slot, ascending, no duplicates.
  // nativeIds maps name -> negative opcode id; the slot is -1 - id.
  std::vector<std::pair<int, const std::string *>> slots;
  for (const auto &[name, id] : nativeIds)
    slots.emplace_back(-1 - id, &name);
  std::sort(slots.begin(), slots.end(),
            [](const auto &x, const auto &y) { return x.first < y.first; });
  putU32(os, static_cast<uint32_t>(slots.size()));
  for (const auto &[slot, name] : slots) {
    putI32(os, slot);
    putStr(os, *name);
  }

  // Variable map (order unspecified; readers must not depend on it).
  putU32(os, static_cast<uint32_t>(prog.vars.size()));
  for (const auto &[name, reg] : prog.vars) {
    putStr(os, name);
    putI32(os, reg);
  }
  putU32(os, static_cast<uint32_t>(prog.constVars.size()));
  for (const auto &name : prog.constVars)
    putStr(os, name);
  putI32(os, prog.registerCount);
  return static_cast<bool>(os);
}

bool readQlc(std::istream &is, CompiledProgram &prog,
             std::vector<std::pair<int, std::string>> &nativeSlots,
             std::string &err) {
  // Out-parameters are left untouched on any failure: build everything in
  // locals and move into the out-parameters only on success.
  CompiledProgram out;
  std::vector<std::pair<int, std::string>> outSlots;
  try {
    std::string buf((std::istreambuf_iterator<char>(is)),
                    std::istreambuf_iterator<char>());
    Reader r{buf};

    // 1. Header: magic + version (never parse an unsupported version).
    if (buf.size() < 12)
      r.fail("truncated header");
    if (buf.compare(0, 4, "QLC1") != 0)
      r.fail("bad magic");
    r.pos = 4;
    uint32_t version = r.u32();
    // v5 only: legacy v1-v4 files (fixed 8-byte instructions) and anything
    // newer are rejected (F1).
    if (version != kQlcVersion)
      r.fail("unsupported version");

    // 2. Functions.
    uint32_t funcCount = r.u32();
    if (funcCount < 1)
      r.fail("empty function table");
    if (funcCount > kMaxInstructions)
      r.fail("too many functions");
    out.functions.reserve(funcCount);
    size_t totalInstrs = 0;
    size_t valueCount = 0;
    for (uint32_t i = 0; i < funcCount; i++) {
      Function fn;
      fn.name = r.str();
      fn.paramCount = r.i32();
      fn.registerCount = r.i32();
      if (fn.paramCount < 0 || fn.registerCount < 0)
        r.fail("negative count");
      uint8_t isGen = r.u8();
      fn.isGenerator = isGen != 0;
      uint32_t capCount = r.u32();
      fn.captures.reserve(capCount);
      for (uint32_t j = 0; j < capCount; j++) {
        CaptureRef ref;
        ref.reg = r.i32();
        fn.captures.push_back(ref);
      }
      // Instruction byte stream (v5 escape-byte format).
      uint32_t codeSize = r.u32();
      // Max instruction is 7 bytes (escaped 3-operand with 2-byte operands).
      if (static_cast<size_t>(codeSize) > kMaxInstructions * 7)
        r.fail("code stream too large");
      fn.code.resize(codeSize);
      for (uint32_t j = 0; j < codeSize; j++)
        fn.code[j] = r.u8();
      // Instruction offset table (strictly increasing).
      uint32_t offCount = r.u32();
      totalInstrs += offCount;
      if (totalInstrs > kMaxInstructions)
        r.fail("too many instructions");
      fn.instrOffsets.resize(offCount);
      for (uint32_t j = 0; j < offCount; j++) {
        uint32_t off = r.u32();
        if (off >= codeSize)
          r.fail("instruction offset beyond code stream");
        if (j > 0 && off <= fn.instrOffsets[j - 1])
          r.fail("instruction offsets not strictly increasing");
        fn.instrOffsets[j] = off;
      }
      // Location side-table: parallel to the offset table.
      uint32_t locCount = r.u32();
      if (locCount != offCount)
        r.fail("location table not parallel to instruction offsets");
      fn.loc.reserve(locCount);
      for (uint32_t j = 0; j < locCount; j++)
        fn.loc.emplace_back(r.u16(), r.u16());
      // Decode every instruction: catches truncation, invalid escape
      // modifiers, and unknown opcode identities; the stream must be
      // exactly tiled by the decoded instructions.
      size_t lastEnd = 0;
      for (size_t j = 0; j < fn.instrOffsets.size(); j++) {
        DecodedInstruction di;
        try {
          decodeInstruction(fn.code.data(), fn.instrOffsets[j], codeSize, di);
        } catch (const QLangError &e) {
          r.fail(std::string("invalid instruction: ") + e.message());
        }
        if (!isValidOpcode(di.op))
          r.fail("invalid opcode");
        lastEnd = fn.instrOffsets[j] + di.size;
        if (lastEnd > codeSize)
          r.fail("instruction overruns code stream");
        if (j + 1 < fn.instrOffsets.size() && lastEnd > fn.instrOffsets[j + 1])
          r.fail("instruction overruns next offset");
      }
      if (lastEnd != codeSize)
        r.fail("code stream has trailing bytes");
      uint32_t constCount = r.u32();
      fn.constants.reserve(constCount);
      for (uint32_t j = 0; j < constCount; j++)
        fn.constants.push_back(decodeValue(r, 0, valueCount));
      out.functions.push_back(std::move(fn));
    }
    if (out.functions[0].name != "main")
      r.fail("functions[0] is not 'main'");

    // 3. Native-slot table (unique, ascending, non-empty names).
    uint32_t nativeCount = r.u32();
    int lastSlot = -1;
    for (uint32_t i = 0; i < nativeCount; i++) {
      int32_t slot = r.i32();
      std::string name = r.str();
      if (slot < 0)
        r.fail("negative native slot");
      if (i > 0 && slot <= lastSlot)
        r.fail("duplicate or out-of-order native slot");
      if (name.empty())
        r.fail("empty native name");
      lastSlot = slot;
      outSlots.emplace_back(slot, std::move(name));
    }

    // 4. Variable map + const vars + register count.
    uint32_t varCount = r.u32();
    for (uint32_t i = 0; i < varCount; i++) {
      std::string name = r.str();
      int32_t reg = r.i32();
      if (reg < 0)
        r.fail("negative register");
      out.vars.emplace(std::move(name), reg);
    }
    uint32_t constVarCount = r.u32();
    for (uint32_t i = 0; i < constVarCount; i++)
      out.constVars.insert(r.str());
    out.registerCount = r.i32();
    if (out.registerCount < 0)
      r.fail("negative register count");

    // 5. Closure funcIndex must reference a known function.
    for (auto &fn : out.functions)
      for (const auto &c : fn.constants)
        if (c.type() == QLangValue::Type::Function) {
          int fi = c.asFunction().funcIndex;
          if (fi < 0 || fi >= static_cast<int>(out.functions.size()))
            r.fail("dangling closure function index");
        }

    if (r.pos != buf.size())
      r.fail("trailing bytes");

    prog = std::move(out);
    nativeSlots = std::move(outSlots);
    return true;
  } catch (const QLangError &e) {
    err = e.message();
    return false;
  } catch (const std::bad_alloc &) {
    err = "qlc: out of memory";
    return false;
  }
}

} // namespace qlang
