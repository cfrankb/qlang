#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "QLangBytecodeIO.h"
#include "QLangCompiler.h"

namespace qlang {

// Merges the loaded `.qlc` module images one program imports into a single
// runnable program (SPEC §bytecodes "technical specifications" 1–5, external
// modules plan v4 Phase 3b).
//
// The linker is purely mechanical: everything that could be decided on meaning
// was decided by the compiler (unknown export, duplicate import, a write
// through an import, an import cycle are all compile errors), so a link can
// only fail on an image that does not fit the program being built — a module
// that was never loaded, or a library image that still carries unresolved
// cross-module references.
//
// What it merges, and why each part needs it:
//
//   * Functions are concatenated. The entry module keeps indices 0..n and each
//     library is appended in the order its top level runs, so a call into a
//     library becomes a `callfn` with the merged index.
//   * Each module gets a slice of the shared global register area (its
//     `registerCount` wide, at a cumulative offset). A module top level runs in
//     a frame aliasing the context's global table at that offset, so a module's
//     own register numbers stay valid and no register operand is rewritten —
//     only the operands that name ANOTHER module's cell, which the compiler
//     recorded as pending links.
//   * The constant pools merge into one, folded through `valueKey` so a literal
//     two modules share occupies one slot (SPEC §bytecodes "constant pool":
//     unique values). Closure constants are rebased before they are added, so
//     the fold compares merged function indices.
//   * Native slots concatenate, deduped by name where a name is known.
//
// Every operand rewrite happens on the decoded `RawInstr` stream, which is then
// re-encoded by `encodeRawStream()`: operand widths are per-instruction in the
// v6 encoding, so a merged index that needs a wider operand is handled by
// choosing the width again, never by patching a byte.
//
// The result is a program, not an image: `CompiledProgram::initSequence` is
// link-time information `.qlc` v9 has no section for, so `writeQlc` rejects it.
class QLangLinker {
public:
  // The modules available for linking — normally `QLangContext::loadedModules()`.
  // The linker never loads anything: finding a module by name without it being
  // loaded is the registry's job (plan C8).
  explicit QLangLinker(std::vector<std::shared_ptr<LoadedModule>> available);

  // Merges every module `entry` imports (transitively) with `entry` itself.
  // `entryNativeIds` maps a native function name to its negative CALL operand
  // id (the same map `writeQlc` takes, i.e. `QLangCompiler::nativeFuncIds()`);
  // it is used only to dedup the merged native-slot table by name, because a
  // `CompiledProgram` stores resolved function pointers, not names. Without it
  // the entry's slots simply are not deduped against the libraries'.
  //
  // Throws QLangError(Io) when an imported module is not loaded, when the
  // import graph is not acyclic among the loaded images, when a library image
  // is not linkable (no functions, pending links of its own, or an import table
  // of its own — a library that imports another `.ql` needs the opcodes of plan
  // Phase 3c), or when a merged reference would fall outside the merged
  // function table, constant pool or native-slot table.
  CompiledProgram link(const CompiledProgram &entry,
                       const std::map<std::string, int> &entryNativeIds = {}) const;

private:
  std::vector<std::shared_ptr<LoadedModule>> m_available;
};

} // namespace qlang
