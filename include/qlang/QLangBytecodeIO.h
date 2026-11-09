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

// QLC1 v9 .qlc codec (docs/SERIALIZATION.md).
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
// v9 appends the module identity and the export/import tables at the tail
// (SPEC §bytecodes "keyword export", "export table (lookup)", "import table
// (lookup)"; external-modules plan Phase 3a); the instruction encoding and
// every earlier section are unchanged from v8. v1-v8 files are NOT accepted.
constexpr uint32_t kQlcVersion = 9;

// Serialize one value. Throws QLangError(Io) on stream failure.
void serializeValue(std::ostream &os, const QLangValue &v);

// Deserialize one value. Throws QLangError(Io) on truncation, an invalid
// tag, or nesting deeper than 64 levels (docs/SERIALIZATION.md §8).
QLangValue deserializeValue(std::istream &is);

// Write a full program as QLC1 v9. `nativeIds` maps native function name ->
// slot index (from QLangCompiler::nativeFuncIds). Returns false on stream
// failure or when the constant pool holds a non-durable value (F1).
bool writeQlc(std::ostream &os, const CompiledProgram &prog,
              const std::map<std::string, int> &nativeIds);

// Read a full program from QLC1 v9. On success fills `prog` (nativeFuncs is
// left empty) and `nativeSlots` with the (slot, name) pairs to resolve.
// Returns false (with `err` set) on bad magic, unsupported version (v1-v8
// and v10+ are rejected), truncation, invalid opcode or modifier byte, a
// pool operand outside the constant pool, a non-durable value tag, an
// export/import table entry with an empty name, an unknown kind or a
// reference outside the function table or the register range, or trailing
// bytes.
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
