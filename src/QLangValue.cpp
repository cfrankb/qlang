#include "qlang/QLangValue.h"

#include "qlang/QLangErrors.h"
#include "qlang/QLangTypes.h"  // full TypeDescriptor for ExternalRef::self()

namespace qlang {

const char *QLangValue::typeName() const {
  switch (m_type) {
  case Type::Nil: return "nil";
  case Type::Bool: return "bool";
  case Type::Char: return "char";
  case Type::Byte: return "byte";
  case Type::Word: return "word";
  case Type::Short: return "short";
  case Type::Int: return "int";
  case Type::Unsigned: return "unsigned";
  case Type::Qword: return "qword";
  case Type::Long: return "long";
  case Type::Set: return "set";
  case Type::String: return "string";
  case Type::Blob: return "blob";
  case Type::Array: return "array";
  case Type::Float: return "float";
  case Type::Table: return "table";
  case Type::Function: return "function";
  case Type::Generator: return "generator";
  case Type::Object: return "object";
  case Type::Eof: return "eof";
  case Type::External: return "external";
  case Type::BoundMethod: return "bound method";
  case Type::TypeRef: return "type";
  }
  return "?";
}

#define QL_ACCESSOR_GETTER(StorageType, TypeName, ReturnType)                  \
  const ReturnType &QLangValue::as##TypeName() const {                        \
    if (m_type != Type::TypeName)                                              \
      throw QLangError(QLangErrorKind::Type, 0, 0,                            \
                       std::string("expected ") + #TypeName +                  \
                           " but value is " + typeName());                     \
    return std::get<StorageType>(m_storage);                                   \
  }

QL_ACCESSOR_GETTER(uint8_t, Byte, uint8_t)
// asChar reads from the int32_t storage (Char stores int8_t there).
const int32_t &QLangValue::asChar() const {
  if (m_type != Type::Char)
    throw QLangError(QLangErrorKind::Type, 0, 0,
                     std::string("expected Char but value is ") + typeName());
  return std::get<int32_t>(m_storage);
}
QL_ACCESSOR_GETTER(uint16_t, Word, uint16_t)
QL_ACCESSOR_GETTER(int16_t, Short, int16_t)
// asInt accepts Int, Bool, and Char (all store in int32_t storage).
const int32_t &QLangValue::asInt() const {
  if (m_type != Type::Int && m_type != Type::Bool && m_type != Type::Char)
    throw QLangError(QLangErrorKind::Type, 0, 0,
                     std::string("expected Int, Bool or Char but value is ") +
                         typeName());
  return std::get<int32_t>(m_storage);
}
QL_ACCESSOR_GETTER(uint32_t, Unsigned, uint32_t)
// asQword reads from the uint64_t storage.
const uint64_t &QLangValue::asQword() const {
  if (m_type != Type::Qword)
    throw QLangError(QLangErrorKind::Type, 0, 0,
                     std::string("expected Qword but value is ") + typeName());
  return std::get<uint64_t>(m_storage);
}
// asLong reads from the int64_t storage.
QL_ACCESSOR_GETTER(int64_t, Long, int64_t)
QL_ACCESSOR_GETTER(std::string, String, std::string)
QL_ACCESSOR_GETTER(std::vector<uint8_t>, Blob, std::vector<uint8_t>)
QL_ACCESSOR_GETTER(ArrayValue, Array, ArrayValue)
QL_ACCESSOR_GETTER(ArrayValue, Set, ArrayValue)
QL_ACCESSOR_GETTER(double, Float, double)
QL_ACCESSOR_GETTER(TableValue, Table, TableValue)

#undef QL_ACCESSOR_GETTER

#define QL_ACCESSOR_MUT(StorageType, TypeName, ReturnType)                     \
  ReturnType &QLangValue::as##TypeName() {                                     \
    if (m_type != Type::TypeName)                                              \
      throw QLangError(QLangErrorKind::Type, 0, 0,                            \
                       std::string("expected ") + #TypeName +                  \
                           " but value is " + typeName());                     \
    return std::get<StorageType>(m_storage);                                   \
  }

QL_ACCESSOR_MUT(uint8_t, Byte, uint8_t)
// asChar mut reads from the int32_t storage (Char stores int8_t there).
int32_t &QLangValue::asChar() {
  if (m_type != Type::Char)
    throw QLangError(QLangErrorKind::Type, 0, 0,
                     std::string("expected Char but value is ") + typeName());
  return std::get<int32_t>(m_storage);
}
QL_ACCESSOR_MUT(uint16_t, Word, uint16_t)
QL_ACCESSOR_MUT(int16_t, Short, int16_t)
// asInt mut accepts Int, Bool, and Char (all store in int32_t storage).
int32_t &QLangValue::asInt() {
  if (m_type != Type::Int && m_type != Type::Bool && m_type != Type::Char)
    throw QLangError(QLangErrorKind::Type, 0, 0,
                     std::string("expected Int, Bool or Char but value is ") +
                         typeName());
  return std::get<int32_t>(m_storage);
}
QL_ACCESSOR_MUT(uint32_t, Unsigned, uint32_t)
// asQword mut reads from the uint64_t storage.
uint64_t &QLangValue::asQword() {
  if (m_type != Type::Qword)
    throw QLangError(QLangErrorKind::Type, 0, 0,
                     std::string("expected Qword but value is ") + typeName());
  return std::get<uint64_t>(m_storage);
}
// asLong mut reads from the int64_t storage.
QL_ACCESSOR_MUT(int64_t, Long, int64_t)
QL_ACCESSOR_MUT(std::string, String, std::string)
QL_ACCESSOR_MUT(std::vector<uint8_t>, Blob, std::vector<uint8_t>)
QL_ACCESSOR_MUT(ArrayValue, Array, ArrayValue)
QL_ACCESSOR_MUT(ArrayValue, Set, ArrayValue)
QL_ACCESSOR_MUT(double, Float, double)
QL_ACCESSOR_MUT(TableValue, Table, TableValue)

#undef QL_ACCESSOR_MUT

const Closure &QLangValue::asFunction() const {
  if (m_type != Type::Function)
    throw QLangError(QLangErrorKind::Type, 0, 0,
                     std::string("expected Function but value is ") + typeName());
  return *std::get<ClosurePtr>(m_storage);
}

Closure &QLangValue::asFunction() {
  if (m_type != Type::Function)
    throw QLangError(QLangErrorKind::Type, 0, 0,
                     std::string("expected Function but value is ") + typeName());
  return *std::get<ClosurePtr>(m_storage);
}

const GeneratorValue &QLangValue::asGenerator() const {
  if (m_type != Type::Generator)
    throw QLangError(QLangErrorKind::Type, 0, 0,
                     std::string("expected Generator but value is ") + typeName());
  return *std::get<GeneratorValuePtr>(m_storage);
}

GeneratorValue &QLangValue::asGenerator() {
  if (m_type != Type::Generator)
    throw QLangError(QLangErrorKind::Type, 0, 0,
                     std::string("expected Generator but value is ") + typeName());
  return *std::get<GeneratorValuePtr>(m_storage);
}

const ObjectValue &QLangValue::asObject() const {
  if (m_type != Type::Object)
    throw QLangError(QLangErrorKind::Type, 0, 0,
                     std::string("expected Object but value is ") + typeName());
  return std::get<ObjectValue>(m_storage);
}

ObjectValue &QLangValue::asObject() {
  if (m_type != Type::Object)
    throw QLangError(QLangErrorKind::Type, 0, 0,
                     std::string("expected Object but value is ") + typeName());
  return std::get<ObjectValue>(m_storage);
}

const ExternalRef &QLangValue::asExternal() const {
  if (m_type != Type::External)
    throw QLangError(QLangErrorKind::Type, 0, 0,
                     std::string("expected External but value is ") + typeName());
  return std::get<ExternalRef>(m_storage);
}

ExternalRef &QLangValue::asExternal() {
  if (m_type != Type::External)
    throw QLangError(QLangErrorKind::Type, 0, 0,
                     std::string("expected External but value is ") + typeName());
  return std::get<ExternalRef>(m_storage);
}

const BoundMethodRef &QLangValue::asBoundMethod() const {
  if (m_type != Type::BoundMethod)
    throw QLangError(QLangErrorKind::Type, 0, 0,
                     std::string("expected BoundMethod but value is ") +
                         typeName());
  return std::get<BoundMethodRef>(m_storage);
}

BoundMethodRef &QLangValue::asBoundMethod() {
  if (m_type != Type::BoundMethod)
    throw QLangError(QLangErrorKind::Type, 0, 0,
                     std::string("expected BoundMethod but value is ") +
                         typeName());
  return std::get<BoundMethodRef>(m_storage);
}

const TypeRefRef &QLangValue::asTypeRef() const {
  if (m_type != Type::TypeRef)
    throw QLangError(QLangErrorKind::Type, 0, 0,
                     std::string("expected TypeRef but value is ") + typeName());
  return std::get<TypeRefRef>(m_storage);
}

TypeRefRef &QLangValue::asTypeRef() {
  if (m_type != Type::TypeRef)
    throw QLangError(QLangErrorKind::Type, 0, 0,
                     std::string("expected TypeRef but value is ") + typeName());
  return std::get<TypeRefRef>(m_storage);
}

// ExternalRef object resolution: owned.get() if set, else borrowed.lock();
// an expired borrow means the engine object is gone.
const void *ExternalRef::self() const {
  if (owned) return owned.get();
  if (auto p = borrowed.lock()) return p.get();
  throw QLangError(QLangErrorKind::Runtime, 0, 0,
                   std::string("object destroyed: ") + (type ? type->name : "?"));
}

void *ExternalRef::selfMut() {
  if (owned) return owned.get();
  if (auto p = borrowed.lock()) return p.get();
  throw QLangError(QLangErrorKind::Runtime, 0, 0,
                   std::string("object destroyed: ") + (type ? type->name : "?"));
}

int64_t toInt64(const QLangValue &v) {
  switch (v.type()) {
  case QLangValue::Type::Bool: return static_cast<int64_t>(v.asInt());
  case QLangValue::Type::Char: return static_cast<int64_t>(v.asChar());
  case QLangValue::Type::Byte: return static_cast<int64_t>(v.asByte());
  case QLangValue::Type::Word: return static_cast<int64_t>(v.asWord());
  case QLangValue::Type::Short: return static_cast<int64_t>(v.asShort());
  case QLangValue::Type::Int: return static_cast<int64_t>(v.asInt());
  case QLangValue::Type::Unsigned: return static_cast<int64_t>(v.asUnsigned());
  case QLangValue::Type::Qword: {
    uint64_t u = v.asQword();
    if (u > static_cast<uint64_t>(INT64_MAX))
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "Qword value exceeds int64_t range");
    return static_cast<int64_t>(u);
  }
  case QLangValue::Type::Long: return v.asLong();
  case QLangValue::Type::Float: return static_cast<int64_t>(v.asFloat());
  default: return 0;
  }
}

// ---------------------------------------------------------------------------
// as<T>() — QValue -> C++ conversion for external-type bindings (Task 2).
// Wrong script type -> QLangError (Type kind). The numeric specializations
// follow the existing asInt/asFloat rules: asInt accepts Int/Bool/Char, and
// as<double> also accepts Int (1 -> 1.0).
// ---------------------------------------------------------------------------
namespace {
[[noreturn]] void asTypeError(const QLangValue &v, const char *t) {
  throw QLangError(QLangErrorKind::Type, 0, 0,
                   std::string("cannot convert ") + v.typeName() +
                       " to " + t);
}
}

template <> bool QLangValue::as<bool>() const {
  switch (m_type) {
  case Type::Bool:
  case Type::Char:
  case Type::Int: return this->asInt() != 0;
  default: asTypeError(*this, "bool");
  }
}

template <> int32_t QLangValue::as<int32_t>() const {
  // asInt rules: Int/Bool/Char storage.
  if (m_type != Type::Int && m_type != Type::Bool && m_type != Type::Char)
    asTypeError(*this, "int");
  return std::get<int32_t>(m_storage);
}

template <> int64_t QLangValue::as<int64_t>() const {
  // Any numeric script type; non-numeric is a type error.
  auto t = m_type;
  if (t == Type::Bool || t == Type::Char || t == Type::Byte ||
      t == Type::Word || t == Type::Short || t == Type::Int ||
      t == Type::Unsigned || t == Type::Qword || t == Type::Long ||
      t == Type::Float)
    return toInt64(*this);
  asTypeError(*this, "long");
}

template <> double QLangValue::as<double>() const {
  auto t = m_type;
  if (t == Type::Float) return this->asFloat();
  if (t == Type::Int || t == Type::Bool || t == Type::Char ||
      t == Type::Byte || t == Type::Word || t == Type::Short ||
      t == Type::Unsigned || t == Type::Qword || t == Type::Long)
    return static_cast<double>(toInt64(*this));
  asTypeError(*this, "float");
}

template <> float QLangValue::as<float>() const {
  return static_cast<float>(as<double>());
}

template <> std::string QLangValue::as<std::string>() const {
  if (m_type != Type::String) asTypeError(*this, "string");
  return this->asString();
}


} // namespace qlang
