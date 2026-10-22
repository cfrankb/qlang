#pragma once

// Public-facing alias and type-construction helpers for QLangValue.
// Host code uses QValue (short for "QLang Value") instead of the internal
// QLangValue name. Free functions let you construct values from common C++
// types without spelling out the static-factory syntax.

#include "qlang/QLangValue.h"

namespace qlang {

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

inline QValue qval_dword(uint64_t v) { return QValue::Dword(v); }

inline QValue qval_float(double v) { return QValue::Float(v); }

inline QValue qval_str(const char *v) { return QValue::String(std::string(v)); }

inline QValue qval_str(const std::string &v) { return QValue::String(v); }

inline QValue qval_str(std::string &&v) { return QValue::String(std::move(v)); }

inline QValue qval_blob(const std::vector<uint8_t> &v) { return QValue::Blob(v); }

inline QValue qval_blob(std::vector<uint8_t> &&v) { return QValue::Blob(std::move(v)); }

inline QValue qval_array(ArrayValue v) { return QValue::Array(std::move(v)); }

inline QValue qval_table(TableValue v) { return QValue::Table(std::move(v)); }

} // namespace qlang
