// QLM1 type-manifest codec: the TDD suite for docs/SERIALIZATION.md §11
// (portable-module-plan P2+P3). Round-trip of every section, the truncation
// matrix, version/magic rejection at the right field, fingerprint stability
// and single-byte sensitivity, the untrusted-input guards (bad kind/tag
// bytes, duplicate names, trailing bytes), manifestTypeFrom flowing the P1
// describable meta into the manifest form, the self-registering meta
// registry + writeTypeManifest, and (argv[1] = qlang_run binary) the
// --qlmeta-dump CLI smoke.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <sys/wait.h>  // WEXITSTATUS for the CLI smoke

#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"
#include "qlang/QLangManifest.h"
#include "qlang/QLangMeta.h"

using namespace qlang;

static int failures = 0;
static int checks = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    ++checks;                                                           \
    if (!(cond)) {                                                      \
      ++failures;                                                       \
      printf("FAIL %d: %s\n", __LINE__, #cond);                         \
    }                                                                   \
  } while (0)

enum class GMode { Off, On };

// A fixture exercising every manifest section through the real QL_OBJECT
// path: rw prop, ro prop, method, enum, signal, str, typed QL_CTOR.
struct Gadget : qlang::QLObject<Gadget> {
  Gadget() = default;
  explicit Gadget(double v) : w(v) {}
  double w = 0;
  std::string label() const { return "g"; }
  int weigh() const { return static_cast<int>(w); }
  void changed(int);  // declared, never implemented (QL_SIGNAL)
  QL_OBJECT(
      QL_PROPERTY(w), QL_PROPERTY(label), QL_INVOKABLE(weigh),
      QL_ENUM(GMode, Off, On), QL_SIGNAL(changed), QL_STR(label),
      QL_CTOR([](double v) { return std::make_shared<Gadget>(v); }));
};

static ManifestType handType() {
  ManifestType t;
  t.name = "Widget";
  t.hasStr = true;
  t.hasCtor = true;
  t.ctorArgTags = {QTypeTag::Float, QTypeTag::Str};
  ManifestMember rw;
  rw.kind = 0;
  rw.name = "w";
  rw.getTag = rw.setTag = QTypeTag::Float;
  ManifestMember ro;
  ro.kind = 1;
  ro.name = "label";
  ro.getTag = QTypeTag::Str;  // setTag stays Unknown (read-only)
  ManifestMember fn;
  fn.kind = 2;
  fn.name = "resize";
  fn.retTag = QTypeTag::Void;
  fn.argTags = {QTypeTag::Int, QTypeTag::Unknown};  // Unknown round-trips
  t.members = {rw, ro, fn};
  t.enums = {{"Mode", {{"Zero", 0}, {"Neg", -5}, {"Big", 5000000000L}}}};
  t.signals = {{"tick", {QTypeTag::Int}}, {"halt", {}}};
  return t;
}

static std::string manifestErr(const std::string &bytes) {
  std::istringstream is(bytes);
  try {
    readManifest(is);
  } catch (const QLangError &e) {
    return e.message();
  }
  return {};
}

static bool eq(const ManifestType &a, const ManifestType &b) {
  std::ostringstream oa, ob;
  oa << a.name << a.hasStr << a.hasCtor;
  ob << b.name << b.hasStr << b.hasCtor;
  if (oa.str() != ob.str() || a.ctorArgTags != b.ctorArgTags ||
      a.members.size() != b.members.size() ||
      a.enums.size() != b.enums.size() ||
      a.signals.size() != b.signals.size())
    return false;
  for (size_t i = 0; i < a.members.size(); i++) {
    const auto &x = a.members[i];
    const auto &y = b.members[i];
    std::ostringstream xa, ya;
    xa << int(x.kind) << x.name << int(x.getTag) << int(x.setTag)
       << int(x.retTag);
    ya << int(y.kind) << y.name << int(y.getTag) << int(y.setTag)
       << int(y.retTag);
    if (xa.str() != ya.str() || x.argTags != y.argTags) return false;
  }
  for (size_t i = 0; i < a.enums.size(); i++) {
    if (a.enums[i].typeName != b.enums[i].typeName ||
        a.enums[i].entries.size() != b.enums[i].entries.size())
      return false;
    for (size_t j = 0; j < a.enums[i].entries.size(); j++)
      if (a.enums[i].entries[j].name != b.enums[i].entries[j].name ||
          a.enums[i].entries[j].value != b.enums[i].entries[j].value)
        return false;
  }
  for (size_t i = 0; i < a.signals.size(); i++)
    if (a.signals[i].name != b.signals[i].name ||
        a.signals[i].argTags != b.signals[i].argTags)
      return false;
  return true;
}

int main(int argc, char **argv) {
  // 1. Entry round-trip: every kind, enums (negative + >2^32 values),
  // signals, typed ctor args, Unknown/Void tags.
  ManifestType t = handType();
  std::string entry = serializeTypeEntry(t);
  size_t pos = 0;
  ManifestType back = parseTypeEntry(entry, pos);
  CHECK(pos == entry.size());
  CHECK(eq(t, back));

  // 2. Context-form ctor: hasCtor + empty tag list round-trips as empty
  // (the P1 convention: empty + hasCtor = context-taking form).
  ManifestType ctx = handType();
  ctx.name = "Ctx";  // distinct name: readManifest rejects duplicates
  ctx.ctorArgTags.clear();
  std::string ce = serializeTypeEntry(ctx);
  pos = 0;
  ManifestType cb = parseTypeEntry(ce, pos);
  CHECK(cb.hasCtor && cb.ctorArgTags.empty());
  CHECK(ce.size() < entry.size());  // the empty list really is shorter
  size_t full = 0;
  CHECK(eq(t, parseTypeEntry(entry, full)));  // sanity: entry parses whole
  CHECK(full == entry.size());

  // 3. Empty manifest + empty type (nothing in any section).
  std::ostringstream os;
  writeManifest(os, {});
  std::istringstream is(os.str());
  CHECK(readManifest(is).empty());
  ManifestType bare;
  bare.name = "E";
  pos = 0;
  CHECK(eq(bare, parseTypeEntry(serializeTypeEntry(bare), pos)));

  // 4. File round-trip with two entries.
  os.str("");
  writeManifest(os, {t, ctx});
  is.str(os.str());
  std::vector<ManifestType> got = readManifest(is);
  CHECK(got.size() == 2 && eq(got[0], t) && eq(got[1], ctx));

  // 5. Truncation matrix: every prefix of the file is rejected cleanly.
  const std::string file = os.str();
  bool allRejected = true;
  for (size_t n = 0; n < file.size(); n++) {
    std::string m = manifestErr(file.substr(0, n));
    if (m.empty() || m.rfind("qlm: ", 0) != 0) {
      allRejected = false;
      break;
    }
  }
  CHECK(allRejected);

  // 6. Version 0 and 2 rejected, bad magic rejected.
  std::string v = file;
  v[4] = 0;
  CHECK(manifestErr(v).find("unsupported version") != std::string::npos);
  v = file;
  v[4] = 2;
  CHECK(manifestErr(v).find("unsupported version") != std::string::npos);
  v = file;
  v[0] = 'X';
  CHECK(manifestErr(v).find("bad magic") != std::string::npos);

  // 7. Fingerprint: stable across serialize -> parse -> reserialize, and
  // sensitive to a single perturbed byte (a name char, then the flags).
  pos = 0;
  std::string again = serializeTypeEntry(parseTypeEntry(entry, pos));
  CHECK(again == entry);
  v = file;
  v[25] ^= 0x01;  // inside the first entry's name bytes
  CHECK(manifestErr(v).find("fingerprint mismatch") != std::string::npos);
  v = file;
  v[20] ^= 0x01;  // the name length's low byte -> truncation or mismatch
  CHECK(!manifestErr(v).empty());

  // 8. Untrusted body bytes: an out-of-range kind or tag is named (thrown
  // during the body parse, before the fingerprint check). Entry layout:
  // fp(8) namelen(4) name flags membercount(4) kind namelen(4) name [tags]
  std::string e = serializeTypeEntry(t);
  size_t tagAt = 8 + 4 + 6 + 1 + 4 + 1 + 4 + 1;  // first getTag
  CHECK(e.size() > tagAt);
  v = file;
  v.insert(12, e);                     // prepend a duplicate "Widget"
  v[12 + tagAt] = 0x7F;                // invalid tag inside the duplicate
  CHECK(manifestErr(v).find("invalid type tag") != std::string::npos);
  v = file;
  v.insert(12, e);
  v[12 + 8 + 4 + 6 + 1 + 4] = 9;           // member kind -> 9
  CHECK(manifestErr(v).find("invalid member kind") != std::string::npos);

  // 9. Duplicate type name and trailing bytes.
  v = file;
  v.insert(12, serializeTypeEntry(handType()).substr(0));  // exact duplicate
  CHECK(manifestErr(v).find("duplicate type name") != std::string::npos);
  v = file + "junk";
  CHECK(manifestErr(v).find("trailing bytes") != std::string::npos);

  // 10. manifestTypeFrom: the P1 host meta flows into the manifest form —
  // from the TypeMeta directly and from the imported TypeDescriptor.
  ManifestType g = manifestTypeFrom(Gadget::qlang_meta());
  CHECK(g.name == "Gadget" && g.hasStr && g.hasCtor);
  CHECK(g.ctorArgTags == std::vector<QTypeTag>{QTypeTag::Float});
  CHECK(g.enums.size() == 1 && g.enums[0].typeName == "GMode" &&
        g.enums[0].entries.size() == 2);
  CHECK(g.signals.size() == 1 && g.signals[0].name == "changed" &&
        g.signals[0].argTags == std::vector<QTypeTag>{QTypeTag::Int});
  QLangContext c;
  ManifestType gd = manifestTypeFrom(*c.import<Gadget>());
  CHECK(gd.name == "Gadget" && gd.members.size() == g.members.size());
  // Host -> manifest -> bytes -> manifest is the P4/P5 equality-by-
  // construction path: the two manifests serialize identically.
  CHECK(serializeTypeEntry(g) == serializeTypeEntry(gd));

  // 11. Member tags through the host path: rw prop Float/Float, ro prop
  // Str/Unknown, method ret Int + no args.
  bool sawW = false, sawLabel = false, sawWeigh = false;
  for (const auto &m : g.members) {
    if (m.name == "w")
      sawW = m.kind == 0 && m.getTag == QTypeTag::Float &&
             m.setTag == QTypeTag::Float;
    if (m.name == "label")
      sawLabel = m.kind == 1 && m.getTag == QTypeTag::Str &&
                 m.setTag == QTypeTag::Unknown;
    if (m.name == "weigh")
      sawWeigh = m.kind == 2 && m.retTag == QTypeTag::Int && m.argTags.empty();
  }
  CHECK(sawW);
  CHECK(sawLabel);
  CHECK(sawWeigh);

  // 12. Self-registration (P3): the QL_OBJECT registrar runs at dynamic
  // init — walking the process registry finds Gadget, and a dump of the
  // registry entries round-trips to bytes identical to the hand conversion.
  const TypeMeta *reg = nullptr;
  for (auto acc : metaRegistry())
    if (acc().name == "Gadget")
      reg = &acc();
  CHECK(reg != nullptr);
  std::ostringstream dos;
  writeManifest(dos, {manifestTypeFrom(*reg)});
  std::istringstream dis(dos.str());
  std::vector<ManifestType> db = readManifest(dis);
  CHECK(db.size() == 1 && serializeTypeEntry(db[0]) ==
                              serializeTypeEntry(manifestTypeFrom(
                                  Gadget::qlang_meta())));

  // 13. writeTypeManifest: the whole registry (here just Gadget) dumps to a
  // parseable manifest; then a fake duplicate accessor registers a second
  // "Gadget" — the registry keeps both entries and the dump refuses, naming
  // the duplicate (nothing is ever silently dropped).
  std::ostringstream wos;
  writeTypeManifest(wos);
  std::istringstream wis(wos.str());
  CHECK(readManifest(wis).size() == metaRegistry().size());
  static const TypeMeta dupMeta{"Gadget"};
  const size_t before = metaRegistry().size();
  meta_detail::registry_add([]() -> const TypeMeta & { return dupMeta; });
  CHECK(metaRegistry().size() == before + 1);  // kept, not deduplicated
  bool dupThrew = false;
  try {
    std::ostringstream x;
    writeTypeManifest(x);
  } catch (const QLangError &e) {
    dupThrew =
        e.message().find("duplicate type name 'Gadget'") != std::string::npos;
  }
  CHECK(dupThrew);

  // 14. CLI smoke (the qlmeta_dump ctest entry passes the qlang_run binary):
  // --qlmeta-dump exits 0 with a parseable file, and refuses to combine
  // with script modes. (qlang_run itself registers no QL_OBJECT types, so
  // its dump is a valid empty manifest.)
  if (argc > 1) {
    const std::string tmp = "/tmp/opencode/manifest_check_dump.qlm";
    CHECK(WEXITSTATUS(std::system((std::string(argv[1]) + " --qlmeta-dump " +
                                   tmp + " 2>/dev/null")
                                      .c_str())) == 0);
    std::ifstream f(tmp, std::ios::binary);
    CHECK(f.is_open());
    bool parsed = true;
    try {
      readManifest(f);
    } catch (...) {
      parsed = false;
    }
    CHECK(parsed);
    CHECK(WEXITSTATUS(std::system((std::string(argv[1]) + " --qlmeta-dump " +
                                   tmp + " /nonexistent.ql 2>/dev/null")
                                      .c_str())) == 1);
  }

  if (failures) {
    printf("manifest_check: %d FAILURES\n", failures);
    return 1;
  }
  printf("manifest_check: all %d checks passed\n", checks);
  return 0;
}
