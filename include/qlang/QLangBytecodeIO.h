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

// QLS1 v1 .qlc codec (docs/SERIALIZATION.md).
//
// Values are serialized with a 1-byte tag (QLangValue::Type in declaration
// order, fixed) plus a little-endian payload. Non-durable types (Generator,
// Object) serialize as Nil (tag 0).
//
// A .qlc file is a compiled program: header + main section + function
// records + variable map + native-function slots. Native slots store the C++
// function NAME only; the loading context re-resolves them (a missing name
// is an error at load time, see QLangContext::loadProgram).

// Serialize one value. Throws QLangError(Io) on stream failure.
void serializeValue(std::ostream &os, const QLangValue &v);

// Deserialize one value. Throws QLangError(Io) on truncation, an invalid
// tag, or nesting deeper than 128 levels.
QLangValue deserializeValue(std::istream &is);

// Write a full program as QLS1 v1. `nativeIds` maps native function name ->
// slot index (from QLangCompiler::nativeFuncIds). Returns false on stream
// failure.
bool writeQlc(std::ostream &os, const CompiledProgram &prog,
              const std::map<std::string, int> &nativeIds);

// Read a full program from QLS1 v1. On success fills `prog` (nativeFuncs is
// left empty) and `nativeSlots` with the (slot, name) pairs to resolve.
// Returns false (with `err` set) on bad magic, unsupported version,
// truncation, invalid opcode/tag, or trailing bytes.
bool readQlc(std::istream &is, CompiledProgram &prog,
             std::vector<std::pair<int, std::string>> &nativeSlots,
             std::string &err);

} // namespace qlang
