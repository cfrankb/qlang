// A portable module package end to end (plan
// docs/plans/archive/portable-module-follow-on-plan.md Phase 2; docs/SERIALIZATION.md
// §12, docs/EXTERNAL_TYPES.md §14).
//
// A `.qpm` is the delivery form of a portable module: the host's type contract
// (QLM1) plus every `.qlc` image in one file, so a build machine that never
// linked the engine can produce it and any host that satisfies the contract can
// run it. This example plays all three roles:
//
//   1. the contract — `writeTypeManifest()` dumps `Point`, the only QL_OBJECT
//      type in this binary, to host.qlm. That is the artifact
//      `qlang_run --qlmeta-dump` writes, and the only type knowledge a build
//      machine needs.
//   2. the build — two sources are compiled against the manifest with no engine
//      types registered at all: a library that reads the type's members, an entry
//      that constructs it. `Point(...)` cannot bind to anything there, so the
//      entry leaves an image that asks for the constructor by NAME — and the
//      library, which never names it, asks for nothing: the slot table carries
//      the calls a program makes, not what its build was told about (plan
//      native-slot-portability P1). Both images and the manifest go into app.qpm
//      through `writeQpm()`.
//   3. the hosts — one that imports `Point` loads the package through the
//      handshake, links and runs it, and its printed number comes off a live
//      C++ object. Two that do not satisfy the contract are refused before any
//      image is allowed in: one registers nothing, the other registers a
//      `Point` whose `str` has gone away.
//
// What a package cannot see is C++ identity: a host whose same-named type is a
// different build passes all of this byte for byte, and the pointer check the
// typed handoff records (`ExternalRef::check`, resolved in `resolveExternal`) is
// what refuses its values. §14 "Values are the host's to validate"; the
// portable_module suite covers that gate.
//
// Build: cmake --build build --target qlang_portable_module
// Run:   ./build/qlang_portable_module   (exit code 0 means every step held)
#include <qlang/QLangCompiler.h>
#include <qlang/QLangContext.h>
#include <qlang/QLangErrors.h>
#include <qlang/QLangLinker.h>
#include <qlang/QLangManifest.h>
#include <qlang/QLangMeta.h>
#include <qlang/QLangVM.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
using namespace qlang;

// The host's own type. QL_OBJECT self-registers into the manifest registry, so
// the contract in step 1 is dumped from the real descriptor rather than typed by
// hand. `str` and the constructor are exactly the two facts the contract records
// beyond the property list.
struct Point : qlang::QLObject<Point> {
  Point(double x_, double y_) : x(x_), y(y_) {}
  double x = 0, y = 0;

  std::string str() const { return "Point(" + std::to_string(x) + ", " + std::to_string(y) + ")"; }
  static std::shared_ptr<Point> make(double x_, double y_) {
    return std::make_shared<Point>(x_, y_);
  }

  QL_OBJECT(QL_PROPERTY(x), QL_PROPERTY(y), QL_STR(str), QL_CTOR(&Point::make));
};

namespace {

const fs::path kRoot = fs::temp_directory_path() / "qlang_portable_module";
bool g_failed = false;

void report(bool ok, const std::string &what) {
  std::cout << "       " << (ok ? "[ok]   " : "[FAIL] ") << what << "\n";
  if (!ok) g_failed = true;
}

void writeFile(const fs::path &path, const std::string &data) {
  std::ofstream out(path, std::ios::binary);
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

std::string readFile(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The VM prints through std::cout, so swapping its buffer captures a run.
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

// A library: it hands over a function that reads the host object's members. The
// parameter is untyped, so this compile needs no type knowledge at all.
const char kShapeSource[] = "export def norm(p)\n"
                            "  return p.x * p.x + p.y * p.y\n"
                            "end\n";

// An entry: it constructs the type and prints what the library made of it. The
// constructor resolves by name at load, which is the whole point of shipping the
// contract with the images.
const char kEntrySource[] = "from shape import norm\n"
                            "local p = Point(3.5, 2.0)\n"
                            "println(\"norm(p) = \" + norm(p))\n"
                            "println(\"type(p) = \" + type(p))\n"
                            "println(\"str(p) = \" + str(p))\n";

// Compiles `source` as module `name` against `types` (the manifest) and
// serializes it with its native slot table — what `qlang_run --qlmeta <file>
// --compile` does per file, and what `--package` does for both images at once.
std::string imageBytes(QLangContext &ctx, const std::string &source, const std::string &name,
                       const std::vector<ManifestType> &types) {
  QLangCompiler compiler(ctx.parse(source), &ctx);
  std::unordered_set<std::string> nativeNames;
  for (const auto &entry : ctx.nativeFuncs()) nativeNames.insert(entry.first);
  compiler.addExternalFuncs(nativeNames);
  compiler.addExternalTypes(types);
  compiler.addModules(ctx.modules());
  compiler.setModuleName(name);
  compiler.setLibraryMode(true);
  const CompiledProgram prog = compiler.compile();
  std::ostringstream out(std::ios::binary);
  if (!writeQlc(out, prog, compiler.nativeSlotIds()))
    throw QLangError(QLangErrorKind::Io, 0, 0, name + ": the image is not serializable");
  return out.str();
}

// What a host does with a package: the handshake load, then the same link step a
// `.qlc` entry goes through.
std::string loadAndRun(QLangContext &ctx, const fs::path &package) {
  const std::shared_ptr<LoadedModule> entry = ctx.loadPortableModuleFile(package);
  if (!entry) throw QLangError(QLangErrorKind::Io, 0, 0, "the package has no entry image");
  std::map<std::string, int> nativeIds;
  for (const auto &slot : entry->nativeSlots) nativeIds[slot.second] = -1 - slot.first;
  const CompiledProgram merged = QLangLinker(ctx.loadedModules()).link(entry->program, nativeIds);
  std::string out;
  {
    StdoutCapture capture;
    QLangVM(merged, ctx).run();
    out = capture.text();
  }
  return out;
}

std::string refusal(QLangContext &ctx, const fs::path &package) {
  try {
    ctx.loadPortableModuleFile(package);
  } catch (const QLangError &error) {
    return error.message();
  }
  return {};
}

}  // namespace

int main() {
  try {
    fs::create_directories(kRoot);
    const fs::path manifestPath = kRoot / "host.qlm";
    const fs::path packagePath = kRoot / "app.qpm";

    std::cout << "[1] the contract\n";
    {
      std::ofstream out(manifestPath, std::ios::binary);
      writeTypeManifest(out);
    }
    std::vector<ManifestType> types;
    {
      std::istringstream in(readFile(manifestPath), std::ios::binary);
      types = readManifest(in);
    }
    report(types.size() == 1 && types[0].name == "Point", "one contract, for `Point`");
    std::cout << "    " << manifestPath.string() << " (" << fs::file_size(manifestPath)
              << " bytes): " << types[0].members.size() << " properties, str "
              << (types[0].hasStr ? "yes" : "no") << ", ctor "
              << (types[0].hasCtor ? "yes" : "no") << "\n";

    std::cout << "[2] the build (this context registers no engine type)\n";
    QLangContext build;
    const std::string shape = imageBytes(build, kShapeSource, "shape", types);
    std::istringstream shapeIn(shape, std::ios::binary);
    build.loadProgram(shapeIn, "shape:build");  // so `from shape import norm` resolves
    const std::string app = imageBytes(build, kEntrySource, "app", types);
    {
      std::ostringstream out(std::ios::binary);
      writeQpm(out, "app", types, {{"shape", shape}, {"app", app}});
      writeFile(packagePath, out.str());
    }
    report(!shape.empty() && !app.empty(), "both images compiled and packed");
    std::cout << "    " << packagePath.string() << " (" << fs::file_size(packagePath)
              << " bytes): images shape + app, entry app, 1 type\n";

    std::cout << "[3] a host that satisfies the contract\n";
    QLangContext host;
    host.import<Point>();
    const std::string printed = loadAndRun(host, packagePath);
    std::string line;
    std::cout << "    it printed:";
    for (std::istringstream lines(printed); std::getline(lines, line);)
      std::cout << "\n      " << line;
    std::cout << "\n";
    report(printed.find("16.25") != std::string::npos, "norm(p) = 16.25, read off the C++ object");
    report(printed.find("type(p) = Point") != std::string::npos, "type(p) is the external type");
    report(printed.find("str(p) = Point(3.5") != std::string::npos,
           "str(p) is the host's own str()");

    std::cout << "[4] a host that registers nothing\n";
    QLangContext bare;
    const std::string missing = refusal(bare, packagePath);
    std::cout << "    refused: " << missing << "\n";
    report(missing == "missing type 'Point' required by portable module",
           "the fault names the type the package needs");
    report(bare.loadedModules().empty(), "no image of the package was loaded");

    std::cout << "[5] a host whose `Point` has drifted (its `str` is gone)\n";
    QLangContext stale;
    TypeMeta drifted = Point::qlang_meta();
    drifted.hasStr = false;
    stale.registerType(drifted);
    const std::string mismatch = refusal(stale, packagePath);
    std::cout << "    refused: " << mismatch << "\n";
    report(mismatch == "external type 'Point' does not match the portable module",
           "the handshake compared the shapes and refused");
    report(stale.loadedModules().empty(), "no image of the package was loaded");

    if (g_failed) std::cerr << "portable_module: a step did not hold\n";
    return g_failed ? 1 : 0;
  } catch (const QLangError &error) {
    std::cerr << "portable_module failed: " << error.message() << "\n";
    return 1;
  } catch (const std::exception &error) {
    std::cerr << "portable_module failed: " << error.what() << "\n";
    return 1;
  }
}
