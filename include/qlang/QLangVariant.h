#pragma once

#include <cstdint>
#include <map>
#include <memory>
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

// A script table is a keyed map of string -> script value (spec: table).
using TableValue = std::map<std::string, QLangValue>;

// The storage variant for every script variable value. Mirrors the spec's
// variable types: string, blob, byte, word, short, int, unsigned, array,
// float, table, function (closure). Index 0 is monostate (nil).
using QLangVariant = std::variant<std::monostate, uint8_t, uint16_t, int16_t,
                                  int32_t, uint32_t, uint64_t, double,
                                  std::string, std::vector<uint8_t>, ArrayValue,
                                  TableValue, ClosurePtr>;

} // namespace qlang
