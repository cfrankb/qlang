#pragma once

// CRTP base for external C++ types (docs/EXTERNAL_TYPES.md). The class name
// is written exactly once, as this base: `struct Point : qlang::QLObject<Point>`.
// It exposes a `SelfType` alias that the QL_* entry macros use to form
// qualified member pointers (`&SelfType::x`). The virtual destructor makes
// every external type polymorphic so the Phase 3 type check (dynamic_cast
// from a void* at first use) is well-formed; no other base-state or behavior
// is added.
//
// This header is deliberately dependency-free (it is included by QValue.h,
// which QLangMeta.h in turn includes) so the type-check helpers in QValue.h
// can cast through the polymorphic base without an include cycle.

namespace qlang {

template <class Self> struct QLObject {
  using SelfType = Self;
  virtual ~QLObject() = default;
};

} // namespace qlang
