// QLM1 type-manifest + QPM1 portable-module codecs (docs/SERIALIZATION.md
// §11, §12).
//
// Little-endian, byte-by-byte, same writer/Reader discipline as
// QLangBytecodeIO.cpp. The reader treats input as untrusted: every read is
// bounds-checked, counts are capped at the §8 limits, and any failure
// throws QLangError(Io, "qlm: ... (byte offset N)") with no partial state.

#include "qlang/QLangManifest.h"

#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "qlang/QLangBytecodeIO.h"
#include "qlang/QLangErrors.h"

namespace qlang {
namespace {

// Robustness limit (docs/SERIALIZATION.md §8: total decoded values, 64 Mi).
constexpr uint64_t kMaxEntries = 64ull * 1024 * 1024;

// --- Writer helpers (explicit little-endian, no host-endian memcpy) ------
void putU8(std::string &b, uint8_t v) { b.push_back(static_cast<char>(v)); }
void putU32(std::string &b, uint32_t v) {
  for (int i = 0; i < 4; i++)
    b.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void putI64(std::string &b, int64_t v) {
  uint64_t u = static_cast<uint64_t>(v);
  for (int i = 0; i < 8; i++)
    b.push_back(static_cast<char>((u >> (8 * i)) & 0xFF));
}
void putStr(std::string &b, const std::string &s) {
  putU32(b, static_cast<uint32_t>(s.size()));
  b += s;
}
void putTags(std::string &b, const std::vector<QTypeTag> &tags) {
  if (tags.size() > 0xFF)
    throw QLangError(QLangErrorKind::Io, 0, 0,
                     "qlm: too many arguments (>255) in manifest entry");
  putU8(b, static_cast<uint8_t>(tags.size()));
  for (QTypeTag t : tags)
    putU8(b, static_cast<uint8_t>(t));
}

// --- Bounds-checked reader over an in-memory buffer -----------------------
struct Reader {
  const std::string &buf;
  size_t pos = 0;
  // The message prefix: "qlm" for a manifest, "qpm" for a portable module
  // (whose reader reuses this one for its own framing).
  const char *prefix = "qlm";

  [[noreturn]] void fail(const std::string &reason) const {
    throw QLangError(QLangErrorKind::Io, 0, 0,
                     std::string(prefix) + ": " + reason + " (byte offset " +
                         std::to_string(pos) + ")");
  }
  void need(size_t n) const {
    if (pos + n > buf.size())
      fail("truncated read");
  }
  uint8_t u8() {
    need(1);
    return static_cast<uint8_t>(buf[pos++]);
  }
  uint32_t u32() {
    need(4);
    uint32_t v = 0;
    for (int i = 3; i >= 0; i--)
      v = (v << 8) | static_cast<uint8_t>(buf[pos + i]);
    pos += 4;
    return v;
  }
  // A u32 count, rejected above the §8 cap before it can size anything.
  uint32_t count() {
    uint32_t n = u32();
    if (n > kMaxEntries)
      fail("count too large");
    return n;
  }
  uint64_t u64() {
    need(8);
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
      v = (v << 8) | static_cast<uint8_t>(buf[pos + i]);
    pos += 8;
    return v;
  }
  int64_t i64() { return static_cast<int64_t>(u64()); }
  std::string str() {
    uint32_t len = u32();  // bounded by the remaining-bytes check below
    need(len);
    std::string s(buf.data() + pos, len);
    pos += len;
    return s;
  }
  QTypeTag tag() {
    uint8_t t = u8();
    // The valid QTypeTag values: the scalar block 0x00-0x0B, External, Void.
    if (!(t <= 0x0B || t == 0xFE || t == 0xFF))
      fail("invalid type tag");
    return static_cast<QTypeTag>(t);
  }
  std::vector<QTypeTag> tags() {
    uint8_t n = u8();
    std::vector<QTypeTag> v;
    v.reserve(n);
    for (uint8_t i = 0; i < n; i++)
      v.push_back(tag());
    return v;
  }
};

// Canonical body bytes (everything the entry's fingerprint hashes).
std::string typeEntryBody(const ManifestType &t) {
  std::string b;
  putStr(b, t.name);
  putU8(b, (t.hasStr ? 1 : 0) | (t.hasCtor ? 2 : 0));
  putU32(b, static_cast<uint32_t>(t.members.size()));
  for (const auto &m : t.members) {
    if (m.kind > 2)
      throw QLangError(QLangErrorKind::Io, 0, 0,
                       "qlm: invalid member kind in manifest entry");
    putU8(b, m.kind);
    putStr(b, m.name);
    if (m.kind == 2) {
      putU8(b, static_cast<uint8_t>(m.retTag));
      putTags(b, m.argTags);
    } else {
      putU8(b, static_cast<uint8_t>(m.getTag));
      putU8(b, static_cast<uint8_t>(m.setTag));
    }
  }
  putU32(b, static_cast<uint32_t>(t.enums.size()));
  for (const auto &e : t.enums) {
    putStr(b, e.typeName);
    putU32(b, static_cast<uint32_t>(e.entries.size()));
    for (const auto &en : e.entries) {
      putStr(b, en.name);
      putI64(b, static_cast<int64_t>(en.value));  // full long, no truncation
    }
  }
  putU32(b, static_cast<uint32_t>(t.signals.size()));
  for (const auto &s : t.signals) {
    putStr(b, s.name);
    putTags(b, s.argTags);
  }
  if (t.hasCtor)  // section only when hasCtor; empty list = context-taking
    putTags(b, t.ctorArgTags);
  return b;
}

ManifestType parseTypeBody(Reader &r) {
  ManifestType t;
  t.name = r.str();
  uint8_t flags = r.u8();
  if (flags & ~0x3u)
    r.fail("invalid flags byte");
  t.hasStr = (flags & 1) != 0;
  t.hasCtor = (flags & 2) != 0;
  uint32_t memberCount = r.count();
  for (uint32_t i = 0; i < memberCount; i++) {
    ManifestMember m;
    uint8_t kind = r.u8();
    if (kind > 2)
      r.fail("invalid member kind");
    m.kind = kind;
    m.name = r.str();
    if (kind == 2) {
      m.retTag = r.tag();
      m.argTags = r.tags();
    } else {
      m.getTag = r.tag();
      m.setTag = r.tag();
    }
    t.members.push_back(std::move(m));
  }
  uint32_t enumCount = r.count();
  for (uint32_t i = 0; i < enumCount; i++) {
    EnumMeta e;
    e.typeName = r.str();
    uint32_t entryCount = r.count();
    for (uint32_t j = 0; j < entryCount; j++)
      e.entries.push_back({r.str(), static_cast<long>(r.i64())});
    t.enums.push_back(std::move(e));
  }
  uint32_t signalCount = r.count();
  for (uint32_t i = 0; i < signalCount; i++) {
    ManifestSignal s;
    s.name = r.str();
    s.argTags = r.tags();
    t.signals.push_back(std::move(s));
  }
  if (t.hasCtor)
    t.ctorArgTags = r.tags();
  return t;
}

// Copy from any meta-ish holder (TypeMeta and TypeDescriptor share the
// field names; the tag-carrying Member/SignalMeta/EnumMeta are the same
// types in both).
template <class D> ManifestType manifestTypeFromImpl(const D &d) {
  ManifestType t;
  t.name = d.name;
  t.hasStr = d.hasStr;
  t.hasCtor = d.hasCtor;
  t.ctorArgTags = d.ctorArgTags;
  for (const auto &m : d.members) {
    ManifestMember mm;
    mm.kind = static_cast<uint8_t>(m.kind);
    mm.name = m.name;
    mm.getTag = m.getTag;
    mm.setTag = m.setTag;
    mm.retTag = m.retTag;
    mm.argTags = m.argTags;
    t.members.push_back(std::move(mm));
  }
  t.enums = d.enums;
  for (const auto &s : d.signals)
    t.signals.push_back({s.name, s.argTags});
  return t;
}

}  // namespace

ManifestType manifestTypeFrom(const TypeMeta &m) {
  return manifestTypeFromImpl(m);
}
ManifestType manifestTypeFrom(const TypeDescriptor &d) {
  return manifestTypeFromImpl(d);
}

void writeTypeManifest(std::ostream &os) {
  std::vector<ManifestType> types;
  std::unordered_set<std::string> names;
  for (const auto &acc : metaRegistry()) {
    ManifestType t = manifestTypeFrom(acc());
    if (!names.insert(t.name).second)
      throw QLangError(QLangErrorKind::Io, 0, 0,
                       "qlm: duplicate type name '" + t.name +
                           "' in meta registry");
    types.push_back(std::move(t));
  }
  writeManifest(os, types);
}

std::string serializeTypeEntry(const ManifestType &t) {
  std::string body = typeEntryBody(t);
  std::string out;
  uint64_t fp = fnv1a64(body);
  for (int i = 0; i < 8; i++)
    out.push_back(static_cast<char>((fp >> (8 * i)) & 0xFF));
  out += body;
  return out;
}

ManifestType parseTypeEntry(const std::string &data, size_t &pos) {
  Reader r{data};
  r.pos = pos;
  uint64_t stored = r.u64();
  size_t bodyStart = r.pos;
  ManifestType t = parseTypeBody(r);
  if (fnv1a64(data.substr(bodyStart, r.pos - bodyStart)) != stored)
    r.fail("fingerprint mismatch");
  pos = r.pos;
  return t;
}

void writeManifest(std::ostream &os, const std::vector<ManifestType> &types) {
  std::string out = "QLM1";
  putU32(out, kQlmetaVersion);
  putU32(out, static_cast<uint32_t>(types.size()));
  for (const auto &t : types)
    out += serializeTypeEntry(t);
  os.write(out.data(), static_cast<std::streamsize>(out.size()));
  if (!os)
    throw QLangError(QLangErrorKind::Io, 0, 0, "qlm: stream write failed");
}

std::vector<ManifestType> readManifest(std::istream &is) {
  std::string buf((std::istreambuf_iterator<char>(is)),
                  std::istreambuf_iterator<char>());
  Reader r{buf};
  if (buf.substr(0, 4) != "QLM1")
    r.fail("bad magic");
  r.pos = 4;
  uint32_t version = r.u32();
  if (version != kQlmetaVersion)
    r.fail("unsupported version");
  uint32_t typeCount = r.count();
  std::vector<ManifestType> types;
  std::unordered_set<std::string> names;
  for (uint32_t i = 0; i < typeCount; i++) {
    ManifestType t = parseTypeEntry(buf, r.pos);
    if (!names.insert(t.name).second)
      r.fail("duplicate type name");
    types.push_back(std::move(t));
  }
  if (r.pos != buf.size())
    r.fail("trailing bytes");
  return types;
}

// ---------------------------------------------------------------------------
// QPM1 portable module (docs/SERIALIZATION.md §12)
// ---------------------------------------------------------------------------

namespace {

// A section length has to fit the u32 that carries it — silently truncating
// one would write a package whose sections point nowhere.
void putSized(std::string &b, const std::string &s, const char *what) {
  if (s.size() > 0xFFFFFFFFull)
    throw QLangError(QLangErrorKind::Io, 0, 0,
                     std::string("qpm: ") + what + " is too large to write");
  putU32(b, static_cast<uint32_t>(s.size()));
  b += s;
}

// The container's invariants, shared by the writer (never emit a package the
// reader would refuse) and the reader (untrusted input): "" when the set of
// images is one the loader can work with.
std::string qpmInvariants(const std::string &entryName,
                          const std::vector<QpmImage> &images) {
  if (images.empty())
    return "no images";
  std::unordered_set<std::string> names;
  bool hasEntry = false;
  for (const auto &img : images) {
    if (img.name.empty())
      return "an image has no name";
    if (!names.insert(img.name).second)
      return "duplicate image '" + img.name + "'";
    if (img.name == entryName)
      hasEntry = true;
  }
  if (!hasEntry)
    return "entry '" + entryName + "' is not one of the embedded images";
  return std::string();
}

}  // namespace

void writeQpm(std::ostream &os, const std::string &entryName,
              const std::vector<ManifestType> &manifest,
              const std::vector<QpmImage> &images) {
  const std::string bad = qpmInvariants(entryName, images);
  if (!bad.empty())
    throw QLangError(QLangErrorKind::Io, 0, 0, "qpm: " + bad);
  if (images.size() > kMaxEntries)
    throw QLangError(QLangErrorKind::Io, 0, 0, "qpm: too many images");
  std::ostringstream ms(std::ios::binary);
  writeManifest(ms, manifest);  // a full QLM1, embedded as-is
  std::string out = "QPM1";
  putU32(out, kQpmVersion);
  putSized(out, entryName, "entry name");
  putSized(out, ms.str(), "manifest");
  putU32(out, static_cast<uint32_t>(images.size()));
  for (const auto &img : images) {
    putSized(out, img.name, "image name");
    putSized(out, img.bytes, "image");
  }
  os.write(out.data(), static_cast<std::streamsize>(out.size()));
  if (!os)
    throw QLangError(QLangErrorKind::Io, 0, 0, "qpm: stream write failed");
}

QpmPackage readQpm(std::istream &is) {
  std::string buf((std::istreambuf_iterator<char>(is)),
                  std::istreambuf_iterator<char>());
  Reader r{buf};
  r.prefix = "qpm";
  if (buf.substr(0, 4) != "QPM1")
    r.fail("not a QPM1 portable module");
  r.pos = 4;
  uint32_t version = r.u32();
  if (version != kQpmVersion)
    r.fail("unsupported version");
  QpmPackage pkg;
  pkg.entryName = r.str();
  // The manifest and every image are handed to the readers that own their
  // formats, over exactly their own bytes: nothing here re-describes them and
  // nothing here runs before the whole package has been checked.
  const std::string manifest = r.str();
  try {
    std::istringstream ms(manifest, std::ios::binary);
    pkg.manifest = readManifest(ms);
  } catch (const QLangError &e) {
    r.fail("manifest: " + e.message());
  }
  uint32_t imageCount = r.count();
  for (uint32_t i = 0; i < imageCount; i++) {
    QpmImage img;
    img.name = r.str();
    img.bytes = r.str();
    pkg.images.push_back(std::move(img));
  }
  // The container's own invariants come first: a package that mis-files its
  // images says so, instead of the payload check below burying the reason.
  const std::string bad = qpmInvariants(pkg.entryName, pkg.images);
  if (!bad.empty())
    r.fail(bad);
  for (QpmImage &img : pkg.images) {
    std::istringstream bs(img.bytes, std::ios::binary);
    CompiledProgram prog;
    std::vector<std::pair<int, std::string>> slots;
    std::string err;
    if (!readQlc(bs, prog, slots, err))
      r.fail("image '" + img.name + "': " + err);
    // The name a package files an image under has to be the name the image
    // declares: the loader keys its registry by the latter, so a package that
    // disagreed would load a module nothing can import.
    if (prog.moduleName != img.name)
      r.fail("image '" + img.name + "' declares module '" + prog.moduleName +
             "'");
  }
  if (r.pos != buf.size())
    r.fail("trailing bytes");
  return pkg;
}

}  // namespace qlang
