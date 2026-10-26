#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace qlang {

class QLangValue; // forward declaration: ArrayValue references it recursively

// A captured-by-reference variable: an opaque pointer to the enclosing frame's
// register file plus the register index within it. The VM stores a real frame
// pointer here (see QLangVM); the pointer is intentionally opaque so the value
// layer does not depend on the VM. The reference stays live across reassignment
// of the outer variable because it addresses the register, not a snapshot.
struct CaptureRef {
  void* frame = nullptr; // opaque: VM stores Frame*
  int reg = 0;
};

// A user-defined function value (arrow closure). `funcIndex` selects the
// compiled Function in the program; `captures` is the explicit capture list.
struct Closure {
  int funcIndex = -1;
  std::vector<CaptureRef> captures;
};
using ClosurePtr = std::shared_ptr<Closure>;

// A script array is a homogeneous C++ container of mixed script values.
// The value itself is recursively a QLangValue, so arrays nest freely.
using ArrayValue = std::vector<QLangValue>;

// A generator value: lazily yields values on each call.
// Returns std::optional<QLangValue>; nullopt signals exhaustion (EOF).
using GeneratorValue = std::function<std::optional<QLangValue>()>;
using GeneratorValuePtr = std::shared_ptr<GeneratorValue>;

// A script table is a keyed map of string -> script value (spec: table).
// Shared across copies so mutations in callees are visible to callers (self semantics).
using TableValue = std::shared_ptr<std::map<std::string, QLangValue>>;

// Forward declaration for ObjectOps (defined in QLangObjects.h).
struct ObjectOps;

// Forward declaration for class instances (Object type).
// The descriptor holds ObjectOps and field/method bindings.
struct ObjectDescriptor;
using ObjectValue = std::shared_ptr<ObjectDescriptor>;

// ObjectDescriptor: the runtime representation of a class instance.
// Holds the ObjectOps dispatch table and instance field storage.
// Instances are reference-counted via shared_ptr.
struct ObjectDescriptor {
  const struct ObjectOps* ops = nullptr;  // dispatch table (null for raw instances)
  std::map<std::string, QLangValue> fields;  // instance field storage
};

// The storage variant for every script variable value. Mirrors the spec's
// variable types: string, blob, byte, word, short, int, unsigned, array,
// float, table, function (closure), generator. Index 0 is monostate (nil).
using QLangVariant = std::variant<std::monostate, uint8_t, uint16_t, int16_t,
                                  int32_t, uint32_t, uint64_t, double,
                                  std::string, std::vector<uint8_t>, ArrayValue,
                                  TableValue, ClosurePtr, GeneratorValuePtr, ObjectValue>;

} // namespace qlang
