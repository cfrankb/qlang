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

// A shared upvalue cell table (Phase 1 substrate): one Env per closure
// creation (MAKEFUNC / MAKEGEN). While the creating frame is alive, cell i
// aliases the frame register it was seeded from (`owner`/`ownerRegs`), so
// reads and writes see the live register (write-through). When that frame
// pops, the registers are moved into the cells and `owner` is cleared
// (QLangVM::releaseFrame); the shared_ptr keeps captured state alive after
// the creator frame is gone (fixes the dangling-capture defect, phase0 S1).
struct Env {
  std::vector<QLangValue> cells;
  void* owner = nullptr; // opaque: VM stores Frame* (null once the frame pops)
  std::vector<int> ownerRegs; // cell i aliases owner->regs[ownerRegs[i]]
};
using EnvPtr = std::shared_ptr<Env>;

// A captured-by-reference variable: a shared cell (upvalue model).
// Compile-time specs (the constant pool, Function::captures) carry the
// source register in `reg` with `env` null; MAKEFUNC/MAKEGEN bind them to a
// fresh Env at creation time. The reference stays live across reassignment
// of the outer variable because it addresses the cell, not a snapshot.
struct CaptureRef {
  EnvPtr env = nullptr;
  int cell = 0;
  int reg = 0; // compile-time: source register in the creating frame
};

// A user-defined function value (arrow closure). `funcIndex` selects the
// compiled Function in the program; `captures` is the explicit capture list
// (bound cells after MAKEFUNC).
struct Closure {
  int funcIndex = -1;
  std::vector<CaptureRef> captures;
};
using ClosurePtr = std::shared_ptr<Closure>;

// A script array is a homogeneous C++ container of mixed script values.
// The value itself is recursively a QLangValue, so arrays nest freely.
using ArrayValue = std::vector<QLangValue>;

// A generator instance (Phase 1 substrate): owns its saved frame state.
// Each generator value is an independent instance — there is no VM-level
// per-function frame map. Resuming moves the register file onto the live
// frame and back. `exhausted` stays true once the body has returned, so
// later resumes yield nil.
struct GeneratorState {
  int funcIndex = -1;
  std::vector<QLangValue> regs; // saved register file (moved, not copied)
  std::vector<CaptureRef> captures; // bound capture table (EnvPtr cells)
  size_t pc = 0;
  bool exhausted = false;
};
using GeneratorValuePtr = std::shared_ptr<GeneratorState>;

// A script table is a keyed map of string -> script value (spec: table).
// Shared across copies so mutations in callees are visible to callers (self semantics).
using TableValue = std::shared_ptr<std::map<std::string, QLangValue>>;

// Forward declaration for ObjectOps (defined in QLangObjects.h).
struct ObjectOps;

// Forward declarations for external C++ types (defined in QLangTypes.h).
struct TypeDescriptor;
struct Member;

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

// A view over exactly one C++ object of an external type. `owned` is set when
// the script constructed the object (it lives as long as this value);
// `borrowed` is set when the engine handed the object over (it lives as long
// as the engine keeps it alive). Exactly one of the two is set.
struct ExternalRef {
  const TypeDescriptor* type = nullptr;
  std::shared_ptr<void> owned;
  std::weak_ptr<void> borrowed;
  // Resolve the object pointer: owned.get() if set, else borrowed.lock();
  // throws QLangError(Runtime, "object destroyed: <type>") on an expired borrow.
  const void* self() const;
  void* selfMut();
};

// A callable method of an External object: the bound member plus the target
// view. Produced by TABGET on a method member; dispatched by CALLCLOS.
struct BoundMethodRef {
  const Member* member = nullptr;
  ExternalRef target;
};

// The type itself as a value: resolves enum/flag constants and (if the type
// is constructible) constructs instances. Seeded as a script variable at import.
struct TypeRefRef {
  const TypeDescriptor* type = nullptr;
};

// The storage variant for every script variable value. Mirrors the spec's
// variable types: string, blob, byte, word, short, int, unsigned, array,
// float, table, function (closure), generator. Index 0 is monostate (nil).
// The last three alternatives (ExternalRef, BoundMethodRef, TypeRefRef) are
// non-durable: the codec serializes them as Nil (tag 0).
using QLangVariant = std::variant<std::monostate, uint8_t, uint16_t, int16_t,
                                  int32_t, uint32_t, uint64_t, double,
                                  std::string, std::vector<uint8_t>, ArrayValue,
                                  TableValue, ClosurePtr, GeneratorValuePtr,
                                  ObjectValue, int64_t, ExternalRef,
                                  BoundMethodRef, TypeRefRef>;

} // namespace qlang
