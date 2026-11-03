#pragma once

// Public-facing alias and type-construction helpers for QLangValue.
// Host code uses QValue (short for "QLang Value") instead of the internal
// QLangValue name. Free functions let you construct values from common C++
// types without spelling out the static-factory syntax.

#include "qlang/QLangObject.h"
#include "qlang/QLangValue.h"

namespace qlang
{

    // Public alias — host code uses this name; the internal class is QLangValue.
    using QValue = QLangValue;

    // Helpers: construct a QValue from common C++ types. Each returns a value
    // whose runtime type matches the argument's natural QLang type.

    inline QValue qval_nil() { return QValue(); }

    inline QValue qval_bool(bool v) { return QValue::Bool(v ? 1 : 0); }

    inline QValue qval_byte(uint8_t v) { return QValue::Byte(v); }

    inline QValue qval_char(int8_t v) { return QValue::Char(v); }

    inline QValue qval_word(uint16_t v) { return QValue::Word(v); }

    inline QValue qval_short(int16_t v) { return QValue::Short(v); }

    inline QValue qval_int(int32_t v) { return QValue::Int(v); }

    inline QValue qval_unsigned(uint32_t v) { return QValue::Unsigned(v); }

    inline QValue qval_qword(uint64_t v) { return QValue::Qword(v); }

    inline QValue qval_dword(uint64_t v) { return QValue::Unsigned(v); }

    inline QValue qval_float(double v) { return QValue::Float(v); }

    inline QValue qval_str(const char *v) { return QValue::String(std::string(v)); }

    inline QValue qval_str(const std::string &v) { return QValue::String(v); }

    inline QValue qval_str(std::string &&v) { return QValue::String(std::move(v)); }

    inline QValue qval_blob(const std::vector<uint8_t> &v) { return QValue::Blob(v); }

    inline QValue qval_blob(std::vector<uint8_t> &&v) { return QValue::Blob(std::move(v)); }

    inline QValue qval_array(ArrayValue v) { return QValue::Array(std::move(v)); }

    inline QValue qval_table(TableValue v) { return QValue::Table(v); }

    // -------------------------------------------------------------------
    // External C++ type constructors (docs/EXTERNAL_TYPES.md).
    // `type` is the shared handle from import<T>() / registerType()
    // (Phase 3): it keeps the descriptor alive as long as the value does.
    // -------------------------------------------------------------------
    inline QValue qval_external(const std::shared_ptr<TypeDescriptor> &type,
                                const std::shared_ptr<void> &owned) {
        return QValue::External(type, owned);
    }

    inline QValue qval_external(const std::shared_ptr<TypeDescriptor> &type,
                                const std::weak_ptr<void> &borrowed) {
        return QValue::External(type, borrowed);
    }

    // Typed handoff (Phase 3): the concrete type T is recorded so a wrong
    // pointer is a clean type error at first use, not UB. The check casts
    // through the polymorphic QLObject<T> base (dynamic_cast cannot source
    // from a void*).
    template <class T>
    QValue qval_external_owned(const std::shared_ptr<TypeDescriptor> &type,
                               const std::shared_ptr<T> &p) {
        ExternalRef ref;
        ref.type = type;
        ref.owned = p;
        ref.expected = &typeid(T);
        ref.check = [](const void *q) {
          return dynamic_cast<const T *>(static_cast<const QLObject<T> *>(q)) != nullptr;
        };
        return QValue::External(std::move(ref));
    }

    template <class T>
    QValue qval_external_borrowed(const std::shared_ptr<TypeDescriptor> &type,
                                  const std::weak_ptr<T> &p) {
        ExternalRef ref;
        ref.type = type;
        ref.borrowed = p;
        ref.expected = &typeid(T);
        ref.check = [](const void *q) {
          return dynamic_cast<const T *>(static_cast<const QLObject<T> *>(q)) != nullptr;
        };
        return QValue::External(std::move(ref));
    }

    inline QValue qval_bound_method(const Member *member,
                                    const ExternalRef &target) {
        return QValue::BoundMethod(member, target);
    }

    inline QValue qval_type_ref(const std::shared_ptr<TypeDescriptor> &type) {
        return QValue::TypeRef(type);
    }

    // -------------------------------------------------------------------
    // Generic C++ -> QValue for scalars (used by meta bindings). These map
    // a natural C++ type to its QLang value; the overloads keep the call
    // site deduction-based so `qval_of(member)` works for any supported T.
    // -------------------------------------------------------------------
    inline QValue qval_of(bool v) { return QValue::Bool(v ? 1 : 0); }
    inline QValue qval_of(int32_t v) { return QValue::Int(v); }
    inline QValue qval_of(uint32_t v) { return QValue::Unsigned(v); }
    inline QValue qval_of(uint8_t v) { return QValue::Byte(v); }
    inline QValue qval_of(uint16_t v) { return QValue::Word(v); }
    inline QValue qval_of(int16_t v) { return QValue::Short(v); }
    inline QValue qval_of(int64_t v) { return QValue::Long(v); }
    inline QValue qval_of(uint64_t v) { return QValue::Qword(v); }
    inline QValue qval_of(double v) { return QValue::Float(v); }
    inline QValue qval_of(float v) { return QValue::Float(static_cast<double>(v)); }
    inline QValue qval_of(const std::string &v) { return QValue::String(v); }
    inline QValue qval_of(const char *v) { return QValue::String(std::string(v)); }

    // Container helpers: a vector of convertible elements -> a QLang array,
    // an optional -> its value or Nil. Used by meta bindings for member
    // types that are collections.
    template <class T>
    QValue qval_vec(const std::vector<T> &v) {
        ArrayValue arr;
        arr.reserve(v.size());
        for (const auto &e : v)
            arr.push_back(qval_of(e));
        return QValue::Array(std::move(arr));
    }

    template <class T>
    QValue qval_opt(const std::optional<T> &o) {
        if (!o) return QValue();
        return qval_of(*o);
    }

} // namespace qlang
