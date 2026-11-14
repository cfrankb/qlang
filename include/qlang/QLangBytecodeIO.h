#pragma once

#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "QLangBytecode.h"
#include "QLangCompiler.h"
#include "QLangValue.h"

namespace qlang {

// QLC1 v10 .qlc codec (docs/SERIALIZATION.md).
//
// Values are serialized with a 1-byte tag (QLangValue::Type in declaration
// order, fixed) plus an explicit little-endian payload. Non-durable types
// (Generator, Object, External, BoundMethod, TypeRef) are NOT durable:
// writeQlc rejects a program whose constant pool holds one (returns false)
// rather than silently remapping it to Nil, and the reader rejects a file
// carrying their tags.
//
// A .qlc file is a compiled program: header + main section + function
// records + the module constant pool + variable map + native-function slots +
// the module name and its export/import tables. Native slots store the C++
// function NAME only; the loading context re-resolves them (a missing name is
// an error at load time, see QLangContext::loadProgram).

// .qlc format version (docs/SERIALIZATION.md §4). Written by writeQlc and
// validated (strictly) by readQlc; the docs reference this constant.
// v10 appends the declared signature block (SPEC §strict typing) at the end
// of every function record — per-parameter type tags with names, and a
// return annotation — so imported modules enforce types at the call
// boundary. The instruction encoding and every earlier section are unchanged
// from v9. v1-v9 files are NOT accepted.
constexpr uint32_t kQlcVersion = 10;

// Serialize one value. Throws QLangError(Io) on stream failure.
void serializeValue(std::ostream &os, const QLangValue &v);

// Deserialize one value. Throws QLangError(Io) on truncation, an invalid
// tag, or nesting deeper than 64 levels (docs/SERIALIZATION.md §8).
QLangValue deserializeValue(std::istream &is);

// Write a full program as QLC1 v10. `nativeIds` maps native function name ->
// slot index (from QLangCompiler::nativeFuncIds). Returns false on stream
// failure, when the constant pool holds a non-durable value (F1), or when the
// program is a LINKED one (`initSequence` is link-time information the format
// has no section for). A program that merely has imports is a module image and
// is written: its import table plus `getimport`/`callimport` are what the
// format carries for a dependency (plan Phase 3c).
bool writeQlc(std::ostream &os, const CompiledProgram &prog,
              const std::map<std::string, int> &nativeIds);

// Read a full program from QLC1 v9. On success fills `prog` (nativeFuncs is
// left empty) and `nativeSlots` with the (slot, name) pairs to resolve.
// Returns false (with `err` set) on bad magic, unsupported version (v1-v8
// and v10+ are rejected), truncation, invalid opcode or modifier byte, a
// pool operand outside the constant pool, a non-durable value tag, an
// export/import table entry with an empty name or an unknown kind, an export
// reference outside the function table or the register range, a
// `getimport`/`callimport` operand outside the import table, or trailing
// bytes. An IMPORT entry's `ref` names the DEFINING module's tables, so it is
// not range-checked here — the linker remaps it and the VM re-checks it at use.
bool readQlc(std::istream &is, CompiledProgram &prog,
             std::vector<std::pair<int, std::string>> &nativeSlots,
             std::string &err);

// One `.qlc` image a QLangContext has loaded, with the provenance the linker
// needs (external-modules plan v4 Phase 3b). QLangContext::loadProgram()
// produces it, the context's module registry keeps it, and QLangLinker merges
// `program` into the entry program.
struct LoadedModule {
  std::string path; // file it was read from ("" when loaded from a stream)
  // The image. `program.moduleName` is the name `import` binds it under and
  // the registry's key; `program.nativeFuncs` holds the re-resolved natives.
  CompiledProgram program;
  // The image's native-slot table (slot -> C++ function name,
  // docs/SERIALIZATION.md §4.3) as stored on disk, kept for re-resolution and
  // for the driver's module breakdown.
  std::vector<std::pair<int, std::string>> nativeSlots;
};

} // namespace qlang
