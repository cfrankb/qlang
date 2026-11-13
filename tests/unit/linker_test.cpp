// The loader and the linker (SPEC §bytecodes "external modules", "technical
// specifications" 1–5; plan docs/plans/archive/external-modules-plan-v4.md Phases
// 3b/3c).
//
// A library is a `.qlc` image with an export table. `QLangContext::loadProgram`
// reads one into the context's module registry (keyed by the name `import`
// binds it under), the compiler resolves `import` against that table into
// `getimport`/`callimport` operands, and `QLangLinker` merges the images with
// the entry program into one runnable program. This suite pins —
//
//   * the registry: load by stream and by file, lookup by name, load order,
//     a duplicate module name, an unresolvable native slot, `clear()`;
//   * the merge: one function table (concatenated), one constant pool folded
//     through `valueKey` (Q33), one native-slot table deduped by name, one
//     global-register area split into disjoint per-module slices;
//   * the import table (Phase 3c): one merged table deduped by (module, name),
//     so a library may itself import a library and two importers of one export
//     read one cell; `callimport` is resolved into `callfn`/`makegen` and
//     leaves no trace, while `getimport` survives reading the merged cell;
//   * the rewrites (Q32): every operand is rewritten on the decoded
//     `RawInstr` and re-encoded, so a merged index that needs a wider operand
//     is re-widened rather than byte-patched — the 300-def entry below exists
//     to push a merged function index past the 1-byte operand range;
//   * the run (Q35): each imported module's `<init>` executes exactly once,
//     dependencies first, before the entry module's, and an exported variable
//     is one shared cell the defining module writes and importers read (Q30);
//   * the unlinked form: a program whose `getimport`/`callimport` the linker
//     never resolved still runs on the VM, which is what a future runtime
//     registry will hand over (plan Phase 3c);
//   * the boundaries: a program that is already linked, a module that is not
//     loaded, a reference that names nothing, and a linked program handed to
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

// Compile `source` exactly as `qlcc` does (external consts/funcs/modules
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
// that image into `ctx` — the path `qlcc app.ql lib.qlc` takes, minus the
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

// The error text compiling `source` as the entry module produced ("" when it
// compiled). The importer's rules are compile-time ones, so no link is
// attempted here.
std::string entryError(QLangContext &ctx, const std::string &source) {
  CompiledProgram prog;
  std::map<std::string, int> ids;
  std::string err;
  return compile(ctx, source, prog, ids, err, "app") ? "" : err;
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
        "registry: a library that imports nothing has an empty table");

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
// able to resolve every one — and the names it stores are the calls its code
// makes, not the registry its build host happened to have (§5.2).
static void testNativeResolution() {
  // `tri` exists only to give the missing-name report below two names: the source
  // calls it, so it is a real load requirement. docs/SERIALIZATION.md §5.2 requires
  // ONE error naming all of them, not fail-on-first.
  const char *src = R"(
export const MODULE_NAME = "hosty"
export def twice(x)
  return dbl(x) + tri(x)
end
)";
  QLangContext with;
  with.setFunc("dbl", [](QLangContext &, const std::vector<QLangValue> &a) {
    return QLangValue::Int(a.empty() ? 0 : a[0].asInt() * 2);
  });
  with.setFunc("tri", [](QLangContext &, const std::vector<QLangValue> &a) {
    return QLangValue::Int(a.empty() ? 0 : a[0].asInt() * 3);
  });
  std::string err;
  auto lib = addLibrary(with, src, err);
  if (!check(lib != nullptr,
             "natives: a library with a native slot loads (" + err + ")"))
    return;
  // The natives the library calls are named; a native it never calls is not, so
  // a host without it could still load this image.
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
println(tally.TAG)
println(tally.bump())
println(tally.counter)
)";

static const ExportEntry *findExport(const CompiledProgram &prog,
                                     const std::string &name) {
  for (const ExportEntry &e : prog.exports)
    if (e.name == name)
      return &e;
  return nullptr;
}

// The instruction index of the first instruction of opcode `op` in `f` (and its
// operand `b`), or -1.
static int firstInstr(const Function &f, Opcode op, int *operand) {
  for (size_t i = 0; i < f.instrOffsets.size(); i++) {
    DecodedInstruction k;
    decodeInstruction(f.code.data(), f.instrOffsets[i], f.code.size(), k);
    if (k.op == op) {
      if (operand)
        *operand = k.b;
      return static_cast<int>(i);
    }
  }
  return -1;
}

// True when any function of `prog` holds an instruction of opcode `op`.
static bool programHasOpcode(const CompiledProgram &prog, Opcode op) {
  for (const Function &f : prog.functions)
    if (firstInstr(f, op, nullptr) >= 0)
      return true;
  return false;
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

  // The compiler could not name any of this on its own: it emitted real
  // instructions against its OWN import table, and the link renumbered them.
  check(entry.imports.size() == 4,
        "merge: the app bound four names (MODULE_NAME, TAG, counter, bump)");
  check(programHasOpcode(entry, Opcode::CALLIMPORT) &&
            programHasOpcode(entry, Opcode::GETIMPORT),
        "merge: the app's cross-module reads are getimport/callimport");

  // A call cannot survive: the merged program holds the function, so the call
  // names it directly. A cell read does survive — the importer may not address
  // another module's register directly (spec 3), so it keeps reading the cell
  // through the merged table.
  check(!programHasOpcode(merged, Opcode::CALLIMPORT),
        "merge: no callimport survives the link (Phase 3c)");
  check(programHasOpcode(merged, Opcode::GETIMPORT),
        "merge: getimport survives, reading the merged import table");
  check(firstInstr(merged.functions[0], Opcode::CALLFN, nullptr) >= 0,
        "merge: the cross-module call became a callfn");
  check(merged.imports.size() == entry.imports.size(),
        "merge: one merged import table, one entry per bound name");

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
  // against the same context. Neither kTally nor the app calls a native, so neither
  // names one: the table is the natives the code calls, not the context the compile
  // ran in (docs/SERIALIZATION.md §5.2, plan native-slot-portability P1). The gate
  // below is the case where both images really do call one.
  check(lib->nativeSlots.empty() && entry.nativeFuncs.empty() &&
            merged.nativeFuncs.empty(),
        "merge: an image names only the natives its code calls");

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
                          "import doubler\nprintln(dbl(3))\nprintln(doubler.twice(4))\n",
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
  const std::string app = manyDefs(300) + "\nimport wide\nprintln(wide.answer())\n";
  CompiledProgram merged, entry;
  if (!check(buildLinked(ctx, app, merged, err, "wideapp", &entry),
             "rewiden: the app links (" + err + ")"))
    return;

  // Before the link the call is a `callimport` naming the app's OWN import slot
  // for `answer` — a small number, so a 1-byte operand.
  int beforeOperand = -1;
  const int at =
      firstInstr(entry.functions[0], Opcode::CALLIMPORT, &beforeOperand);
  const bool ownSlot =
      at >= 0 && beforeOperand >= 0 &&
      static_cast<size_t>(beforeOperand) < entry.imports.size() &&
      entry.imports[static_cast<size_t>(beforeOperand)].name == "answer";
  if (!check(ownSlot, "rewiden: the call names the app's own import slot"))
    return;
  const size_t before = instrSize(entry.functions[0], at);

  // After it is a `callfn` naming the merged index, past 255, in an instruction
  // that grew.
  int afterOperand = -1;
  const int atMerged =
      firstInstr(merged.functions[0], Opcode::CALLFN, &afterOperand);
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
                         "import tally\nprintln(tally.counter)\n"
                         "tally.bump()\ntally.bump()\nprintln(tally.counter)\n",
                         more, err, "app", &entryMore),
             "cell: the second app links (" + err + ")"))
    return;
  check(runText(more, again) == "tally init\n41\n43\n",
        "cell: every read sees the current value of the cell");
}

// ---------------------------------------------------------------------------
// A library that imports a library (plan Phase 3c): an image carries
// `getimport`/`callimport` operands into its OWN import table, and the merged
// program has one table for everyone
// ---------------------------------------------------------------------------

// The leaf: one variable and the only function allowed to write it.
static const char *kLeaf = R"(
export const MODULE_NAME = "leaf"
export counter = 41
export def bump()
  global counter
  counter = counter + 1
  return counter
end
println("leaf init")
)";

// The middle: imports the leaf and calls through it, so its image is the first
// one this suite writes that carries an import table of its own.
static const char *kMiddle = R"(
import leaf
export const MODULE_NAME = "middle"
export def tick()
  return leaf.bump() + 100
end
export def peek()
  return leaf.counter
end
println("middle init")
)";

static void testLibraryChain() {
  QLangContext ctx;
  std::string err;
  if (!addLibrary(ctx, kLeaf, err)) {
    check(false, "chain: the leaf loads (" + err + ")");
    return;
  }
  auto middle = addLibrary(ctx, kMiddle, err);
  if (!check(middle != nullptr,
             "chain: a library that imports one loads (" + err + ")"))
    return;
  check(middle->program.imports.size() == 3,
        "chain: the image carries its own import table (Phase 3c)");

  // The app imports the middle selectively and the leaf's cell directly, so
  // `counter` is bound by two images and has to land on one merged cell. (A
  // flat `import` of both would collide on MODULE_NAME, which is a compile
  // error by design.)
  CompiledProgram merged, entry;
  const bool linked =
      buildLinked(ctx,
                  "from middle import tick, peek\nfrom leaf import counter\n"
                  "println(tick())\nprintln(peek())\nprintln(counter)\n",
                  merged, err, "app", &entry);
  if (!check(linked, "chain: the app links against both (" + err + ")"))
    return;

  // Dependencies first: the leaf's top level runs before the middle's (Q35).
  check(merged.initSequence.size() == 2 &&
            merged.initSequence[0].module == "leaf" &&
            merged.initSequence[1].module == "middle",
        "chain: a transitive dependency inits first");
  check(runText(merged, ctx) == "leaf init\nmiddle init\n142\n42\n42\n",
        "chain: a library calls through its own import table");

  // One merged table deduped by (module, name): the app bound three names and
  // the middle three, but leaf's counter/bump/MODULE_NAME are the same three
  // entities, so five entries describe all six bindings.
  check(entry.imports.size() == 3, "chain: the app bound three names");
  check(merged.imports.size() == 5,
        "chain: two importers of one export share one entry");
  int counterEntries = 0;
  for (const ImportEntry &im : merged.imports)
    if (im.name == "counter")
      counterEntries++;
  check(counterEntries == 1, "chain: the shared cell is one table entry");

  // The middle's own cross-module references resolved too: its `callimport`
  // became a call of the merged table's function.
  check(!programHasOpcode(merged, Opcode::CALLIMPORT) &&
            programHasOpcode(merged, Opcode::GETIMPORT),
        "chain: the library's own references resolved as well");
  const ImportEntry *cell = nullptr;
  for (const ImportEntry &im : merged.imports)
    if (im.name == "counter")
      cell = &im;
  check(cell && cell->ref >= 0 &&
            cell->ref < static_cast<int>(merged.registerCount),
        "chain: the shared entry names a merged cell");
}

// ---------------------------------------------------------------------------
// The unlinked form (plan Phase 3c): both opcodes are VM instructions, so a
// program whose import table already holds merged handles runs with no link —
// the shape a future runtime registry hands over
// ---------------------------------------------------------------------------

static void testUnlinkedOpcodes() {
  QLangContext ctx;
  std::string err;
  if (!addLibrary(ctx, kTally, err)) {
    check(false, "unlinked: the library loads (" + err + ")");
    return;
  }
  CompiledProgram merged, entry;
  const bool linked = buildLinked(ctx, kTallyApp, merged, err, "app", &entry);
  if (!check(linked, "unlinked: the app links (" + err + ")"))
    return;

  // Splice a `callimport` back over the `callfn` the link produced, naming an
  // import entry whose `ref` is already a merged function index — what a module
  // image's table looks like once its dependency is in the same program.
  CompiledProgram prog = merged;
  const ImportEntry *bump = nullptr;
  for (const ImportEntry &im : prog.imports)
    if (im.name == "bump")
      bump = &im;
  if (!check(bump != nullptr, "unlinked: the merged table names the callee"))
    return;
  const ImportEntry direct = *bump;
  const int calleeSlot = static_cast<int>(prog.imports.size());
  prog.imports.push_back(direct);

  Function &top = prog.functions[0];
  std::vector<RawInstr> raw;
  decodeRawStream(top, raw);
  int callAt = -1, getAt = -1;
  for (size_t i = 0; i < raw.size(); i++) {
    if (raw[i].op == Opcode::CALLFN && callAt < 0)
      callAt = static_cast<int>(i);
    if (raw[i].op == Opcode::GETIMPORT && getAt < 0)
      getAt = static_cast<int>(i);
  }
  if (!check(callAt >= 0 && getAt >= 0,
             "unlinked: the top level calls and reads across"))
    return;
  raw[static_cast<size_t>(callAt)].op = Opcode::CALLIMPORT;
  raw[static_cast<size_t>(callAt)].b = calleeSlot;
  encodeRawStream(top, raw);
  check(runText(prog, ctx) ==
            "tally init\nshared literal\nshared literal\n42\n42\n",
        "unlinked: the VM resolves the call through the import table");

  // Rewriting the surviving `getimport` into a `callimport` of the same cell
  // proves the two really do read the same table.
  CompiledProgram readCall = prog;
  {
    std::vector<RawInstr> r;
    decodeRawStream(readCall.functions[0], r);
    r[static_cast<size_t>(getAt)].op = Opcode::CALLIMPORT;
    encodeRawStream(readCall.functions[0], r);
  }
  check(runError(readCall, ctx).find("is not a function") != std::string::npos,
        "unlinked: callimport of a variable says so");

  // And the guards are the VM's own, because nothing checked them earlier: an
  // operand outside the table, and a read of a function entry.
  CompiledProgram badIndex = prog;
  {
    std::vector<RawInstr> r;
    decodeRawStream(badIndex.functions[0], r);
    r[static_cast<size_t>(callAt)].b =
        static_cast<int>(badIndex.imports.size()) + 5;
    encodeRawStream(badIndex.functions[0], r);
  }
  check(runError(badIndex, ctx).find("outside the program's import table") !=
            std::string::npos,
        "unlinked: an operand outside the import table is caught");

  CompiledProgram badRead = prog;
  {
    std::vector<RawInstr> r;
    decodeRawStream(badRead.functions[0], r);
    r[static_cast<size_t>(getAt)].b = calleeSlot;
    encodeRawStream(badRead.functions[0], r);
  }
  check(runError(badRead, ctx).find("is a function, not a value") !=
            std::string::npos,
        "unlinked: getimport of a function says so");
}

// ---------------------------------------------------------------------------
// The importer's rules (plan Phase 3c): an imported name is read-only where it
// is used, and since Phase 3c it is reachable from any depth, so the guards have
// to fire where the reads now are
// ---------------------------------------------------------------------------

static void testImportGuards() {
  QLangContext ctx;
  std::string err;
  if (!addLibrary(ctx, kTally, err) || !addLibrary(ctx, kGreeter, err)) {
    check(false, "guards: both libraries load (" + err + ")");
    return;
  }

  // `import tally` binds `tally` and nothing else, so the bare names are not in
  // scope at all: the module's entities are reachable only through the
  // qualifier (SPEC §modules "import statement").
  check(entryError(ctx, "import tally\nbump()\n")
                .find("unknown function 'bump'") != std::string::npos,
        "guards: a module import leaves the bare function name unbound");
  check(entryError(ctx, "import tally\nprintln(counter)\n")
                .find("undeclared variable 'counter'") != std::string::npos,
        "guards: a module import leaves the bare variable name unbound");
  check(entryError(ctx, "import tally\ncounter = 1\nprintln(counter)\n")
                .empty(),
        "guards: a bare name a module also has is the importer's own variable");

  // The defining module owns what the qualifier reaches: an importer reads and
  // calls it, it never writes it (SPEC §bytecodes "technical specifications" 3,
  // Q30).
  check(entryError(ctx, "import tally\ntally.counter = 1\n")
                .find("cannot assign to module member 'tally.counter'") !=
            std::string::npos,
        "guards: assigning through a module qualifier is refused");
  check(entryError(ctx, "import tally\ntally.counter += 1\n")
                .find("cannot assign to module member 'tally.counter'") !=
            std::string::npos,
        "guards: compound assignment through a qualifier is refused the same way");
  check(entryError(ctx, "from tally import counter\ncounter++\n")
                .find("cannot increment/decrement imported name 'counter'") !=
            std::string::npos,
        "guards: incrementing a flat import is refused");
  // A flat import keeps the guard it always had: the name is read-only.
  check(entryError(ctx, "from tally import counter\ncounter = 1\n")
                .find("cannot assign to imported name 'counter'") !=
            std::string::npos,
        "guards: a flat import is read-only too");
  check(entryError(ctx, "from tally import counter\ndelete counter\n")
                .find("cannot delete imported name 'counter'") !=
            std::string::npos,
        "guards: deleting a flat import is refused");

  // A function export is owned the same way: the table holds its index, so a
  // rebind would leave the table naming one function while the name gave
  // another (SPEC §bytecodes "keyword export": exported functions are not
  // modifiable).
  check(entryError(ctx, "import tally\ntally.bump = 1\n")
                .find("cannot assign to module member 'tally.bump'") !=
            std::string::npos,
        "guards: rebinding a module's function is refused");
  // A function is not a value, so reading it as one says what to do instead.
  check(entryError(ctx, "import tally\nprintln(tally.bump)\n")
                .find("not a first-class function value") != std::string::npos,
        "guards: reading a module's function as a value is refused");

  // None of this was top-level-only: a qualifier resolves from a function body,
  // which is where the guards have to fire (and where the read has to work).
  check(entryError(ctx, "import tally\ndef f()\n  tally.counter = 1\nend\nf()\n")
                .find("cannot assign to module member") != std::string::npos,
        "guards: a write inside a function body is refused too");
  check(entryError(ctx,
                   "import tally\ndef f()\n  return tally.counter\nend\nprintln(f())\n")
                .empty(),
        "guards: reading a module member inside a body is legal (3c)");

  // What the table cannot answer is refused while compiling rather than at the
  // first call.
  check(entryError(ctx, "from tally import nope\n")
                .find("does not export 'nope'") != std::string::npos,
        "guards: an export the module does not have is refused");
  check(entryError(ctx,
                   "from tally import counter\nfrom greeter import counter\n")
                .find("is already imported from module 'tally'") !=
            std::string::npos,
        "guards: two modules claiming one name is refused");
  // `import` binds names into the module's scope, so it has no meaning inside a
  // function body; dropping it there silently left the name unbound and the
  // call failed at run time instead.
  check(entryError(ctx, "def f()\n  import tally\n  return 1\nend\n")
                .find("must appear at module top level") != std::string::npos,
        "guards: an import inside a function body is refused");
}

// ---------------------------------------------------------------------------
// A closure across the boundary (plan Phase 3c): the value is created in module
// A, reads module A's live cell, and module B calls it from inside one of its
// own functions
// ---------------------------------------------------------------------------

// `adder` is a closure value, not a `def`, so it is exported as a variable: the
// importer reads the cell and calls what it holds.
static const char *kFactory = R"(
export const MODULE_NAME = "factory"
export counter = 5
export adder = (k)[counter] { return counter + k }
counter = 40
)";

static void testClosureAcrossBoundary() {
  QLangContext ctx;
  std::string err;
  if (!addLibrary(ctx, kFactory, err)) {
    check(false, "closure: the library loads (" + err + ")");
    return;
  }
  CompiledProgram merged, entry;
  const std::string app =
      "from factory import adder\n"
      "def useIt(k)\n  return adder(k)\nend\n"
      "println(adder(1))\nprintln(useIt(2))\n";
  const bool linked = buildLinked(ctx, app, merged, err, "app", &entry);
  if (!check(linked, "closure: the app links (" + err + ")")) return;

  // 41 and 42: the closure reads the cell as the defining module left it, from
  // a frame that is not the one that created it and, in the second case, from
  // the importing module's own function.
  check(runText(merged, ctx) == "41\n42\n",
        "closure: a module's closure keeps its own cell across the boundary");
  // The read is a table read rather than a register the link copied: the app
  // reads the cell and calls what it holds, at both depths.
  check(programHasOpcode(entry, Opcode::GETIMPORT) &&
            programHasOpcode(entry, Opcode::CALLCLOS),
        "closure: the app reads the cell and calls what it holds");
  check(!programHasOpcode(merged, Opcode::CALLIMPORT),
        "closure: the link leaves no unresolved call behind");
}

// ---------------------------------------------------------------------------
// A generator across the boundary (plan Phase 3c): the linker resolves the
// `callimport` into a `makegen` whose closure names the MERGED function, and the
// VM's own `callimport` builds the same value with no link at all
// ---------------------------------------------------------------------------

// The generator reads its own module's exported variable, so its capture is an
// absolute table slot — the only kind a cross-module call can seed (a capture
// named by a register belongs to the defining module's frame, which the importer
// has no access to).
static const char *kSeries = R"(
export const MODULE_NAME = "series"
export base = 10
export def steps(n)
  global base
  for i = 1 to n { yield base + i }
end
)";

static void testGeneratorAcrossBoundary() {
  QLangContext ctx;
  std::string err;
  if (!addLibrary(ctx, kSeries, err)) {
    check(false, "gen: the library loads (" + err + ")");
    return;
  }
  const std::string app =
      "from series import steps\n"
      "local g = steps(2)\n"
      "local a = g()\nlocal b = g()\nlocal c = g()\n"
      "println(a + b)\n"
      "if c == nil { println(\"exhausted\") } else { println(c) }\n";
  CompiledProgram merged, entry;
  const bool linked = buildLinked(ctx, app, merged, err, "app", &entry);
  if (!check(linked, "gen: the app links (" + err + ")"))
    return;

  // The index the library's generator has in the merged function table.
  int want = -1;
  for (size_t i = 0; i < merged.functions.size(); i++)
    if (merged.functions[i].name == "steps")
      want = static_cast<int>(i);
  if (!check(want > 0, "gen: the merged function table holds the generator"))
    return;

  // The app called an import; the merged program builds the generator the way
  // the defining module does, from a pool closure naming the merged function.
  check(programHasOpcode(entry, Opcode::CALLIMPORT),
        "gen: the app calls the imported generator");
  int spec = -1;
  const int at = firstInstr(merged.functions[0], Opcode::MAKEGEN, &spec);
  if (!check(!programHasOpcode(merged, Opcode::CALLIMPORT) && at >= 0,
             "gen: the link resolved the call into a makegen"))
    return;
  if (!check(spec >= 0 &&
             static_cast<size_t>(spec) < merged.constants.size(),
             "gen: the makegen names a pool entry"))
    return;
  const QLangValue &cv = merged.constants[static_cast<size_t>(spec)];
  check(cv.type() == QLangValue::Type::Function &&
            cv.asFunction().funcIndex == want,
        "gen: the closure names the merged function, not the library's index");

  // The fold (Q33): the defining module's own generator constant is the SAME
  // value the link synthesized, because the captures were rebased before the
  // pool merged — so exactly one slot holds the function's own capture spec.
  // The module's def-time `makefunc` spec names the SAME absolute cell
  // sentinel (F1 fix: one identity for a module-level const), so it folds
  // into that one slot too — before the fix it named a frame register and
  // correctly stayed a slot of its own, which was the defect.
  const Function &genFn = merged.functions[static_cast<size_t>(want)];
  auto sameSpec = [&genFn](const Closure &cl) {
    if (cl.captures.size() != genFn.captures.size())
      return false;
    for (size_t i = 0; i < cl.captures.size(); i++)
      if (cl.captures[i].reg != genFn.captures[i].reg)
        return false;
    return true;
  };
  int folded = 0, specs = 0;
  for (const QLangValue &v : merged.constants) {
    if (v.type() != QLangValue::Type::Function ||
        v.asFunction().funcIndex != want)
      continue;
    specs++;
    if (sameSpec(v.asFunction()))
      folded++;
  }
  check(folded == 1, "gen: the synthesized closure folds with the module's");
  check(specs == 1,
        "gen: the def-time capture spec folds with the module's too");

  // 11 + 12 and then nil: the generator ran against the defining module's cell
  // and terminated on its own.
  check(runText(merged, ctx) == "23\nexhausted\n",
        "gen: the merged program yields the defining module's values");

  // The unlinked form: the same program with the `makegen` spliced back into the
  // `callimport` the compiler emitted, naming an import entry whose `ref` is
  // already a merged function index — what a registry hands the VM directly.
  CompiledProgram prog = merged;
  const ImportEntry *steps = nullptr;
  for (const ImportEntry &im : prog.imports)
    if (im.name == "steps")
      steps = &im;
  if (!check(steps != nullptr && steps->kind == ExportKind::Func &&
                 steps->ref == want,
             "gen: the merged table names the generator"))
    return;
  const int slot = static_cast<int>(prog.imports.size());
  prog.imports.push_back(*steps);
  {
    std::vector<RawInstr> raw;
    decodeRawStream(prog.functions[0], raw);
    // The operand changes meaning with the opcode: a pool index becomes an
    // import-table index, which the modifier byte has to announce.
    raw[static_cast<size_t>(at)].op = Opcode::CALLIMPORT;
    raw[static_cast<size_t>(at)].kind = RightKind::Register;
    raw[static_cast<size_t>(at)].b = slot;
    encodeRawStream(prog.functions[0], raw);
  }
  check(runText(prog, ctx) == "23\nexhausted\n",
        "gen: the VM builds the generator from the import table");
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
  // A library whose OWN import names a module nobody loaded. Since Phase 3c an
  // import table in an image is legal, so what fails is the graph, not the
  // table.
  {
    auto m = stubModule("tally");
    m->program.imports.push_back(
        ImportEntry{"other", "x", ExportKind::Func, 0});
    try {
      QLangLinker l({m});
      l.link(entry);
      check(false,
            "boundary: a library importing an unloaded module must be rejected");
    } catch (const QLangError &e) {
      check(std::string(e.what()).find("other") != std::string::npos &&
                std::string(e.what()).find("not loaded") != std::string::npos,
            std::string("boundary: it names the missing dependency (") +
                e.what() + ")");
    }
  }

  // A `callimport` whose entry names a variable. The compiler cannot produce
  // one, so the linker's own check is the last line of defence.
  {
    CompiledProgram bad = entry;
    for (ImportEntry &im : bad.imports)
      if (im.name == "bump")
        im.kind = ExportKind::Var;
    try {
      QLangLinker l(ctx.loadedModules());
      l.link(bad);
      check(false, "boundary: a callimport of a variable must be rejected");
    } catch (const QLangError &e) {
      check(std::string(e.what()).find("not a function") != std::string::npos,
            std::string("boundary: a callimport of a variable says so (") +
                e.what() + ")");
    }
  }

  // An import `ref` naming something the defining module does not have. The
  // reader refuses an out-of-range EXPORT ref and the compiler copies refs out
  // of that table, so an image cannot carry this — but rebasing it blindly
  // would land the cell in the next module's slice, which is the one failure
  // mode that would run silently instead of failing.
  {
    CompiledProgram bad = entry;
    for (ImportEntry &im : bad.imports)
      if (im.name == "counter")
        im.ref = 999;
    try {
      QLangLinker l(ctx.loadedModules());
      l.link(bad);
      check(false,
            "boundary: an import ref outside its module must be rejected");
    } catch (const QLangError &e) {
      check(std::string(e.what()).find("outside the module") !=
                std::string::npos,
            std::string("boundary: an import ref outside its module says so (") +
                e.what() + ")");
    }
  }

  // An import operand naming a slot the module's own table does not have.
  {
    CompiledProgram bad = entry;
    bad.imports.clear();
    try {
      QLangLinker l(ctx.loadedModules());
      l.link(bad);
      check(false,
            "boundary: an import operand outside the table must be rejected");
    } catch (const QLangError &e) {
      check(std::string(e.what()).find("outside the module's import table") !=
                std::string::npos,
            std::string("boundary: an out-of-range import operand says so (") +
                e.what() + ")");
    }
  }

  // An import graph that closes on itself: the loader-side half of the rule the
  // compiler also enforces.
  {
    auto a = stubModule("tally");
    a->program.imports.push_back(
        ImportEntry{"other", "x", ExportKind::Func, 0});
    auto b = stubModule("other");
    b->program.imports.push_back(
        ImportEntry{"tally", "y", ExportKind::Func, 0});
    try {
      QLangLinker l({a, b});
      l.link(entry);
      check(false, "boundary: an import cycle must be rejected");
    } catch (const QLangError &e) {
      check(std::string(e.what()).find("cycle") != std::string::npos,
            std::string("boundary: a cycle says so (") + e.what() + ")");
    }
  }

  // A merged program is not an image: v9 has no section for the init sequence,
  // and writing one out would silently drop every module's top level.
  {
    std::ostringstream os(std::ios::binary);
    check(!writeQlc(os, merged, {}),
          "boundary: a merged program is not serializable");
  }

  // An UNLINKED program with imports is an image (plan Phase 3c): its import
  // table plus `getimport`/`callimport` are exactly what v9 carries for a
  // dependency, so both survive the round trip.
  {
    std::ostringstream os(std::ios::binary);
    if (!check(writeQlc(os, entry, {}),
               "boundary: a program with imports is serializable"))
      return;
    std::istringstream is(os.str(), std::ios::binary);
    CompiledProgram back;
    std::vector<std::pair<int, std::string>> slots;
    std::string rerr;
    if (!check(readQlc(is, back, slots, rerr),
               "boundary: the image with imports reads back (" + rerr + ")"))
      return;
    check(back.imports.size() == entry.imports.size(),
          "boundary: the import table survived the image");
    check(programHasOpcode(back, Opcode::CALLIMPORT) &&
              programHasOpcode(back, Opcode::GETIMPORT),
          "boundary: both opcodes survived the image");
  }
}

// ---------------------------------------------------------------------------
// F1 (module-export/import plan P0): a module-level const read by a `def` of
// the same module has ONE identity — the `<init>` store, the def's capture
// read, the export reference and the importer's `getimport` all name it. The
// defect: the def-time capture spec named a frame-relative register, so
// MAKEFUNC bound a frame-local cell and releasing the module-init frame moved
// the value out of the table, leaving the importers reading an emptied cell.
// ---------------------------------------------------------------------------

static const char *kConsts = R"(
export const MODULE_NAME = "consts"
export const TAG = "tagged"
export def tag()
  return "fn:" + TAG
end
)";

static void testModuleConstOneIdentity() {
  QLangContext ctx;
  std::string err;
  auto lib = addLibrary(ctx, kConsts, err);
  if (!check(lib != nullptr, "const: the library loads (" + err + ")")) return;
  const std::string app =
      "from consts import TAG\n"
      "from consts import tag\n"
      "println(TAG)\nprintln(tag())\nprintln(TAG)\n";
  CompiledProgram merged, entry;
  if (!check(buildLinked(ctx, app, merged, err, "app", &entry),
             "const: the app links (" + err + ")"))
    return;

  // Every reader prints the value — including the cell read AFTER the def
  // ran, the F1 signature (the release used to empty the cell).
  check(runText(merged, ctx) == "tagged\nfn:tagged\ntagged\n",
        "const: the def and the importer read the one cell, before and after");

  // The identity is structural, not just behavioral: the cell the init frame
  // stores, the export's ref, the importer's cell and the def's capture are
  // the same slot.
  const ModuleInit *mi = nullptr;
  for (const ModuleInit &m : merged.initSequence)
    if (m.module == "consts") mi = &m;
  if (!check(mi != nullptr, "const: the init sequence records the module"))
    return;
  int tagConst = -1;
  for (size_t ci = 0; ci < merged.constants.size(); ci++)
    if (merged.constants[ci].type() == QLangValue::Type::String &&
        merged.constants[ci].asString() == "tagged")
      tagConst = static_cast<int>(ci);
  const Function &init = merged.functions[static_cast<size_t>(mi->funcIndex)];
  int storeReg = -1;
  for (size_t k = 0; k < init.instrOffsets.size(); k++) {
    DecodedInstruction d;
    decodeInstruction(init.code.data(), init.instrOffsets[k], init.code.size(),
                      d);
    if (d.op == Opcode::MOV && d.b == tagConst) storeReg = d.a;
  }
  if (!check(storeReg >= 0, "const: the init stores the const value")) return;
  const int slot = mi->regBase + storeReg;

  const ExportEntry *exp = findExport(lib->program, "TAG");
  check(exp && exp->ref == storeReg,
        "const: the export names the cell the init wrote");
  const ImportEntry *cell = nullptr;
  for (const ImportEntry &im : merged.imports)
    if (im.name == "TAG") cell = &im;
  check(cell && cell->ref == slot,
        "const: the importer's cell is the one the init wrote");
  const Function *tagFn = nullptr;
  for (const Function &f : merged.functions)
    if (f.name == "tag" && !f.captures.empty()) tagFn = &f;
  check(tagFn && tagFn->captures[0].reg == -1 - slot,
        "const: the def's capture names the same merged cell");
}

// ---------------------------------------------------------------------------
// F2 (module-export/import plan P1): MODULE_NAME is identity, not an entity —
// the compiler skips it when expanding `from m import *`, so two star-imports
// coexist. It stays reachable explicitly and through the qualifier, and stays
// in the export table (the linker's identity checks are untouched).
// ---------------------------------------------------------------------------

static const char *kStarA = R"(
export const MODULE_NAME = "sa"
export const A = "from-sa"
)";

static const char *kStarB = R"(
export const MODULE_NAME = "sb"
export const B = "from-sb"
)";

static void testStarImportIdentity() {
  QLangContext ctx;
  std::string err;
  if (!check(addLibrary(ctx, kStarA, err) != nullptr,
             "star: library sa loads (" + err + ")"))
    return;
  if (!check(addLibrary(ctx, kStarB, err) != nullptr,
             "star: library sb loads (" + err + ")"))
    return;
  const std::string app =
      "from sa import *\nfrom sb import *\n"
      "println(A)\nprintln(B)\n"
      "from sa import MODULE_NAME\nprintln(MODULE_NAME)\n"
      "import sb\nprintln(sb.MODULE_NAME)\n";
  CompiledProgram merged;
  if (!check(buildLinked(ctx, app, merged, err, "app"),
             "star: two star-imported modules link (" + err + ")"))
    return;
  check(
      runText(merged, ctx) == "from-sa\nfrom-sb\nsa\nsb\n",
      "star: both stars bind, MODULE_NAME explicit and qualified still reach");
}

int main() {
  testRegistry();
  testLoadProgramFile();
  testNativeResolution();
  testMerge();
  testOperandRewidening();
  testInitOrder();
  testSharedCell();
  testLibraryChain();
  testUnlinkedOpcodes();
  testImportGuards();
  testClosureAcrossBoundary();
  testGeneratorAcrossBoundary();
  testModuleConstOneIdentity();
  testStarImportIdentity();
  testBoundaries();

  std::cout << (fails == 0 ? "ALL PASS" : "FAILURES") << ": " << passes
            << " passed, " << fails << " failed\n";
  return fails == 0 ? 0 : 1;
}
