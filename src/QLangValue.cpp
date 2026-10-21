#include "qlang/QLangValue.h"

#include "qlang/QLangErrors.h"

namespace qlang {

const char *QLangValue::typeName() const {
  switch (m_type) {
  case Type::Nil: return "nil";
  case Type::Bool: return "bool";
  case Type::Byte: return "byte";
  case Type::Word: return "word";
  case Type::Short: return "short";
  case Type::Int: return "int";
  case Type::Unsigned: return "unsigned";
  case Type::String: return "string";
  case Type::Blob: return "blob";
  case Type::Array: return "array";
  case Type::Float: return "float";
  case Type::Table: return "table";
  case Type::Function: return "function";
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
QL_ACCESSOR_GETTER(uint16_t, Word, uint16_t)
QL_ACCESSOR_GETTER(int16_t, Short, int16_t)
// asInt accepts both Int and Bool (they share int32_t storage).
const int32_t &QLangValue::asInt() const {
  if (m_type != Type::Int && m_type != Type::Bool)
    throw QLangError(QLangErrorKind::Type, 0, 0,
                     std::string("expected Int or Bool but value is ") +
                         typeName());
  return std::get<int32_t>(m_storage);
}
QL_ACCESSOR_GETTER(uint32_t, Unsigned, uint32_t)
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
QL_ACCESSOR_MUT(uint16_t, Word, uint16_t)
QL_ACCESSOR_MUT(int16_t, Short, int16_t)
// asInt mut accepts both Int and Bool (they share int32_t storage).
int32_t &QLangValue::asInt() {
  if (m_type != Type::Int && m_type != Type::Bool)
    throw QLangError(QLangErrorKind::Type, 0, 0,
                     std::string("expected Int or Bool but value is ") +
                         typeName());
  return std::get<int32_t>(m_storage);
}
QL_ACCESSOR_MUT(uint32_t, Unsigned, uint32_t)
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

} // namespace qlang
