#include "qlang/QLangObjects.h"

#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"

namespace qlang {

namespace table_ops {

// Reserved instance/prototype keys for the closed field set (Phase 1).
// `__fields__` holds the field schema (name -> index); `__class__` holds the
// class name for error messages. Must match the keys emitted by the compiler.
static const char* kFieldMetaKey = "__fields__";
static const char* kClassMetaKey = "__class__";

// True when the table is a class instance: it carries the `__fields__` schema.
static bool isClassInstance(const std::map<std::string, QLangValue>& kv) {
  auto it = kv.find(kFieldMetaKey);
  return it != kv.end() && it->second.type() == QLangValue::Type::Table;
}

// Class name for closed-field-set error messages ("<class>" if absent).
static std::string classNameOf(const std::map<std::string, QLangValue>& kv) {
  auto it = kv.find(kClassMetaKey);
  if (it != kv.end() && it->second.type() == QLangValue::Type::String)
    return it->second.asString();
  return "<class>";
}

QLangValue getField(const QLangValue& obj, const std::string& name,
                    QLangContext& ctx) {
  if (obj.type() != QLangValue::Type::Table) {
    throw QLangError(QLangErrorKind::Type, 0, 0,
                     "cannot access member of non-table value");
  }
  const auto& kv = *obj.asTable();
  auto it = kv.find(name);

  // Closed field set (Phase 1.3): a class instance rejects access to a member
  // that is neither already present (methods, metadata, set fields) nor a
  // declared field. A declared-but-unset field reads as nil.
  if (it == kv.end() && isClassInstance(kv)) {
    const auto& fkv = *kv.find(kFieldMetaKey)->second.asTable();
    if (fkv.find(name) == fkv.end()) {
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       classNameOf(kv) + " has no field '" + name + "'");
    }
    return QLangValue();
  }

  if (it == kv.end()) {
    throw QLangError(QLangErrorKind::Runtime, 0, 0,
                     "table has no member '" + name + "'");
  }
  return it->second;
}

void setField(QLangValue& obj, const std::string& name,
              const QLangValue& value, QLangContext& ctx) {
  if (obj.type() != QLangValue::Type::Table) {
    throw QLangError(QLangErrorKind::Type, 0, 0,
                     "cannot assign member of non-table value");
  }
  auto tv = obj.asTable();
  if (!tv) {
    tv = std::make_shared<std::map<std::string, QLangValue>>();
    obj = QLangValue::Table(tv);
  }
  // Closed field set (Phase 1.3): a class instance only allows setting
  // declared fields (methods and metadata are not settable).
  if (isClassInstance(*tv)) {
    const auto& fkv = *tv->find(kFieldMetaKey)->second.asTable();
    if (fkv.find(name) == fkv.end()) {
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       classNameOf(*tv) + " has no field '" + name + "'");
    }
  }
  tv->operator[](name) = value;
}

QLangValue callMethod(const QLangValue& obj, const std::string& name,
                       const std::vector<QLangValue>& args,
                       QLangContext& ctx) {
  // Get the method (closure) from the table.
  QLangValue method = getField(obj, name, ctx);
  if (method.type() != QLangValue::Type::Function) {
    throw QLangError(QLangErrorKind::Type, 0, 0,
                     "member '" + name + "' is not a function");
  }
  // Call the closure with the object as the first argument (self).
  std::vector<QLangValue> callArgs;
  callArgs.reserve(args.size() + 1);
  callArgs.push_back(obj);  // self
  for (const auto& arg : args) {
    callArgs.push_back(arg);
  }
  // Create a closure value and call it.
  QLangValue closure = method;
  // We need to call the closure through the VM. For now, we'll use
  // QLangContext::call to invoke it.
  // This is a simplified implementation - the full implementation
  // would need access to the VM to call closures properly.
  // For Phase 0, we just need the dispatch layer to exist.
  // The actual closure calling will be handled by the VM's CALLCLOS opcode.
  return ctx.callClosure(method, callArgs);
}

QLangValue toStr(const QLangValue& obj, QLangContext& ctx) {
  // Return "<table>" as the string representation.
  // This is the default for tables; classes will override with their own.
  return QLangValue::String("<table>");
}

const ObjectOps* base(const QLangValue& obj) {
  // Reserved for inheritance - null in v1.
  return nullptr;
}

// The global table ObjectOps instance.
const ObjectOps kTableOps = {
    .kindName = "table",
    .getField = getField,
    .setField = setField,
    .callMethod = callMethod,
    .toStr = toStr,
    .base = base,
};

} // namespace table_ops

const ObjectOps* getObjectOps(const QLangValue& obj) {
  // Tables use the table ops dispatch layer.
  if (obj.type() == QLangValue::Type::Table) {
    return &table_ops::kTableOps;
  }
  // Objects use their own ops from the ObjectDescriptor.
  if (obj.type() == QLangValue::Type::Object) {
    const auto& desc = obj.asObject();
    return desc->ops;
  }
  return nullptr;
}

} // namespace qlang