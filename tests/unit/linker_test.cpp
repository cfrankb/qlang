// The loader and the linker (SPEC §bytecodes "external modules", "technical
// specifications" 1–5; plan docs/plans/external-modules-plan-v4.md Phase 3b).
//
// A library is a `.qlc` image with an export table. `QLangContext::loadProgram`
// reads one into the context's module registry (keyed by the name `import`
// binds it under), the compiler resolves `import` against that table, and
// `QLangLinker` merges the images with the entry program into one runnable
// program. This suite pins the parts Phase 3b owns —
//
//   * the registry: load by stream and by file, lookup by name, load order,
//     a duplicate module name, an unresolvable native slot, `clear()`;
//   * the merge: one function table (concatenated), one constant pool folded
//     through `valueKey` (Q33), one native-slot table deduped by name, one
//     global-register area split into disjoint per-module slices;
//   * the rewrites (Q32): every operand is rewritten on the decoded
//     `RawInstr` and re-encoded, so a merged index that needs a wider operand
//     is re-widened rather than byte-patched — the 300-def entry below exists
//     to push a merged function index past the 1-byte operand range;
//   * the run (Q35): each imported module's `<init>` executes exactly once,
//     dependencies first, before the entry module's, and an exported variable
//     is one shared cell the defining module writes and importers read (Q30);
//   * the boundaries: a program that is already linked, a module that is not
//     loaded, a library that is not linkable, and a linked program handed to
//     `writeQlc` each fail with a `QLangError` instead of producing a program
//     that runs against the wrong functions or cells.
//
// The compile-time half (unknown export, duplicate import, a write through an
// import, `import` inside a function body) is pinned by `export` and the
// compiler's own guards; only what the linker itself can reject is repeated
// here.
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

// Compile `source` exactly as `qlang_run` does (external consts/funcs/modules
// taken from the context), and wire the native slots the same way, so a linked
// program can actually call the natives its modules imported. `nativeIds` is
// the name -> negative CALL operand map the writer and the linker both take.
bool compile(QLangContext &ctx, const std::string &source, CompiledProgram &out,
             std::map<std::string, int> &nativeIds, std::string &err,
             const std::string &moduleName = "", bool libraryMode = false) {
  try {
    std::vector<StmtPtr> program = ctx.parse(source);
    QLangCompiler compiler(std::move(program), &ctx);
    compiler.addExternalConsts({});
    std::unordered_set<std::string> nativeNames;
    for (const auto &[name, _] : ctx.nativeFuncs()) nativeNames.insert(name);
    compiler.addExternalFuncs(nativeNames);
    compiler.addModules(ctx.modules());
    if (!moduleName.empty()) compiler.setModuleName(moduleName);
    compiler.setLibraryMode(libraryMode);
    out = compiler.compile();
    const auto &ids = compiler.nativeFuncIds();
    nativeIds = std::map<std::string, int>(ids.begin(), ids.end());
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

// The VM prints through std::cout, so swapping its buffer captures a run
// (same trick as codec_check's qlc-exec gate).
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

// Run `prog` on `ctx`, returning what it printed ("" when it threw).
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

// Run `prog`, returning the error text when the VM threw ("" on success).
std::string runError(CompiledProgram &prog, QLangContext &ctx) {
  QLangVM vm(prog, ctx);
  StdoutCapture cap;
  try {
    vm.run();
  } catch (const QLangError &e) {
    return e.what();
  } catch (const std::exception &e) {
    return e.what();
  }
  return "";
}

// Compiles `source` as a library, serializes it to a `.qlc` image and loads
// that image into `ctx` — the path `qlang_run app.ql lib.qlc` takes, minus the
// driver (which has no library-mode flag yet). Returns nullptr with the error
// text when any step fails.
std::shared_ptr<LoadedModule> addLibrary(QLangContext &ctx,
                                         const std::string &source,
                                         std::string &err) {
  CompiledProgram prog;
  std::map<std::string, int> nativeIds;
  if (!compile(ctx, source, prog, nativeIds, err, "", true)) return nullptr;
  std::ostringstream os(std::ios::binary);
  if (!writeQlc(os, prog, nativeIds)) {
    err = "library image could not be written";
    return nullptr;
  }
  std::istringstream is(os.str(), std::ios::binary);
  try {
    return ctx.loadProgram(is, "memory.qlc");
  } catch (const QLangError &e) {
    err = e.what();
    return nullptr;
  }
}

// Compiles `source` as the entry module against the libraries `ctx` already
// loaded, links, and reports the merged program. Returns false with the error
// text when the compile or the link throws.
bool buildLinked(QLangContext &ctx, const std::string &source,
                 CompiledProgram &merged, std::string &err,
                 const std::string &moduleName = "app",
                 CompiledProgram *entryOut = nullptr) {
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

// The library every behavioural test imports: two functions, a constant and a
// variable, and a top level that says so when it runs.
const char *kGreeter = R"(
export const MODULE_NAME = "greeter"
export const AUTHOR = "Louis"
export counter = 0
export def bump()
  global counter
  counter = counter + 1
  return counter
end
export def greet(name)
  return "hello " + name
end
println("greeter init")
)";

// Number of pool slots holding a string equal to `s`.
int poolCount(const CompiledProgram &prog, const std::string &s) {
  int n = 0;
  for (const QLangValue &v : prog.constants)
    if (v.type() == QLangValue::Type::String && v.asString() == s) n++;
  return n;
}

// The operand `b` of instruction `i` of function `f`.
int operandB(const Function &f, size_t i) {
  DecodedInstruction k;
  decodeInstruction(f.code.data(), f.instrOffsets[i], f.code.size(), k);
  return k.b;
}

// The first function index a CALLFN in `f` names, or -1.
int firstCallFnTarget(const Function &f) {
  for (size_t i = 0; i < f.instrOffsets.size(); i++) {
    DecodedInstruction k;
    decodeInstruction(f.code.data(), f.instrOffsets[i], f.code.size(), k);
    if (k.op == Opcode::CALLFN) return k.b;
  }
  return -1;
}

} // namespace

// ---------------------------------------------------------------------------
// The module registry: loadProgram / loadProgramFile / loadedModule(s)
// ---------------------------------------------------------------------------

static void testRegistry() {
  QLangContext ctx;
  std::string err;
  auto lib = addLibrary(ctx, kGreeter, err);
  if (!check(lib != nullptr, "registry: a library image loads (" + err + ")"))
    return;
  check(lib->program.moduleName == "greeter",
        "registry: keyed by its own MODULE_NAME");
  check(lib->path == "memory.qlc", "registry: remembers where it came from");
  check(ctx.loadedModule("greeter") == lib.get(),
        "registry: loadedModule finds it by name");
  check(ctx.loadedModule("nope") == nullptr,
        "registry: an unknown name resolves to nullptr");
  check(ctx.loadedModules().size() == 1, "registry: one module loaded");

  // The image is a module, not a runnable script: its top level is `<init>`
  // and it carries the export table `import` resolves against.
  check(!lib->program.functions.empty() &&
            lib->program.functions[0].name == "<init>",
        "registry: the loaded top level is <init>");
  check(lib->program.exports.size() == 5,
        "registry: the export table survived the image");
  check(lib->program.imports.empty(),
        "registry: a library imports nothing (Phase 3b)");

  // A second image under the same name is a duplicate, not an override: two
  // images behind one name would make `import greeter` ambiguous.
  check(addLibrary(ctx, kGreeter, err) == nullptr &&
            err.find("already loaded") != std::string::npos,
        "registry: a duplicate module name is rejected (" + err + ")");
  check(ctx.loadedModules().size() == 1,
        "registry: the rejected load changed nothing");

  ctx.clear();
  check(ctx.loadedModules().empty() && ctx.loadedModule("greeter") == nullptr,
        "registry: clear() drops the loaded modules");
}

// A `.qlc` on disk loads through the file entry point, and a missing or
// non-image file reports instead of crashing.
static void testLoadProgramFile() {
  QLangContext ctx;
  std::string err;
  CompiledProgram prog;
  std::map<std::string, int> ids;
  if (!compile(ctx, kGreeter, prog, ids, err, "", true)) {
    check(false, "loadProgramFile: library compiles (" + err + ")");
    return;
  }
  std::ostringstream os(std::ios::binary);
  if (!writeQlc(os, prog, ids)) {
    check(false, "loadProgramFile: image writes");
    return;
  }

  const std::filesystem::path dir = std::filesystem::temp_directory_path();
  const std::filesystem::path path = dir / "qlang_linker_greeter.qlc";
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << os.str();
  }
  try {
    auto lib = ctx.loadProgramFile(path.string());
    check(lib != nullptr && lib->program.moduleName == "greeter",
          "loadProgramFile: loads the image the driver would load");
    check(lib->path == path.string(),
          "loadProgramFile: the path is the file it came from");
  } catch (const QLangError &e) {
    check(false, std::string("loadProgramFile: loads (") + e.what() + ")");
  }
  try {
    ctx.loadProgramFile((dir / "qlang_linker_missing.qlc").string());
    check(false, "loadProgramFile: a missing file must throw");
  } catch (const QLangError &e) {
    check(std::string(e.what()).find("cannot open") != std::string::npos,
          std::string("loadProgramFile: a missing file says so (") + e.what() + ")");
  }
  // A file that is not an image (here: the source text) is a read error.
  const std::filesystem::path notImage = dir / "qlang_linker_source.qlc";
  {
    std::ofstream out(notImage, std::ios::binary | std::ios::trunc);
    out << kGreeter;
  }
  try {
    ctx.loadProgramFile(notImage.string());
    check(false, "loadProgramFile: a non-image file must throw");
  } catch (const QLangError &e) {
    check(true, std::string("loadProgramFile: a non-image is rejected (") + e.what() + ")");
  }
  std::error_code ec;
  std::filesystem::remove(path, ec);
  std::filesystem::remove(notImage, ec);
}

// An image stores native slots by NAME, so the context that loads it has to be
// able to resolve every one.
static void testNativeResolution() {
  const char *src = R"(
export const MODULE_NAME = "hosty"
export def twice(x)
  return dbl(x)
end
)";
  QLangContext with;
  with.setFunc("dbl", [](QLangContext &, const std::vector<QLangValue> &a) {
    return QLangValue::Int(a.empty() ? 0 : a[0].asInt() * 2);
  });
  // A second native so the missing-name report below has two names to list:
  // docs/SERIALIZATION.md §5.2 requires ONE error naming all of them, not
  // fail-on-first.
  with.setFunc("tri", [](QLangContext &, const std::vector<QLangValue> &a) {
    return QLangValue::Int(a.empty() ? 0 : a[0].asInt() * 3);
  });
  std::string err;
  auto lib = addLibrary(with, src, err);
  if (!check(lib != nullptr,
             "natives: a library with a native slot loads (" + err + ")"))
    return;
  // Every native the context offers gets a slot (the driver hands the compiler
  // the whole native set), so the check is that `dbl` is among them.
  int dblSlot = -1;
  for (const auto &[slot, name] : lib->nativeSlots)
    if (name == "dbl")
      dblSlot = slot;
  check(dblSlot >= 0, "natives: the slot table names the function");
  check(dblSlot >= 0 &&
            static_cast<size_t>(dblSlot) < lib->program.nativeFuncs.size() &&
            lib->program.nativeFuncs[static_cast<size_t>(dblSlot)] != nullptr,
        "natives: the slot is resolved against the context");

  CompiledProgram prog;
  std::map<std::string, int> ids;
  if (!compile(with, src, prog, ids, err, "", true)) return;
  std::ostringstream os(std::ios::binary);
  if (!writeQlc(os, prog, ids)) {
    check(false, "natives: image writes");
    return;
  }
  std::istringstream is(os.str(), std::ios::binary);
  QLangContext without;
  try {
    without.loadProgram(is, "hosty.qlc");
    check(false, "natives: an unresolvable slot name must throw");
  } catch (const QLangError &e) {
    const std::string msg = e.what();
    check(msg.find("dbl") != std::string::npos,
          std::string("natives: the error names the function (") + msg + ")");
    check(msg.find("tri") != std::string::npos,
          std::string("natives: one error names EVERY missing function (") + msg + ")");
  }
  check(without.loadedModules().empty(),
        "natives: a module that failed to load is not registered");
}

// ---------------------------------------------------------------------------
// The merge: one function table, one folded pool, one native table, disjoint
// register slices (SPEC §bytecodes "technical specifications" 1, 2; Q33)
// ---------------------------------------------------------------------------

// Its `TAG` value is the same literal the app below prints, so the merged pool
// has something to fold (Q33). Its top level says when it runs.
static const char *kTally = R"(
export const MODULE_NAME = "tally"
export const TAG = "shared literal"
export counter = 41
export def bump()
  global counter
  counter = counter + 1
  return counter
end
println("tally init")
)";

// The three cross-module reference kinds the linker rewrites: a const read, a
// function call, and a read of the variable that call changed.
static const char *kTallyApp = R"(
import tally
println("shared literal")
println(TAG)
println(bump())
println(counter)
)";

static const ExportEntry *findExport(const CompiledProgram &prog,
                                     const std::string &name) {
  for (const ExportEntry &e : prog.exports)
    if (e.name == name)
      return &e;
  return nullptr;
}

// The instruction index of the first CALLFN in `f` (and its operand), or -1.
static int firstCallFn(const Function &f, int *operand) {
  for (size_t i = 0; i < f.instrOffsets.size(); i++) {
    DecodedInstruction k;
    decodeInstruction(f.code.data(), f.instrOffsets[i], f.code.size(), k);
    if (k.op == Opcode::CALLFN) {
      if (operand)
        *operand = k.b;
      return static_cast<int>(i);
    }
  }
  return -1;
}

// Encoded size of instruction `i` of `f`.
static size_t instrSize(const Function &f, int i) {
  if (i < 0 || static_cast<size_t>(i) >= f.instrOffsets.size())
    return 0;
  if (static_cast<size_t>(i) + 1 < f.instrOffsets.size())
    return f.instrOffsets[i + 1] - f.instrOffsets[i];
  return f.code.size() - f.instrOffsets[i];
}

static void testMerge() {
  QLangContext ctx;
  ctx.setFunc("dbl", [](QLangContext &, const std::vector<QLangValue> &a) {
    return QLangValue::Int(a.empty() ? 0 : a[0].asInt() * 2);
  });
  std::string err;
  auto lib = addLibrary(ctx, kTally, err);
  if (!check(lib != nullptr, "merge: the library loads (" + err + ")")) return;
  CompiledProgram merged, entry;
  if (!check(buildLinked(ctx, kTallyApp, merged, err, "app", &entry),
             "merge: the app links against it (" + err + ")"))
    return;
  const CompiledProgram &image = lib->program;
  const int entryFuncs = static_cast<int>(entry.functions.size());
  const int entryWidth = std::max(entry.registerCount, 1);
  const int libWidth = std::max(image.registerCount, 1);

  // One function table (spec 2): the entry keeps 0..n, the library is appended
  // in the order its top level runs.
  check(merged.moduleName == "app", "merge: the merged module is the entry");
  check(merged.functions.size() ==
            static_cast<size_t>(entryFuncs + static_cast<int>(image.functions.size())),
        "merge: one function table, concatenated");
  check(merged.functions[entryFuncs].name == "<init>",
        "merge: the library's top level follows the entry's");

  // One global-register area split into disjoint per-module slices.
  check(merged.registerCount == entryWidth + libWidth,
        "merge: the register area spans every module slice");
  if (check(merged.initSequence.size() == 1, "merge: one module init recorded")) {
    check(merged.initSequence[0].funcIndex == entryFuncs,
          "merge: the init names the library's top level");
    check(merged.initSequence[0].regBase == entryWidth,
          "merge: the init's slice starts where the entry's ends");
    check(merged.initSequence[0].registerCount == libWidth,
          "merge: the init's slice is as wide as the library");
    check(merged.initSequence[0].module == "tally",
          "merge: the init records the module it belongs to");
  }

  // The compiler could not name any of this on its own: three references were
  // pending, and the link consumed them all.
  check(entry.imports.size() == 4,
        "merge: the app bound four names (MODULE_NAME, TAG, counter, bump)");
  check(entry.pendingLinks.size() == 3,
        "merge: three cross-module references were left pending");
  check(merged.pendingLinks.empty(),
        "merge: the link leaves no unresolved reference behind");

  // The merged import table describes the merged program: every ref is a merged
  // handle, a function index or an absolute cell.
  bool refsOk = true;
  for (const ImportEntry &im : merged.imports) {
    const ExportEntry *exp = findExport(image, im.name);
    if (!exp) {
      refsOk = false;
      break;
    }
    const int want = (im.kind == ExportKind::Func) ? entryFuncs + exp->ref
                                                   : entryWidth + exp->ref;
    if (im.ref != want) {
      refsOk = false;
      break;
    }
  }
  check(refsOk, "merge: the import table names merged handles");
  const ImportEntry *cell = nullptr;
  for (const ImportEntry &im : merged.imports)
    if (im.name == "counter")
      cell = &im;
  check(cell && cell->ref >= entryWidth && cell->ref < entryWidth + libWidth,
        "merge: an imported variable names a cell inside the defining slice");
  const ImportEntry *callee = nullptr;
  for (const ImportEntry &im : merged.imports)
    if (im.name == "bump")
      callee = &im;
  check(callee && callee->ref >= entryFuncs &&
            callee->ref < static_cast<int>(merged.functions.size()),
        "merge: an imported function names a slot inside the merged table");

  // One constant table (spec 1), folded through valueKey (Q33): the literal both
  // modules carry existed once per image and must exist once in the merge.
  check(poolCount(image, "shared literal") == 1 &&
            poolCount(entry, "shared literal") == 1,
        "merge: each image carried the shared literal once");
  check(poolCount(merged, "shared literal") == 1,
        "merge: the merged pool folds the shared literal (Q33)");
  std::unordered_set<std::string> keys;
  bool unique = true;
  for (const QLangValue &v : merged.constants)
    if (!keys.insert(valueKey(v)).second)
      unique = false;
  check(unique, "merge: the merged pool holds unique values");

  // Native slots concatenate but dedup by name, and both images were compiled
  // against the same context, so the library contributes no new slot.
  check(merged.nativeFuncs.size() == lib->nativeSlots.size(),
        "merge: the library's native slots dedup against the entry's");
  int dblSlot = -1;
  for (const auto &[slot, name] : lib->nativeSlots)
    if (name == "dbl")
      dblSlot = slot;
  check(dblSlot >= 0 &&
            static_cast<size_t>(dblSlot) < merged.nativeFuncs.size() &&
            merged.nativeFuncs[static_cast<size_t>(dblSlot)] != nullptr,
        "merge: a native both modules offer keeps one resolved slot");

  // End to end: a native the entry calls directly and the library calls from
  // inside a function both go through that one slot.
  QLangContext both;
  both.setFunc("dbl", [](QLangContext &, const std::vector<QLangValue> &a) {
    return QLangValue::Int(a.empty() ? 0 : a[0].asInt() * 2);
  });
  auto doubler = addLibrary(
      both,
      "export const MODULE_NAME = \"doubler\"\n"
      "export def twice(x)\n  return dbl(x)\nend\n",
      err);
  if (check(doubler != nullptr, "merge: the native library loads (" + err + ")")) {
    CompiledProgram nativeMerged, nativeEntry;
    if (check(buildLinked(both,
                          "import doubler\nprintln(dbl(3))\nprintln(twice(4))\n",
                          nativeMerged, err, "app", &nativeEntry),
              "merge: the native app links (" + err + ")"))
      check(runText(nativeMerged, both) == "6\n8\n",
            "merge: one native slot serves both modules");
  }
}

// ---------------------------------------------------------------------------
// The rewrites (Q32): a merged index that outgrows its operand is re-widened,
// never byte-patched
// ---------------------------------------------------------------------------

// `n` throwaway defs, enough to push the entry's own function table past the
// 1-byte operand range so a merged index into a small library needs a wider one.
static std::string manyDefs(int n) {
  std::string src;
  for (int i = 0; i < n; i++)
    src += "def h" + std::to_string(i) + "()\n  return " + std::to_string(i) +
           "\nend\n";
  return src;
}

static void testOperandRewidening() {
  QLangContext ctx;
  std::string err;
  const char *lib = R"(
export const MODULE_NAME = "wide"
export def answer()
  return 42
end
)";
  if (!addLibrary(ctx, lib, err)) {
    check(false, "rewiden: the library loads (" + err + ")");
    return;
  }
  const std::string app = manyDefs(300) + "\nimport wide\nprintln(answer())\n";
  CompiledProgram merged, entry;
  if (!check(buildLinked(ctx, app, merged, err, "wideapp", &entry),
             "rewiden: the app links (" + err + ")"))
    return;

  // Before the link the operand is the library's own index for `answer` (its
  // top level is 0, so 1) — small enough for a 1-byte operand.
  int beforeOperand = -1;
  const int at = firstCallFn(entry.functions[0], &beforeOperand);
  if (!check(at >= 0 && beforeOperand == 1,
             "rewiden: the call names the defining module's index"))
    return;
  const size_t before = instrSize(entry.functions[0], at);

  // After it is the merged index, past 255, in an instruction that grew.
  int afterOperand = -1;
  const int atMerged = firstCallFn(merged.functions[0], &afterOperand);
  const int want = static_cast<int>(entry.functions.size()) + 1;
  check(atMerged == at, "rewiden: the rewrite keeps the instruction count");
  check(afterOperand == want,
        "rewiden: the call names the merged function index");
  check(want > 255, "rewiden: the merged index really exceeds one byte");
  check(instrSize(merged.functions[0], atMerged) > before,
        "rewiden: the operand was re-widened, not patched");
  DecodedInstruction k;
  decodeInstruction(merged.functions[0].code.data(),
                    merged.functions[0].instrOffsets[atMerged],
                    merged.functions[0].code.size(), k);
  check(k.escaped && k.b == want,
        "rewiden: the re-encoded stream carries the wider operand");

  // And the merged program runs: the widened call lands on the library's
  // function, not on whatever else occupies the entry's table.
  check(runText(merged, ctx) == "42\n",
        "rewiden: the merged program calls across the boundary");
}

// ---------------------------------------------------------------------------
// The run (Q35) and the shared cell (Q30)
// ---------------------------------------------------------------------------

static const char *kInitA = R"(
export const MODULE_NAME = "inita"
export const FROM_A = "A"
println("init A")
)";

static const char *kInitB = R"(
export const MODULE_NAME = "initb"
export const FROM_B = "B"
println("init B")
)";

static void testInitOrder() {
  QLangContext ctx;
  std::string err;
  if (!addLibrary(ctx, kInitA, err) || !addLibrary(ctx, kInitB, err)) {
    check(false, "init: both libraries load (" + err + ")");
    return;
  }
  // Two modules, imported in this order: each top level runs once, before the
  // entry's, in the order the entry bound them.
  const std::string app =
      "from initb import FROM_B\nfrom inita import FROM_A\n"
      "println(\"entry\")\nprintln(FROM_B)\nprintln(FROM_A)\n";
  CompiledProgram merged, entry;
  if (!check(buildLinked(ctx, app, merged, err, "app", &entry),
             "init: the app links (" + err + ")"))
    return;
  check(merged.initSequence.size() == 2, "init: both modules recorded");
  check(merged.initSequence[0].module == "initb" &&
            merged.initSequence[1].module == "inita",
        "init: the sequence follows the entry's import order");
  check(runText(merged, ctx) == "init B\ninit A\nentry\nB\nA\n",
        "init: the top levels run before the entry's, in order");

  // The same module reached twice (a selective import, then a flat one) still
  // runs exactly once.
  QLangContext twice;
  if (!addLibrary(twice, kInitA, err)) {
    check(false, "init: the library loads (" + err + ")");
    return;
  }
  CompiledProgram once, entryOnce;
  if (!check(buildLinked(twice,
                         "from inita import FROM_A\nimport inita\n"
                         "println(FROM_A)\n",
                         once, err, "app", &entryOnce),
             "init: the double import links (" + err + ")"))
    return;
  check(once.initSequence.size() == 1,
        "init: a module imported twice is linked once");
  check(runText(once, twice) == "init A\nA\n",
        "init: the top level runs exactly once");
}

static void testSharedCell() {
  QLangContext ctx;
  std::string err;
  if (!addLibrary(ctx, kTally, err)) {
    check(false, "cell: the library loads (" + err + ")");
    return;
  }
  CompiledProgram merged, entry;
  if (!check(buildLinked(ctx, kTallyApp, merged, err, "app", &entry),
             "cell: the app links (" + err + ")"))
    return;
  // The library's init prints, the app prints its own literal and the library's
  // const, then the defining module's own function writes the shared cell and
  // the importer reads it back (spec 3 + Q30: one cell, the definer writes).
  check(runText(merged, ctx) ==
            "tally init\nshared literal\nshared literal\n42\n42\n",
        "cell: the importer reads what the definer wrote (Q30)");

  // A second call sees the second write: the cell is live, not a snapshot taken
  // when the import was bound.
  QLangContext again;
  if (!addLibrary(again, kTally, err)) {
    check(false, "cell: the library loads again (" + err + ")");
    return;
  }
  CompiledProgram more, entryMore;
  if (!check(buildLinked(again,
                         "import tally\nprintln(counter)\nbump()\nbump()\n"
                         "println(counter)\n",
                         more, err, "app", &entryMore),
             "cell: the second app links (" + err + ")"))
    return;
  check(runText(more, again) == "tally init\n41\n43\n",
        "cell: every read sees the current value of the cell");
}

// ---------------------------------------------------------------------------
// The boundaries: what the linker itself rejects
// ---------------------------------------------------------------------------

// A hand-built image with the minimum the linker's guards look at: a name, a
// top level, and whatever defect the caller adds.
static std::shared_ptr<LoadedModule> stubModule(const std::string &name) {
  auto m = std::make_shared<LoadedModule>();
  m->path = "stub";
  m->program.moduleName = name;
  Function top;
  top.name = "<init>";
  m->program.functions.push_back(top);
  return m;
}

static void testBoundaries() {
  QLangContext ctx;
  std::string err;
  if (!addLibrary(ctx, kTally, err)) {
    check(false, "boundary: the library loads (" + err + ")");
    return;
  }
  CompiledProgram merged, entry;
  if (!check(buildLinked(ctx, kTallyApp, merged, err, "app", &entry),
             "boundary: the app links (" + err + ")"))
    return;

  // A merged program is not a module: linking it again would hand every slice
  // to a second linker and double the register area.
  try {
    QLangLinker linker(ctx.loadedModules());
    linker.link(merged);
    check(false, "boundary: an already linked program must be rejected");
  } catch (const QLangError &e) {
    check(std::string(e.what()).find("already linked") != std::string::npos,
          std::string("boundary: a linked program says so (") + e.what() + ")");
  }

  // An import whose image was never loaded (finding it by name is C8's job).
  try {
    QLangLinker nobody({});
    nobody.link(entry);
    check(false, "boundary: an unloaded module must be rejected");
  } catch (const QLangError &e) {
    check(std::string(e.what()).find("tally") != std::string::npos &&
              std::string(e.what()).find("not loaded") != std::string::npos,
          std::string("boundary: an unloaded module names it (") + e.what() +
              ")");
  }

  // A library image that is not a module: no top level, unresolved references
  // of its own, or a dependency it cannot carry in an image.
  {
    auto m = stubModule("tally");
    m->program.functions.clear();
    try {
      QLangLinker l({m});
      l.link(entry);
      check(false, "boundary: a module without functions must be rejected");
    } catch (const QLangError &e) {
      check(std::string(e.what()).find("no functions") != std::string::npos,
            std::string("boundary: an empty module says so (") + e.what() + ")");
    }
  }
  {
    auto m = stubModule("tally");
    m->program.pendingLinks.push_back(PendingLink{});
    try {
      QLangLinker l({m});
      l.link(entry);
      check(false, "boundary: a half-linked module must be rejected");
    } catch (const QLangError &e) {
      check(std::string(e.what()).find("unresolved") != std::string::npos,
            std::string("boundary: a half-linked module says so (") +
                e.what() + ")");
    }
  }
  {
    auto m = stubModule("tally");
    m->program.imports.push_back(
        ImportEntry{"other", "x", ExportKind::Func, 0, ""});
    try {
      // `other` is loaded, so the graph resolves and the image itself is what
      // the linker refuses.
      QLangLinker l({m, stubModule("other")});
      l.link(entry);
      check(false, "boundary: a library with imports must be rejected");
    } catch (const QLangError &e) {
      check(std::string(e.what()).find("GETIMPORT") != std::string::npos,
            std::string("boundary: a library with imports says why (") +
                e.what() + ")");
    }
  }

  // An import graph that closes on itself: the loader-side half of the rule the
  // compiler also enforces.
  {
    auto a = stubModule("tally");
    a->program.imports.push_back(
        ImportEntry{"other", "x", ExportKind::Func, 0, ""});
    auto b = stubModule("other");
    b->program.imports.push_back(
        ImportEntry{"tally", "y", ExportKind::Func, 0, ""});
    try {
      QLangLinker l({a, b});
      l.link(entry);
      check(false, "boundary: an import cycle must be rejected");
    } catch (const QLangError &e) {
      check(std::string(e.what()).find("cycle") != std::string::npos,
            std::string("boundary: a cycle says so (") + e.what() + ")");
    }
  }

  // Neither link artifact belongs in an image: a merged program would lose the
  // init sequence, an unlinked one would run against the wrong cells.
  {
    std::ostringstream os(std::ios::binary);
    check(!writeQlc(os, merged, {}),
          "boundary: a merged program is not serializable");
    std::ostringstream os2(std::ios::binary);
    check(!writeQlc(os2, entry, {}),
          "boundary: a program with pending links is not serializable");
  }
}

int main() {
  testRegistry();
  testLoadProgramFile();
  testNativeResolution();
  testMerge();
  testOperandRewidening();
  testInitOrder();
  testSharedCell();
  testBoundaries();

  std::cout << (fails == 0 ? "ALL PASS" : "FAILURES") << ": " << passes
            << " passed, " << fails << " failed\n";
  return fails == 0 ? 0 : 1;
}
