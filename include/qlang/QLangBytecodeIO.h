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

// QLC1 v6 .qlc codec (docs/SERIALIZATION.md).
//
// Values are serialized with a 1-byte tag (QLangValue::Type in declaration
// order, fixed) plus an explicit little-endian payload. Non-durable types
// (Generator, Object, External, BoundMethod, TypeRef) are NOT durable:
// writeQlc rejects a program whose constant pool holds one (returns false)
// rather than silently remapping it to Nil, and the reader rejects a file
// carrying their tags.
//
// A .qlc file is a compiled program: header + main section + function
// records + variable map + native-function slots. Native slots store the C++
// function NAME only; the loading context re-resolves them (a missing name
// is an error at load time, see QLangContext::loadProgram).

// .qlc format version (docs/SERIALIZATION.md §4). Written by writeQlc and
// validated (strictly) by readQlc; the docs reference this constant.
// v6 is the 2/2/4 modifier-byte instruction format (flat per-function code
// stream + instruction offset table); v1-v5 files are NOT accepted.
constexpr uint32_t kQlcVersion = 6;

// Serialize one value. Throws QLangError(Io) on stream failure.
void serializeValue(std::ostream &os, const QLangValue &v);

// Deserialize one value. Throws QLangError(Io) on truncation, an invalid
// tag, or nesting deeper than 64 levels (docs/SERIALIZATION.md §8).
QLangValue deserializeValue(std::istream &is);

// Write a full program as QLC1 v6. `nativeIds` maps native function name ->
// slot index (from QLangCompiler::nativeFuncIds). Returns false on stream
// failure or when a constant pool holds a non-durable value (F1).
bool writeQlc(std::ostream &os, const CompiledProgram &prog,
              const std::map<std::string, int> &nativeIds);

// Read a full program from QLC1 v6. On success fills `prog` (nativeFuncs is
// left empty) and `nativeSlots` with the (slot, name) pairs to resolve.
// Returns false (with `err` set) on bad magic, unsupported version (v1-v5
// and v7+ are rejected), truncation, invalid opcode or modifier byte, a
// non-durable value tag, or trailing bytes.
bool readQlc(std::istream &is, CompiledProgram &prog,
             std::vector<std::pair<int, std::string>> &nativeSlots,
             std::string &err);

} // namespace qlang
