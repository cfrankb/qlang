#include "qlang/QLangObjects.h"

#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"
#include "qlang/QLangValue.h"

#include <map>
#include <string>
#include <vector>

namespace qlang {

namespace table_ops {

// Reserved instance/prototype keys for the closed field set (Phase 1).
// `__fields__` holds the field schema (name -> index); `__class__` holds the
// class name for error messages. Must match the keys emitted by the compiler.
static const char* kFieldMetaKey = "__fields__";
static const char* kClassMetaKey = "__class__";
static const char* kConstFieldMetaKey = "__const_fields__";
// Reserved instance key holding the inferred field types (Phase 5): a table
// mapping each type-constrained field name to its type-name string, emitted
// by the compiler and copied onto each instance so SETFIELD can reject values
// that violate the type inferred from the field's default value.
static const char* kFieldTypeMetaKey = "__field_types__";
// Static members (new extension beyond v1): `__statics__` is the shared
// class-level table (prototype + every instance point at the same map).
// `__static_const__` (a set inside `__statics__`) marks read-only statics.
static const char* kStaticMetaKey = "__statics__";
static const char* kStaticConstMetaKey = "__static_const__";

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

// True when a class instance's `name` is a const (read-only) field, per the
// `__const_fields__` set emitted by the compiler (Phase 2).
static bool isConstField(const std::map<std::string, QLangValue>& kv,
                         const std::string& name) {
  auto it = kv.find(kConstFieldMetaKey);
  if (it == kv.end() || it->second.type() != QLangValue::Type::Table)
    return false;
  const auto& cfkv = *it->second.asTable();
  return cfkv.find(name) != cfkv.end();
}

// True when a type name is in the numeric tower (Phase 5). Numeric types are
// mutually assignable (the tower is designed for inter-conversion), so a
// field inferred as one numeric type accepts values of any numeric type.
static bool isNumericType(const std::string& t) {
  return t == "char" || t == "byte" || t == "word" || t == "short" ||
                  t == "int" || t == "unsigned" || t == "qword" || t == "long" || t == "float";
}

// Phase 5: true when a value of type `got` may be stored in a field whose
// inferred type is `expected`. Exact match is always ok; a `nil` inferred
// type means "no constraint"; numeric types are mutually compatible.
static bool fieldTypeCompatible(const std::string& got,
                                const std::string& expected) {
  if (got == expected) return true;
  if (expected == "nil") return true;
  if (isNumericType(got) && isNumericType(expected)) return true;
  return false;
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
      // Statics fallback: not a declared field, but may be a class-level
      // static member (shared `__statics__` table).
      auto st = kv.find(kStaticMetaKey);
      if (st != kv.end() && st->second.type() == QLangValue::Type::Table) {
        const auto& skv = *st->second.asTable();
        auto sit = skv.find(name);
        if (sit != skv.end())
          return sit->second;
      }
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
      // Statics fallback: not a declared field, but may be a class-level
      // static member. Writes go into the shared `__statics__` map.
      auto st = tv->find(kStaticMetaKey);
      if (st != tv->end() && st->second.type() == QLangValue::Type::Table) {
        const auto& skv = *st->second.asTable();
        if (skv.find(name) != skv.end()) {
          // Static const (new extension): reject assignment.
          auto sc = skv.find(kStaticConstMetaKey);
          if (sc != skv.end() && sc->second.type() == QLangValue::Type::Table) {
            const auto& sckv = *sc->second.asTable();
            if (sckv.find(name) != sckv.end())
              throw QLangError(QLangErrorKind::Runtime, 0, 0,
                               "cannot assign to const static field '" +
                                   name + "'");
          }
          // ponytail: no `__field_types__` constraint on static writes —
          // add if a real type-safety need appears.
          st->second.asTable()->operator[](name) = value;
          return;
        }
      }
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       classNameOf(*tv) + " has no field '" + name + "'");
    }
    // Const fields (Phase 2.2): reject assignment to read-only fields.
    if (isConstField(*tv, name)) {
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "cannot assign to const field '" + name + "'");
    }
  }
  // Field type inference (Phase 5): reject a value whose type violates the
  // type inferred from the field's default value (`__field_types__`).
  {
    auto ftt = tv->find(kFieldTypeMetaKey);
    if (ftt != tv->end() && ftt->second.type() == QLangValue::Type::Table) {
      const auto& ftkv = *ftt->second.asTable();
      auto fit = ftkv.find(name);
      if (fit != ftkv.end() && fit->second.type() == QLangValue::Type::String) {
        const std::string& expected = fit->second.asString();
        std::string got = value.typeName();
        if (!fieldTypeCompatible(got, expected)) {
          throw QLangError(
              QLangErrorKind::Type, 0, 0,
              "type mismatch: field '" + name + "' expects " + expected +
              ", got " + got);
        }
      }
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
  // First argument: for a static method resolved from `__statics__`, the
  // hidden anchor is the shared statics table (no self). For an instance
  // method, it is the object (self).
  std::vector<QLangValue> callArgs;
  callArgs.reserve(args.size() + 1);
  const auto& kv = *obj.asTable();
  auto st = kv.find(kStaticMetaKey);
  bool fromStatics = st != kv.end() && st->second.type() == QLangValue::Type::Table &&
                     st->second.asTable()->find(name) != st->second.asTable()->end();
  if (fromStatics)
    callArgs.push_back(st->second);  // anchor = shared statics table
  else
    callArgs.push_back(obj);         // self
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
  // Same dispatch as QLangVM::displayString: `__str__` if present, then the
  // class name for instances, then the generic "<table>".
  if (obj.type() == QLangValue::Type::Table) {
    const auto& kv = *obj.asTable();
    auto it = kv.find("__str__");
    if (it != kv.end() && it->second.type() == QLangValue::Type::Function)
      return ctx.callClosure(it->second, {obj});
    auto kit = kv.find(kClassMetaKey);
    if (kit != kv.end() && kit->second.type() == QLangValue::Type::String)
      return QLangValue::String(kit->second.asString());
  }
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