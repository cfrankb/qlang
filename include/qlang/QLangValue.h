#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace qlang {

class QLangValue; // forward declaration: ArrayValue references it recursively

// A script array is a homogeneous C++ container of mixed script values.
// The value itself is recursively a QLangValue, so arrays nest freely.
using ArrayValue = std::vector<QLangValue>;

// A script table is a keyed map of string -> script value (spec: table).
using TableValue = std::map<std::string, QLangValue>;

// The storage type of every script variable. Mirrors the spec's variable
// types: string, blob, byte, word, short, int, unsigned, array, float, table.
class QLangValue {
public:
  enum class Type {
    Nil,      // unset / absent
    Byte,     // uint8_t
    Word,     // uint16_t
    Short,    // int16_t
    Int,      // int32_t
    Unsigned, // uint32_t
    String,   // null-terminated text
    Blob,     // arbitrary binary data
    Array,    // list of mixed values
    Float,    // double
    Table     // keyed string -> value map
  };

  QLangValue() : m_type(Type::Nil) {}
  explicit QLangValue(Type t) : m_type(t) {}

  // Constructors / factory helpers.
  static QLangValue Byte(uint8_t v) {
    QLangValue q(Type::Byte);
    q.m_storage = v;
    return q;
  }
  static QLangValue Word(uint16_t v) {
    QLangValue q(Type::Word);
    q.m_storage = v;
    return q;
  }
  static QLangValue Short(int16_t v) {
    QLangValue q(Type::Short);
    q.m_storage = v;
    return q;
  }
  static QLangValue Int(int32_t v) {
    QLangValue q(Type::Int);
    q.m_storage = v;
    return q;
  }
  static QLangValue Unsigned(uint32_t v) {
    QLangValue q(Type::Unsigned);
    q.m_storage = v;
    return q;
  }
  static QLangValue String(std::string v) {
    QLangValue q(Type::String);
    q.m_storage = std::move(v);
    return q;
  }
  static QLangValue Blob(std::vector<uint8_t> v) {
    QLangValue q(Type::Blob);
    q.m_storage = std::move(v);
    return q;
  }
  static QLangValue Array(ArrayValue v) {
    QLangValue q(Type::Array);
    q.m_storage = std::move(v);
    return q;
  }
  static QLangValue Float(double v) {
    QLangValue q(Type::Float);
    q.m_storage = v;
    return q;
  }
  static QLangValue Table(TableValue v) {
    QLangValue q(Type::Table);
    q.m_storage = std::move(v);
    return q;
  }

  Type type() const { return m_type; }
  bool isNil() const { return m_type == Type::Nil; }
  const char *typeName() const;

  // Accessors. Each throws QLangError (Type kind) if the storage does not
  // match the requested type, so callers get a clear, located failure.
  const uint8_t &asByte() const;
  uint8_t &asByte();
  const uint16_t &asWord() const;
  uint16_t &asWord();
  const int16_t &asShort() const;
  int16_t &asShort();
  const int32_t &asInt() const;
  int32_t &asInt();
  const uint32_t &asUnsigned() const;
  uint32_t &asUnsigned();
  const std::string &asString() const;
  std::string &asString();
  const std::vector<uint8_t> &asBlob() const;
  std::vector<uint8_t> &asBlob();
  const ArrayValue &asArray() const;
  ArrayValue &asArray();
  const double &asFloat() const;
  double &asFloat();
  const TableValue &asTable() const;
  TableValue &asTable();

private:
  Type m_type;
  std::variant<std::monostate, uint8_t, uint16_t, int16_t, int32_t, uint32_t,
               double, std::string, std::vector<uint8_t>, ArrayValue,
               TableValue>
      m_storage;
};

} // namespace qlang
