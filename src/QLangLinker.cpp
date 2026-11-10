// The linker (external-modules plan v4 Phases 3b/3c): merges the loaded `.qlc`
// images a program imports with the program itself into one runnable
// `CompiledProgram`.
//
// The rules it implements are SPEC §bytecodes "technical specifications":
// one shared constant table (1), one function table (2), a shared cell per
// exported variable that the defining module writes and importers read (3), and
// module top levels that run once, in dependency order, before the entry's (Q35
// in the plan). Everything about *meaning* is the compiler's job — an unknown
// export, a duplicate import, a write through an import or an import cycle is a
// compile error — so this file only moves numbers, and only four kinds of
// number need moving:
//
//   * a function index naming another module's function,
//   * a constant-pool index naming another module's literal (or a closure
//     spec whose function index / global cell is module-local),
//   * a native slot, which is per-module by construction,
//   * an import-table index (`getimport`/`callimport` operand `b`), because the
//     merged program carries one merged import table instead of one per module.
//
// Register operands are NOT rewritten. A module top level runs in a frame that
// aliases the context's global table at the module's own offset, so each module
// keeps the register numbers its compiler assigned while occupying a disjoint
// slice of the table. A cross-module reference needs no exception either:
// `getimport`/`callimport` name the import table rather than a register, so the
// only rewriting a cross-module reference needs is the entry's own index — and
// `callimport`, which can only ever name a function the merged program already
// has, is resolved into `callfn`/`makegen` and disappears (plan Phase 3c).
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
  // Module import index -> merged import index. Two modules importing the same
  // export share one merged entry, so the map is not the identity even when the
  // module is the only importer of its own dependency.
  std::vector<int> importMap;
};

// The merged import table. Keyed by (defining module, exported name) — the pair
// that decides what an entry resolves to — so a name two modules import from
// the same library becomes one cell, exactly like the shared cell SPEC
// §bytecodes "technical specifications" 3 describes.
class ImportTable {
public:
  int add(const ImportEntry &im) {
    const std::string key = im.module + '\0' + im.name;
    auto it = m_byKey.find(key);
    if (it != m_byKey.end())
      return it->second;
    const int idx = static_cast<int>(m_entries.size());
    m_entries.push_back(im);
    m_byKey[key] = idx;
    return idx;
  }
  std::vector<ImportEntry> take() { return std::move(m_entries); }

private:
  std::vector<ImportEntry> m_entries;
  std::unordered_map<std::string, int> m_byKey;
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

  // A library image is a module, and since Phase 3c a module may itself import
  // another: its cross-module references are `getimport`/`callimport` operands
  // into its own import table, which collectOrder() has already followed
  // transitively. Nothing else needs unwinding here.
  for (const LoadedModule *m : order)
    if (m->program.functions.empty())
      linkErr("module '" + m->program.moduleName + "' has no functions");

  // --- 2. Function table and global-register slices -------------------------
  CompiledProgram out;
  out.moduleName = entry.moduleName;
  out.functions = entry.functions;
  out.vars = entry.vars;
  out.constVars = entry.constVars;
  out.exports = entry.exports;

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

  // One merged import table (plan Phase 3c): the entry's entries are added
  // first and in order, so the indices its `getimport` operands already carry
  // stay valid, and every module's own table is folded into the same structure
  // so two modules importing the same export read one shared cell. The `ref`s
  // are still defining-module handles here; step 6 turns them into merged ones.
  ImportTable imports;
  std::vector<int> entryImportMap(entry.imports.size());
  for (size_t i = 0; i < entry.imports.size(); i++)
    entryImportMap[i] = imports.add(entry.imports[i]);
  for (ModuleLink &ml : links) {
    const std::vector<ImportEntry> &mine = ml.mod->program.imports;
    ml.importMap.resize(mine.size());
    for (size_t i = 0; i < mine.size(); i++)
      ml.importMap[i] = imports.add(mine[i]);
  }
  out.imports = imports.take();

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

  // --- 4. Capture specs, then one merged constant pool (SPEC §bytecodes
  // "constant pool") ---------------------------------------------------------
  // A global capture names an absolute table slot through the compiler's
  // `-1 - slot` sentinel, so it moves with the module exactly like a cell.
  // Rebasing the functions' capture specs BEFORE the pool merges is what lets
  // step 5 build an imported generator's MAKEGEN closure straight from the
  // merged function and still have it fold with the defining module's own
  // constant (Q33).
  for (ModuleLink &ml : links)
    for (size_t i = 0; i < ml.mod->program.functions.size(); i++) {
      Function &fn = out.functions[ml.funcBase + i];
      for (CaptureRef &cr : fn.captures)
        if (cr.reg < 0)
          cr.reg -= ml.regBase;
    }

  ConstantPool pool;
  std::vector<int> entryPoolMap(entry.constants.size());
  for (size_t i = 0; i < entry.constants.size(); i++)
    entryPoolMap[i] = pool.add(entry.constants[i]);
  for (ModuleLink &ml : links) {
    ml.poolMap.resize(ml.mod->program.constants.size());
    for (size_t i = 0; i < ml.mod->program.constants.size(); i++)
      ml.poolMap[i] =
          pool.add(rebaseConstant(ml.mod->program.constants[i], ml.funcBase,
                                  ml.regBase));
  }

  // --- 5. Operand rewriting (plan Q32: on RawInstr, never on bytes) ---------
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
      case OperandRole::ImportIndex: {
        const std::vector<int> &importMap =
            owner ? owner->importMap : entryImportMap;
        if (r.b < 0 || r.b >= static_cast<int>(importMap.size()))
          linkErr(who + ": import index " + std::to_string(r.b) +
                  " is outside the module's import table");
        const int mergedImport = importMap[r.b];
        if (r.op != Opcode::CALLIMPORT) {
          // `getimport` survives the link (plan Phase 3c): the merged table
          // describes the merged program, so only the index moves. Step 6 turns
          // the entry's `ref` into the merged cell it will read.
          if (mergedImport != r.b)
            changed = true;
          r.b = mergedImport;
          break;
        }
        // A call names a function the merged program already has, so the import
        // is resolved away: no `callimport` survives a link.
        const ImportEntry &im = out.imports[static_cast<size_t>(mergedImport)];
        if (im.kind != ExportKind::Func)
          linkErr("callimport names '" + im.name + "' of module '" + im.module +
                  "', which is not a function");
        const int fi = linkFor(im.module).funcBase + im.ref;
        if (fi < 0 || fi >= static_cast<int>(out.functions.size()))
          linkErr("imported function '" + im.name + "' of module '" + im.module +
                  "' resolves outside the merged function table");
        if (out.functions[static_cast<size_t>(fi)].isGenerator) {
          // MAKEGEN's operand is a pool Closure, so one is synthesized from the
          // merged function. Its captures were rebased in step 4, which is what
          // lets it fold with the defining module's own generator constant.
          auto closure = std::make_shared<Closure>();
          closure->funcIndex = fi;
          closure->captures = out.functions[static_cast<size_t>(fi)].captures;
          // Changing the opcode also changes what operand `b` MEANS: `callimport`
          // named a table entry through a register-class operand, `makegen` names
          // a pool entry, and the modifier byte has to say so (the codec rejects
          // a register kind on `makegen`). `callfn` keeps the register class, so
          // only this branch moves the kind.
          r.op = Opcode::MAKEGEN;
          r.kind = RightKind::ConstPool;
          r.b = pool.add(QLangValue::Function(closure));
        } else {
          r.op = Opcode::CALLFN;
          r.kind = RightKind::Register;
          r.b = fi;
        }
        changed = true;
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

  // --- 6. Constant pool, import table, init sequence ------------------------
  // Taken only now, because step 5 can add a closure constant for an imported
  // generator.
  out.constants = pool.take();

  // The merged program's import table describes the merged program: every `ref`
  // becomes the merged handle (a function index or an absolute cell), which is
  // what a surviving `getimport` operand reads. A `ref` outside the module it
  // comes from would land in another module's slice or past the last function,
  // so it is refused rather than rebased into a cell that belongs to someone
  // else.
  for (ImportEntry &im : out.imports) {
    const ModuleLink &ml = linkFor(im.module);
    const bool func = im.kind == ExportKind::Func;
    const int limit = func ? static_cast<int>(ml.mod->program.functions.size())
                           : ml.mod->program.registerCount;
    if (im.ref < 0 || im.ref >= limit)
      linkErr("module '" + im.module + "' export '" + im.name + "' names a " +
              (func ? "function index" : "cell") + " outside the module (" +
              std::to_string(im.ref) + " of " + std::to_string(limit) + ")");
    im.ref = func ? ml.funcBase + im.ref : ml.regBase + im.ref;
  }

  // Q35: each imported module's top level runs exactly once, dependencies
  // first, before the entry module's.
  for (const ModuleLink &ml : links)
    out.initSequence.push_back(
        ModuleInit{ml.funcBase, ml.regBase, ml.width, ml.name});

  return out;
}

} // namespace qlang
