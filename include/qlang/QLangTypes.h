#pragma once

// Runtime type descriptor for external C++ types (docs/EXTERNAL_TYPES.md).
//
// A `TypeDescriptor` is the runtime form of one registered type. The handle
// obtained from `import<T>()` / `registerType()` is a
// `std::shared_ptr<TypeDescriptor>`: it stays valid as long as the host (or
// any script value) holds it, including across `clear()` (Phase 3).

#include <functional>
#include <memory>
#include <string>
#include <typeinfo>
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
  std::vector<SignalMeta> signals;  // QL_SIGNAL
  // Concrete C++ type of constructed objects (Phase 3): copied from the
  // TypeMeta at registration; `typeCheck` validates a resolved object
  // pointer against it (null when the ctor returns a void-typed shared_ptr).
  const std::type_info* typeInfo = nullptr;
  std::function<bool(const void*)> typeCheck = nullptr;
  // Describable shape (portable-module-plan P1), copied from the TypeMeta:
  // clause presence + the typed QL_CTOR's argument tags (empty + hasCtor
  // means the context-taking form). Members/signals carry their own tags.
  bool hasStr = false;
  bool hasCtor = false;
  std::vector<QTypeTag> ctorArgTags;
};

} // namespace qlang
