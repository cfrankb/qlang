#pragma once

#include <string>

#include "QLangValue.h"

namespace qlang {

// Forward declaration for QLangContext (used in dispatch callbacks).
class QLangContext;

// ObjectOps: the dispatch layer for object operations.
// This is the neutral runtime infrastructure shared between QLang classes
// and external C++ types. Both object kinds go through the same four
// dispatch operations (GETFIELD / SETFIELD / CALLMETHOD / TOSTRING).
//
// The table below shows the dispatch points:
//
// Dispatch point | Table/Class behavior | External behavior
// -------------- | -------------------- | -----------------
// field get      | map lookup / instance field | registered FieldBinding::get
// field set      | map insert / field store | registered FieldBinding::set (nullptr -> read-only error)
// method call    | class method with `self` | MethodBinding::invoke with `self`
// string conversion | `__str__` | registered `__str__` binding (else -> type name + error)
//
// Why dispatch-first: Zero class semantics (no `self` allocation, no instance
// storage, no `__init__` protocol, no inheritance) and zero C++-RTTI.
// Four function pointers and a lookup. The only existing consumer is the
// **table kind**: today's table key-lookup/insert semantics ported into the
// pointers — a pure refactor.
struct ObjectOps {
  const char* kindName;  // Human-readable name for the object kind (e.g., "table")

  // GETFIELD: Get a field value from an object by name.
  // Returns the field value, or throws if not found.
  QLangValue (*getField)(const QLangValue& obj, const std::string& name,
                          QLangContext& ctx);

  // SETFIELD: Set a field value on an object by name.
  // Throws if the field cannot be set (e.g., read-only, not found).
  void (*setField)(QLangValue& obj, const std::string& name,
                   const QLangValue& value, QLangContext& ctx);

  // CALLMETHOD: Call a method on an object by name with arguments.
  // Returns the method's return value, or throws if not found.
  QLangValue (*callMethod)(const QLangValue& obj, const std::string& name,
                            const std::vector<QLangValue>& args,
                            QLangContext& ctx);

  // TOSTRING: Convert an object to its string representation.
  // Used by str() and f-strings.
  QLangValue (*toStr)(const QLangValue& obj, QLangContext& ctx);

  // Reserved for future inheritance support. Null in v1.
  const ObjectOps* (*base)(const QLangValue& obj);
};

// getObjectOps: Returns the ObjectOps for a value's type, or nullptr if
// the type doesn't have object operations (e.g., Int, String, etc.).
// Currently returns the table ops for Table type.
const ObjectOps* getObjectOps(const QLangValue& obj);

// Table kind ObjectOps implementation.
// These functions implement the table key-lookup/insert semantics for the
// table type. This is the "neutral" implementation that the table kind
// uses today.
namespace table_ops {

// Get a field from a table by key name.
QLangValue getField(const QLangValue& obj, const std::string& name,
                    QLangContext& ctx);

// Set a field on a table by key name.
void setField(QLangValue& obj, const std::string& name,
              const QLangValue& value, QLangContext& ctx);

// Call a method on a table (look up the key and call if it's a function).
QLangValue callMethod(const QLangValue& obj, const std::string& name,
                       const std::vector<QLangValue>& args,
                       QLangContext& ctx);

// Convert a table to string representation.
QLangValue toStr(const QLangValue& obj, QLangContext& ctx);

// Reserved for inheritance (null in v1).
const ObjectOps* base(const QLangValue& obj);

// The global table ObjectOps instance.
extern const ObjectOps kTableOps;

} // namespace table_ops

} // namespace qlang