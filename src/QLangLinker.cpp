// The linker (external-modules plan v4 Phase 3b): merges the loaded `.qlc`
// images a program imports with the program itself into one runnable
// `CompiledProgram`.
//
// The rules it implements are SPEC §bytecodes "technical specifications":
// one shared constant table (1), one function table (2), a shared cell per
// exported variable that the defining module writes and importers read (3), and
// module top levels that run once, in dependency order, before the entry's (Q35
// in the plan). Everything about *meaning* is the compiler's job — an unknown
// export, a duplicate import, a write through an import or an import cycle is a
// compile error — so this file only moves numbers, and only three kinds of
// number need moving:
//
//   * a function index naming another module's function,
//   * a constant-pool index naming another module's literal (or a closure
//     spec whose function index / global cell is module-local),
//   * a native slot, which is per-module by construction.
//
// Register operands are NOT rewritten. A module top level runs in a frame that
// aliases the context's global table at the module's own offset, so each module
// keeps the register numbers its compiler assigned while occupying a disjoint
// slice of the table. The one exception is an operand the compiler recorded as
// a pending link: it names ANOTHER module's cell, which the entry program could
// not name on its own.
#include "qlang/QLangLinker.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "qlang/QLangErrors.h"

namespace qlang {
namespace {

[[noreturn]] void linkErr(const std::string &msg) {
  throw QLangError(QLangErrorKind::Io, 0, 0, msg);
}

// Where one module landed in the merged program, and how its own table indices
// map into the merged tables.
struct ModuleLink {
  const LoadedModule *mod = nullptr;
  std::string name;
  int regBase = 0;  // offset of the module's slice of the global register area
  int funcBase = 0; // merged index of the module's functions[0] (its top level)
  int width = 0;    // registers the module's top level addresses
  std::vector<int> poolMap;   // module pool index -> merged pool index
  std::vector<int> nativeMap; // module native slot -> merged native slot
};

// Depth-first, dependencies first: a module is emitted after everything it
// imports, which is the order the top levels run in (Q35). `done` makes a module
// imported by two names run exactly once; `onPath` catches a graph that reaches
// a module already being visited. The compiler rejects a cycle too
// (checkImportCycles), so this is the loader-side half of the same rule.
void collectOrder(const std::string &name,
                  const std::vector<std::shared_ptr<LoadedModule>> &avail,
                  std::vector<const LoadedModule *> &out,
                  std::unordered_set<std::string> &done,
                  std::unordered_set<std::string> &onPath) {
  if (done.count(name))
    return;
  if (onPath.count(name))
    linkErr("import cycle involving module '" + name + "'");
  const LoadedModule *found = nullptr;
  for (const auto &m : avail)
    if (m && m->program.moduleName == name)
      found = m.get();
  if (!found)
    linkErr("module '" + name + "' is not loaded");
  onPath.insert(name);
  for (const ImportEntry &im : found->program.imports)
    collectOrder(im.module, avail, out, done, onPath);
  onPath.erase(name);
  done.insert(name);
  out.push_back(found);
}

// Rebase a constant that is moving into the merged pool. Only a closure spec
// carries a module-local number: `funcIndex` names a function of its own module,
// and a capture register of `-1 - slot` (the compiler's sentinel for a module
// global) names a cell of its own module. Both are absolute within the module,
// so both shift by the module's merged base. A fresh Closure is built rather
// than mutated: the same shared_ptr can sit in several pool slots.
QLangValue rebaseConstant(const QLangValue &v, int funcBase, int regBase) {
  if (v.type() != QLangValue::Type::Function)
    return v;
  const Closure &spec = v.asFunction();
  auto out = std::make_shared<Closure>();
  out->funcIndex = spec.funcIndex + funcBase;
  out->captures.reserve(spec.captures.size());
  for (const CaptureRef &cr : spec.captures) {
    CaptureRef moved = cr;
    if (moved.reg < 0)
      moved.reg -= regBase; // -1 - slot  ->  -1 - (regBase + slot)
    out->captures.push_back(moved);
  }
  return QLangValue::Function(out);
}

// The merged native-slot table. Slots are per-module, so every `call` operand
// has to be renumbered; two modules that import the same C++ function should
// share one slot, and the only way to know they do is the name (a program keeps
// the resolved pointer, not the name).
class NativeTable {
public:
  int add(const std::string &name, const QLangContext::NativeFunc &fn) {
    if (!name.empty()) {
      auto it = m_byName.find(name);
      if (it != m_byName.end())
        return it->second;
    }
    const int idx = static_cast<int>(m_funcs.size());
    m_funcs.push_back(fn);
    if (!name.empty())
      m_byName[name] = idx;
    return idx;
  }
  std::vector<QLangContext::NativeFunc> take() { return std::move(m_funcs); }

private:
  std::vector<QLangContext::NativeFunc> m_funcs;
  std::unordered_map<std::string, int> m_byName;
};

// The (slot, name) pairs of one image, indexed by slot. A `.qlc` stores the
// pairs ascending and gap-free per the reader's invariants, but the linker does
// not assume that: an unnamed slot still has to map somewhere.
std::unordered_map<int, std::string> slotNames(
    const std::vector<std::pair<int, std::string>> &slots) {
  std::unordered_map<int, std::string> out;
  for (const auto &[slot, name] : slots)
    out[slot] = name;
  return out;
}

} // namespace

QLangLinker::QLangLinker(std::vector<std::shared_ptr<LoadedModule>> available)
    : m_available(std::move(available)) {}

CompiledProgram QLangLinker::link(const CompiledProgram &entry,
                                 const std::map<std::string, int> &entryNativeIds) const {
  if (!entry.initSequence.empty())
    linkErr("program is already linked");

  // --- 1. Which modules take part, in the order their top levels run --------
  std::vector<const LoadedModule *> order;
  {
    std::unordered_set<std::string> done, onPath;
    for (const ImportEntry &im : entry.imports)
      collectOrder(im.module, m_available, order, done, onPath);
  }

  // A library image is a module, not a half-linked program: its own cross-module
  // references would need pending links, and those are a link artifact the
  // `.qlc` format does not carry (writeQlc rejects them). The compiler refuses
  // to produce such an image; this is the loader-side guard for a hand-built or
  // future one.
  for (const LoadedModule *m : order) {
    if (m->program.functions.empty())
      linkErr("module '" + m->program.moduleName + "' has no functions");
    if (!m->program.pendingLinks.empty())
      linkErr("module '" + m->program.moduleName +
              "' has unresolved cross-module references and cannot be linked");
    if (!m->program.imports.empty())
      linkErr("module '" + m->program.moduleName + "' imports another .ql module, "
              "which a library cannot do until GETIMPORT/CALLIMPORT (plan "
              "Phase 3c)");
  }

  // --- 2. Function table and global-register slices -------------------------
  CompiledProgram out;
  out.moduleName = entry.moduleName;
  out.functions = entry.functions;
  out.vars = entry.vars;
  out.constVars = entry.constVars;
  out.exports = entry.exports;
  out.imports = entry.imports;

  const int entryWidth = std::max(entry.registerCount, 1);
  int regTotal = entryWidth;

  std::vector<ModuleLink> links;
  links.reserve(order.size());
  for (const LoadedModule *m : order) {
    ModuleLink ml;
    ml.mod = m;
    ml.name = m->program.moduleName;
    ml.regBase = regTotal;
    ml.funcBase = static_cast<int>(out.functions.size());
    ml.width = std::max(m->program.registerCount, 1);
    out.functions.insert(out.functions.end(), m->program.functions.begin(),
                         m->program.functions.end());
    regTotal += ml.width;
    links.push_back(std::move(ml));
  }
  out.registerCount = regTotal;

  auto linkFor = [&links](const std::string &name) -> const ModuleLink & {
    for (const ModuleLink &ml : links)
      if (ml.name == name)
        return ml;
    linkErr("module '" + name + "' is not loaded");
  };
  // The module a merged function index belongs to (the entry module is nullptr:
  // its bases are 0 and its maps are the entry's own).
  auto ownerOf = [&links, &entry](int funcIndex) -> const ModuleLink * {
    if (funcIndex < static_cast<int>(entry.functions.size()))
      return nullptr;
    for (const ModuleLink &ml : links)
      if (funcIndex >= ml.funcBase &&
          funcIndex < ml.funcBase + static_cast<int>(ml.mod->program.functions.size()))
        return &ml;
    return nullptr;
  };

  // --- 3. Native slots ------------------------------------------------------
  // The entry's slot names are not in its program (only resolved pointers are),
  // so the caller hands over the same name -> id map the writer takes.
  NativeTable natives;
  std::vector<int> entryNativeMap;
  {
    std::unordered_map<int, std::string> names;
    for (const auto &[name, id] : entryNativeIds)
      names[-1 - id] = name;
    entryNativeMap.resize(entry.nativeFuncs.size());
    for (size_t s = 0; s < entry.nativeFuncs.size(); s++) {
      auto it = names.find(static_cast<int>(s));
      entryNativeMap[s] =
          natives.add(it == names.end() ? std::string() : it->second,
                      entry.nativeFuncs[s]);
    }
  }
  for (ModuleLink &ml : links) {
    const std::unordered_map<int, std::string> names = slotNames(ml.mod->nativeSlots);
    const size_t count = std::max(ml.mod->program.nativeFuncs.size(),
                                  names.empty() ? size_t{0}
                                                : names.size());
    ml.nativeMap.resize(count);
    for (size_t s = 0; s < count; s++) {
      auto it = names.find(static_cast<int>(s));
      const QLangContext::NativeFunc fn =
          s < ml.mod->program.nativeFuncs.size()
              ? ml.mod->program.nativeFuncs[s]
              : QLangContext::NativeFunc();
      ml.nativeMap[s] =
          natives.add(it == names.end() ? std::string() : it->second, fn);
    }
  }
  out.nativeFuncs = natives.take();

  // --- 4. One merged constant pool, folded (SPEC §bytecodes "constant pool") --
  // An entry closure constant that a pending link says names a LIBRARY function
  // (the imported-generator path) has to be rebased with that library's bases
  // while the entry's own closures stay put.
  std::unordered_map<int, int> closureConsts; // entry pool index -> link index
  for (const PendingLink &pl : entry.pendingLinks) {
    if (pl.kind != PendingLinkKind::ClosureConst)
      continue;
    if (pl.importIndex < 0 ||
        pl.importIndex >= static_cast<int>(entry.imports.size()))
      linkErr("pending link names import " + std::to_string(pl.importIndex) +
              ", the program has " + std::to_string(entry.imports.size()));
    const std::string &mod = entry.imports[pl.importIndex].module;
    for (size_t i = 0; i < links.size(); i++)
      if (links[i].name == mod)
        closureConsts[pl.poolIndex] = static_cast<int>(i);
  }

  ConstantPool pool;
  std::vector<int> entryPoolMap(entry.constants.size());
  for (size_t i = 0; i < entry.constants.size(); i++) {
    auto it = closureConsts.find(static_cast<int>(i));
    if (it == closureConsts.end())
      entryPoolMap[i] = pool.add(entry.constants[i]);
    else
      entryPoolMap[i] = pool.add(rebaseConstant(entry.constants[i],
                                                links[it->second].funcBase,
                                                links[it->second].regBase));
  }
  for (ModuleLink &ml : links) {
    ml.poolMap.resize(ml.mod->program.constants.size());
    for (size_t i = 0; i < ml.mod->program.constants.size(); i++)
      ml.poolMap[i] =
          pool.add(rebaseConstant(ml.mod->program.constants[i], ml.funcBase,
                                  ml.regBase));
  }
  out.constants = pool.take();

  // --- 5. Operand rewriting (plan Q32: on RawInstr, never on bytes) ---------
  // Pending links are addressed by (function, instruction); the entry module is
  // the only one that can have them.
  std::unordered_map<int, std::vector<const PendingLink *>> pendingByFunc;
  for (const PendingLink &pl : entry.pendingLinks)
    pendingByFunc[pl.funcIndex].push_back(&pl);

  auto mapPool = [](const std::vector<int> &map, int idx,
                    const std::string &who) {
    if (idx < 0 || idx >= static_cast<int>(map.size()))
      linkErr(who + ": constant index " + std::to_string(idx) +
              " is outside the module's constant pool");
    return map[idx];
  };
  auto mapNative = [](const std::vector<int> &map, int slot,
                      const std::string &who) {
    if (slot < 0 || slot >= static_cast<int>(map.size()))
      linkErr(who + ": native slot " + std::to_string(slot) +
              " is outside the module's native-slot table");
    return map[slot];
  };

  for (int f = 0; f < static_cast<int>(out.functions.size()); f++) {
    const ModuleLink *owner = ownerOf(f);
    const std::vector<int> &poolMap = owner ? owner->poolMap : entryPoolMap;
    const std::vector<int> &nativeMap =
        owner ? owner->nativeMap : entryNativeMap;
    const std::string who =
        owner ? owner->name : entry.moduleName;

    std::vector<RawInstr> raw;
    decodeRawStream(out.functions[f], raw);

    bool changed = false;
    for (size_t i = 0; i < raw.size(); i++) {
      RawInstr &r = raw[i];

      // A cross-module reference the compiler left for the linker. Its operand
      // is a handle into the DEFINING module, so it is resolved directly to the
      // merged number and the role-based pass below must not touch it again.
      auto pit = pendingByFunc.find(f);
      if (pit != pendingByFunc.end()) {
        bool handled = false;
        for (const PendingLink *pl : pit->second) {
          if (pl->instrIndex != static_cast<int>(i))
            continue;
          if (pl->importIndex < 0 ||
              pl->importIndex >= static_cast<int>(out.imports.size()))
            linkErr("pending link names import " +
                    std::to_string(pl->importIndex) + ", the program has " +
                    std::to_string(out.imports.size()));
          const ImportEntry &im = out.imports[pl->importIndex];
          const ModuleLink &ml = linkFor(im.module);
          if (pl->kind == PendingLinkKind::ClosureConst) {
            // MAKEGEN's operand is a pool index; the closure it names was
            // already rebased while the pool merged.
            if (pl->poolIndex != r.b)
              linkErr("pending link points at constant " +
                      std::to_string(pl->poolIndex) + " but the instruction "
                      "names " + std::to_string(r.b));
            r.b = mapPool(entryPoolMap, pl->poolIndex, who);
          } else if (im.kind == ExportKind::Func) {
            r.b = ml.funcBase + im.ref;
            if (r.b < 0 || r.b >= static_cast<int>(out.functions.size()))
              linkErr("imported function '" + im.name + "' of module '" +
                      im.module + "' resolves outside the merged function table");
          } else {
            r.b = ml.regBase + im.ref;
            if (r.b < 0 || r.b >= out.registerCount)
              linkErr("imported variable '" + im.name + "' of module '" +
                      im.module + "' resolves outside the merged register area");
          }
          handled = true;
          changed = true;
          break;
        }
        if (handled)
          continue;
      }

      switch (operandRole(r.op, 1, r.kind)) {
      case OperandRole::PoolIndex:
        r.b = mapPool(poolMap, r.b, who);
        changed = true;
        break;
      case OperandRole::FunctionIndex: {
        const int merged = (owner ? owner->funcBase : 0) + r.b;
        if (r.b < 0 || merged >= static_cast<int>(out.functions.size()))
          linkErr(who + ": function index " + std::to_string(r.b) +
                  " is outside the module's function table");
        if (merged != r.b)
          changed = true;
        r.b = merged;
        break;
      }
      case OperandRole::NativeSlot:
        // Only a NEGATIVE operand is a native slot; a non-negative one is a
        // builtin id, which is global and stays as it is.
        if (r.b < 0) {
          r.b = -1 - mapNative(nativeMap, -1 - r.b, who);
          changed = true;
        }
        break;
      default:
        break;
      }
    }

    // Re-encode only a stream an actual rewrite touched: untouched code keeps
    // the exact bytes its module's compiler produced.
    if (changed)
      encodeRawStream(out.functions[f], raw);
  }

  // --- 6. Capture specs, import table, init sequence ------------------------
  // A global capture names an absolute table slot through the compiler's
  // `-1 - slot` sentinel, so it moves with the module exactly like a cell.
  for (ModuleLink &ml : links)
    for (size_t i = 0; i < ml.mod->program.functions.size(); i++) {
      Function &fn = out.functions[ml.funcBase + i];
      for (CaptureRef &cr : fn.captures)
        if (cr.reg < 0)
          cr.reg -= ml.regBase;
    }

  // The merged program's import table describes the merged program: every `ref`
  // becomes the merged handle (a function index or an absolute cell).
  for (ImportEntry &im : out.imports) {
    const ModuleLink &ml = linkFor(im.module);
    im.ref = (im.kind == ExportKind::Func) ? ml.funcBase + im.ref
                                           : ml.regBase + im.ref;
  }

  // Q35: each imported module's top level runs exactly once, dependencies
  // first, before the entry module's.
  for (const ModuleLink &ml : links)
    out.initSequence.push_back(
        ModuleInit{ml.funcBase, ml.regBase, ml.width, ml.name});

  // The link consumed them; a program with pending links left is not runnable.
  out.pendingLinks.clear();
  return out;
}

} // namespace qlang
