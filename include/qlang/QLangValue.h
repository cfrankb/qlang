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
    Qword,    // uint64_t
    String,   // null-terminated text
    Blob,     // arbitrary binary data
    Array,    // list of mixed values
    Float,    // double
    Table,    // keyed string -> value map
    Function, // user-defined function (arrow closure)
    Generator, // lazy generator (yield-based function)
    Object,   // class instance (uses ObjectOps dispatch)
    Eof,      // end-of-file sentinel value
    Long,     // int64_t
    Set,      // set of unique values (ArrayValue storage)
    // External C++ types (docs/EXTERNAL_TYPES.md). Appended after Set so the
    // durable serialization tags (0-19) stay stable; the three are
    // non-durable and the codec emits tag 0 (Nil) for them.
    External,    // a view over exactly one C++ object (ExternalRef)
    BoundMethod, // a callable method of an External object (BoundMethodRef)
    TypeRef      // the type itself as a value (TypeRefRef)
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
  static QLangValue Qword(uint64_t v) {
    QLangValue q(Type::Qword);
    q.m_storage = v;
    return q;
  }
  static QLangValue Long(int64_t v) {
    QLangValue q(Type::Long);
    q.m_storage = v;
    return q;
  }
  static QLangValue String(std::string v) {
    QLangValue q(Type::String);
    q.m_storage = std::move(v);
    return q;
  }
  // Container factories (Phase 2: array/set/blob are pointer-backed).
  // Each type has three forms: adopt an existing payload (SHARE it, used by
  // `deepcopy` and by anything that must not fork the storage), build a FRESH
  // payload from elements, and build a fresh empty payload. The zero-argument
  // form exists because `QLangValue::Blob({})` would be ambiguous between the
  // payload and the element-vector overloads.
  static QLangValue Blob(BlobValue v) {
    QLangValue q(Type::Blob);
    q.m_storage = v ? std::move(v) : std::make_shared<std::vector<uint8_t>>();
    return q;
  }
  static QLangValue Blob(std::vector<uint8_t> v) {
    auto p = std::make_shared<std::vector<uint8_t>>(std::move(v));
    QLangValue q(Type::Blob);
    q.m_storage = std::move(p);
    return q;
  }
  static QLangValue Blob() { return Blob(std::vector<uint8_t>{}); }
  static QLangValue Array(ArrayValue v) {
    QLangValue q(Type::Array);
    q.m_storage = v ? std::move(v) : std::make_shared<std::vector<QLangValue>>();
    return q;
  }
  static QLangValue Array(std::vector<QLangValue> v) {
    auto p = std::make_shared<std::vector<QLangValue>>(std::move(v));
    QLangValue q(Type::Array);
    q.m_storage = std::move(p);
    return q;
  }
  static QLangValue Array() { return Array(std::vector<QLangValue>{}); }
  static QLangValue Set(ArrayValue v) {
    QLangValue q(Type::Set);
    q.m_storage = v ? std::move(v) : std::make_shared<std::vector<QLangValue>>();
    return q;
  }
  static QLangValue Set(std::vector<QLangValue> v) {
    auto p = std::make_shared<std::vector<QLangValue>>(std::move(v));
    QLangValue q(Type::Set);
    q.m_storage = std::move(p);
    return q;
  }
  static QLangValue Set() { return Set(std::vector<QLangValue>{}); }
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
  static QLangValue Object(ObjectValue v) {
    QLangValue q(Type::Object);
    q.m_storage = std::move(v);
    return q;
  }
  // External C++ types (docs/EXTERNAL_TYPES.md). `type` is a shared handle
  // (Phase 3): the descriptor outlives the value. `owned` is set when the
  // script constructed the object; `borrowed` when the engine handed it
  // over. The untyped factories carry no type check; use the typed
  // qval_external_owned<T>/qval_external_borrowed<T> helpers (QValue.h) for
  // a clean type-mismatch error at first use.
  static QLangValue External(const std::shared_ptr<TypeDescriptor> &type,
                             const std::shared_ptr<void> &owned) {
    QLangValue q(Type::External);
    q.m_storage = ExternalRef{type, owned, {}, nullptr, nullptr};
    return q;
  }
  static QLangValue External(const std::shared_ptr<TypeDescriptor> &type,
                             const std::weak_ptr<void> &borrowed) {
    QLangValue q(Type::External);
    q.m_storage = ExternalRef{type, {}, borrowed, nullptr, nullptr};
    return q;
  }
  // From a fully prepared ref (typed handoff helpers set expected/check).
  static QLangValue External(ExternalRef ref) {
    QLangValue q(Type::External);
    q.m_storage = std::move(ref);
    return q;
  }
  static QLangValue BoundMethod(const Member *member,
                                const ExternalRef &target) {
    QLangValue q(Type::BoundMethod);
    q.m_storage = BoundMethodRef{member, target};
    return q;
  }
  static QLangValue TypeRef(const std::shared_ptr<TypeDescriptor> &type) {
    QLangValue q(Type::TypeRef);
    q.m_storage = TypeRefRef{type};
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
  const uint64_t &asQword() const;
  uint64_t &asQword();
  const int64_t &asLong() const;
  int64_t &asLong();
  const std::string &asString() const;
  std::string &asString();
  // Container accessors return the shared payload handle, not the payload
  // itself: `v.asArray()->size()`, `(*v.asArray())[i]`. The mutable overload
  // hands back the same handle, so mutating through it mutates every alias
  // (SPEC §types "value semantics": non-primitives share). `payload.get()` is
  // the value's identity for `deepcopy`'s cycle map.
  const BlobValue &asBlob() const;
  BlobValue &asBlob();
  const ArrayValue &asArray() const;
  ArrayValue &asArray();
  const ArrayValue &asSet() const;
  ArrayValue &asSet();
  const double &asFloat() const;
  double &asFloat();
  const TableValue &asTable() const;
  TableValue &asTable();
  const Closure &asFunction() const;
  Closure &asFunction();
  const GeneratorState &asGenerator() const;
  GeneratorState &asGenerator();
  const ObjectValue &asObject() const;
  ObjectValue &asObject();
  const ExternalRef &asExternal() const;
  ExternalRef &asExternal();
  const BoundMethodRef &asBoundMethod() const;
  BoundMethodRef &asBoundMethod();
  const TypeRefRef &asTypeRef() const;
  TypeRefRef &asTypeRef();

  // Convert a script value to a C++ type T (bool, int32_t, int64_t, double,
  // float, std::string). Throws QLangError (Type kind) on a mismatch.
  // Specializations are in QLangValue.cpp (Task 2).
  template <class T> T as() const;

private:
  Type m_type;
  QLangVariant m_storage;
};

// A declared type annotation (SPEC §strict typing): builtin `type` (Nil =
// none) plus `className` when the annotation names a user class or external
// type (`p:Person`).
struct TypeAnnotation {
  QLangValue::Type type = QLangValue::Type::Nil;
  std::string className;
  bool declared() const {
    return type != QLangValue::Type::Nil || !className.empty();
  }
};

} // namespace qlang
