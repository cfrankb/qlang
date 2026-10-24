#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "QLangVariant.h"

namespace qlang {

class QLangValue;

// Convert any numeric QLangValue to int64_t. Returns 0 for non-numeric types.
int64_t toInt64(const QLangValue &v);

// The storage type of every script variable. Mirrors the spec's variable
// types: string, blob, byte, word, short, int, unsigned, array, float, table.
class QLangValue {
public:
  enum class Type {
    Nil,      // unset / absent
    Bool,     // bool (true/false; stored as Int 1/0 at runtime)
    Char,     // int8_t (signed; distinct from Byte which is uint8_t)
    Byte,     // uint8_t
    Word,     // uint16_t
    Short,    // int16_t
    Int,      // int32_t
    Unsigned, // uint32_t
    Dword,    // uint64_t
    String,   // null-terminated text
    Blob,     // arbitrary binary data
    Array,    // list of mixed values
    Float,    // double
    Table,    // keyed string -> value map
    Function, // user-defined function (arrow closure)
    Generator, // lazy generator (yield-based function)
    Eof         // end-of-file sentinel value
  };

  QLangValue() : m_type(Type::Nil) {}
  explicit QLangValue(Type t) : m_type(t) {}

  // Constructors / factory helpers.
  static QLangValue Byte(uint8_t v) {
    QLangValue q(Type::Byte);
    q.m_storage = v;
    return q;
  }
  static QLangValue Char(int8_t v) {
    QLangValue q(Type::Char);
    q.m_storage = static_cast<int32_t>(v);
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
  static QLangValue Bool(int32_t v) {
    QLangValue q(Type::Bool);
    q.m_storage = v;
    return q;
  }
  static QLangValue Unsigned(uint32_t v) {
    QLangValue q(Type::Unsigned);
    q.m_storage = v;
    return q;
  }
  static QLangValue Dword(uint64_t v) {
    QLangValue q(Type::Dword);
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
  static QLangValue Table() {
    auto m = std::make_shared<std::map<std::string, QLangValue>>();
    QLangValue q(Type::Table);
    q.m_storage = std::move(m);
    return q;
  }
  static QLangValue Function(ClosurePtr v) {
    QLangValue q(Type::Function);
    q.m_storage = std::move(v);
    return q;
  }
  static QLangValue Generator(GeneratorValuePtr v) {
    QLangValue q(Type::Generator);
    q.m_storage = std::move(v);
    return q;
  }

  Type type() const { return m_type; }
  bool isNil() const { return m_type == Type::Nil; }
  bool isEof() const { return m_type == Type::Eof; }
  const char *typeName() const;

  // Accessors. Each throws QLangError (Type kind) if the storage does not
  // match the requested type, so callers get a clear, located failure.
  const uint8_t &asByte() const;
  uint8_t &asByte();
  const int32_t &asChar() const;
  int32_t &asChar();
  const uint16_t &asWord() const;
  uint16_t &asWord();
  const int16_t &asShort() const;
  int16_t &asShort();
  const int32_t &asInt() const;
  int32_t &asInt();
  const uint32_t &asUnsigned() const;
  uint32_t &asUnsigned();
  const uint64_t &asDword() const;
  uint64_t &asDword();
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
  const Closure &asFunction() const;
  Closure &asFunction();
  const GeneratorValue &asGenerator() const;
  GeneratorValue &asGenerator();

private:
  Type m_type;
  QLangVariant m_storage;
};

} // namespace qlang
