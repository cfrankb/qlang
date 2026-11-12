// portable_module_test.cpp — QPM1 portable modules (portable-module-plan P5).
//
// Container round-trip, the byte layout hand-walked field by field, every rejection
// path with its byte offset, the load-time handshake (missing / mismatched host type,
// refused before anything loads) and an end-to-end drive of `qlang_run --package`
// plus `qlang_run <file>.qpm` through the library loader.
//
// Payloads are real `.qlc` images: `readQpm` validates each one through `readQlc`, so
// a fabricated payload would only be testing that validator. What matters here is that
// the image bytes survive byte-for-byte, which is checked as such.

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "qlang/QLangBytecodeIO.h"
#include "qlang/QLangCompiler.h"
#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"
#include "qlang/QLangManifest.h"
#include "qlang/QLangMeta.h"

using namespace qlang;

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool cond, const std::string &what, int line) {
  if (cond) {
    ++g_pass;
  } else {
    ++g_fail;
    std::cerr << "FAIL line " << line << ": " << what << "\n";
  }
}
#define CHECK(c) check((c), #c, __LINE__)
#define CHECK_MSG(c, m) check((c), (m), __LINE__)

/// `readQpm(bytes)` must throw exactly `want`.
void expectFault(const std::string &bytes, const std::string &want, int line) {
  std::string got = "<accepted>";
  try {
    std::istringstream in(bytes, std::ios::binary);
    qlang::readQpm(in);
  } catch (const qlang::QLangError &e) {
    got = e.message();
  }
  check(got == want, "fault text\n  expected: " + want + "\n  actual:   " + got, line);
}
#define CHECK_FAULT(b, w) expectFault((b), (w), __LINE__)

/// `readQpm(bytes)` must throw a fault starting with `prefix`. For the reasons that
/// belong to an embedded reader (the manifest's, the image's own), whose wording is
/// that reader's contract rather than this container's.
void expectFaultStartingWith(const std::string &bytes, const std::string &prefix, int line) {
  std::string got = "<accepted>";
  try {
    std::istringstream in(bytes, std::ios::binary);
    qlang::readQpm(in);
  } catch (const qlang::QLangError &e) {
    got = e.message();
  }
  check(got.compare(0, prefix.size(), prefix) == 0,
        "fault starts with '" + prefix + "'\n  actual:   " + got, line);
  check(got.find("(byte offset ") != std::string::npos, "the fault names a byte offset", line);
}
#define CHECK_FAULT_PREFIX(b, p) expectFaultStartingWith((b), (p), __LINE__)

/// `readQpm(bytes)` must accept.
void expectAccept(const std::string &bytes, int line) {
  try {
    std::istringstream in(bytes, std::ios::binary);
    qlang::readQpm(in);
    ++g_pass;
  } catch (const qlang::QLangError &e) {
    check(false, "expected accept, got " + e.message(), line);
  }
}
#define CHECK_ACCEPT(b) expectAccept((b), __LINE__)

uint32_t getU32(const std::string &b, size_t at) {
  uint32_t v = 0;
  for (size_t i = 0; i < 4; ++i)
    v |= static_cast<uint32_t>(static_cast<unsigned char>(b[at + i])) << (8 * i);
  return v;
}

void setU32(std::string &b, size_t at, uint32_t v) {
  for (size_t i = 0; i < 4; ++i) b[at + i] = static_cast<char>((v >> (8 * i)) & 0xFF);
}

// Where each field of a package sits, walked the way the reader walks it, so a
// corruption test patches the field it means instead of guessing an offset.
struct Fields {
  size_t version = 0;
  size_t entryLen = 0;
  size_t entry = 0;
  size_t manifestLen = 0;
  size_t manifest = 0;
  size_t imageCount = 0;
  std::vector<size_t> imageNameLen;
  std::vector<size_t> imageLen;
  size_t end = 0;  // one past the last image
};

Fields walk(const std::string &b) {
  Fields f;
  f.version = 4;
  size_t at = 8;
  f.entryLen = at;
  f.entry = at + 4;
  at += 4 + getU32(b, at);
  f.manifestLen = at;
  f.manifest = at + 4;
  at += 4 + getU32(b, at);
  f.imageCount = at;
  at += 4;
  while (at < b.size()) {
    f.imageNameLen.push_back(at);
    at += 4 + getU32(b, at);
    f.imageLen.push_back(at);
    at += 4 + getU32(b, at);
  }
  f.end = at;
  return f;
}

// ---------------------------------------------------------------------------
// Scratch area + the driver (the file tests never write into the source tree)
// ---------------------------------------------------------------------------

// Unique per run, so stale content can never masquerade as a pass.
const std::string kTmp = "/tmp/opencode/qpm-" + std::to_string(::getpid()) + "/";
std::string g_cli;  // build/qlang_run, from argv[1] ("" = not given: skip the CLI set)

void writeFile(const std::string &path, const std::string &data) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f << data;
  // A scratch write that failed would silently skip every check that reads it back.
  check(f.good(), "the scratch file '" + path + "' is writable", __LINE__);
}

bool runCli(const std::vector<std::string> &args, std::string &out, int &code) {
  std::string cmd;
  for (const auto &a : args) cmd += (cmd.empty() ? "" : " ") + a;
  FILE *p = popen((cmd + " 2>&1").c_str(), "r");
  if (!p) return false;
  char buf[4096];
  out.clear();
  while (fgets(buf, sizeof(buf), p)) out += buf;
  const int st = pclose(p);
  code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
  return WIFEXITED(st);
}

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

struct Point : qlang::QLObject<Point> {
  Point(double x_, double y_) : x(x_), y(y_) {}
  double x = 0, y = 0;
  std::string str() const { return "Point"; }

  QL_OBJECT(QL_PROPERTY(x), QL_PROPERTY(y));
};

qlang::QLangContext g_host;  // the natives + modules a compile sees

/// Compiles `source` as module `mod` the way `qlang_run` does and returns its
/// `.qlc` image bytes.
std::string qlcImage(const std::string &source, const std::string &mod) {
  qlang::QLangCompiler compiler(g_host.parse(source), &g_host);
  std::unordered_set<std::string> nativeNames;
  for (const auto &n : g_host.nativeFuncs()) nativeNames.insert(n.first);
  compiler.addExternalFuncs(nativeNames);
  compiler.addModules(g_host.modules());
  compiler.setModuleName(mod);
  compiler.setLibraryMode(true);
  const qlang::CompiledProgram prog = compiler.compile();
  const auto &ids = compiler.nativeFuncIds();
  std::ostringstream out(std::ios::binary);
  if (!writeQlc(out, prog, std::map<std::string, int>(ids.begin(), ids.end())))
    throw std::runtime_error("the fixture image is not serializable");
  return out.str();
}

std::string g_calc;  // a self-contained module image, "calc"
std::string g_geo;   // another one, "geo" (an image is filed under the name it declares)

std::string pack(const std::string &entry, const std::vector<ManifestType> &types = {},
                 std::vector<QpmImage> images = {}) {
  std::ostringstream out(std::ios::binary);
  writeQpm(out, entry, types, images);
  return out.str();
}

/// The contract a host with `Point` registered would have written. Built once: a
/// context keeps its type registry, so `import` twice on one context is a refusal.
const std::vector<ManifestType> &pointContract() {
  static const std::vector<ManifestType> types = {manifestTypeFrom(*g_host.import<Point>())};
  return types;
}

/// The message `writeQpm` refuses this package with ("<accepted>" when it wrote it).
std::string writeRefusal(const std::string &entry, std::vector<QpmImage> images) {
  std::ostringstream out(std::ios::binary);
  try {
    writeQpm(out, entry, {}, images);
  } catch (const qlang::QLangError &e) {
    return e.message();
  }
  return "<accepted>";
}

// ===========================================================================
// Round-trip
// ===========================================================================

void testRoundTrip() {
  std::cout << "-- container round-trip\n";

  {  // one image, no types
    const std::string bytes = pack("calc", {}, {{"calc", g_calc}});
    std::istringstream in(bytes, std::ios::binary);
    const QpmPackage pkg = readQpm(in);
    CHECK(pkg.entryName == "calc");
    CHECK(pkg.manifest.empty());
    CHECK_MSG(pkg.images.size() == 1, "one image");
    CHECK(pkg.images[0].name == "calc");
    CHECK_MSG(pkg.images[0].bytes == g_calc, "the payload is byte-for-byte what writeQlc wrote");
  }

  {  // several images, written order preserved, manifest entry intact
    const std::vector<ManifestType> types = pointContract();
    const std::string bytes = pack("calc", types, {{"geo", g_geo}, {"calc", g_calc}});
    std::istringstream in(bytes, std::ios::binary);
    const QpmPackage pkg = readQpm(in);
    CHECK(pkg.entryName == "calc");
    CHECK_MSG(pkg.images.size() == 2, "both images come back");
    CHECK_MSG(pkg.images[0].name == "geo" && pkg.images[1].name == "calc",
              "the image order is the written order");
    CHECK_MSG(pkg.manifest.size() == 1, "the manifest type comes back");
    CHECK_MSG(serializeTypeEntry(pkg.manifest[0]) == serializeTypeEntry(types[0]),
              "the manifest entry round-trips to the same canonical entry");
  }

  {  // an empty payload is legal bytes-wise but never a loadable image, so the
    // smallest legal package is one real image
    CHECK_ACCEPT(pack("calc", {}, {{"calc", g_calc}}));
  }
}

// ===========================================================================
// The bytes on disk
// ===========================================================================

void testLayout() {
  std::cout << "-- container layout (hand-walked)\n";

  const std::vector<ManifestType> types = pointContract();
  std::ostringstream manifest(std::ios::binary);
  writeManifest(manifest, types);

  const std::string bytes = pack("calc", types, {{"lib", g_calc}, {"calc", g_calc}});
  const std::string &b = bytes;
  const Fields f = walk(b);

  CHECK(b.compare(0, 4, "QPM1") == 0);
  CHECK(getU32(b, f.version) == kQpmVersion);
  CHECK(getU32(b, f.entryLen) == 4);
  CHECK(b.compare(f.entry, 4, "calc") == 0);
  CHECK_MSG(getU32(b, f.manifestLen) == manifest.str().size(),
            "the manifest block is the full QLM1 image");
  CHECK_MSG(b.compare(f.manifest, getU32(b, f.manifestLen), manifest.str()) == 0,
            "the manifest bytes are byte-for-byte what writeManifest produced");
  CHECK(getU32(b, f.imageCount) == 2);
  CHECK_MSG(f.imageNameLen.size() == 2 && f.imageLen.size() == 2, "both images are walkable");
  CHECK(b.compare(f.imageNameLen[0] + 4, 3, "lib") == 0);
  CHECK(getU32(b, f.imageLen[0]) == g_calc.size());
  CHECK(b.compare(f.imageLen[0] + 4, g_calc.size(), g_calc) == 0);
  CHECK(b.compare(f.imageNameLen[1] + 4, 4, "calc") == 0);
  CHECK(b.compare(f.imageLen[1] + 4, 4, "QLC1") == 0);
  CHECK_MSG(f.end == b.size(), "no trailing bytes");

  // A type-free package still carries a full (empty) QLM1, so a reader never has to
  // special-case a missing manifest.
  const std::string bare = pack("calc", {}, {{"calc", g_calc}});
  const Fields bf = walk(bare);
  CHECK(getU32(bare, bf.manifestLen) == 12);
  CHECK(bare.compare(bf.manifest, 4, "QLM1") == 0);
  CHECK(getU32(bare, bf.manifest + 8) == 0);  // zero entries
}

// ===========================================================================
// Rejections: the input is untrusted
// ===========================================================================

void testRejections() {
  std::cout << "-- container rejections\n";

  const std::string base = pack("calc", {}, {{"calc", g_calc}});
  const Fields f = walk(base);

  {  // the magic decides the format
    auto b = base;
    b.replace(0, 4, "QLC1");
    CHECK_FAULT(b, "qpm: not a QPM1 portable module (byte offset 0)");
  }
  {  // the version is accepted strictly
    auto b = base;
    setU32(b, f.version, kQpmVersion + 1);
    CHECK_FAULT(b, "qpm: unsupported version (byte offset 8)");
  }
  {
    CHECK_FAULT(base.substr(0, 3), "qpm: not a QPM1 portable module (byte offset 0)");
  }
  {  // truncation lands inside the last image's bytes
    CHECK_FAULT(base.substr(0, base.size() - 1),
                "qpm: truncated read (byte offset " + std::to_string(f.imageLen[0] + 4) + ")");
  }
  {
    CHECK_FAULT(base + "x",
                "qpm: trailing bytes (byte offset " + std::to_string(base.size()) + ")");
  }
  {  // the entry has to be one of the images (patched in place, so framing holds)
    auto b = base;
    b[f.entry] = 'z';
    CHECK_FAULT(b, "qpm: entry 'zalc' is not one of the embedded images (byte offset " +
                       std::to_string(f.end) + ")");
  }
  {  // a count beyond the §8 cap is refused before it can size anything
    auto b = base;
    setU32(b, f.imageCount, 0xFFFFFFFFu);
    CHECK_FAULT(b, "qpm: count too large (byte offset " + std::to_string(f.imageCount + 4) + ")");
  }
  {  // a length field bigger than the whole buffer
    auto b = base;
    setU32(b, f.manifestLen, 0xFFFFFFFFu);
    CHECK_FAULT(b, "qpm: truncated read (byte offset " + std::to_string(f.manifest) + ")");
  }
  {
    auto b = base;
    setU32(b, f.imageNameLen[0], 0xFFFFFFFFu);
    CHECK_FAULT(b,
                "qpm: truncated read (byte offset " + std::to_string(f.imageNameLen[0] + 4) + ")");
  }
  {  // a payload size that lies: the bytes the header promised are not there
    auto b = base;
    setU32(b, f.imageLen[0], 1u << 20);
    CHECK_FAULT(b, "qpm: truncated read (byte offset " + std::to_string(f.imageLen[0] + 4) + ")");
  }

  // The writer never emits a package the reader would refuse, so the container's own
  // invariants are pinned on the write side.
  CHECK_MSG(writeRefusal("a", {{"a", g_calc}, {"a", g_calc}}) == "qpm: duplicate image 'a'",
            "a duplicate image name is refused");
  CHECK_MSG(writeRefusal("a", {{"a", g_calc}, {"", g_calc}}) == "qpm: an image has no name",
            "an image with no name is refused");
  CHECK_MSG(writeRefusal("ghost", {{"a", g_calc}}) ==
                "qpm: entry 'ghost' is not one of the embedded images",
            "an entry that is not one of the images is refused");
  CHECK_MSG(writeRefusal("", {}) == "qpm: no images", "an empty package is refused");

  // A malformed manifest keeps its own reason and gains the container's context: the
  // block is patched where its entry count lives (QLM1 = magic + version + count),
  // which only the manifest reader can be wrong about.
  {
    auto b = pack("calc", pointContract(), {{"calc", g_calc}});
    setU32(b, walk(b).manifest + 8, 99);  // claim 99 entries follow
    CHECK_FAULT_PREFIX(b, "qpm: manifest: qlm: ");
  }
  {  // a payload that is not a .qlc image: the fault keeps the embedded format's own
    // reason (whose offset is inside that image) and adds where the package put it
    auto b = base;
    b[f.imageLen[0] + 4] = 'X';
    CHECK_FAULT(b, "qpm: image 'calc': qlc: bad magic (byte offset 0) (byte offset " +
                       std::to_string(f.end) + ")");
  }
  {  // the name a package files an image under has to be the name the image declares:
    // the loader keys its registry by the latter
    const std::string b = pack("other", {}, {{"other", g_calc}});
    CHECK_FAULT(b, "qpm: image 'other' declares module 'calc' (byte offset " +
                       std::to_string(walk(b).end) + ")");
  }
}

// ===========================================================================
// The load-time handshake
// ===========================================================================

void testHandshake() {
  std::cout << "-- handshake\n";

  const std::string bytes = pack("calc", pointContract(), {{"calc", g_calc}});
  writeFile(kTmp + "contract.qpm", bytes);
  writeFile(kTmp + "bare.qpm", pack("calc", {}, {{"calc", g_calc}}));

  {  // a host that registers nothing cannot honour the contract
    qlang::QLangContext ctx;
    try {
      ctx.loadPortableModuleFile(kTmp + "contract.qpm");
      CHECK_MSG(false, "a package whose types the host lacks is refused");
    } catch (const qlang::QLangError &e) {
      CHECK_MSG(e.kind() == qlang::QLangErrorKind::Type, "the fault is a type error");
      CHECK_MSG(e.line() == 0, "a handshake fault carries no source position");
      CHECK_MSG(e.message() == "missing type 'Point' required by portable module",
                "the fault names the missing type");
    }
    CHECK_MSG(ctx.loadedModules().empty(),
              "nothing loaded: the handshake runs before any image touches the context");
  }

  {  // a host that registers the same name with a different shape
    qlang::QLangContext ctx;
    qlang::TypeMeta other = Point::qlang_meta();
    other.hasStr = !other.hasStr;  // same name, different describable shape
    ctx.registerType(other);
    try {
      ctx.loadPortableModuleFile(kTmp + "contract.qpm");
      CHECK_MSG(false, "a shape mismatch is refused");
    } catch (const qlang::QLangError &e) {
      CHECK_MSG(e.message() == "external type 'Point' does not match the portable module",
                "the fault names the mismatched type");
    }
    CHECK(ctx.loadedModules().empty());
  }

  {  // a host that matches
    qlang::QLangContext ctx;
    ctx.import<Point>();
    const std::shared_ptr<LoadedModule> entry = ctx.loadPortableModuleFile(kTmp + "contract.qpm");
    CHECK_MSG(entry != nullptr, "a matching host accepts the package");
    CHECK_MSG(entry && entry->program.moduleName == "calc", "the entry module is returned");
    CHECK_MSG(ctx.loadedModule("calc") != nullptr, "the entry is in the registry");
    CHECK_MSG(ctx.loadedModule("calc")->path == kTmp + "contract.qpm:calc",
              "a loaded image remembers the package it came from");
  }

  {  // a package with no manifest is no contract: it loads anywhere
    qlang::QLangContext ctx;
    CHECK(ctx.loadPortableModuleFile(kTmp + "bare.qpm") != nullptr);
    CHECK(ctx.loadedModule("calc") != nullptr);
  }

  {  // every context that satisfies the contract loads the same package
    qlang::QLangContext a;
    qlang::QLangContext b;
    a.import<Point>();
    b.import<Point>();
    CHECK(a.loadPortableModuleFile(kTmp + "contract.qpm") != nullptr);
    CHECK(b.loadPortableModuleFile(kTmp + "contract.qpm") != nullptr);
  }
}

// ===========================================================================
// The driver end to end
// ===========================================================================

void testDriver() {
  std::cout << "-- qlang_run --package and .qpm input\n";
  if (g_cli.empty()) {
    std::cout << "   (no qlang_run given on the command line; skipping)\n";
    return;
  }
  int code = -1;
  std::string out;

  writeFile(kTmp + "geom.ql", "export def area(name)\n  return 3.14 * 2 * 2\nend\n");
  writeFile(kTmp + "app.ql", "from geom import area\nlocal a = area(\"x\")\nprintln(a)\n");

  // Writing: one image per module, the entry last, and the run still happens.
  CHECK_MSG(runCli({g_cli, kTmp + "app.ql", "--package=" + kTmp + "app.qpm"}, out, code) &&
                code == 0,
            "app.ql --package=app.qpm succeeds");
  CHECK_MSG(out.find("portable module written: " + kTmp + "app.qpm (2 image(s), 0 type(s))") !=
                std::string::npos,
            "the notice names the file and what went in");
  CHECK_MSG(out.find("12.56") != std::string::npos, "--package still runs the program (like -c)");

  // Reading: the embedded library really links and runs.
  CHECK_MSG(runCli({g_cli, kTmp + "app.qpm"}, out, code) && code == 0, "app.qpm runs");
  CHECK_MSG(out.find("12.56") != std::string::npos, "the imported function really ran");

  // The same file through the library loader: libraries first, entry last.
  {
    qlang::QLangContext ctx;
    const std::shared_ptr<LoadedModule> entry = ctx.loadPortableModuleFile(kTmp + "app.qpm");
    const std::vector<std::shared_ptr<LoadedModule>> mods = ctx.loadedModules();
    CHECK_MSG(mods.size() == 2, "both embedded modules are registered");
    if (mods.size() == 2) {
      CHECK_MSG(mods[0]->program.moduleName == "geom", "the library loads first");
      CHECK_MSG(mods[1]->program.moduleName == "app", "the entry loads last");
    }
    CHECK_MSG(entry && entry->program.moduleName == "app", "the entry module is the one returned");
  }

  // A package can be packaged again: its entry is in the registry.
  CHECK_MSG(runCli({g_cli, kTmp + "app.qpm", "--package=" + kTmp + "again.qpm"}, out, code) &&
                code == 0,
            "a .qpm feeds --package again");

  // The magic decides the format, not the file name.
  writeFile(kTmp + "plain.qpm", "local v = 41 + 1\nprintln(v)\n");
  CHECK_MSG(runCli({g_cli, kTmp + "plain.qpm"}, out, code) && code == 0,
            "a .qpm without the magic runs as source");
  CHECK_MSG(out.find("42") != std::string::npos, "the source really ran");

  // A package that does not read is an artifact failure, not a source fallback.
  writeFile(kTmp + "corrupt.qpm", std::string("QPM1\x02\x00\x00\x00", 8));
  CHECK_MSG(runCli({g_cli, kTmp + "corrupt.qpm"}, out, code) && code == 3,
            "a package with a bad version exits 3");
  CHECK_MSG(out.find("qpm: unsupported version (byte offset 8)") != std::string::npos,
            "the driver reports the container's own fault");

  // Mode combinations.
  CHECK_MSG(runCli({g_cli, kTmp + "geom.ql", "--qlmeta-dump", "--package=x.qpm"}, out, code) &&
                code == 1,
            "--package refuses --qlmeta-dump");
  CHECK_MSG(out.find("--qlmeta-dump does not combine with script/compile modes") !=
                std::string::npos,
            "the refusal says why");
  CHECK(runCli({g_cli, "--help"}, out, code) && code == 0);
  CHECK(out.find("--package <file.qpm>") != std::string::npos);
}

}  // namespace

int main(int argc, char **argv) {
  if (::mkdir(kTmp.c_str(), 0700) != 0) {
    std::cerr << "FAIL: cannot create the scratch area " << kTmp << "\n";
    return 1;
  }
  if (argc > 1) g_cli = argv[1];

  try {
    g_calc = qlcImage("export def twice(x)\n  return x + x\nend\n", "calc");
    g_geo = qlcImage("export def triple(x)\n  return x * 3\nend\n", "geo");
    testRoundTrip();
    testLayout();
    testRejections();
    testHandshake();
  } catch (const std::exception &e) {
    check(false, std::string("unexpected fault: ") + e.what(), __LINE__);
  }
  testDriver();

  std::cout << "\n" << g_pass << " passed, " << g_fail << " failed\n";
  return g_fail == 0 ? 0 : 1;
}
