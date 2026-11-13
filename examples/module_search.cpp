// Module search paths in practice (SPEC §api "module search paths" and
// §api "external search callback"; plan docs/plans/missing-features-plan.md
// §C8a).
//
// An `import <name>` the context cannot answer from its registry is FOUND,
// not named: QLangContext::findModule resolves it through the host's search
// callback and then a list of directories (`.qlc` before `.ql` within each,
// first match wins), so a host that knows where its libraries live never has
// to enumerate them. This example builds one small library image, drops it in
// a temporary directory, and shows the resolution order end to end:
//
//   1. search paths — `import greeter` finds libs/greeter.qlc with nothing
//      preloaded: the entry compiles, links and runs.
//   2. the callback — the host answers the same request from memory (think a
//      resource bundle); its image shadows the file on the path.
//   3. a registered C++ module of the same name outranks the callback AND the
//      file, and the callback is not consulted for it at all.
//
// Build: cmake --build build --target module_search
// Run:   ./build/module_search

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include <qlang/QLangBytecodeIO.h>
#include <qlang/QLangCompiler.h>
#include <qlang/QLangContext.h>
#include <qlang/QLangErrors.h>
#include <qlang/QLangLinker.h>
#include <qlang/QLangVM.h>

using namespace qlang;

namespace {

// The library every part imports. The answer string says which copy answered,
// so the three resolutions are told apart by what the program prints.
std::string librarySource(const std::string &answer) {
  return "export const MODULE_NAME = \"greeter\"\n"
         "export def greet(who)\n"
         "  return \"hello, \" + who + \" " + answer + "\"\n"
         "end\n";
}

// The entry program: nothing tells it where `greeter` came from — that is the
// whole point of C8a.
const char *kEntrySource =
    "import greeter\nprintln(greeter.greet(\"world\"))\n";

// Compiles `source` as a library and returns the `.qlc` bytes. No natives and
// no C++ modules are offered to the compiler, so the image's native-slot
// table is empty and any context can load it (docs/SERIALIZATION.md §4.3).
std::string libraryBytes(const std::string &source) {
  QLangContext scratch;
  scratch.setModuleSearchPaths({}); // discovery off: this compile is isolated
  std::vector<StmtPtr> program = scratch.parse(source);
  QLangCompiler compiler(std::move(program), &scratch);
  compiler.addExternalConsts({});
  compiler.addExternalFuncs({});
  compiler.addModules({});
  compiler.setLibraryMode(true);
  CompiledProgram prog = compiler.compile();
  std::ostringstream os(std::ios::binary);
  if (!writeQlc(os, prog, {}))
    throw QLangError(QLangErrorKind::Io, 0, 0, "the library image could not be written");
  return os.str();
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

// Compiles kEntrySource against whatever `ctx` resolves, links the entry with
// every loaded image (QLangLinker), runs the merged program, and returns what
// it printed. This is the same pipeline qlcc drives for a script.
std::string runEntry(QLangContext &ctx) {
  std::vector<StmtPtr> program = ctx.parse(kEntrySource);
  QLangCompiler compiler(std::move(program), &ctx);
  compiler.addExternalConsts({});
  std::unordered_set<std::string> nativeNames;
  for (const auto &[name, _] : ctx.nativeFuncs())
    nativeNames.insert(name);
  compiler.addExternalFuncs(nativeNames);
  compiler.addModules(ctx.modules());
  compiler.setModuleName("app");
  CompiledProgram entry = compiler.compile();

  // Wire the natives the compiler assigned slots to, exactly as qlcc
  // does: negative ids index `nativeFuncs` from the end, resolved module
  // functions from the front.
  for (const auto &[name, func] : ctx.nativeFuncs()) {
    auto it = compiler.nativeFuncIds().find(name);
    if (it == compiler.nativeFuncIds().end())
      continue;
    const int idx = -1 - it->second;
    while (entry.nativeFuncs.size() <= static_cast<size_t>(idx))
      entry.nativeFuncs.push_back(nullptr);
    entry.nativeFuncs[static_cast<size_t>(idx)] = func;
  }
  const auto &resolved = compiler.nativeFuncsResolved();
  for (size_t i = 0; i < resolved.size(); i++) {
    while (entry.nativeFuncs.size() <= i)
      entry.nativeFuncs.push_back(nullptr);
    if (resolved[i])
      entry.nativeFuncs[i] = resolved[i];
  }
  // Only the natives the merged image calls (§5.2).
  std::map<std::string, int> nativeIds = compiler.nativeSlotIds();

  QLangLinker linker(ctx.loadedModules());
  CompiledProgram merged = linker.link(entry, nativeIds);
  QLangVM vm(merged, ctx);
  StdoutCapture cap;
  vm.run();
  std::string text = cap.text();
  while (!text.empty() && text.back() == '\n')
    text.pop_back();
  return text;
}

// A host-built image for the callback to hand back: the same shape
// `loadProgram` produces, built from bytes the host already has in memory.
std::shared_ptr<LoadedModule> embeddedModule(const std::string &bytes,
                                             const std::string &path) {
  auto mod = std::make_shared<LoadedModule>();
  std::istringstream in(bytes, std::ios::binary);
  std::string err;
  if (!readQlc(in, mod->program, mod->nativeSlots, err))
    throw QLangError(QLangErrorKind::Io, 0, 0, "embedded image: " + err);
  mod->path = path;
  return mod;
}

} // namespace

int main() {
  try {
    // A temporary tree: libs/greeter.qlc answers "(from the disk image)".
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "qlang_module_search_example";
    std::filesystem::remove_all(root);
    const std::filesystem::path libs = root / "libs";
    std::filesystem::create_directories(libs);
    const std::string diskBytes = libraryBytes(librarySource("(from the disk image)"));
    {
      std::ofstream out(libs / "greeter.qlc", std::ios::binary);
      out << diskBytes;
    }

    // 1. Search paths: nothing is preloaded; `import greeter` finds the file.
    {
      QLangContext ctx;
      ctx.setModuleSearchPaths({libs.string()}); // or addModuleSearchPath(dir)
      std::cout << "[1] search path -> " << runEntry(ctx) << "\n";
      const LoadedModule *mod = ctx.loadedModule("greeter");
      std::cout << "    resolved at " << (mod ? mod->path : std::string("?"))
                << " (resolved once: a second findModule returns the cache)\n";
    }

    // 2. The callback: the host answers from memory and shadows the file.
    //    nullptr + empty error would fall through to the paths; nullptr + a
    //    message would stop resolution and report the message.
    {
      QLangContext ctx;
      ctx.setModuleSearchPaths({libs.string()});
      const std::string bundleBytes =
          libraryBytes(librarySource("(from the host's bundle)"));
      ctx.setModuleSearchCallback(
          [bundleBytes](const std::string &name,
                        std::string &) -> std::shared_ptr<LoadedModule> {
            if (name != "greeter")
              return nullptr; // not mine: let the search paths answer
            return embeddedModule(bundleBytes, ":/resources/greeter.qlc");
          });
      std::cout << "[2] callback    -> " << runEntry(ctx) << "\n";
    }

    // 3. A registered C++ module outranks the callback and the file, and the
    //    callback is never consulted for the name.
    {
      QLangContext ctx;
      ctx.setModuleSearchPaths({libs.string()});
      int callbackCalls = 0;
      ctx.setModuleSearchCallback(
          [&callbackCalls](const std::string &,
                           std::string &) -> std::shared_ptr<LoadedModule> {
            callbackCalls++;
            return nullptr;
          });
      QLangContext::Module greeter;
      greeter.name = "greeter";
      greeter.funcs["greet"] = [](QLangContext &,
                                  const std::vector<QLangValue> &args) {
        return QLangValue::String("hello, " + args[0].asString() +
                                  " (from C++)");
      };
      ctx.registerModule("greeter", greeter);
      std::cout << "[3] C++ module  -> " << runEntry(ctx) << "\n";
      std::cout << "    the callback was consulted " << callbackCalls
                << " time(s) for a native module's name\n";
    }

    std::filesystem::remove_all(root);
    return 0;
  } catch (const QLangError &e) {
    std::cerr << "module_search: " << e.what() << "\n";
    return 1;
  }
}
