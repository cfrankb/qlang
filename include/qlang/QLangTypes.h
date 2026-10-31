#pragma once

// Runtime type descriptor for external C++ types (docs/EXTERNAL_TYPES.md).
//
// A `TypeDescriptor` is the context-owned runtime form of one registered
// type. The pointer obtained from `import<T>()` / `registerType()` is stable
// for the context's lifetime; `clear()` invalidates all of them (host code
// must re-run `import<T>()` afterwards).

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "qlang/QLangMeta.h"
#include "qlang/QLangValue.h"

namespace qlang {

// Forward declaration for QLangContext (used in the construct signature).
class QLangContext;

struct TypeDescriptor {
  std::string name;
  std::vector<Member> members;  // deduced from the QL_OBJECT list
  std::function<QLangValue(const void*)> toString;
  std::function<std::shared_ptr<void>(QLangContext&,
                                      const std::vector<QLangValue>&)>
      construct;
  std::vector<EnumMeta> enums;  // QL_ENUM / QL_FLAGS
};

} // namespace qlang
