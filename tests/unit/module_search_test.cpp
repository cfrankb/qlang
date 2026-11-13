// Module search paths and the external search callback (SPEC §api
// "module search paths" + §api "external search callback"; plan
// docs/plans/missing-features-plan.md §C8a + §C8b).
//
// `QLangContext::findModule` is the one resolution order the spec gives: a
// registered C++ module, an entry already in the registry, the host's callback,
// then the search paths in order — `.qlc` before `.ql` within a directory, a
// directory that is not there skipped, the first match winning, and the result
// cached under the name that was asked for. The compiler reaches it through
// `import`, so a library nobody loaded is found instead of being a compile
// error, and a discovered image's own imports arrive with it.
//
// This suite pins —
//
//   * the path list: the default is the working directory at construction,
//     `set` replaces, `add` appends, an empty list disables discovery;
//   * the order: first match wins, a missing directory (or a plain file used as
//     a path) is skipped, `.qlc` beats `.ql` in the same directory, and a `.ql`
//     with no image beside it is compiled and taken in like the image it stands
//     for (plan C8b);
//   * the cache: one resolution per context, reached again by the requested
//     name or by the module's own, and `clear()` drops the registry and the
//     cache but keeps the paths and the callback;
//   * the callback's three answers (module / nullptr+"" / nullptr+msg), that it
//     shadows a file but never a C++ module, that it is not consulted for a
//     native module, and that a callback which throws is reported instead of
//     escaping;
//   * transitive resolution: a discovered image brings its own dependencies, a
//     missing dependency names the module that asked, and a cycle terminates;
//   * the identity rule: `foo.qlc` declaring `MODULE_NAME = "bar"` registers as
//     `bar` and is still what `import foo` binds (Q36);
//   * compiling a discovered `.ql` (plan C8b): the scratch compile leaves the
//     caller's globals alone, a `.ql` that imports another `.ql` resolves it
//     through the parent's one path list, a cycle between two sources names the
//     chain instead of recursing, and a source that fails to compile names the
//     file it came from;
//   * the end to end: an entry compiled with nothing pre-loaded resolves, links
//     and runs, and a C++ module of the same name still beats the file.
//
// The images are built the way the driver builds them (compile in library mode,
// `writeQlc`), written into a temporary tree, and removed again.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "qlang/QLangBytecode.h"
#include "qlang/QLangBytecodeIO.h"
#include "qlang/QLangCompiler.h"
#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"
#include "qlang/QLangLinker.h"
#include "qlang/QLangVM.h"

using namespace qlang;

static int passes = 0, fails = 0;

static bool check(bool cond, const std::string &what) {
  if (cond) {
    passes++;
  } else {
    fails++;
    std::cout << "  FAIL: " << what << "\n";
  }
  return cond;
}

namespace {

// A temporary directory tree, removed when the test that made it finishes. The
// name is the test's own, so the trees of two tests never collide.
class TempTree {
public:
  explicit TempTree(const std::string &name) {
    root = std::filesystem::temp_directory_path() / ("qlang_c8a_" + name);
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
  }
  ~TempTree() {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }
  TempTree(const TempTree &) = delete;
  TempTree &operator=(const TempTree &) = delete;

  // A (created) subdirectory of the tree, as a search-path string.
  std::string dir(const std::string &sub) const {
    const auto p = root / sub;
    std::filesystem::create_directories(p);
    return p.string();
  }
  // Writes `contents` under the tree (creating parents) and returns the path.
  std::string write(const std::string &sub, const std::string &contents) const {
    const auto p = root / sub;
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << contents;
    out.close();
    return p.string();
  }

  std::filesystem::path root;
};

// Compile `source` exactly as `qlcc` does (external consts/funcs/modules
// from the context, natives wired into the program), so an image built here
// behaves like one the driver wrote.
//
// `seedExternals` mirrors the driver's habit of telling the compiler about
// every native and C++ module function the context has — which the .qlc writer
// then records as the image's native-slot table, so such an image can only be
// loaded by a context with the same builtins (addModules alone already pins
// the default `math` module's min/max/sum/rand). Library images here are built
// with it OFF: they call no native, and an image with an empty slot table
// loads into any context, including one after `clear()` (which drops every
// native by design).
bool compile(QLangContext &ctx, const std::string &source, CompiledProgram &out,
             std::map<std::string, int> &nativeIds, std::string &err,
             const std::string &moduleName = "", bool libraryMode = false,
             bool seedExternals = true) {
  try {
    std::vector<StmtPtr> program = ctx.parse(source);
    QLangCompiler compiler(std::move(program), &ctx);
    compiler.addExternalConsts({});
    std::unordered_set<std::string> nativeNames;
    if (seedExternals)
      for (const auto &[name, _] : ctx.nativeFuncs()) nativeNames.insert(name);
    compiler.addExternalFuncs(nativeNames);
    compiler.addModules(seedExternals ? ctx.modules()
                                      : std::vector<QLangContext::Module>{});
    if (!moduleName.empty()) compiler.setModuleName(moduleName);
    compiler.setLibraryMode(libraryMode);
    out = compiler.compile();
    nativeIds = compiler.nativeSlotIds();
    for (const auto &[name, func] : ctx.nativeFuncs()) {
      auto it = nativeIds.find(name);
      if (it == nativeIds.end()) continue;
      const int idx = -1 - it->second;
      while (out.nativeFuncs.size() <= static_cast<size_t>(idx))
        out.nativeFuncs.push_back(nullptr);
      out.nativeFuncs[static_cast<size_t>(idx)] = func;
    }
    const auto &resolved = compiler.nativeFuncsResolved();
    for (size_t i = 0; i < resolved.size(); i++) {
      while (out.nativeFuncs.size() <= i) out.nativeFuncs.push_back(nullptr);
      if (resolved[i]) out.nativeFuncs[i] = resolved[i];
    }
    err.clear();
    return true;
  } catch (const QLangError &e) {
    err = e.what();
    return false;
  }
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

// Compiles `source` as a library and returns the `.qlc` BYTES to put on disk.
// `ctx`'s registry supplies whatever the library imports itself, so a
// dependency has to be loaded there before its dependent is compiled.
std::string libraryImage(QLangContext &ctx, const std::string &source,
                         std::string &err) {
  CompiledProgram prog;
  std::map<std::string, int> ids;
  if (!compile(ctx, source, prog, ids, err, "", true, /*seedExternals=*/false))
    return "";
  std::ostringstream os(std::ios::binary);
  if (!writeQlc(os, prog, ids)) {
    err = "the library image could not be written";
    return "";
  }
  return os.str();
}

// `libraryImage` for a library that imports nothing (its own scratch context,
// with discovery off so a stray file in the cwd cannot join in).
std::string standaloneImage(const std::string &source, std::string &err) {
  QLangContext scratch;
  scratch.setModuleSearchPaths({});
  return libraryImage(scratch, source, err);
}

// Compiles `source` as the entry against what `ctx` has by then resolved, links
// the result, and reports the merged program.
bool buildLinked(QLangContext &ctx, const std::string &source,
                 CompiledProgram &merged, std::string &err,
                 CompiledProgram *entryOut = nullptr,
                 const std::string &moduleName = "app") {
  CompiledProgram entry;
  std::map<std::string, int> nativeIds;
  if (!compile(ctx, source, entry, nativeIds, err, moduleName)) return false;
  if (entryOut) *entryOut = entry;
  try {
    QLangLinker linker(ctx.loadedModules());
    merged = linker.link(entry, nativeIds);
    err.clear();
    return true;
  } catch (const QLangError &e) {
    err = e.what();
    return false;
  }
}

// Runs `prog`, returning what it printed ("" when it threw).
std::string runText(CompiledProgram &prog, QLangContext &ctx) {
  QLangVM vm(prog, ctx);
  StdoutCapture cap;
  try {
    vm.run();
  } catch (const QLangError &) {
    return "";
  } catch (const std::exception &) {
    return "";
  }
  return cap.text();
}

// The message of the QLangError `f` throws ("" when it throws nothing,
// "std::exception: ..." when it throws something else — a callback that breaks
// its contract must not escape as a foreign exception).
template <class F> std::string errorOf(F &&f) {
  try {
    f();
  } catch (const QLangError &e) {
    return e.what();
  } catch (const std::exception &e) {
    return std::string("std::exception: ") + e.what();
  }
  return "";
}

// Whether the import table binds `name` from `module`, and whether it binds
// nothing else. An `import m` binds every export of `m` under its qualifier —
// MODULE_NAME included — so "imported one module" is a set of entries sharing
// one module name, not one entry.
bool hasImport(const CompiledProgram &prog, const std::string &module,
               const std::string &name) {
  for (const ImportEntry &e : prog.imports)
    if (e.module == module && e.name == name)
      return true;
  return false;
}
bool importsOnlyFrom(const CompiledProgram &prog, const std::string &module) {
  if (prog.imports.empty())
    return false;
  for (const ImportEntry &e : prog.imports)
    if (e.module != module)
      return false;
  return true;
}

bool endsWith(const std::string &s, const std::string &suffix) {
  return s.size() >= suffix.size() &&
         s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// The library builds a resolved module path with std::filesystem's native
// separator (backslash on Windows); this suite writes forward slashes. Compare
// the generic form so a path is equal to itself on every platform — the full
// directory prefix still has to match, so provenance is still pinned.
bool samePath(const std::string &a, const std::string &b) {
  return std::filesystem::path(a).generic_string() ==
         std::filesystem::path(b).generic_string();
}

// A library whose only observable is what `who()` answers, so two images with
// the same module name can be told apart by what a linked program prints.
std::string whoLibrary(const std::string &moduleName, const std::string &answer,
                       std::string &err) {
  return standaloneImage(
      "export const MODULE_NAME = \"" + moduleName + "\"\n"
      "export def who()\n"
      "  return \"" + answer + "\"\n"
      "end\n",
      err);
}

// --- the path list itself ---------------------------------------------------

void testPathApi() {
  QLangContext ctx;
  check(ctx.moduleSearchPaths().size() == 1,
        "api: a fresh context has exactly one search path");
  if (!ctx.moduleSearchPaths().empty())
    check(ctx.moduleSearchPaths()[0] == std::filesystem::current_path().string(),
          "api: the default path is the working directory at construction");

  ctx.setModuleSearchPaths({"one", "two"});
  check(ctx.moduleSearchPaths().size() == 2,
        "api: setModuleSearchPaths replaces the list");
  ctx.addModuleSearchPath("three");
  check(ctx.moduleSearchPaths().size() == 3 &&
            ctx.moduleSearchPaths()[2] == "three",
        "api: addModuleSearchPath appends");

  ctx.setModuleSearchPaths({});
  check(ctx.moduleSearchPaths().empty(),
        "api: an empty list means search no paths at all");
  check(ctx.findModule("whatever") == nullptr,
        "api: with no paths, nothing is found and nothing throws");
  check(ctx.findModule("") == nullptr, "api: the empty name finds nothing");
  check(ctx.moduleSearchCallback() == nullptr,
        "api: no callback by default");
}

// --- the search: order, skips, .qlc before .ql ------------------------------

void testSearchOrder() {
  TempTree tree("order");
  const std::string first = tree.dir("first");
  const std::string second = tree.dir("second");
  const std::string mixed = tree.dir("mixed");
  const std::string sourceOnly = tree.dir("sourceonly");

  std::string err;
  const std::string firstImage = whoLibrary("one", "first", err);
  if (!check(!firstImage.empty(), "order: the first image builds (" + err + ")"))
    return;
  const std::string secondImage = whoLibrary("one", "second", err);
  tree.write("first/one.qlc", firstImage);
  tree.write("second/one.qlc", secondImage);
  // The same module as a `.qlc` and as a `.ql`: the image wins.
  tree.write("mixed/two.qlc", whoLibrary("two", "image", err));
  tree.write("mixed/two.ql", "export const MODULE_NAME = \"two\"\n");
  // A source file with no image beside it: C8b compiles it.
  tree.write("sourceonly/three.ql", "export const MODULE_NAME = \"three\"\n"
                                    "export def who()\n"
                                    "  return \"source\"\n"
                                    "end\n");

  QLangContext ctx;
  ctx.setModuleSearchPaths({first, second});
  auto mod = ctx.findModule("one");
  if (!check(mod != nullptr, "order: the module is found")) return;
  check(samePath(mod->path, first + "/one.qlc"),
        "order: the first directory that has it wins");
  check(ctx.loadedModules().size() == 1,
        "order: the losing candidate is never loaded");

  // A directory that is not there is skipped, not an error; a plain file used
  // as a search path is skipped too.
  QLangContext skip;
  skip.setModuleSearchPaths({"/qlang/does/not/exist", first});
  check(skip.findModule("one") != nullptr,
        "order: a missing directory is skipped, not an error");
  QLangContext notdir;
  notdir.setModuleSearchPaths({first + "/one.qlc"});
  check(notdir.findModule("one") == nullptr,
        "order: a search path that is not a directory is skipped");

  QLangContext m;
  m.setModuleSearchPaths({mixed});
  auto image = m.findModule("two");
  if (!check(image != nullptr, "order: .qlc is tried before .ql")) return;
  check(endsWith(image->path, ".qlc"), "order: the image is what was taken in");

  QLangContext s;
  s.setModuleSearchPaths({sourceOnly});
  auto lone = s.findModule("three");
  if (!check(lone != nullptr, "order: a lone .ql is compiled, not refused"))
    return;
  check(samePath(lone->path, sourceOnly + "/three.ql"),
        "order: the compiled source is the file that answered");
  check(lone->program.moduleName == "three" &&
            s.loadedModule("three") == lone.get(),
        "order: the compiled source is registered like an image");
  CompiledProgram fromSource;
  if (!check(buildLinked(s, "import three\nprintln(three.who())\n", fromSource, err),
             "order: a compiled source links into a program (" + err + ")"))
    return;
  check(runText(fromSource, s) == "source\n",
        "order: the compiled source is what the program runs");

  // An empty entry means the working directory (SPEC: the default list is the
  // cwd; an explicit "" is the same directory).
  QLangContext cwd;
  cwd.setModuleSearchPaths({""});
  check(cwd.findModule("one") == nullptr,
        "order: an empty entry searches the working directory (which has none)");
}

// --- the cache --------------------------------------------------------------

void testResolveOnce() {
  TempTree tree("cache");
  const std::string libs = tree.dir("libs");
  std::string err;
  const std::string image = whoLibrary("greeter", "disk", err);
  if (!check(!image.empty(), "cache: the image builds (" + err + ")")) return;
  const std::string written = tree.write("libs/greeter.qlc", image);

  QLangContext ctx;
  ctx.setModuleSearchPaths({libs});
  auto mod = ctx.findModule("greeter");
  if (!check(mod != nullptr, "cache: resolved from the search path")) return;
  check(samePath(mod->path, written),
        "cache: the image remembers where it came from");
  check(ctx.loadedModule("greeter") == mod.get(),
        "cache: a resolved image is in the registry");

  auto again = ctx.findModule("greeter");
  check(again == mod, "cache: the second request returns the same image");
  check(ctx.loadedModules().size() == 1,
        "cache: resolution happens once per context");

  // clear() drops the registry and the cache, and keeps the configuration.
  ctx.clear();
  check(ctx.loadedModule("greeter") == nullptr,
        "cache: clear() drops the registry");
  check(ctx.moduleSearchPaths().size() == 1 &&
            ctx.moduleSearchPaths()[0] == libs,
        "cache: clear() keeps the search paths");
  auto after = ctx.findModule("greeter");
  check(after != nullptr && after != mod,
        "cache: after clear() the module is found again (re-resolved)");
}

// --- the callback -----------------------------------------------------------

// A host-built image for the callback to hand back: the same shape
// `loadProgram` produces, built here rather than read from a file.
std::shared_ptr<LoadedModule> embeddedModule(const std::string &moduleName,
                                             const std::string &answer,
                                             const std::string &path) {
  QLangContext scratch;
  scratch.setModuleSearchPaths({});
  std::string err;
  const std::string bytes = whoLibrary(moduleName, answer, err);
  if (bytes.empty()) return nullptr;
  std::istringstream in(bytes, std::ios::binary);
  auto mod = std::make_shared<LoadedModule>();
  std::string rerr;
  if (!readQlc(in, mod->program, mod->nativeSlots, rerr)) return nullptr;
  mod->path = path;
  return mod;
}

void testCallback() {
  TempTree tree("callback");
  const std::string libs = tree.dir("libs");
  std::string err;
  tree.write("libs/greeter.qlc", whoLibrary("greeter", "disk", err));

  // nullptr + empty error: not found, continue with the paths.
  {
    QLangContext ctx;
    ctx.setModuleSearchPaths({libs});
    int calls = 0;
    ctx.setModuleSearchCallback(
        [&](const std::string &, std::string &e) -> std::shared_ptr<LoadedModule> {
          calls++;
          e.clear();
          return nullptr;
        });
    auto mod = ctx.findModule("greeter");
    if (!check(mod != nullptr, "callback: nullptr+empty falls through to the paths"))
      return;
    check(calls == 1, "callback: consulted once per resolution");
    check(samePath(mod->path, libs + "/greeter.qlc"),
          "callback: the fall-through really used the file");
  }

  // nullptr + a message: found but unusable — stop, and do NOT read the file.
  {
    QLangContext ctx;
    ctx.setModuleSearchPaths({libs});
    ctx.setModuleSearchCallback(
        [](const std::string &name, std::string &e) -> std::shared_ptr<LoadedModule> {
          e = "'" + name + "' is corrupt in the resource bundle";
          return nullptr;
        });
    const std::string msg = errorOf([&] { ctx.findModule("greeter"); });
    check(msg.find("corrupt in the resource bundle") != std::string::npos,
          "callback: nullptr+message stops resolution and reports: " + msg);
    check(ctx.loadedModules().empty(),
          "callback: a refusal does not fall through to the file");
  }

  // A module: resolved, registered under its own name, cached, and it shadows
  // the file the same name would have found.
  {
    QLangContext ctx;
    ctx.setModuleSearchPaths({libs});
    ctx.setModuleSearchCallback(
        [](const std::string &name, std::string &) -> std::shared_ptr<LoadedModule> {
          if (name != "greeter") return nullptr;
          return embeddedModule("greeter", "embedded", ":/res/greeter.qlc");
        });
    auto mod = ctx.findModule("greeter");
    if (!check(mod != nullptr, "callback: a returned module is resolved")) return;
    check(mod->path == ":/res/greeter.qlc",
          "callback: the callback's image wins over the file");
    check(ctx.loadedModule("greeter") == mod.get(),
          "callback: a host-built image is registered by the context");
    check(ctx.findModule("greeter") == mod,
          "callback: the result is cached like any other");
    check(ctx.loadedModules().size() == 1,
          "callback: exactly one module is registered");
  }

  // A callback that throws is reported, not propagated.
  {
    QLangContext ctx;
    ctx.setModuleSearchPaths({libs});
    ctx.setModuleSearchCallback(
        [](const std::string &, std::string &) -> std::shared_ptr<LoadedModule> {
          throw std::runtime_error("the resource bundle is closed");
        });
    const std::string msg = errorOf([&] { ctx.findModule("greeter"); });
    check(msg.find("std::exception") == std::string::npos &&
              msg.find("the resource bundle is closed") != std::string::npos,
          "callback: a throwing callback becomes a resolution error: " + msg);
  }

  // A registered C++ module outranks the callback AND the file, and the
  // callback is not consulted for it at all.
  {
    QLangContext ctx;
    ctx.setModuleSearchPaths({libs});
    QLangContext::Module mathy;
    mathy.name = "greeter";
    mathy.funcs["who"] = [](QLangContext &, const std::vector<QLangValue> &) {
      return QLangValue::String("native");
    };
    ctx.registerModule("greeter", mathy);
    int calls = 0;
    ctx.setModuleSearchCallback(
        [&](const std::string &, std::string &e) -> std::shared_ptr<LoadedModule> {
          calls++;
          return nullptr;
        });
    check(ctx.findModule("greeter") == nullptr,
          "callback: a C++ module is answered as 'nothing to load'");
    check(calls == 0, "callback: never consulted for a registered C++ module");
    check(ctx.loadedModules().empty(),
          "callback: a C++ module name never reaches the paths");
  }

  // Assigning nullptr restores the default (file search only).
  {
    QLangContext ctx;
    ctx.setModuleSearchPaths({libs});
    ctx.setModuleSearchCallback(
        [](const std::string &, std::string &e) -> std::shared_ptr<LoadedModule> {
          e = "always refuses";
          return nullptr;
        });
    check(errorOf([&] { ctx.findModule("greeter"); }) != "",
          "callback: a refusing callback is in effect");
    ctx.setModuleSearchCallback(nullptr);
    check(ctx.moduleSearchCallback() == nullptr,
          "callback: assigning nullptr clears it");
    check(ctx.findModule("greeter") != nullptr,
          "callback: assigning nullptr restores the default behaviour");
  }
}

// --- transitive resolution --------------------------------------------------

void testDependencies() {
  TempTree tree("deps");
  const std::string libs = tree.dir("libs");
  const std::string more = tree.dir("more");
  std::string err;

  const std::string depImage = standaloneImage(
      "export const MODULE_NAME = \"dep\"\n"
      "export def dep()\n"
      "  return 7\n"
      "end\n",
      err);
  if (!check(!depImage.empty(), "deps: the dependency builds (" + err + ")")) return;

  // A library that imports it, compiled with the dependency loaded.
  std::string useImage;
  {
    QLangContext scratch;
    scratch.setModuleSearchPaths({});
    std::istringstream in(depImage, std::ios::binary);
    if (!check(scratch.loadProgram(in, "dep.qlc") != nullptr,
               "deps: the dependency loads into the build context"))
      return;
    useImage = libraryImage(
        scratch,
        "export const MODULE_NAME = \"use\"\n"
        "import dep\n"
        "export def value()\n"
        "  return dep.dep()\n"
        "end\n",
        err);
  }
  if (!check(!useImage.empty(), "deps: the dependent builds (" + err + ")")) return;

  // The two live in different directories, both on the path.
  tree.write("libs/use.qlc", useImage);
  tree.write("more/dep.qlc", depImage);

  QLangContext ctx;
  ctx.setModuleSearchPaths({libs, more});
  auto use = ctx.findModule("use");
  if (!check(use != nullptr, "deps: the dependent is found")) return;
  check(ctx.loadedModule("dep") != nullptr,
        "deps: its own import arrived with it");
  check(ctx.loadedModules().size() == 2,
        "deps: both images are registered");
  check(samePath(ctx.loadedModule("dep")->path, more + "/dep.qlc"),
        "deps: the dependency was found on the same path list");

  // The same pair with the dependency nowhere: the failure names the module
  // that asked, not just the name that is missing.
  TempTree broken("depsbroken");
  const std::string only = broken.dir("only");
  broken.write("only/use.qlc", useImage);
  QLangContext miss;
  miss.setModuleSearchPaths({only});
  const std::string msg = errorOf([&] { miss.findModule("use"); });
  check(msg.find("use") != std::string::npos &&
            msg.find("'dep'") != std::string::npos,
        "deps: a missing dependency names both sides: " + msg);
  check(miss.loadedModule("use") == nullptr,
        "deps: a dependent whose dependency is missing is not left registered");
}

// A cycle between two files on disk must terminate: the registry answers the
// second visit, and the compiler is what rejects the graph.
void testCycleTerminates() {
  TempTree tree("cycle");
  const std::string libs = tree.dir("libs");
  std::string err;

  // Each half is compiled against a STUB of the other (a stub that imports
  // nothing), because the compiler refuses to compile a module against a
  // loaded image that transitively imports it back — the very cycle under
  // test. The stubs still declare the same MODULE_NAME, so the two real
  // images record import entries that name each other: on disk the graph is
  // a -> b -> a.
  const std::string stubA = standaloneImage(
      "export const MODULE_NAME = \"a\"\n"
      "export def af()\n"
      "  return 0\n"
      "end\n",
      err);
  const std::string stubB = standaloneImage(
      "export const MODULE_NAME = \"b\"\n"
      "export def noop()\n"
      "  return 0\n"
      "end\n",
      err);
  if (!check(!stubA.empty() && !stubB.empty(),
             "cycle: the stubs build (" + err + ")"))
    return;
  std::string aImage, bImage;
  {
    QLangContext scratch;
    scratch.setModuleSearchPaths({});
    std::istringstream bin(stubB, std::ios::binary);
    scratch.loadProgram(bin, "b.qlc");
    aImage = libraryImage(
        scratch,
        "export const MODULE_NAME = \"a\"\n"
        "import b\n"
        "export def af()\n"
        "  return b.noop()\n"
        "end\n",
        err);
  }
  if (!check(!aImage.empty(), "cycle: a builds (" + err + ")")) return;
  {
    QLangContext scratch;
    scratch.setModuleSearchPaths({});
    std::istringstream ain(stubA, std::ios::binary);
    scratch.loadProgram(ain, "a.qlc");
    bImage = libraryImage(
        scratch,
        "export const MODULE_NAME = \"b\"\n"
        "import a\n"
        "export def bf()\n"
        "  return a.af()\n"
        "end\n",
        err);
  }
  if (!check(!bImage.empty(), "cycle: b builds (" + err + ")")) return;
  tree.write("libs/a.qlc", aImage);
  tree.write("libs/b.qlc", bImage);

  QLangContext ctx;
  ctx.setModuleSearchPaths({libs});
  auto a = ctx.findModule("a"); // would hang if the walk re-entered a
  if (!check(a != nullptr, "cycle: resolving a cycle terminates")) return;
  check(ctx.loadedModule("b") != nullptr,
        "cycle: the cycle's other half came with it");
  check(ctx.loadedModules().size() == 2,
        "cycle: neither image is loaded twice");

  // Discovery does not smuggle a cycle past the compiler: an entry that is
  // itself part of the cycle (named `a`, importing `b`, which on disk leads
  // back to `a`) is rejected even though every image arrived by search.
  QLangContext guarded;
  guarded.setModuleSearchPaths({libs});
  CompiledProgram merged;
  const bool ok =
      buildLinked(guarded, "import b\nlocal x = b.bf()\n", merged, err, nullptr, "a");
  check(!ok && err.find("import cycle") != std::string::npos,
        "cycle: the compiler still rejects the graph: " + err);
}

// --- identity: the file's name is not the module's name ---------------------

void testDeclaredName() {
  TempTree tree("identity");
  const std::string libs = tree.dir("libs");
  std::string err;
  const std::string image = whoLibrary("bar", "declared", err);
  if (!check(!image.empty(), "identity: the image builds (" + err + ")")) return;
  tree.write("libs/foo.qlc", image); // the file says foo, the module says bar

  QLangContext ctx;
  ctx.setModuleSearchPaths({libs});
  auto mod = ctx.findModule("foo");
  if (!check(mod != nullptr, "identity: the file is found by its name")) return;
  check(mod->program.moduleName == "bar",
        "identity: the image registers under its declared MODULE_NAME");
  check(ctx.loadedModule("bar") == mod.get(),
        "identity: the registry keys it by the declared name");
  check(ctx.loadedModule("foo") == nullptr,
        "identity: the filename is not a registry key");
  check(ctx.findModule("foo") == mod,
        "identity: the requested name stays cached");
  check(ctx.findModule("bar") == mod,
        "identity: the declared name reaches the same image");

  // `import foo` binds the module named bar, and the program runs.
  CompiledProgram merged;
  if (!check(buildLinked(ctx, "import foo\nprintln(foo.who())\n", merged, err),
             "identity: `import foo` compiles against bar (" + err + ")"))
    return;
  check(importsOnlyFrom(merged, "bar") && hasImport(merged, "bar", "who"),
        "identity: every import entry names the defining module, bar");
  check(runText(merged, ctx) == "declared\n",
        "identity: the discovered module runs");
}

// --- the end to end: nothing pre-loaded -------------------------------------

void testCompileDiscovers() {
  TempTree tree("e2e");
  const std::string libs = tree.dir("libs");
  std::string err;
  tree.write("libs/greeter.qlc", whoLibrary("greeter", "found", err));

  QLangContext ctx;
  ctx.setModuleSearchPaths({libs});
  CompiledProgram entry, merged;
  if (!check(buildLinked(ctx, "import greeter\nprintln(greeter.who())\n", merged, err, &entry),
             "e2e: an entry with nothing pre-loaded compiles (" + err + ")"))
    return;
  check(importsOnlyFrom(entry, "greeter") && hasImport(entry, "greeter", "who"),
        "e2e: the discovered module produced its import entries");
  check(ctx.loadedModules().size() == 1,
        "e2e: the compiler's resolution registered the library");
  check(runText(merged, ctx) == "found\n", "e2e: the discovered module runs");

  // The alias form discovers the same way: `import m as a` records the alias
  // and flat-imports the exports, so the bound name is `who`, not `a.who`.
  QLangContext aliased;
  aliased.setModuleSearchPaths({libs});
  CompiledProgram amerged;
  check(buildLinked(aliased, "import greeter as g\nprintln(g.who())\n", amerged, err),
        "e2e: `import greeter as g` discovers the module (" + err + ")");
  check(runText(amerged, aliased) == "found\n",
        "e2e: the aliased import runs too");
}

// ---------------------------------------------------------------------------
// F3 (module-export/import plan P2): per-name alias `from m import f as g`.
// The alias is the importing module's binding name only — the import table and
// the listing keep naming (defining module, exported name), collisions speak
// the alias, and a native module's functions alias the same way.
// ---------------------------------------------------------------------------

void testPerNameAlias() {
  TempTree tree("alias");
  const std::string libs = tree.dir("libs");
  std::string err;
  tree.write("libs/greeter.qlc", whoLibrary("greeter", "found", err));

  QLangContext ctx;
  ctx.setModuleSearchPaths({libs});
  CompiledProgram entry, merged;
  if (!check(buildLinked(ctx,
                         "from greeter import who as greet\nprintln(greet())\n",
                         merged, err, &entry),
             "alias: `from m import f as g` compiles (" + err + ")"))
    return;
  check(runText(merged, ctx) == "found\n", "alias: the aliased name runs");
  check(hasImport(entry, "greeter", "who") &&
            !hasImport(entry, "greeter", "greet"),
        "alias: the import table names the exported name, not the alias");

  // Re-importing the same entity under the same alias is a no-op, not a
  // collision — the same rule the flat forms already had.
  QLangContext again;
  again.setModuleSearchPaths({libs});
  CompiledProgram amerged;
  if (!check(buildLinked(again,
                         "from greeter import who as g\n"
                         "from greeter import who as g\nprintln(g())\n",
                         amerged, err),
             "alias: the same aliased re-import is a no-op (" + err + ")"))
    return;
  check(runText(amerged, again) == "found\n",
        "alias: the no-op re-import still runs");

  // Two names claiming one alias collide, and the error speaks the alias.
  QLangContext collide;
  collide.setModuleSearchPaths({libs});
  CompiledProgram cm;
  const std::string msg = [&] {
    std::string m;
    buildLinked(collide,
                "from greeter import who as x\n"
                "from greeter import MODULE_NAME as x\n",
                cm, m);
    return m;
  }();
  check(msg.find("'x' is already imported") != std::string::npos,
        "alias: the collision reports the alias (" + msg + ")");

  // An unknown export keeps reporting the name that is actually missing.
  QLangContext unknown;
  unknown.setModuleSearchPaths({libs});
  CompiledProgram um;
  const std::string umsg = [&] {
    std::string m;
    buildLinked(unknown, "from greeter import nope as g\n", um, m);
    return m;
  }();
  check(
      umsg.find("does not export 'nope'") != std::string::npos,
      "alias: the unknown-name error reports the exported name (" + umsg + ")");

  // The C++ module path aliases the same way.
  QLangContext native;
  QLangContext::Module mod;
  mod.name = "greeter";
  mod.funcs["who"] = [](QLangContext &, const std::vector<QLangValue> &) {
    return QLangValue::String("native");
  };
  native.registerModule("greeter", mod);
  CompiledProgram nprog;
  std::map<std::string, int> ids;
  if (!check(compile(native,
                     "from greeter import who as greet\nprintln(greet())\n",
                     nprog, ids, err, "app"),
             "alias: a native function aliases too (" + err + ")"))
    return;
  check(runText(nprog, native) == "native\n", "alias: the aliased native runs");
}

// ---------------------------------------------------------------------------
// Reading through a module that is loaded but never imported here says exactly
// that (audit F5): the old advice, "reach what it offers as m.<name>", was the
// thing the code was already doing.
// ---------------------------------------------------------------------------

void testNotImportedModule() {
  TempTree tree("notimp");
  const std::string libs = tree.dir("libs");
  std::string err;
  tree.write("libs/leaf.ql",
             "export const MODULE_NAME = \"leaf\"\n"
             "export const L = \"leaf-value\"\n"
             "export def lfun()\n"
             "  return \"lfun\"\n"
             "end\n");
  tree.write("libs/mid.ql",
             "export const MODULE_NAME = \"mid\"\n"
             "import leaf\n"
             "export def mf()\n"
             "  return leaf.lfun()\n"
             "end\n");

  // leaf arrives through mid, but this entry never imported it.
  const std::string notread = [&] {
    QLangContext ctx;
    ctx.setModuleSearchPaths({libs});
    CompiledProgram merged;
    std::string m;
    buildLinked(ctx, "import mid\nprintln(leaf.L)\n", merged, m);
    return m;
  }();
  check(notread.find("module 'leaf' is not imported in this module") !=
            std::string::npos,
        "not imported: a qualified read says so (" + notread + ")");

  const std::string notcalled = [&] {
    QLangContext ctx;
    ctx.setModuleSearchPaths({libs});
    CompiledProgram merged;
    std::string m;
    buildLinked(ctx, "import mid\nprintln(leaf.lfun())\n", merged, m);
    return m;
  }();
  check(notcalled.find("module 'leaf' is not imported in this module") !=
            std::string::npos,
        "not imported: a qualified call says so (" + notcalled + ")");

  // Importing it binds the qualifier and the read works.
  QLangContext fixed;
  fixed.setModuleSearchPaths({libs});
  CompiledProgram merged;
  if (!check(buildLinked(fixed, "import mid\nimport leaf\nprintln(leaf.L)\n",
                         merged, err),
             "not imported: importing the module fixes the read (" + err + ")"))
    return;
  check(runText(merged, fixed) == "leaf-value\n",
        "not imported: the imported read runs");

  // An imported module read as a plain value keeps the old, correct advice.
  QLangContext bare;
  bare.setModuleSearchPaths({libs});
  CompiledProgram bm;
  std::string berr;
  buildLinked(bare, "import leaf\nprintln(leaf)\n", bm, berr);
  check(berr.find("'leaf' is a module, not a value") != std::string::npos,
        "not imported: an imported module still says it is not a value (" +
            berr + ")");
}

// A C++ module of the same name as a file on the path is not an ambiguity: the
// native wins and the file is never opened (SPEC order, step 1).
void testNativeBeatsFile() {
  TempTree tree("native");
  const std::string libs = tree.dir("libs");
  std::string err;
  tree.write("libs/greeter.qlc", whoLibrary("greeter", "disk", err));

  QLangContext ctx;
  ctx.setModuleSearchPaths({libs});
  QLangContext::Module mod;
  mod.name = "greeter";
  mod.funcs["who"] = [](QLangContext &, const std::vector<QLangValue> &) {
    return QLangValue::String("native");
  };
  ctx.registerModule("greeter", mod);

  CompiledProgram prog;
  std::map<std::string, int> ids;
  if (!check(compile(ctx, "import greeter\nprintln(greeter.who())\n", prog, ids, err, "app"),
             "native: the C++ module is not an ambiguity (" + err + ")"))
    return;
  check(prog.imports.empty(), "native: no .ql import was created");
  check(ctx.loadedModules().empty(), "native: the file was never loaded");
  check(runText(prog, ctx) == "native\n", "native: the C++ module answered");
}

// --- compiling a discovered `.ql` (plan C8b) --------------------------------

// A `.ql` hit is compiled on the spot: on a private scratch context, against
// this context's natives and loaded modules, with its own imports resolved back
// through this context. What comes out is an ordinary image — the registry, the
// linker and the run behave exactly as for the `.qlc` the file could have been
// precompiled to (SPEC §modules "technical requirements" 4).
void testCompiledSource() {
  TempTree tree("compiled");
  const std::string libs = tree.dir("libs");
  const std::string deep = tree.dir("deep");
  std::string err;

  // No MODULE_NAME at all: the name it is found under is its name (plan Q37).
  tree.write("libs/stemmy.ql",
             "export def hi()\n"
             "  return \"stem\"\n"
             "end\n");
  // Declares a name that is NOT the file's: the declared one wins the registry,
  // exactly as `foo.qlc` declaring `bar` does (plan Q36).
  tree.write("libs/mis.ql",
             "export const MODULE_NAME = \"renamed\"\n"
             "export def hi()\n"
             "  return \"declared\"\n"
             "end\n");
  // A source whose OWN import is another source, in a different directory: the
  // nested compile resolves it through this context, so one path list covers
  // the whole tree.
  tree.write("libs/user.ql",
             "export const MODULE_NAME = \"user\"\n"
             "from dep import hi\n"
             "export def hi()\n"
             "  return \"user+\" + hi()\n"
             "end\n");
  tree.write("deep/dep.ql",
             "export const MODULE_NAME = \"dep\"\n"
             "export def hi()\n"
             "  return \"dep\"\n"
             "end\n");
  // Two sources that import each other: a cycle has to stop, naming the chain.
  tree.write("libs/cyc_a.ql",
             "import cyc_b\n"
             "export def hi()\n"
             "  return cyc_b.hi()\n"
             "end\n");
  tree.write("libs/cyc_b.ql",
             "import cyc_a\n"
             "export def hi()\n"
             "  return \"b\"\n"
             "end\n");

  QLangContext ctx;
  ctx.setModuleSearchPaths({libs, deep});
  const size_t globalsBefore = ctx.globalCount();

  auto stem = ctx.findModule("stemmy");
  if (!check(stem != nullptr, "source: a source file is compiled (" + err + ")"))
    return;
  check(stem->program.moduleName == "stemmy",
        "source: with no MODULE_NAME the file's stem is the name");
  check(ctx.loadedModule("stemmy") == stem.get(),
        "source: the compiled source is registered");
  check(ctx.globalCount() == globalsBefore,
        "source: the scratch compile leaves the caller's globals alone");
  check(ctx.findModule("stemmy") == stem, "source: it is cached like an image");

  auto mis = ctx.findModule("mis");
  if (!check(mis != nullptr, "source: a source with its own name compiles"))
    return;
  check(mis->program.moduleName == "renamed",
        "source: the declared MODULE_NAME is its name");
  check(ctx.loadedModule("renamed") == mis.get() &&
            ctx.loadedModule("mis") == nullptr,
        "source: the registry keys the declared name, not the filename");

  CompiledProgram merged;
  if (!check(buildLinked(ctx, "import user\nprintln(user.hi())\n", merged, err),
             "source: a source that imports a source compiles (" + err + ")"))
    return;
  check(ctx.loadedModule("dep") != nullptr,
        "source: the nested source arrived through this context");
  check(runText(merged, ctx) == "user+dep\n",
        "source: a nested source import runs");

  // The declared-name alias works through a source too.
  QLangContext aliased;
  aliased.setModuleSearchPaths({libs});
  CompiledProgram amerged;
  if (!check(buildLinked(aliased, "import mis\nprintln(mis.hi())\n", amerged, err),
             "source: the requested name aliases the declared one (" + err + ")"))
    return;
  check(runText(amerged, aliased) == "declared\n",
        "source: the aliased source module runs");

  QLangContext cyc;
  cyc.setModuleSearchPaths({libs});
  const std::string cerr = errorOf([&] { cyc.findModule("cyc_a"); });
  check(cerr.find("import cycle") != std::string::npos &&
            cerr.find("cyc_a.ql") != std::string::npos &&
            cerr.find("cyc_b.ql") != std::string::npos,
        "source: a cycle names the chain instead of recursing: " + cerr);
  check(cyc.loadedModules().empty(), "source: a cycle registers no module");

  // A source that does not compile names the file it came from, and the failed
  // lookup is not left in place: a fixed file still resolves.
  QLangContext bad;
  bad.setModuleSearchPaths({libs});
  const std::string berr = errorOf([&] { bad.findModule("nothing"); });
  check(berr.empty(), "source: a name with no file at all is not an error here");
  tree.write("libs/nothing.ql", "export def bad( {\n  return 1\nend\n");
  const std::string ferr = errorOf([&] { bad.findModule("nothing"); });
  check(ferr.find("nothing.ql") != std::string::npos,
        "source: a compile failure names the discovered file: " + ferr);
  check(bad.loadedModules().empty(), "source: a failed compile registers nothing");
  tree.write("libs/nothing.ql",
             "export def hi()\n"
             "  return \"fixed\"\n"
             "end\n");
  check(bad.findModule("nothing") != nullptr,
        "source: a fixed file resolves — the in-flight entry did not stick");
}

} // namespace

int main() {
  testPathApi();
  testSearchOrder();
  testResolveOnce();
  testCallback();
  testDependencies();
  testCycleTerminates();
  testDeclaredName();
  testCompileDiscovers();
  testPerNameAlias();
  testNotImportedModule();
  testNativeBeatsFile();
  testCompiledSource();

  std::cout << (fails == 0 ? "ALL PASS" : "FAILURES") << ": " << passes
            << " passed, " << fails << " failed\n";
  return fails == 0 ? 0 : 1;
}
