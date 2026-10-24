#include "qlang/QLangValue.h"

#include "qlang/QLangErrors.h"

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
  case Type::Dword: return "dword";
  case Type::String: return "string";
  case Type::Blob: return "blob";
  case Type::Array: return "array";
  case Type::Float: return "float";
  case Type::Table: return "table";
  case Type::Function: return "function";
  case Type::Generator: return "generator";
  case Type::Eof: return "eof";
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
// asDword reads from the uint64_t storage.
const uint64_t &QLangValue::asDword() const {
  if (m_type != Type::Dword)
    throw QLangError(QLangErrorKind::Type, 0, 0,
                     std::string("expected Dword but value is ") + typeName());
  return std::get<uint64_t>(m_storage);
}
QL_ACCESSOR_GETTER(std::string, String, std::string)
QL_ACCESSOR_GETTER(std::vector<uint8_t>, Blob, std::vector<uint8_t>)
QL_ACCESSOR_GETTER(ArrayValue, Array, ArrayValue)
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
// asDword mut reads from the uint64_t storage.
uint64_t &QLangValue::asDword() {
  if (m_type != Type::Dword)
    throw QLangError(QLangErrorKind::Type, 0, 0,
                     std::string("expected Dword but value is ") + typeName());
  return std::get<uint64_t>(m_storage);
}
QL_ACCESSOR_MUT(std::string, String, std::string)
QL_ACCESSOR_MUT(std::vector<uint8_t>, Blob, std::vector<uint8_t>)
QL_ACCESSOR_MUT(ArrayValue, Array, ArrayValue)
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

int64_t toInt64(const QLangValue &v) {
  switch (v.type()) {
  case QLangValue::Type::Bool: return static_cast<int64_t>(v.asInt());
  case QLangValue::Type::Char: return static_cast<int64_t>(v.asChar());
  case QLangValue::Type::Byte: return static_cast<int64_t>(v.asByte());
  case QLangValue::Type::Word: return static_cast<int64_t>(v.asWord());
  case QLangValue::Type::Short: return static_cast<int64_t>(v.asShort());
  case QLangValue::Type::Int: return static_cast<int64_t>(v.asInt());
  case QLangValue::Type::Unsigned: return static_cast<int64_t>(v.asUnsigned());
  case QLangValue::Type::Dword: {
    uint64_t u = v.asDword();
    if (u > static_cast<uint64_t>(INT64_MAX))
      throw QLangError(QLangErrorKind::Runtime, 0, 0,
                       "dword value exceeds int64_t range");
    return static_cast<int64_t>(u);
  }
  case QLangValue::Type::Float: return static_cast<int64_t>(v.asFloat());
  default: return 0;
  }
}

} // namespace qlang
