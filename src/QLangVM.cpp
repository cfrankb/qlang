#include "qlang/QLangVM.h"

#include <cstdint>
#include <cstdio>
#include <deque>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>
#include <cmath>

#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"
#include "qlang/QLangObjects.h"

namespace qlang {

namespace {

// Throw shim for the free helper functions below: when a VM is reachable it
// attaches the pc-resolved line/col, the owning module name and the frame
// chain to the error (SPEC # error reporting); without one it falls back to
// a plain 0,0 error.
[[noreturn]] static void vThrow(QLangVM *vm, QLangErrorKind kind,
                                const std::string &msg, int line = 0,
                                int col = 0) {
  if (vm) {
    if (line > 0) vm->vmThrowAt(kind, line, col, msg);
    vm->vmThrow(kind, msg);
  }
  throw QLangError(kind, line, col, msg);
}

bool isNumeric(const QLangValue &v) {
  auto t = v.type();
  return t == QLangValue::Type::Bool || t == QLangValue::Type::Char ||
         t == QLangValue::Type::Byte || t == QLangValue::Type::Word ||
         t == QLangValue::Type::Short || t == QLangValue::Type::Int ||
         t == QLangValue::Type::Unsigned || t == QLangValue::Type::Qword ||
         t == QLangValue::Type::Long ||
         t == QLangValue::Type::Float;
}

double toDouble(const QLangValue &v) {
  switch (v.type()) {
  case QLangValue::Type::Float: return v.asFloat();
  default: return static_cast<double>((long long)toInt64(v));
  }
}

// Text form of a value, used by toStr/print/printf and string concatenation.
// External and type values render through their ObjectOps toStr binding when
// one succeeds (QL_STR / the type's default form); a type without a str
// binding falls through to the static form in the switch below, so print()
// stays sane.
std::string valueToString(const QLangValue &v, QLangContext &ctx) {
  if (v.type() == QLangValue::Type::External ||
      v.type() == QLangValue::Type::TypeRef) {
    const ObjectOps *ops = getObjectOps(v);
    if (ops && ops->toStr) {
      try {
        return ops->toStr(v, ctx).asString();
      } catch (const QLangError &) {
        // No str binding (external type without QL_STR): static form below.
      }
    }
  }
  switch (v.type()) {
  case QLangValue::Type::Nil: return "nil";
  case QLangValue::Type::String: return v.asString();
  case QLangValue::Type::Bool: return v.asInt() ? "true" : "false";
  case QLangValue::Type::Char: return std::to_string(v.asChar());
  case QLangValue::Type::Byte: return std::to_string(v.asByte());
  case QLangValue::Type::Word: return std::to_string(v.asWord());
  case QLangValue::Type::Short: return std::to_string(v.asShort());
  case QLangValue::Type::Int: return std::to_string(v.asInt());
  case QLangValue::Type::Unsigned: return std::to_string(v.asUnsigned());
    case QLangValue::Type::Qword: return std::to_string(v.asQword());
  case QLangValue::Type::Long: return std::to_string(v.asLong());
  case QLangValue::Type::Float: {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.15g", v.asFloat());
    return std::string(buf);
  }
  case QLangValue::Type::Blob: return "<blob>";
  case QLangValue::Type::Array: return "<array>";
  case QLangValue::Type::Set: return "<set>";
  case QLangValue::Type::Table: return "<table>";
  case QLangValue::Type::Function: return "<function>";
  case QLangValue::Type::Generator: return "<generator>";
  case QLangValue::Type::Eof: return "<eof>";
  case QLangValue::Type::Object: return "<object>";
  case QLangValue::Type::External: {
    const auto &ref = v.asExternal();
    return std::string("<external: ") + (ref.type ? ref.type->name : "?") +
           ">";
  }
  case QLangValue::Type::BoundMethod: return "<bound method>";
  case QLangValue::Type::TypeRef: {
    const auto &ref = v.asTypeRef();
    return std::string("<type ") + (ref.type ? ref.type->name : "?") + ">";
  }
  }
  return "?";
}

// Phase 7 / Phase 3 (H3): if the table carries a `__destroy__` method,
// re-wrap it with a finalizer deleter so the method runs exactly once at
// final release. The deleter keeps the closure and the owning program
// (shared_ptr, Phase 2) alive; it no longer dispatches script — it only
// enqueues (table, closure, program) on the context's finalizer queue,
// which is drained at a safe point (run()/call()/callClosure() start,
// clear(), or an explicit processFinalizers()). No-op for plain tables and
// instances without `__destroy__`.
//
// Ownership subtlety: `shared_ptr(raw, deleter)` creates a SECOND
// control block; the original shared_ptr (still in `v`) would then free
// the map as soon as it is overwritten, dangling the wrapped pointer.
// Instead the wrapped deleter captures a copy of the original
// shared_ptr and hands it to the queue — the map is freed exactly once,
// when the drained finalizer entry is destroyed.
//
// The deleter is a NAMED type so `attachFinalizer` can recognise its own
// registration with std::get_deleter (see below).
struct FinalizerDeleter {
  QLangContext *scope;
  QLangValue closure;
  std::shared_ptr<const CompiledProgram> prog;
  TableValue orig;
  void operator()(std::map<std::string, QLangValue> *) const {
    scope->enqueueFinalizer(orig, closure, prog);
  }
};

static void attachFinalizer(QLangValue &v, QLangContext &scope) {
  if (v.type() != QLangValue::Type::Table)
    return;
  const auto &tbl = *v.asTable();
  auto dit = tbl.find("__destroy__");
  if (dit == tbl.end() || dit->second.type() != QLangValue::Type::Function)
    return;
  // Idempotent: a table already carrying the registration keeps it. Wrapping
  // twice would build a second control block whose deleter enqueues the first,
  // so `__destroy__` would run once per attach — SPEC §classes "runs exactly
  // once". This is the normal path for a `__deepcopy__` override that returns
  // an instance built by the class constructor (which emitted TABFINALIZE).
  if (std::get_deleter<FinalizerDeleter>(v.asTable()))
    return;
  QLangValue closure = dit->second; // keep the method alive in the deleter
  auto prog = scope.liveProgram(); // keeps the program alive until release
  auto wrapped = std::shared_ptr<std::map<std::string, QLangValue>>(
      v.asTable().get(), FinalizerDeleter{&scope, std::move(closure), prog,
                                          v.asTable()});
  v = QLangValue::Table(wrapped);
}

// Pool-load materialization (SPEC §value semantics, plan Phase 2). A container
// constant lives in the function's constant pool for the whole program run,
// so handing the pool payload to a register would alias it: every later
// mutation of the register would rewrite the literal for every subsequent
// evaluation (`local a = [1]; a[] = 2; local b = [1]` would see [1,2]).
// Containers therefore get a fresh payload here — recursively, so a nested
// container inside a literal is fresh too. Everything else (scalars, the
// immutable string, closures, generators, external views) is copied as-is;
// sharing those handles is the intended semantics.
// Constant-pool values are built from literals and const-exprs, so they are
// acyclic and the recursion is bounded by the literal's nesting depth.
static QLangValue freshContainerValue(const QLangValue &v) {
  using Type = QLangValue::Type;
  switch (v.type()) {
  case Type::Blob: {
    const auto &src = v.asBlob();
    if (!src)
      return v;
    return QLangValue::Blob(std::make_shared<std::vector<uint8_t>>(*src));
  }
  case Type::Array:
  case Type::Set: {
    const auto &src = v.asArray();
    if (!src)
      return v;
    auto out = std::make_shared<std::vector<QLangValue>>();
    out->reserve(src->size());
    for (const auto &el : *src)
      out->push_back(freshContainerValue(el));
    return v.type() == Type::Set ? QLangValue::Set(std::move(out))
                                 : QLangValue::Array(std::move(out));
  }
  case Type::Table: {
    const auto &src = v.asTable();
    if (!src)
      return v;
    auto out = std::make_shared<std::map<std::string, QLangValue>>();
    for (const auto &kv : *src)
      out->emplace(kv.first, freshContainerValue(kv.second));
    return QLangValue::Table(std::move(out));
  }
  default:
    return v;
  }
}

// release(x) (Phase 3, H2): drop the owned shared_ptr of an owned External so
// the script can free script-constructed objects (the retention leak). The
// value becomes Nil — the object is gone, and using it after release is a
// clean nil error, not a dangling view. A borrowed external (the host owns
// it) or a non-external is a type error. Mutates the argument register in
// place (callBuiltin takes const copies, so this is special-cased in CALL).
static void releaseExternal(QLangValue &v, int line, int col, QLangVM *vm) {
  if (v.type() != QLangValue::Type::External)
    vThrow(vm, QLangErrorKind::Type, "release: expected an external object",
           line, col);
  const auto &ref = v.asExternal();
  if (!ref.owned)
    vThrow(vm, QLangErrorKind::Type,
           "release: not an owned external (borrowed objects are released by "
           "the host)",
           line, col);
  v = QLangValue(); // destroys the ExternalRef -> releases the object
}

// Numeric operand with a clear error if the type is wrong.
int64_t num(const QLangValue &v, const char *op, QLangVM *vm = nullptr) {
  if (!isNumeric(v))
    vThrow(vm, QLangErrorKind::Type,
           std::string("operator '") + op +
               "' operand must be numeric, got " + v.typeName());
  return (long long)toInt64(v);
}

// `vm` (optional) enables the object-aware ordering rules of SPEC
// `## operators < and >`: string vs table/array compares against the
// object's display form (__str__ / class name / implicit collection
// rendering), and table vs table compares each value at the respective
// indexes. Callers without a VM (e.g. the `in` opcode) get the legacy
// numeric/string-only behavior.
int cmp(const QLangValue &a, const QLangValue &b, const char *op,
        QLangVM *vm = nullptr) {
  bool numA = isNumeric(a), numB = isNumeric(b);
  if (numA && numB) {
    if (a.type() == QLangValue::Type::Float ||
        b.type() == QLangValue::Type::Float) {
      double x = toDouble(a), y = toDouble(b);
      return (x < y) ? -1 : (x > y) ? 1 : 0;
    }
    int64_t x = (long long)toInt64(a), y = (long long)toInt64(b);
    return (x < y) ? -1 : (x > y) ? 1 : 0;
  }
  if (a.type() == QLangValue::Type::String &&
      b.type() == QLangValue::Type::String) {
    return a.asString().compare(b.asString());
  }
  // string vs object (table/class/array): binary comparison with __str__.
  if (vm) {
    if (a.type() == QLangValue::Type::String &&
        (b.type() == QLangValue::Type::Table ||
         b.type() == QLangValue::Type::Array))
      return a.asString().compare(vm->displayString(b));
    if (b.type() == QLangValue::Type::String &&
        (a.type() == QLangValue::Type::Table ||
         a.type() == QLangValue::Type::Array))
      return vm->displayString(a).compare(b.asString());
  }
  // array vs array: each value at the relative index; the first differing
  // value decides, the shorter array is less.
  if (a.type() == QLangValue::Type::Array && b.type() == QLangValue::Type::Array) {
    const auto &ea = *a.asArray(), &eb = *b.asArray();
    size_t n = std::min(ea.size(), eb.size());
    for (size_t i = 0; i < n; i++) {
      int c = cmp(ea[i], eb[i], op, vm);
      if (c != 0) return c;
    }
    return ea.size() < eb.size() ? -1 : ea.size() > eb.size() ? 1 : 0;
  }
  // table vs table: comparison of each key value (key order); the first
  // differing value decides, the shorter table is less. Class instances
  // (tables carrying __class__) are not order-comparable (SPEC: table|class
  // and class|any -> invalid comparison) — they fall through to the throw.
  // table|array also falls through to the throw (invalid comparison).
  if (a.type() == QLangValue::Type::Table && b.type() == QLangValue::Type::Table) {
    const auto &ta = *a.asTable(), &tb = *b.asTable();
    auto ca = ta.find("__class__"), cb = tb.find("__class__");
    bool aClass = ca != ta.end() && ca->second.type() == QLangValue::Type::String;
    bool bClass = cb != tb.end() && cb->second.type() == QLangValue::Type::String;
    if (!aClass && !bClass) {
      auto ita = ta.begin(), itb = tb.begin();
      size_t n = std::min(ta.size(), tb.size());
      for (size_t i = 0; i < n; i++, ++ita, ++itb) {
        int c = cmp(ita->second, itb->second, op, vm);
        if (c != 0) return c;
      }
      return ta.size() < tb.size() ? -1 : ta.size() > tb.size() ? 1 : 0;
    }
  }
  vThrow(vm, QLangErrorKind::Type,
         std::string("operator '") + op +
             "' cannot compare " + a.typeName() + " and " +
             b.typeName());
}

// Forward decl: set membership test (defined below with the other set helpers);
// used by isEqual's Set case.
static bool setContains(const std::vector<QLangValue> &elems,
                        const QLangValue &v);

// Equality per spec §23 (== and !=): Int 1/0. nil == nil true; nil != x true.
// bool == int (bool stored as Int 1/0 at runtime). Number types compare equal
// when their int64 values match. Any other comparison: same type and same
// value. `!=` is the exact negation (handled by the caller).
bool isEqual(const QLangValue &a, const QLangValue &b) {
  // nil == nil; nil != anything else.
  if (a.type() == QLangValue::Type::Nil || b.type() == QLangValue::Type::Nil)
    return a.type() == b.type();
  // bool compared as int64 (bool stored as Int 1/0 at runtime).
  if (a.type() == QLangValue::Type::Bool || b.type() == QLangValue::Type::Bool)
    return toInt64(a) == toInt64(b);
  // number types compare equal when their int64 values match.
  if (isNumeric(a) && isNumeric(b))
    return toInt64(a) == toInt64(b);
  // otherwise: same type and same value.
  if (a.type() != b.type())
    return false;
  switch (a.type()) {
  case QLangValue::Type::String:
    return a.asString() == b.asString();
  // Containers compare by CONTENT, never by handle: two aliases of one
  // payload are equal, and so are two distinct payloads holding equal values
  // (SPEC §operators "== and !=" — identity is not an equality relation).
  case QLangValue::Type::Blob:
    return *a.asBlob() == *b.asBlob();
  case QLangValue::Type::Array: {
    const auto &ea = *a.asArray(), &eb = *b.asArray();
    if (ea.size() != eb.size()) return false;
    for (size_t i = 0; i < ea.size(); ++i)
      if (!isEqual(ea[i], eb[i])) return false;
    return true;
  }
  case QLangValue::Type::Table: {
    const auto &ta = *a.asTable(), tb = *b.asTable();
    if (ta.size() != tb.size()) return false;
    for (const auto &kv : ta) {
      auto it = tb.find(kv.first);
      if (it == tb.end()) return false;
      if (!isEqual(kv.second, it->second)) return false;
    }
    return true;
  }
  case QLangValue::Type::Set: {
    // Exact match: same unique elements, order-independent (set order is
    // implementation-defined per SPEC §sets).
    const auto &ea = *a.asSet(), &eb = *b.asSet();
    if (ea.size() != eb.size()) return false;
    for (const QLangValue &e : ea)
      if (!setContains(eb, e)) return false;
    return true;
  }
  default:
    return false;
  }
}

// Set element equality (SPEC §sets: sets hold numbers and strings only).
// Same type + same value; distinct numeric types are distinct elements.
static bool setElementEq(const QLangValue &a, const QLangValue &b) {
  if (a.type() != b.type()) return false;
  switch (a.type()) {
  case QLangValue::Type::String: return a.asString() == b.asString();
  case QLangValue::Type::Float: return a.asFloat() == b.asFloat();
  case QLangValue::Type::Bool:
  case QLangValue::Type::Char:
  case QLangValue::Type::Byte:
  case QLangValue::Type::Word:
  case QLangValue::Type::Short:
  case QLangValue::Type::Int:
  case QLangValue::Type::Unsigned:
  case QLangValue::Type::Qword:
  case QLangValue::Type::Long:
    return toInt64(a) == toInt64(b);
  default: return false;
  }
}

// True if `elems` already holds a value equal to `v` (set dedup). Takes the
// element vector, not the shared payload: callers deref once.
static bool setContains(const std::vector<QLangValue> &elems,
                        const QLangValue &v) {
  for (const auto &e : elems)
    if (setElementEq(e, v)) return true;
  return false;
}

// Build set storage from source values: numbers and strings only, duplicates
// dropped, first-seen order kept. Throws (Type) on any other element type.
// Returns a plain element vector — the QLangValue::Set factory wraps it in a
// fresh payload, so the result never aliases `src`.
static std::vector<QLangValue> toSetElements(const std::vector<QLangValue> &src,
                                             QLangVM *vm = nullptr) {
  std::vector<QLangValue> out;
  for (const auto &v : src) {
    if (v.type() != QLangValue::Type::String && !isNumeric(v))
      vThrow(vm, QLangErrorKind::Type,
             std::string("set element must be a number or string, got ") +
                 v.typeName());
    if (!setContains(out, v))
      out.push_back(v);
  }
  return out;
}

// ARRAPP: append one value to a collection. Arrays append unconditionally;
// sets take numbers/strings only and dedup (SPEC §sets: `s[] = v`).
static void appendValue(QLangValue &base, const QLangValue &val,
                        QLangVM *vm = nullptr) {
  if (base.type() == QLangValue::Type::Array) {
    (*base.asArray()).push_back(val);
    return;
  }
  if (base.type() == QLangValue::Type::Set) {
    if (val.type() != QLangValue::Type::String && !isNumeric(val))
      vThrow(vm, QLangErrorKind::Type,
             std::string("set element must be a number or string, got ") +
                 val.typeName());
    if (!setContains(*base.asSet(), val))
      (*base.asSet()).push_back(val);
    return;
  }
  vThrow(vm, QLangErrorKind::Type, "cannot append to non-array value");
}

// Length for len() (SPEC §build-in functions): string/array/table/blob
// element counts; numbers return storage byte size; class instances and
// any other type return 1; nil/undefined is an error.
int64_t lenOf(const QLangValue &v, QLangVM *vm = nullptr) {
  switch (v.type()) {
  case QLangValue::Type::Nil:
    vThrow(vm, QLangErrorKind::Runtime, "len: undefined/nil has no length");
  case QLangValue::Type::String: return v.asString().size();
  case QLangValue::Type::Array: return static_cast<int64_t>(v.asArray()->size());
  case QLangValue::Type::Set: return static_cast<int64_t>(v.asSet()->size());
  case QLangValue::Type::Blob: return static_cast<int64_t>(v.asBlob()->size());
  case QLangValue::Type::Table: {
    auto t = v.asTable();
    if (t->find("__class__") != t->end()) return 1; // class instance
    return static_cast<int64_t>(t->size());
  }
  case QLangValue::Type::Char:
  case QLangValue::Type::Byte: return 1;
  case QLangValue::Type::Word:
  case QLangValue::Type::Short: return 2;
  case QLangValue::Type::Int:
  case QLangValue::Type::Unsigned: return 4;
    case QLangValue::Type::Qword:
  case QLangValue::Type::Long:
  case QLangValue::Type::Float: return 8;
  default: return 1; // bool, function, generator, ...
  }
}

// Sequential access for `for i in <collection>`: returns the idx-th element of
// an array, the idx-th table value in insertion (std::map) order, or the
// idx-th character of a string (returned as a byte value).
QLangValue seqGet(const QLangValue &col, int64_t idx, QLangVM *vm = nullptr) {
  if (col.type() == QLangValue::Type::String) {
    const auto &s = col.asString();
    if (idx < 0 || idx >= static_cast<int64_t>(s.size()))
      vThrow(vm, QLangErrorKind::Runtime, "string index out of range");
    return QLangValue::Byte(static_cast<uint8_t>(s[static_cast<size_t>(idx)]));
  }
  if (col.type() == QLangValue::Type::Array) {
    const auto &elems = *col.asArray();
    if (idx < 0 || idx >= static_cast<int64_t>(elems.size()))
      vThrow(vm, QLangErrorKind::Runtime, "array index out of range");
    return elems[static_cast<size_t>(idx)];
  }
  if (col.type() == QLangValue::Type::Set) {
    const auto &elems = *col.asSet();
    if (idx < 0 || idx >= static_cast<int64_t>(elems.size()))
      vThrow(vm, QLangErrorKind::Runtime, "set index out of range");
    return elems[static_cast<size_t>(idx)];
  }
  if (col.type() == QLangValue::Type::Table) {
    const auto &kv = *col.asTable();
    if (idx < 0 || idx >= static_cast<int64_t>(kv.size()))
      vThrow(vm, QLangErrorKind::Runtime, "index out of range");
    auto it = kv.begin();
    std::advance(it, static_cast<std::ptrdiff_t>(idx));
    return it->second;
  }
  vThrow(vm, QLangErrorKind::Type,
                   std::string("cannot iterate non-collection of type ") +
                       col.typeName());
}

// Dispatch a built-in function call. `args` are the evaluated operands; the
// return value is stored by the caller into the call's destination register.
// `scope` is the host scope (seeded at run start, synced at run end).
// `prog.vars` maps variable names to frame registers; `deleted` tracks
QLangValue callBuiltin(Builtin id, const std::vector<QLangValue> &args,
                       QLangContext &scope, const CompiledProgram &prog,
                       int line = 0, int col = 0,
                       QLangVM *vm = nullptr) {
  auto need = [&](size_t n, const char *fn) {
    if (args.size() != static_cast<size_t>(n))
      vThrow(vm, QLangErrorKind::Runtime,
             std::string(fn) + " expects " + std::to_string(n) +
             " arguments, got " + std::to_string(args.size()),
             line, col);
  };
  // Throw with the caller's line/col and the VM frame context (SPEC #
  // error reporting).
  auto thr = [&](QLangErrorKind k, const std::string &m) {
    vThrow(vm, k, m, line, col);
  };

  switch (id) {
  case Builtin::ToStr:
    need(1, "str");
    // `__str__` dispatch (tables/class instances), class-name fallback,
    // generic text form — all in displayString.
    return QLangValue::String(vm->displayString(args[0]));
  case Builtin::ToInt:
    need(1, "toInt");
    if (isNumeric(args[0]))
      return QLangValue::Int(static_cast<int32_t>((long long)toInt64(args[0])));
    if (args[0].type() == QLangValue::Type::String) {
      try {
        return QLangValue::Int(static_cast<int32_t>(std::stoi(args[0].asString())));
      } catch (...) {
        return QLangValue::Int(0);
      }
    }
    // nil and other non-numeric types return 0 per spec §237
    return QLangValue::Int(0);
  case Builtin::Type: {
    need(1, "type");
    // Phase 0.5: class instances report their class name.
    // Type::Object with non-null ops -> use ops->kindName.
    if (args[0].type() == QLangValue::Type::Object) {
      auto &desc = args[0].asObject();
      if (desc && desc->ops)
        return QLangValue::String(desc->ops->kindName);
    }
    // External C++ types and type values report the declared type name
    // (no RTTI: the name comes from the descriptor, not type_info).
    if (args[0].type() == QLangValue::Type::External ||
        args[0].type() == QLangValue::Type::TypeRef) {
      const TypeDescriptor *t =
          args[0].type() == QLangValue::Type::External
              ? args[0].asExternal().type.get()
              : args[0].asTypeRef().type.get();
      if (t)
        return QLangValue::String(t->name);
    }
    // Type::Table with __class__ key -> use that (table-based classes; the
    // constructor stamps kClassMetaKey, not "__type__").
    if (args[0].type() == QLangValue::Type::Table) {
      const auto &t = *args[0].asTable();
      auto it = t.find("__class__");
      if (it != t.end() && it->second.type() == QLangValue::Type::String)
        return it->second;
    }
    return QLangValue::String(args[0].typeName());
  }
  case Builtin::Print:
    need(1, "print");
    scope.output(OUT_INFO, vm->displayString(args[0]), scope.moduleName(),
                 line);
    return QLangValue();
  case Builtin::Println: {
    need(1, "println");
    std::string text = vm->displayString(args[0]);
    text += '\n';
    scope.output(OUT_INFO, text, scope.moduleName(), line);
    return QLangValue();
  }
  case Builtin::Printf: {
    if (args.empty())
      thr(QLangErrorKind::Runtime,
                       "printf expects at least a format string");
    if (args[0].type() != QLangValue::Type::String)
      thr(QLangErrorKind::Type,
                       "printf: format string must be a string");
    std::string fmt = args[0].asString();
    std::string out;
    size_t ai = 1; // next format argument
    for (size_t i = 0; i < fmt.size();) {
      if (fmt[i] == '%' && i + 1 < fmt.size()) {
        char spec = fmt[i + 1];
        if (spec == '%') {
          out += '%';
          i += 2;
          continue;
        }
        if (ai >= args.size())
          thr(QLangErrorKind::Runtime,
                           "printf: not enough arguments for format spec");
        const QLangValue &av = args[ai++];
        switch (spec) {
        case 'd': out += std::to_string((long long)toInt64(av)); break;
        case 'f': {
          char buf[64];
          std::snprintf(buf, sizeof(buf), "%.6f", toDouble(av));
          out += buf;
          break;
        }
        case 's': out += vm->displayString(av); break;
        case 'c': out += static_cast<char>((long long)toInt64(av)); break;
        default: out += spec; break;
        }
        i += 2;
      } else {
        out += fmt[i++];
      }
    }
    scope.output(OUT_INFO, out, scope.moduleName(), line);
    return QLangValue();
  }
  case Builtin::Sprintf: {
    // sprintf(fmt, ...) — same format as printf but returns the formatted string.
    if (args.empty())
      thr(QLangErrorKind::Runtime,
                       "sprintf expects at least a format string");
    if (args[0].type() != QLangValue::Type::String)
      thr(QLangErrorKind::Type,
                       "sprintf: format string must be a string");
    std::string fmt = args[0].asString();
    std::string out;
    size_t ai = 1; // next format argument
    for (size_t i = 0; i < fmt.size();) {
      if (fmt[i] == '%' && i + 1 < fmt.size()) {
        // Skip optional width/precision: digits and dots after %
        size_t j = i + 1;
        while (j < fmt.size() && (fmt[j] == '.' || (fmt[j] >= '0' && fmt[j] <= '9')))
          j++;
        if (j >= fmt.size()) {
          // Trailing %, treat as literal
          out += '%';
          i++;
          continue;
        }
        char spec = fmt[j];
        if (spec == '%') {
          out += '%';
          i = j + 1;
          continue;
        }
        if (ai >= args.size())
          thr(QLangErrorKind::Runtime,
                           "sprintf: not enough arguments for format spec");
        const QLangValue &av = args[ai++];
        switch (spec) {
        case 'd': out += std::to_string((long long)toInt64(av)); break;
        case 'f': {
          // Extract precision from format (e.g. %.2f).
          int precision = 6;
          {
            size_t k = i + 1;
            if (k < fmt.size() && fmt[k] == '.') {
              k++;
              int prec = 0;
              while (k < fmt.size() && fmt[k] >= '0' && fmt[k] <= '9') {
                prec = prec * 10 + (fmt[k] - '0');
                k++;
              }
              if (k > i + 2)
                precision = prec;
            }
          }
          char buf[64];
          std::snprintf(buf, sizeof(buf), "%.*f", precision, toDouble(av));
          out += buf;
          break;
        }
        case 's': out += vm->displayString(av); break;
        case 'c': out += static_cast<char>((long long)toInt64(av)); break;
        default: out += spec; break;
        }
        i = j + 1;
      } else {
        out += fmt[i++];
      }
    }
    return QLangValue::String(out);
  }
  case Builtin::Format: {
    // format(template, arg1, arg2, ...) — f-string interpolation.
    // Supports full printf-style format specifiers: %s, %d, %f, %.*f, %5d, etc.
    if (args.empty())
      thr(QLangErrorKind::Runtime,
                       "format expects at least a template string");
    if (args[0].type() != QLangValue::Type::String)
      thr(QLangErrorKind::Type,
                       "format: template must be a string");
    std::string fmt = args[0].asString();
    std::string out;
    size_t ai = 1;
    for (size_t i = 0; i < fmt.size(); ) {
      if (fmt[i] == '%' && i + 1 < fmt.size()) {
        // Parse the full format spec: %[flags][width][.precision]conversion
        size_t j = i + 1;
        // Skip flags: -, +, space, 0, #
        while (j < fmt.size() && (fmt[j] == '-' || fmt[j] == '+' ||
                                   fmt[j] == ' ' || fmt[j] == '0' || fmt[j] == '#'))
          j++;
        // Skip width digits
        while (j < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[j])))
          j++;
        // Skip precision: .digits
        if (j < fmt.size() && fmt[j] == '.') {
          j++;
          while (j < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[j])))
            j++;
        }
        if (j >= fmt.size()) {
          out += '%';
          i++;
          continue;
        }
        char spec = fmt[j];
        if (spec == '%') {
          out += '%';
          i = j + 1;
          continue;
        }
        if (ai >= args.size())
          thr(QLangErrorKind::Runtime,
                           "format: not enough arguments for template placeholder");
        const QLangValue &av = args[ai++];
        // Use the original format substring (including flags/width/precision)
        // to let snprintf handle all formatting natively.
        // j points at the conversion specifier, so include it: length = j - i + 1.
        std::string sub(fmt, i, j - i + 1);
        if (av.type() == QLangValue::Type::String) {
          char buf[256];
          std::snprintf(buf, sizeof(buf), sub.c_str(), av.asString().c_str());
          out += buf;
        } else if (av.type() == QLangValue::Type::Float) {
          char buf[256];
          // %s with a double is UB — render like valueToString.
          if (spec == 's')
            std::snprintf(buf, sizeof(buf), "%.15g", toDouble(av));
          else
            std::snprintf(buf, sizeof(buf), sub.c_str(), toDouble(av));
          out += buf;
        } else if (av.type() == QLangValue::Type::Bool) {
          // %s placeholder -> "true"/"false" text; numeric specs -> 1/0.
          // (snprintf("%s", long long) segfaults — never pass the int form
          // to a string conversion.)
          if (spec == 's') {
            out += toInt64(av) ? "true" : "false";
          } else {
            char buf[256];
            std::snprintf(buf, sizeof(buf), sub.c_str(), (long long)toInt64(av));
            out += buf;
          }
        } else if (av.type() == QLangValue::Type::Qword) {
          // uint64_t — %llu; a plain %d/%lld truncates or overflows.
          std::string actualFmt = sub;
          char last = actualFmt.back();
          if (last == 's' || last == 'd' || last == 'i' || last == 'u')
            actualFmt = actualFmt.substr(0, actualFmt.size() - 1) + "llu";
          char buf[256];
          std::snprintf(buf, sizeof(buf), actualFmt.c_str(),
                        (unsigned long long)av.asQword());
          out += buf;
        } else if (isNumeric(av)) {
          // If the placeholder is %s but the arg is numeric, use %d instead.
          // The arg is long long, so the conversion must be 'lld' — a plain
          // 'd' reads only 32 bits (4294967295 would print as -1).
          std::string actualFmt = sub;
          if (sub == "%s" || sub == "%+s" || sub == "% s" || sub == "%0s" ||
              sub == "%#s") {
            // Build a numeric placeholder with the same width/flags.
            // Strip the 's' and replace with 'lld'.
            std::string prefix = sub.substr(0, sub.size() - 1);
            actualFmt = prefix + "lld";
          } else if (sub.back() == 'd' || sub.back() == 'i') {
            actualFmt = sub.substr(0, sub.size() - 1) + "lld";
          }
          char buf[256];
          std::snprintf(buf, sizeof(buf), actualFmt.c_str(), (long long)toInt64(av));
          out += buf;
        } else {
          out += vm->displayString(av);
        }
        i = j + 1;
      } else {
        out += fmt[i++];
      }
    }
    return QLangValue::String(out);
  }
  case Builtin::Len: {
    need(1, "len");
    return QLangValue::Int(static_cast<int32_t>(lenOf(args[0], vm)));
  }
  case Builtin::Blob: {
    need(0, "blob");
    return QLangValue::Blob();
  }
  case Builtin::Set: {
    // set(1,5,7) variadic, or set([1,2,3]) with a single array argument
    // (SPEC §sets: array coerced into set).
    std::vector<QLangValue> src;
    if (args.size() == 1 && args[0].type() == QLangValue::Type::Array)
      src = *args[0].asArray();
    else
      src = args;
    return QLangValue::Set(toSetElements(src, vm));
  }
  case Builtin::Substr: {
    // substr(str, start, [length]) — 0-based start (consistent with array
    // indexing, spec §100); length optional (defaults to end of string).
    if (args.size() < 2 || args.size() > 3)
      thr(QLangErrorKind::Runtime,
                       "substr expects 2 or 3 args (str, start, [length])");
    if (args[0].type() != QLangValue::Type::String)
      thr(QLangErrorKind::Type,
                       "substr: first argument must be a string");
    const std::string &s = args[0].asString();
    int64_t start = (long long)toInt64(args[1]);
    int64_t len = (args.size() == 3)
                      ? (long long)toInt64(args[2])
                      : static_cast<int64_t>(s.size()) - start;
    if (start < 0) start = 0;
    if (len < 0) len = 0;
    if (start > static_cast<int64_t>(s.size()))
      return QLangValue::String("");
    if (start + len > static_cast<int64_t>(s.size()))
      len = static_cast<int64_t>(s.size()) - start;
    return QLangValue::String(
        s.substr(static_cast<size_t>(start), static_cast<size_t>(len)));
  }
  case Builtin::Slice: {
    // slice(array, start, [length]) — 0-based start; length optional (defaults
    // to rest of array). Returns a new array.
    if (args.size() < 2 || args.size() > 3)
      thr(QLangErrorKind::Runtime,
                       "slice expects 2 or 3 args (array, start, [length])");
    if (args[0].type() != QLangValue::Type::Array)
      thr(QLangErrorKind::Type,
                       "slice: first argument must be an array");
    const std::vector<QLangValue> &arr = *args[0].asArray();
    int64_t start = (long long)toInt64(args[1]);
    int64_t len = (args.size() == 3)
                      ? (long long)toInt64(args[2])
                      : static_cast<int64_t>(arr.size()) - start;
    if (start < 0) start = 0;
    if (len < 0) len = 0;
    if (start >= static_cast<int64_t>(arr.size()))
      return QLangValue::Array();
    if (start + len > static_cast<int64_t>(arr.size()))
      len = static_cast<int64_t>(arr.size()) - start;
    std::vector<QLangValue> result;
    result.reserve(static_cast<size_t>(len));
    for (int64_t i = 0; i < len; ++i)
      result.push_back(arr[static_cast<size_t>(start) + i]);
    return QLangValue::Array(std::move(result));
  }
  case Builtin::Range: {
    // range(start, end) — inclusive on both ends; returns [start, start+1, ...,
    // end]. For start > end returns empty array.
    if (args.size() != 2)
      thr(QLangErrorKind::Runtime,
                       "range expects 2 args (start, end)");
    int64_t start = (long long)toInt64(args[0]);
    int64_t end = (long long)toInt64(args[1]);
    std::vector<QLangValue> result;
    if (start <= end) {
      result.reserve(static_cast<size_t>(end - start + 1));
      for (int64_t i = start; i <= end; ++i)
        result.push_back(QLangValue::Int(static_cast<int32_t>(i)));
    }
    return QLangValue::Array(std::move(result));
  }
  case Builtin::Join: {
    // join(separator, array) — concatenates array elements with separator.
    // Non-string elements are converted via valueToString.
    if (args.size() != 2)
      thr(QLangErrorKind::Runtime,
                       "join expects 2 args (separator, array)");
    if (args[1].type() != QLangValue::Type::Array)
      thr(QLangErrorKind::Type,
                       "join: second argument must be an array");
    const std::string &sep = args[0].asString();
    const auto &elems = *args[1].asArray();
    std::string result;
    for (size_t i = 0; i < elems.size(); ++i) {
      if (i > 0) result += sep;
      result += vm->displayString(elems[i]);
    }
    return QLangValue::String(result);
  }
  case Builtin::IsSet: {
    // isset(var_name) — returns true (1) if the named variable exists in
    // scope (or current frame registers during execution), false (0) otherwise.
    need(1, "isset");
    if (args[0].type() != QLangValue::Type::String)
      thr(QLangErrorKind::Type,
                       "isset: argument must be a string (variable name)");
    const std::string &name = args[0].asString();
    // Top-level names live in the context's global table (the main frame
    // aliases it), so one check covers pre-seeded, in-flight and deleted
    // state (Phase 2).
    return QLangValue::Bool(scope.hasGlobal(name) ? 1 : 0);
  }
  case Builtin::Assert: {
    // assert(bool, str) — throws a runtime error with the given message if
    // the boolean argument is false (0).
    need(2, "assert");
    // Validate args regardless of condition.
    if (args[1].type() != QLangValue::Type::String)
      thr(QLangErrorKind::Type,
          "assert: second argument must be a string (error message)");
    // Truthiness check (SPEC §boolean evaluation, shared with the
    // TST and NOT/LAND/LOR paths).
    if (!vm->isTrue(args[0]))
      thr(QLangErrorKind::Runtime, args[1].asString());
    return QLangValue();
  }
  case Builtin::Next: {
    need(1, "next");
    if (args[0].type() != QLangValue::Type::Generator)
      thr(QLangErrorKind::Type, "next expects a generator");
    // Phase 1: the generator value IS its instance; resume it in place.
    GeneratorState &gen = const_cast<GeneratorState &>(args[0].asGenerator());
    return vm->resumeGenerator(gen);
  }
  case Builtin::SetMaxSteps: {
    need(1, "set_max_steps");
    long long v = toInt64(args[0]);
    if (v < 0) v = 0;
    scope.setMaxSteps(static_cast<std::size_t>(v));
    return QLangValue();
  }
  case Builtin::GetMaxSteps:
    return QLangValue::Int(static_cast<int32_t>(scope.maxSteps()));
  }
  thr(QLangErrorKind::Runtime, "unknown builtin");
}

} // namespace

// Right-operand materialization for the dispatch loop (plan §2.2/§2.3). A
// member (Frame is private) defined after the anonymous namespace above, whose
// freshContainerValue it still sees.
QLangValue QLangVM::operandValue(const DecodedInstruction &i, Frame &frame,
                                 const std::vector<QLangValue> &consts,
                                 bool cloneContainers) {
  switch (i.rightKind) {
  case RightKind::Register:
    return frame.regs[i.b];
  case RightKind::ConstPool:
    return cloneContainers ? freshContainerValue(consts[i.b]) : consts[i.b];
  case RightKind::Immediate:
    break; // materialized from the payload fields below
  case RightKind::Reserved:
    break; // unreachable: the decoder rejects it before dispatch (plan §2.2)
  }
  switch (i.immPayload) {
  case ImmPayload::Signed:
    // Only `i64` widens to Long; the 1/2/4-byte forms stay Int (plan §2.3).
    return i.immForm == ImmForm::I64
               ? QLangValue::Long(i.imm)
               : QLangValue::Int(static_cast<int32_t>(i.imm));
  case ImmPayload::Unsigned:
    return i.immForm == ImmForm::U64
               ? QLangValue::Qword(i.immUnsigned)
               : QLangValue::Unsigned(static_cast<uint32_t>(i.immUnsigned));
  case ImmPayload::Float:
    return QLangValue::Float(i.immFloat);
  case ImmPayload::True:
    return QLangValue::Bool(1);
  case ImmPayload::False:
    return QLangValue::Bool(0);
  case ImmPayload::Nil:
    return QLangValue();
  case ImmPayload::None:
    break;
  }
  // A decoded immediate always names a payload; reaching here means a stream
  // built without the decoder.
  throw QLangError(QLangErrorKind::Runtime, 0, 0,
                   "instruction has no right operand");
}

QLangVM::QLangVM(const CompiledProgram &program, QLangContext &scope)
    : m_prog(program), m_scope(scope) {
  adoptGlobals(program);
}

void QLangVM::adoptGlobals(const CompiledProgram &prog) {
  // Programs compiled against this context already use table slots as their
  // main-program registers (declareVar asks the table); this is a no-op for
  // them. Standalone-compiled programs (no context at compile time) get
  // their layout adopted so the main frame's alias stays consistent.
  auto &t = m_scope.globalTable();
  for (const auto &[name, reg] : prog.vars) {
    size_t r = static_cast<size_t>(reg);
    auto it = t.names.find(name);
    if (it == t.names.end()) {
      if (t.slots.size() < r + 1)
        t.slots.resize(r + 1);
      t.names[name] = r;
    } else if (it->second != r) {
      // Conflict: keep the table's value, relocate it to the program's
      // register so the program's code (which reads reg) sees it.
      if (t.slots.size() < r + 1)
        t.slots.resize(r + 1);
      t.slots[r] = t.slots[it->second];
      t.slots[it->second] = QLangValue();
      t.names[name] = r;
    }
  }
}

// Resolve a byte-offset pc to an instruction index: the largest index whose
// offset is < pc (binary search over fn.instrOffsets). pc is a byte offset in
// the flat code stream, not an index; fn.loc is parallel to instrOffsets. The
// VM advances frame.pc past the current instruction before executing it, so on
// a fault pc points one-past the failing instruction's end — the failing
// instruction is the last one whose offset is strictly < pc.
size_t QLangVM::instructionIndexAt(const Function &fn, size_t pc) const {
  const auto &offs = fn.instrOffsets;
  if (offs.empty())
    return 0;
  size_t lo = 0, hi = offs.size();
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (offs[mid] < pc)
      lo = mid + 1;
    else
      hi = mid;
  }
  return (lo > 0) ? lo - 1 : 0;
}

// Build the execution trace (innermost frame first) from the current frame
// stack and throw a QLangError carrying it plus the resolved line/col and
// the owning module name (SPEC # error reporting).
void QLangVM::vmThrowAt(QLangErrorKind kind, int line, int col,
                        const std::string &msg) const {
  std::vector<QLangFrameRef> frames;
  int l = line, c = col;
  if (m_cur >= 0) {
    for (int f = m_cur; f >= 0; f--) {
      const Frame &fr = m_frames[static_cast<size_t>(f)];
      const Function &fn =
          m_prog.functions[static_cast<size_t>(fr.funcIndex)];
      int fl = 0;
      size_t idx = instructionIndexAt(fn, fr.pc);
      if (idx < fn.loc.size())
        fl = fn.loc[idx].first;
      frames.push_back(QLangFrameRef{fn.name, fl});
    }
    // Default to the innermost frame's resolved position (the failing
    // instruction).
    if (l == 0) {
      l = frames.front().line;
      const Function &fn =
          m_prog.functions[static_cast<size_t>(m_frames[static_cast<size_t>(m_cur)].funcIndex)];
      size_t pc = m_frames[static_cast<size_t>(m_cur)].pc;
      size_t idx = instructionIndexAt(fn, pc);
      if (idx < fn.loc.size()) c = fn.loc[idx].second;
    }
  }
  throw QLangError(kind, l, c, msg, m_scope.moduleName(), std::move(frames));
}

void QLangVM::vmThrow(QLangErrorKind kind, const std::string &msg) const {
  vmThrowAt(kind, 0, 0, msg);
}

bool QLangVM::isTrue(const QLangValue &v) {
  switch (v.type()) {
  case QLangValue::Type::Nil: return false;
  case QLangValue::Type::Eof: return false;
  case QLangValue::Type::String: return !v.asString().empty();
  case QLangValue::Type::Array: return !v.asArray()->empty();
  case QLangValue::Type::Table: return !v.asTable()->empty();
  case QLangValue::Type::Blob: return !v.asBlob()->empty();
  case QLangValue::Type::Set: return !v.asSet()->empty();
  case QLangValue::Type::Generator: return true;
  case QLangValue::Type::Function:
    // SPEC §boolean evaluation: truthiness of a function/lambda is
    // evaluated based on its return value — call it (no args) and
    // evaluate the result. The step guard in the VM loop bounds any
    // self-returning recursion.
    return isTrue(callFunction(v.asFunction().funcIndex, {}));
  case QLangValue::Type::Float: return v.asFloat() != 0.0;
  default: return isNumeric(v) && (long long)toInt64(v) != 0;
  }
}

int QLangVM::cmpFlags(const QLangValue &a, const QLangValue &b, QLangVM *vm) {
  // SPEC §bytecodes: cmp can only be used if both operands are Int.
  if (a.type() != QLangValue::Type::Int || b.type() != QLangValue::Type::Int)
    vm->vmThrow(QLangErrorKind::Type, "cmp operand must be an int");
  int64_t av = (int64_t)toInt64(a), bv = (int64_t)toInt64(b);
  int f = 0;
  if (av > bv) f |= CMP_GR;
  if (av == bv) f |= CMP_EQ;
  if (av < bv) f |= CMP_LT;
  return f;
}

// Flag-setting comparisons over general values (SPEC §bytecodes "extended
// OpCode eq, lt, gt, lte, gte"; plan docs/plans/m3-flag-flow-plan.md D3). They
// reuse the value-comparison rules verbatim — valueEqual for `==` (ObjectOps
// `__str__` dispatch included), cmp with the same op-name strings for the
// orderings — so the messages and the object/array/table ordering are
// inherited, not reimplemented.
//
// The spec's table is a *capability mask*: each comparator sets only the bit of
// the relation it tests, so the result is one-hot or zero and flagsSatisfied
// needs no change. `ltef`/`gtef` therefore never set two bits at once (the spec
// rejects that state: `je` tests `f == eq` and would stop firing for a == b).
int QLangVM::flagCompare(Opcode op, const QLangValue &a, const QLangValue &b) {
  switch (op) {
  case Opcode::EQF: return valueEqual(a, b) ? CMP_EQ : 0;
  case Opcode::LTF: return cmp(a, b, "<", this) < 0 ? CMP_LT : 0;
  case Opcode::GTF: return cmp(a, b, ">", this) > 0 ? CMP_GR : 0;
  case Opcode::LTEF: {
    int c = cmp(a, b, "<=", this);
    return c < 0 ? CMP_LT : c == 0 ? CMP_EQ : 0;
  }
  case Opcode::GTEF: {
    int c = cmp(a, b, ">=", this);
    return c > 0 ? CMP_GR : c == 0 ? CMP_EQ : 0;
  }
  default: return 0;
  }
}

bool QLangVM::flagsSatisfied(Opcode op, int f) {
  // The bits are set independently and are mutually exclusive (a>b, a==b,
  // a<b are exhaustive), so `f` is exactly one of GR/EQ/LT. The >= / <=
  // jumps therefore test for "that bit OR eq" — NOT for the combined
  // (GR|EQ) pattern, which can never occur.
  switch (op) {
  case Opcode::JG: return f == CMP_GR;                 // a > b
  case Opcode::JGE: return (f & (CMP_GR | CMP_EQ)) != 0; // a >= b
  case Opcode::JL: return f == CMP_LT;                 // a < b
  case Opcode::JLE: return (f & (CMP_LT | CMP_EQ)) != 0; // a <= b
  case Opcode::JE: return f == CMP_EQ;                 // a == b
  case Opcode::JNE: return (f & CMP_EQ) == 0;          // a != b
  default: return false;
  }
}

QLangValue QLangVM::callClosure(const QLangValue &value, const std::vector<QLangValue> &args) {
  if (value.type() == QLangValue::Type::Generator) {
    // Phase 1: the generator value IS its instance (heap state behind the
    // shared_ptr); resume it in place. The const_cast is safe: const only
    // applies to the QLangValue wrapper, not to the shared state.
    return resumeGenerator(const_cast<GeneratorState &>(value.asGenerator()));
  }
  if (value.type() != QLangValue::Type::Function)
    vmThrow(QLangErrorKind::Type, "attempt to call a non-function value");
  return callFunction(value.asFunction().funcIndex, args);
}

// deepcopy(x) (SPEC §functions "deepcopy", §classes "deepcopy", plan
// docs/plans/deepcopy-plan.md): an independent recursive copy of `v`.
// Primitives and the immutable string are copied by value; arrays/sets/blobs/
// tables build new payloads and recurse; class instances (tables stamped with
// `__class__`) share the class-level meta keys and every function value,
// copying only the data fields. Closures, generators, ObjectDescriptors and
// the external views carry live state or view a C++ object: the SAME handle
// comes back. `seen` maps an already-copied payload address to its copy so a
// value that transitively contains itself terminates and the cycle is
// preserved in the result. Since Phase 2 every container — array, set, blob
// and table — is pointer-backed, so all four are keyable; a self-referencing
// array (`a[] = a`) now round-trips as a cycle instead of recursing forever.
// A table carrying a callable `__deepcopy__` is copied by that hook instead
// of the default field walk (see below); the dispatch needs the VM call path,
// which is why this is a member and not a free function.
QLangValue QLangVM::deepCopyValue(
    const QLangValue &v, std::unordered_map<const void *, QLangValue> &seen) {
  using Type = QLangValue::Type;
  switch (v.type()) {
  // Value-backed payloads: the variant copy IS the deep copy (string is the
  // spec's deliberate exception — copied, never shared).
  case Type::Nil:
  case Type::Eof:
  case Type::Bool:
  case Type::Char:
  case Type::Byte:
  case Type::Word:
  case Type::Short:
  case Type::Int:
  case Type::Unsigned:
  case Type::Qword:
  case Type::Long:
  case Type::Float:
  case Type::String:
    return v;
  case Type::Blob: {
    // Bytes only — nothing nested to recurse into, but the payload is shared
    // storage now, so the copy must be a new buffer.
    const auto &src = v.asBlob();
    if (!src)
      return v;
    auto sit = seen.find(src.get());
    if (sit != seen.end())
      return sit->second;
    auto copy = std::make_shared<std::vector<uint8_t>>(*src);
    QLangValue out = QLangValue::Blob(std::move(copy));
    seen[src.get()] = out;
    return out;
  }
  case Type::Array:
  case Type::Set: {
    const auto &src = v.asArray();
    if (!src)
      return v;
    auto sit = seen.find(src.get());
    if (sit != seen.end())
      return sit->second; // cycle: emit the existing copy
    auto out = std::make_shared<std::vector<QLangValue>>();
    out->reserve(src->size());
    QLangValue res = v.type() == Type::Set ? QLangValue::Set(out)
                                           : QLangValue::Array(out);
    seen[src.get()] = res; // register BEFORE recursing (cycle-safe)
    for (const auto &el : *src)
      out->push_back(deepCopyValue(el, seen));
    return res;
  }
  case Type::Table: {
    const auto &src = v.asTable();
    if (!src)
      return v; // nil table handle: nothing to copy
    auto sit = seen.find(src.get());
    if (sit != seen.end())
      return sit->second; // cycle: emit the existing copy
    // `__deepcopy__` override (SPEC §classes "deepcopy"): a table carrying a
    // callable `__deepcopy__` is copied by that hook — it runs on the
    // ORIGINAL (self = v, the same dispatch shape as `__str__` in
    // displayString and `__destroy__` in runFinalizer), returns the copy, and
    // may throw to refuse (the error propagates out of deepcopy). Resolution
    // happens AFTER the cycle check so a hook result reached through a cycle
    // is not re-invoked.
    auto hit = src->find("__deepcopy__");
    if (hit != src->end() && hit->second.type() == Type::Function &&
        m_deepCopyHooked.find(src.get()) == m_deepCopyHooked.end()) {
      // Re-entrancy: a hook that deep-copies `self` re-enters on the same
      // payload; the nested call skips the hook and takes the default field
      // copy below, so `def __deepcopy__() { return deepcopy(self) }`
      // terminates. Erase on every exit, including a throwing hook.
      m_deepCopyHooked.insert(src.get());
      struct HookGuard {
        std::unordered_set<const void *> &set;
        const void *key;
        ~HookGuard() { set.erase(key); }
      } guard{m_deepCopyHooked, src.get()};
      trace(TRACE_VM_LOOP, "deepcopy: __deepcopy__ hook on %p\n", src.get());
      QLangValue r = callClosure(hit->second, {v});
      // A hook must return a distinct instance or throw (SPEC §classes
      // "deepcopy"): nil or the original handle would silently alias the
      // copy to the source, defeating the whole point of the copy.
      if (r.type() != Type::Table || !r.asTable())
        vmThrow(QLangErrorKind::Runtime,
                std::string("__deepcopy__ must return an instance, got ") +
                    r.typeName());
      if (r.asTable().get() == src.get())
        vmThrow(QLangErrorKind::Runtime,
                "__deepcopy__ must return a new instance, not self");
      seen[src.get()] = r; // other references to the original share the copy
      return r;
    }
    auto copy = std::make_shared<std::map<std::string, QLangValue>>();
    QLangValue out = QLangValue::Table(copy);
    seen[src.get()] = out; // register BEFORE recursing (cycle-safe)
    // Class instance: the meta keys describe the CLASS (closed field set,
    // const set, field types, static anchor) and are shared, never copied
    // (SPEC §classes "deepcopy"). `__class__` itself is a string -> copied.
    const bool isInstance = src->count("__class__") > 0;
    for (const auto &kv : *src) {
      if (isInstance && (kv.first == "__fields__" ||
                         kv.first == "__const_fields__" ||
                         kv.first == "__field_types__" ||
                         kv.first == "__statics__")) {
        (*copy)[kv.first] = kv.second; // shared class-level handle
        continue;
      }
      // Function values (methods, `__destroy__`) share the closure handle
      // through the recursion below (Function returns the same handle).
      (*copy)[kv.first] = deepCopyValue(kv.second, seen);
    }
    return out;
  }
  // Live-state / view types: shared, never copied (SPEC §functions
  // "deepcopy"). ObjectDescriptor has no language constructor today;
  // sharing the handle is the locked default (plan open decision 2).
  case Type::Function:
  case Type::Generator:
  case Type::Object:
  case Type::External:
  case Type::BoundMethod:
  case Type::TypeRef:
    return v;
  }
  return v; // unreachable: every Type alternative is covered above
}

// Deliver a signal (docs/spec/17-signals.md). The sender must be an external
// object (v1: external types only); the signal is resolved against its type
// descriptor; the argument count and types are checked against the declared
// signature (the same as<A>() conversion rules a method call uses). Each
// live connection's handler is then invoked, in connection order. A handler
// that emits recurses into deliverSignal; the step budget contains runaway
// recursion (there is no re-entrancy guard).
void QLangVM::deliverSignal(const QLangValue &sender, const std::string &signal,
                            const std::vector<QLangValue> &args) {
  if (sender.type() != QLangValue::Type::External)
    vmThrow(QLangErrorKind::Type, "emit on a non-external object");
  const auto &ref = sender.asExternal();
  if (!ref.type)
    vmThrow(QLangErrorKind::Type, "emit on a non-external object");
  const TypeDescriptor &desc = *ref.type;
  const SignalMeta *sig = nullptr;
  for (const auto &s : desc.signals)
    if (s.name == signal) {
      sig = &s;
      break;
    }
  if (!sig)
    vmThrow(QLangErrorKind::Type, "unknown signal '" + signal + "' on type '" +
                                      desc.name + "'");
  if (args.size() != sig->argChecks.size())
    vmThrow(QLangErrorKind::Type,
             "signal '" + signal + "' on type '" + desc.name + "' expects " +
                 std::to_string(sig->argChecks.size()) + " argument(s), got " +
                 std::to_string(args.size()));
  for (size_t k = 0; k < args.size(); k++)
    if (!sig->argChecks[k](args[k]))
      vmThrow(QLangErrorKind::Type,
               "signal '" + signal + "' on type '" + desc.name + "': argument " +
                   std::to_string(k + 1) + " has the wrong type");
  // Deliver to each live connection, in connection order. The vector is a
  // copy, so a handler that connects/disconnects during delivery does not
  // disturb the iteration.
  for (const auto &conn : m_scope.signalConnections(sender, signal))
    callHandler(conn, args);
}

// Invoke one signal connection's handler (docs/spec/17-signals.md).
void QLangVM::callHandler(const QLangContext::SignalConnection &conn,
                          const std::vector<QLangValue> &args) {
  if (!conn.method.empty()) {
    // String-slot form: resolve the method on the target at emit time.
    callSlotHandler(conn.target, conn.method, args);
    return;
  }
  if (conn.handler.type() == QLangValue::Type::Function) {
    callClosure(conn.handler, args);
    return;
  }
  if (conn.handler.type() == QLangValue::Type::BoundMethod) {
    // External C++ method: invoke the bound member on the target (like the
    // CALLCLOS case). selfMut() throws on an expired borrow; C++ exceptions
    // from the binding become runtime errors.
    const auto &bm = conn.handler.asBoundMethod();
    ExternalRef target = bm.target; // copy: selfMut() is non-const
    void *self = target.selfMut();
    try {
      bm.member->invoke(self, m_scope, args);
    } catch (const QLangError &) {
      throw;
    } catch (const std::exception &e) {
      vmThrow(QLangErrorKind::Runtime, e.what());
    }
    return;
  }
  // A handler that is neither a function nor a bound method is a silent
  // no-op (it was valid at connect time; it may have been reassigned).
}

// The string-slot form of callHandler: resolve `method` on `target` at emit
// time and invoke it with `args`. A missing method is a silent no-op (the
// event-handler miss rule); a method that throws propagates (the handler's
// error becomes the emit's error).
void QLangVM::callSlotHandler(const QLangValue &target, const std::string &method,
                              const std::vector<QLangValue> &args) {
  const ObjectOps *ops = getObjectOps(target);
  if (!ops)
    return;
  QLangValue m;
  try {
    m = ops->getField(target, method, m_scope);
  } catch (const QLangError &) {
    return; // missing method: silent no-op
  }
  if (m.type() == QLangValue::Type::Function) {
    // Table-stored closures are called as-is (no self); class methods take
    // self as the first argument (the script's method-call convention).
    // Class instances are tables carrying the `__fields__` schema.
    std::vector<QLangValue> callArgs = args;
    if (target.type() == QLangValue::Type::Object || isClassInstance(target))
      callArgs.insert(callArgs.begin(), target);
    callClosure(m, callArgs);
    return;
  }
  if (m.type() == QLangValue::Type::BoundMethod) {
    const auto &bm = m.asBoundMethod();
    ExternalRef t = bm.target;
    void *self = t.selfMut();
    try {
      bm.member->invoke(self, m_scope, args);
    } catch (const QLangError &) {
      throw;
    } catch (const std::exception &e) {
      vmThrow(QLangErrorKind::Runtime, e.what());
    }
    return;
  }
  // Any other resolved value (a plain field, not a method) is a silent no-op.
}

// Display form of a value (print/println/printf/sprintf/f-string/join). A
// table with a `__str__` method renders by calling it with the table as the
// sole argument; class instances without `__str__` render as their class
// name. The re-entrancy guard makes an `__str__` that displays `self`
// terminate: the nested display skips `__str__` and falls back.
std::string QLangVM::displayString(const QLangValue &v) {
  if (v.type() == QLangValue::Type::Table) {
    const auto &t = *v.asTable();
    if (!m_inDisplayString) {
      auto it = t.find("__str__");
      if (it != t.end() && it->second.type() == QLangValue::Type::Function) {
        m_inDisplayString = true;
        QLangValue r = callClosure(it->second, {v});
        m_inDisplayString = false;
        if (r.type() == QLangValue::Type::String) return r.asString();
        return valueToString(r, m_scope);
      }
    }
    auto kit = t.find("__class__");
    if (kit != t.end() && kit->second.type() == QLangValue::Type::String)
      return kit->second.asString();
    // Plain table: implicit __str__ renders the entries (SPEC `# implicit
    // __str__ rules`): {key=value, key=value}
    std::string out = "{";
    bool first = true;
    for (const auto &kv : t) {
      if (!first) out += ", ";
      first = false;
      out += kv.first + "=" + renderElement(kv.second);
    }
    return out + "}";
  }
  if (v.type() == QLangValue::Type::Array) {
    // Implicit __str__ renders the elements: [e1, e2, ...]
    const auto &a = *v.asArray();
    std::string out = "[";
    for (size_t i = 0; i < a.size(); i++) {
      if (i) out += ", ";
      out += renderElement(a[i]);
    }
    return out + "]";
  }
  if (v.type() == QLangValue::Type::Set) {
    // Set __str__: set(e1, e2, ...) (SPEC §sets).
    const auto &a = *v.asSet();
    std::string out = "set(";
    for (size_t i = 0; i < a.size(); i++) {
      if (i) out += ", ";
      out += renderElement(a[i]);
    }
    return out + ")";
  }
  return valueToString(v, m_scope);
}

std::string QLangVM::renderElement(const QLangValue &v) {
  if (v.type() == QLangValue::Type::String)
    return "\"" + v.asString() + "\"";
  return displayString(v);
}

// True when a table value is an object: a class instance (carries the
// __class__ meta key) or a table exposing a __str__ method.
static bool isObjectValue(const QLangValue &t) {
  const auto &m = *t.asTable();
  auto c = m.find("__class__");
  if (c != m.end() && c->second.type() == QLangValue::Type::String) return true;
  auto s = m.find("__str__");
  return s != m.end() && s->second.type() == QLangValue::Type::Function;
}

// Equality per spec §operator == and != with object dispatch: a string
// compared with an object is equal only when the object's __str__ value
// matches the string. displayString provides the __str__ call plus the
// class-name fallback, with the re-entrancy guard. Every other combination
// is the free isEqual (nil rules, numeric value equality, same-type values).
bool QLangVM::valueEqual(const QLangValue &a, const QLangValue &b) {
  if (a.type() == QLangValue::Type::String && b.type() == QLangValue::Type::Table &&
      isObjectValue(b))
    return a.asString() == displayString(b);
  if (b.type() == QLangValue::Type::String && a.type() == QLangValue::Type::Table &&
      isObjectValue(a))
    return b.asString() == displayString(a);
  return isEqual(a, b);
}

// VM dispatch for binary opcodes: routes EQ/NE through valueEqual (object
// __str__ dispatch) and everything else to the static doBinary.
QLangValue QLangVM::doBinaryVm(Opcode op, const QLangValue &a, const QLangValue &b) {
  if (op == Opcode::EQ || op == Opcode::NE) {
    bool eq = valueEqual(a, b);
    return QLangValue::Bool((op == Opcode::EQ) == eq ? 1 : 0);
  }
  // Ordering comparisons need the VM for the object-aware rules (SPEC
  // `## operators < and >`): string vs object via __str__, table vs table
  // per-index.
  const char *ord = nullptr;
  if (op == Opcode::LT) ord = "<";
  else if (op == Opcode::GT) ord = ">";
  else if (op == Opcode::LE) ord = "<=";
  else if (op == Opcode::GE) ord = ">=";
  if (ord) {
    int c = cmp(a, b, ord, this);
    switch (op) {
    case Opcode::LT: return QLangValue::Bool(c < 0 ? 1 : 0);
    case Opcode::GT: return QLangValue::Bool(c > 0 ? 1 : 0);
    case Opcode::LE: return QLangValue::Bool(c <= 0 ? 1 : 0);
    default:         return QLangValue::Bool(c >= 0 ? 1 : 0);
    }
  }
  return doBinary(op, a, b, this);
}

// Convert a value to the target declared type. Numeric types truncate/wrap
// (C-style cast semantics); other types pass through unchanged.
// For typed declarations — lenient: pass through non-numeric mismatches.
// For runtime cast — use OP_CAST instead, which is strict.
QLangValue convert(const QLangValue &v, QLangValue::Type type) {
  switch (type) {
  case QLangValue::Type::Bool:
    return QLangValue::Bool((long long)toInt64(v) ? 1 : 0);
  case QLangValue::Type::Char:
    return QLangValue::Char(static_cast<int8_t>((long long)toInt64(v)));
  case QLangValue::Type::Byte:
    return QLangValue::Byte(static_cast<uint8_t>((long long)toInt64(v)));
  case QLangValue::Type::Word:
    return QLangValue::Word(static_cast<uint16_t>((long long)toInt64(v)));
  case QLangValue::Type::Short:
    return QLangValue::Short(static_cast<int16_t>((long long)toInt64(v)));
  case QLangValue::Type::Int:
    return QLangValue::Int(static_cast<int32_t>((long long)toInt64(v)));
  case QLangValue::Type::Unsigned:
    return QLangValue::Unsigned(static_cast<uint32_t>((long long)toInt64(v)));
    case QLangValue::Type::Qword:
    return QLangValue::Qword(static_cast<uint64_t>((long long)toInt64(v)));
  case QLangValue::Type::Long:
    return QLangValue::Long(static_cast<int64_t>((long long)toInt64(v)));
  case QLangValue::Type::Set:
    // Typed declaration: array coerced into a set (SPEC §sets); a set passes
    // through; anything else passes through (lenient, like array/table).
    if (v.type() == QLangValue::Type::Array)
      return QLangValue::Set(toSetElements(*v.asArray()));
    return v;
  default: return v;
  }
}

// Strict runtime cast: throws on invalid source→target combinations.
// Used by OP_CAST for `cast<Type>(expr)` — distinct from OP_CONV which is
// lenient for typed declarations.
QLangValue strict_convert(const QLangValue &v, QLangValue::Type type,
                          QLangVM *vm = nullptr) {
  // Numeric targets: must be numeric source.
  if (type == QLangValue::Type::Bool || type == QLangValue::Type::Char ||
      type == QLangValue::Type::Byte || type == QLangValue::Type::Word ||
      type == QLangValue::Type::Short || type == QLangValue::Type::Int ||
            type == QLangValue::Type::Unsigned || type == QLangValue::Type::Qword ||
      type == QLangValue::Type::Long ||
      type == QLangValue::Type::Float) {
    if (v.type() == QLangValue::Type::Array || v.type() == QLangValue::Type::Table ||
        v.type() == QLangValue::Type::Set || v.type() == QLangValue::Type::Blob) {
      vThrow(vm, QLangErrorKind::Type,
             std::string("cannot cast ") + v.typeName() + " to " +
             QLangValue(type).typeName());
    }
    int64_t n = (long long)toInt64(v);
    switch (type) {
    case QLangValue::Type::Bool:
      return QLangValue::Bool(n ? 1 : 0);
    case QLangValue::Type::Char:
      return QLangValue::Char(static_cast<int8_t>(n));
    case QLangValue::Type::Byte:
      return QLangValue::Byte(static_cast<uint8_t>(n));
    case QLangValue::Type::Word:
      return QLangValue::Word(static_cast<uint16_t>(n));
    case QLangValue::Type::Short:
      return QLangValue::Short(static_cast<int16_t>(n));
    case QLangValue::Type::Int:
      return QLangValue::Int(static_cast<int32_t>(n));
    case QLangValue::Type::Unsigned:
      return QLangValue::Unsigned(static_cast<uint32_t>(n));
        case QLangValue::Type::Qword:
      return QLangValue::Qword(static_cast<uint64_t>(n));
    case QLangValue::Type::Long:
      return QLangValue::Long(static_cast<int64_t>(n));
    case QLangValue::Type::Float:
      return QLangValue::Float(toDouble(v));
    default:
      return v;
    }
  }
  // String target: convert anything to string.
  if (type == QLangValue::Type::String) {
    return QLangValue::String(valueToString(v, vm->m_scope));
  }
  // Collection targets: array <-> set convert (SPEC cast table); table and
  // same-type collections pass through; anything else is rejected.
  if (type == QLangValue::Type::Array || type == QLangValue::Type::Table ||
      type == QLangValue::Type::Set) {
    // A conversion builds a NEW payload: sharing the source handle would let a
    // mutation of the cast result reach the original (Phase 2 shares on
    // assignment and call, not across an explicit cast).
    if (type == QLangValue::Type::Array && v.type() == QLangValue::Type::Set)
      return QLangValue::Array(*v.asSet()); // cast<array>(set)
    if (type == QLangValue::Type::Set && v.type() == QLangValue::Type::Array)
      return QLangValue::Set(toSetElements(*v.asArray(), vm)); // cast<set>(array)
    if (v.type() != QLangValue::Type::Array && v.type() != QLangValue::Type::Table &&
        v.type() != QLangValue::Type::Set) {
      vThrow(vm, QLangErrorKind::Type,
             std::string("cannot cast ") + v.typeName() + " to " +
             QLangValue(type).typeName());
    }
    return v;
  }
  return v;
}

QLangValue QLangVM::doUnary(Opcode op, const QLangValue &a, QLangVM *vm) {
  auto thr = [&](QLangErrorKind k, const std::string &m) { vThrow(vm, k, m); };
  switch (op) {
  case Opcode::NEG:
    return QLangValue::Int(static_cast<int32_t>(-num(a, "-", vm)));
  case Opcode::NOT:
    return QLangValue::Bool(vm->isTrue(a) ? 0 : 1);
  case Opcode::INC:
    return QLangValue::Int(static_cast<int32_t>(num(a, "+", vm) + 1));
  case Opcode::DEC:
    return QLangValue::Int(static_cast<int32_t>(num(a, "-", vm) - 1));
  case Opcode::EVALKEY: {
    // $ operator: evaluate identifier and stringify (scalar-to-string)
    // Per spec: reject arrays, tables, and classes (objects)
    switch (a.type()) {
    case QLangValue::Type::Array:
    case QLangValue::Type::Table:
    case QLangValue::Type::Object:
    case QLangValue::Type::Function:
    case QLangValue::Type::Generator:
      thr(QLangErrorKind::Type,
                       std::string("operator '$' cannot stringify ") +
                         a.typeName() + " (collections and functions not supported)");
    default:
      return QLangValue::String(valueToString(a, vm->m_scope));
    }
  }
  default:
    thr(QLangErrorKind::Runtime,
                     std::string("unary operation not implemented: ") +
                     Bytecode::name(op));
  }
}

QLangValue QLangVM::doBinary(Opcode op, const QLangValue &a, const QLangValue &b,
                             QLangVM *vm) {
  auto thr = [&](QLangErrorKind k, const std::string &m) { vThrow(vm, k, m); };
  switch (op) {
  case Opcode::ADD:
    // Left-is-string rule: if the left operand is a string, concat both sides.
    if (a.type() == QLangValue::Type::String)
      return QLangValue::String(a.asString() + valueToString(b, vm->m_scope));
    if (b.type() == QLangValue::Type::String)
      thr(QLangErrorKind::Type,
                       "operator '+' cannot mix numeric and string operands");
    if (a.type() == QLangValue::Type::Array &&
        b.type() == QLangValue::Type::Array) {
      // Array concatenation: aa + ab -> new array (spec: "append both arrays").
      // The result is a fresh payload — `a + b` must not reach into `a`.
      auto result = std::make_shared<std::vector<QLangValue>>(*a.asArray());
      const auto &extra = *b.asArray();
      result->insert(result->end(), extra.begin(), extra.end());
      return QLangValue::Array(std::move(result));
    }
    if (a.type() == QLangValue::Type::Float ||
        b.type() == QLangValue::Type::Float)
      return QLangValue::Float(toDouble(a) + toDouble(b));
    return QLangValue::Int(static_cast<int32_t>(num(a, "+", vm) + num(b, "+", vm)));
  case Opcode::SUB:
    if (a.type() == QLangValue::Type::Float ||
        b.type() == QLangValue::Type::Float)
      return QLangValue::Float(toDouble(a) - toDouble(b));
    return QLangValue::Int(static_cast<int32_t>(num(a, "-", vm) - num(b, "-", vm)));
  case Opcode::MUL:
    if (a.type() == QLangValue::Type::Float ||
        b.type() == QLangValue::Type::Float)
      return QLangValue::Float(toDouble(a) * toDouble(b));
    return QLangValue::Int(static_cast<int32_t>(num(a, "*", vm) * num(b, "*", vm)));
  case Opcode::DIV: {
    if (a.type() == QLangValue::Type::Float ||
        b.type() == QLangValue::Type::Float) {
      double d = toDouble(b);
      if (d == 0.0)
        thr(QLangErrorKind::Type, "division by zero");
      return QLangValue::Float(toDouble(a) / d);
    }
    int64_t d = num(b, "/", vm);
    if (d == 0)
      thr(QLangErrorKind::Type, "division by zero");
    return QLangValue::Int(static_cast<int32_t>(num(a, "/", vm) / d));
  }
  case Opcode::MOD: {
    if (a.type() == QLangValue::Type::Float ||
        b.type() == QLangValue::Type::Float) {
      double d = toDouble(b);
      if (d == 0.0)
        thr(QLangErrorKind::Type, "modulo by zero");
      return QLangValue::Float(std::fmod(toDouble(a), d));
    }
    int64_t d = num(b, "%", vm);
    if (d == 0)
      thr(QLangErrorKind::Type, "modulo by zero");
    return QLangValue::Int(static_cast<int32_t>(num(a, "%", vm) % d));
  }
  case Opcode::POW: {
    // Exponential: pow(base, exp)
    if (a.type() == QLangValue::Type::Float ||
        b.type() == QLangValue::Type::Float) {
      return QLangValue::Float(std::pow(toDouble(a), toDouble(b)));
    }
    int64_t base = num(a, "**", vm);
    int64_t exp = num(b, "**", vm);
    if (exp < 0) {
      // Negative exponent with integer operands: return 0 (integer arithmetic)
      if (base == 1) return QLangValue::Int(1);
      if (base == -1) return QLangValue::Int(exp % 2 == 0 ? 1 : -1);
      return QLangValue::Int(0);
    }
    int64_t result = 1;
    for (int64_t i = 0; i < exp; i++)
      result *= base;
    return QLangValue::Int(static_cast<int32_t>(result));
  }
  case Opcode::AND:
    return QLangValue::Int(static_cast<int32_t>(num(a, "&", vm) & num(b, "&", vm)));
  case Opcode::OR:
    return QLangValue::Int(static_cast<int32_t>(num(a, "|", vm) | num(b, "|", vm)));
  case Opcode::XOR:
    return QLangValue::Int(static_cast<int32_t>(num(a, "^", vm) ^ num(b, "^", vm)));
  case Opcode::SHL:
    return QLangValue::Int(static_cast<int32_t>(num(a, "<<", vm) << num(b, "<<", vm)));
  case Opcode::SHR:
    return QLangValue::Int(static_cast<int32_t>(num(a, ">>", vm) >> num(b, ">>", vm)));
  case Opcode::LAND:
    return QLangValue::Bool(vm->isTrue(a) && vm->isTrue(b) ? 1 : 0);
  case Opcode::LOR:
    return QLangValue::Bool(vm->isTrue(a) || vm->isTrue(b) ? 1 : 0);
  case Opcode::EQ:
    return QLangValue::Bool(isEqual(a, b) ? 1 : 0);
  case Opcode::NE:
    return QLangValue::Bool(!isEqual(a, b) ? 1 : 0);
  case Opcode::LT:
    return QLangValue::Bool(cmp(a, b, "<") < 0 ? 1 : 0);
  case Opcode::GT:
    return QLangValue::Bool(cmp(a, b, ">") > 0 ? 1 : 0);
  case Opcode::LE:
    return QLangValue::Bool(cmp(a, b, "<=") <= 0 ? 1 : 0);
  case Opcode::GE:
    return QLangValue::Bool(cmp(a, b, ">=") >= 0 ? 1 : 0);
  case Opcode::INx: {
    // Membership test: `a in b` where b is an array (element match) or a
    // table (key match). Returns a Bool (spec §143-147).
    const QLangValue &col = b;
    if (col.type() == QLangValue::Type::Array) {
      for (const QLangValue &e : *col.asArray())
        if (cmp(a, e, "in") == 0)
          return QLangValue::Bool(1);
      return QLangValue::Bool(0);
    }
    if (col.type() == QLangValue::Type::Table) {
      std::string key = valueToString(a, vm->m_scope);
      return QLangValue::Bool(col.asTable()->count(key) ? 1 : 0);
    }
    if (col.type() == QLangValue::Type::Set) {
      // `x in set` mirrors `x in [..]` (SPEC §sets); element equality is the
      // same exact-match rule set dedup uses.
      return QLangValue::Bool(setContains(*col.asSet(), a) ? 1 : 0);
    }
    thr(QLangErrorKind::Type,
                     std::string("operator 'in' requires an array, table or set, got ") +
                         col.typeName());
  }
  default:
    thr(QLangErrorKind::Runtime,
                     std::string("binary operation not implemented: ") +
                     Bytecode::name(op));
  }
}

void QLangVM::pushCall(int calleeIndex, RegFile &callerRegs,
                       int resultDest, int argBase, int argc) {
  trace(TRACE_FRAME, "[pushCall] calleeIndex=%d functions.size()=%zu dest=%d argc=%d callerRegs=%zu\n", calleeIndex, m_prog.functions.size(), resultDest, argc, callerRegs.size());
  const Function &callee = m_prog.functions[calleeIndex];
  Frame cf;
  cf.funcIndex = calleeIndex;
  cf.callDest = resultDest;
  cf.regs.resize(callee.registerCount, QLangValue());
  // Copy the call arguments (caller regs argBase+1..argBase+argc) into the
  // callee's parameter registers 0..argc-1.
  for (int k = 0; k < argc; k++)
    cf.regs[k] = callerRegs[argBase + 1 + k];
  trace(TRACE_FRAME, "[pushCall] after arg copy, argc=%d\n", argc);
  m_frames.push_back(std::move(cf));
  trace(TRACE_FRAME, "[pushCall] after push_back\n");
  m_cur = static_cast<int>(m_frames.size()) - 1;
  m_frames[m_cur].pc = 0;
  trace(TRACE_FRAME, "[pushCall] done\n");
}

// ---------------------------------------------------------------------------
// Phase 1 executor: one dispatch loop (runFrames) with a returned/yielded
// result protocol. run(), callFunction() and generator resumption all drive
// it; there is no second opcode switch anywhere else.
// ---------------------------------------------------------------------------

namespace {
// RAII: if a QLangError unwinds through a live VM, release every frame's
// capture aliases so no Env keeps pointing at a dead frame. (Full exception
// semantics — scope sync-out on throw — land in Phase 2.)
struct CaptureReleaseGuard {
  QLangVM *vm;
  ~CaptureReleaseGuard() {
    vm->releaseAllCaptures();
  }
};
} // namespace

void QLangVM::bindCaptures(Frame *frame, const Function &fn,
                           const std::vector<CaptureRef> &spec,
                           std::vector<CaptureRef> &out) {
  if (spec.empty())
    return;
  // Local captures share one fresh Env; global captures need none (they
  // alias the context's table slots, which outlive every frame).
  EnvPtr env;
  for (size_t i = 0; i < spec.size(); i++) {
    const CaptureRef &cr = spec[i];
    // A negative spec register is the compiler's sentinel for "this capture is
    // a module global": the value is `-1 - <absolute global-table slot>`, so it
    // needs no name lookup. That matters for a linked program, whose merged
    // `vars` map holds only the entry module's names, and for a program loaded
    // from a `.qlc` image, which does not carry capture names at all
    // (external-modules plan v4 Phase 3b).
    if (cr.reg < 0) {
      out.push_back(CaptureRef{nullptr, -cr.reg - 1, -1}); // GLOBAL
      continue;
    }
    const std::string &name =
        (i < fn.captureNames.size()) ? fn.captureNames[i] : std::string();
    auto vit = m_prog.vars.find(name);
    if (!name.empty() && vit != m_prog.vars.end()) {
      // GLOBAL: alias the table slot directly (slot == main-frame register).
      // Live across frames, VM instances and run() boundaries — a `global`
      // read always sees the current top-level value (S2/P3).
      out.push_back(CaptureRef{nullptr, static_cast<int>(vit->second), -1});
      continue;
    }
    // LOCAL: cell seeded from the creating frame's register.
    if (!env) {
      env = std::make_shared<Env>();
      env->cells.reserve(spec.size());
      env->ownerRegs.reserve(spec.size());
    }
    int cell = static_cast<int>(env->cells.size());
    if (frame)
      env->cells.push_back(frame->regs[static_cast<size_t>(cr.reg)]);
    else
      env->cells.push_back(QLangValue());
    env->ownerRegs.push_back(cr.reg);
    out.push_back(CaptureRef{env, cell, cr.reg});
  }
  if (env && frame) {
    env->owner = static_cast<void *>(frame);
    frame->envs.push_back(env);
  }
}

const ImportEntry &QLangVM::importEntry(int index, const char *mnemonic) const {
  if (index < 0 || static_cast<size_t>(index) >= m_prog.imports.size())
    vmThrow(QLangErrorKind::Runtime,
            std::string(mnemonic) + ": import index " + std::to_string(index) +
                " is outside the program's import table (" +
                std::to_string(m_prog.imports.size()) + " entries)");
  return m_prog.imports[static_cast<size_t>(index)];
}

QLangValue &QLangVM::cellAt(const CaptureRef &cr) {
  if (cr.env == nullptr) {
    // Global capture: the context's table slot (see bindCaptures).
    return m_scope.globalTable().slots[static_cast<size_t>(cr.cell)];
  }
  Env &e = *cr.env;
  if (e.owner != nullptr) {
    Frame *fr = static_cast<Frame *>(e.owner);
    int reg = e.ownerRegs[static_cast<size_t>(cr.cell)];
    return fr->regs[static_cast<size_t>(reg)];
  }
  return e.cells[static_cast<size_t>(cr.cell)];
}

void QLangVM::releaseFrame(Frame &frame) {
  for (const EnvPtr &env : frame.envs) {
    if (env->owner == static_cast<void *>(&frame)) {
      for (size_t i = 0; i < env->ownerRegs.size() && i < env->cells.size();
           i++) {
        int reg = env->ownerRegs[i];
        if (reg >= 0 && reg < static_cast<int>(frame.regs.size()))
          env->cells[i] = std::move(frame.regs[static_cast<size_t>(reg)]);
      }
      env->owner = nullptr;
      env->ownerRegs.clear();
    }
  }
  frame.envs.clear();
}

void QLangVM::releaseAllCaptures() {
  for (Frame &f : m_frames)
    releaseFrame(f);
}

void QLangVM::popFrame() {
  releaseFrame(m_frames[static_cast<size_t>(m_cur)]);
  m_frames.pop_back();
  --m_cur;
}

QLangValue QLangVM::resumeGenerator(GeneratorState &gen) {
  if (gen.exhausted)
    return QLangValue();
  const Function &fn = m_prog.functions[static_cast<size_t>(gen.funcIndex)];
  // Move the saved state onto a live frame, run the single loop until
  // RETURN/YIELD, and let the YIELD/RETURN path move the frame back.
  Frame frame;
  frame.funcIndex = gen.funcIndex;
  frame.regs.owned = std::move(gen.regs);
  // The first resume restores a register file sized to the argument count
  // (MAKEGEN); grow it to the function's full register count so body
  // registers are in bounds.
  if (frame.regs.size() < static_cast<size_t>(fn.registerCount))
    frame.regs.resize(static_cast<size_t>(fn.registerCount), QLangValue());
  frame.captures = std::move(gen.captures);
  frame.pc = gen.pc;
  frame.callDest = -1;
  m_frames.push_back(std::move(frame));
  m_cur = static_cast<int>(m_frames.size()) - 1;
  m_suspended.reset();

  RunResult r = runFrames();
  if (r == RunResult::Yielded) {
    // The YIELD case stashed the frame state in the resume slot because this
    // frame has no caller dest register holding the instance.
    if (m_suspended) {
      gen = std::move(*m_suspended);
      m_suspended.reset();
    }
    return m_returnValue;
  }
  // RETURN (or fall-off end): the frame is already popped.
  gen.exhausted = true;
  return QLangValue();
}

void QLangVM::run() {
  CaptureReleaseGuard guard{this};
  // Set up the main frame (functions[0]). Its register file ALIASES the
  // context's global-table slots (Phase 2): top-level names live in the
  // table directly, so there is no seeding in and no sync out — the table
  // is current at every point, including mid-run throws.
  auto &slots = m_scope.globalSlots();
  if (slots.size() < static_cast<size_t>(m_prog.registerCount))
    slots.resize(static_cast<size_t>(m_prog.registerCount), QLangValue());
  // A linked program runs every imported module's top level first, in
  // dependency order, exactly once, before the entry module's (SPEC §bytecodes
  // "technical specifications" 3, plan Q35). Each is a main-like frame aliasing
  // the same table at its own register offset, so a module's globals are live
  // in the table by the time the entry module reads them.
  for (const ModuleInit &mi : m_prog.initSequence) {
    Frame initFrame;
    initFrame.funcIndex = mi.funcIndex;
    initFrame.regs.aliased = &slots;
    initFrame.regs.base = static_cast<size_t>(mi.regBase);
    m_frames.push_back(std::move(initFrame));
    m_cur = 0;
    while (m_cur >= 0) {
      if (runFrames() == RunResult::Yielded)
        break;
    }
  }
  Frame mainFrame;
  mainFrame.funcIndex = 0;
  mainFrame.regs.aliased = &slots;
  m_frames.push_back(std::move(mainFrame));
  m_cur = 0;

  // Drive the single executor until the main frame has returned.
  while (m_cur >= 0) {
    RunResult r = runFrames();
    if (r == RunResult::Yielded) {
      // A top-level YIELD is only reachable if a generator frame leaked onto
      // the main stack; the instance already holds the suspended state.
      break;
    }
    // Returned: the top frame was popped; loop again (the caller resumes) or
    // terminate when the main frame is gone.
  }
}

RunResult QLangVM::runFrames() {
  while (m_cur >= 0) {
    Frame &frame = m_frames[static_cast<size_t>(m_cur)];
    const Function &fn =
        m_prog.functions[static_cast<size_t>(frame.funcIndex)];
    const auto &code = fn.code;
    const auto &consts = m_prog.constants;

    if (frame.pc >= code.size()) {
      // Fell off the end without an explicit return: unwind with nil.
      // (No scope sync: the main frame aliases the global table — Phase 2.)
      m_returnValue = frame.regs.empty() ? QLangValue() : frame.regs[0];
      popFrame();
      if (m_cur >= 0)
        // Generator frame fell off its end: stop at the resume boundary.
        return RunResult::Returned;
      continue;
    }

    // The step counter lives on the context (Phase 5, F6): it accumulates
    // across dispatches and is never reset by the VM, so the host can read
    // stepsUsed() before/after a call to compute the per-dispatch cost.
    if (m_scope.step() > m_scope.maxSteps())
      vmThrow(QLangErrorKind::Runtime, "infinite loop (step limit)");

    // Decode the instruction at the current byte offset and advance pc past
    // it BEFORE execution: jump offsets are relative to the END of the jump
    // instruction (off = target - (jump_pc + jump_size)), so every pc update
    // below is a delta from the post-decode position.
    size_t pc = frame.pc;
    DecodedInstruction i;
    decodeInstruction(code.data(), pc, code.size(), i);
    frame.pc += i.size;
    Opcode op = i.op;
    trace(TRACE_VM_LOOP,
          "[VM] func=%d pc=%zu op=%d a=%d b=%d c=%d\n", frame.funcIndex,
          pc, (int)op, i.a, i.b, i.c);

    switch (op) {
    case Opcode::MOV:
      // One materializer: the modifier's kind field decides register / pool /
      // inline literal, and a pool value is cloned because it is stored into a
      // register (see freshContainerValue).
      frame.regs[i.a] = operandValue(i, frame, consts, true);
      break;
    case Opcode::DEEPCOPY: {
      // r[a] = independent recursive copy of r[b] (SPEC §functions
      // "deepcopy"). Fresh identity map per instruction: cycles inside one
      // value are preserved, nothing carries across instructions. A table
      // carrying `__deepcopy__` is copied by that hook (SPEC §classes
      // "deepcopy"); deepCopyValue dispatches it on the original.
      std::unordered_map<const void *, QLangValue> seen;
      frame.regs[i.a] = deepCopyValue(frame.regs[i.b], seen);
      // The copy is a new instance with its own `__destroy__` finalization
      // (SPEC §classes "deepcopy"); attachFinalizer is a no-op without the
      // method, idempotent on a hook result the constructor already
      // registered, and never touches the original's registration.
      attachFinalizer(frame.regs[i.a], m_scope);
      break;
    }
    case Opcode::NEG:
      frame.regs[i.a] = doUnary(i.op, frame.regs[i.b], this);
      break;
    case Opcode::NOT:
      frame.regs[i.a] = doUnary(i.op, frame.regs[i.b], this);
      break;
    case Opcode::INC:
      // In-place on r[a] (1-operand: only `a` is stored in the stream).
      frame.regs[i.a] = doUnary(i.op, frame.regs[i.a], this);
      break;
    case Opcode::DEC:
      // In-place on r[a] (1-operand: only `a` is stored in the stream).
      frame.regs[i.a] = doUnary(i.op, frame.regs[i.a], this);
      break;
    case Opcode::EVALKEY:
      frame.regs[i.a] = doUnary(i.op, frame.regs[i.b], this);
      break;
    case Opcode::CONV:
      frame.regs[i.a] =
          convert(frame.regs[i.b], static_cast<QLangValue::Type>(i.c));
      break;
    case Opcode::CAST:
      frame.regs[i.a] = strict_convert(frame.regs[i.b],
                                       static_cast<QLangValue::Type>(i.c),
                                       this);
      break;
    case Opcode::DELETEx: {
      // Only a top-level delete (main frame) erases a table slot — a delete
      // in a nested frame is a local-register clear with no scope effect
      // (P2). The slot is marked erased and its value cleared; re-assigning
      // revives it.
      if (m_cur == 0)
        m_scope.eraseGlobalSlot(static_cast<size_t>(i.a));
      frame.regs[static_cast<size_t>(i.a)] = QLangValue();
      break;
    }
    case Opcode::ARRGET: {
      QLangValue &base = frame.regs[i.b];
      if (base.type() != QLangValue::Type::Array)
        vmThrow(QLangErrorKind::Type, "cannot subscript non-array value");
      int64_t idx = (long long)toInt64(frame.regs[i.c]);
      const auto &elems = *base.asArray();
      if (idx < 0 || idx >= static_cast<int64_t>(elems.size()))
        vmThrow(QLangErrorKind::Runtime, "array index out of range");
      frame.regs[i.a] = elems[static_cast<size_t>(idx)];
      break;
    }
    case Opcode::ARRPUT: {
      QLangValue &base = frame.regs[i.a];
      if (base.type() != QLangValue::Type::Array)
        vmThrow(QLangErrorKind::Type, "cannot assign into non-array value");
      int64_t idx = (long long)toInt64(frame.regs[i.b]);
      auto &elems = *base.asArray();
      if (idx < 0 || idx >= static_cast<int64_t>(elems.size()))
        vmThrow(QLangErrorKind::Runtime, "array index out of range");
      elems[static_cast<size_t>(idx)] = frame.regs[i.c];
      break;
    }
    case Opcode::ARRAPP: {
      // Arrays append; sets insert with dedup (SPEC §sets: `s[] = v`).
      appendValue(frame.regs[i.a], frame.regs[i.b], this);
      break;
    }
    case Opcode::ARRAPPV: {
      QLangValue &base = frame.regs[i.a];
      if (base.type() != QLangValue::Type::Array)
        vmThrow(QLangErrorKind::Type, "cannot append non-array to array");
      if (frame.regs[i.b].type() != QLangValue::Type::Array)
        vmThrow(QLangErrorKind::Type, "cannot append non-array to array");
      auto &elems = *base.asArray();
      const ArrayValue &extra = frame.regs[i.b].asArray();
      // `a += b` where b shares a's payload (`a += a`, Phase 2 aliasing) would
      // invalidate the iterators mid-insert: snapshot the source first.
      if (extra.get() == base.asArray().get()) {
        std::vector<QLangValue> copy = elems;
        elems.insert(elems.end(), copy.begin(), copy.end());
      } else {
        elems.insert(elems.end(), extra->begin(), extra->end());
      }
      break;
    }
    case Opcode::ARRLEN:
      frame.regs[i.a] = QLangValue::Int(
          static_cast<int32_t>(lenOf(frame.regs[i.b], this)));
      break;
    case Opcode::LEN:
      frame.regs[i.a] = QLangValue::Int(
          static_cast<int32_t>(lenOf(frame.regs[i.b], this)));
      break;
    case Opcode::SEQGET:
      frame.regs[i.a] = seqGet(frame.regs[i.b],
                               (long long)toInt64(frame.regs[i.c]), this);
      break;
    case Opcode::TABGET: {
      QLangValue &base = frame.regs[i.b];
      const std::string &key = valueToString(frame.regs[i.c], m_scope);
      // Use ObjectOps dispatch for member access (Phase 0.4)
      const ObjectOps *ops = getObjectOps(base);
      if (ops) {
        frame.regs[i.a] = ops->getField(base, key, m_scope);
      } else {
        // Fallback for types without ObjectOps (should not happen for
        // tables/objects)
        vmThrow(QLangErrorKind::Type,
                        "cannot access member of non-object value");
      }
      break;
    }
    case Opcode::TABCREATE: {
      frame.regs[i.a] = QLangValue::Table();
      trace(TRACE_FULL, "[VM-TABCREATE] a=%d shared_ptr=%p\n", i.a,
            frame.regs[i.a].asTable().get());
      break;
    }
    case Opcode::TABFINALIZE: {
      attachFinalizer(frame.regs[i.a], m_scope);
      break;
    }
    case Opcode::TABPUT: {
      QLangValue &base = frame.regs[i.a];
      const std::string &key = valueToString(frame.regs[i.b], m_scope);
      // Use ObjectOps dispatch for member assignment (Phase 0.4)
      const ObjectOps *ops = getObjectOps(base);
      if (ops) {
        ops->setField(base, key, frame.regs[i.c], m_scope);
      } else {
        // Fallback for types without ObjectOps (should not happen for
        // tables/objects)
        vmThrow(QLangErrorKind::Type, "cannot assign member of non-object value");
      }
      break;
    }
    case Opcode::INDEXGET: {
      // Polymorphic subscript read (SPEC §bytecodes "indexget"): arrays take an
      // integer index, tables/class instances/external objects resolve the
      // stringified key through ObjectOps -- the same path TABGET uses for
      // member access, so t["age"] and t.age behave identically (closed field
      // set, const guards).
      QLangValue &base = frame.regs[i.b];
      if (base.type() == QLangValue::Type::Array) {
        int64_t idx = (long long)toInt64(frame.regs[i.c]);
        const auto &elems = *base.asArray();
        if (idx < 0 || idx >= static_cast<int64_t>(elems.size()))
          vmThrow(QLangErrorKind::Runtime, "array index out of range");
        frame.regs[i.a] = elems[static_cast<size_t>(idx)];
        break;
      }
      const ObjectOps *ops = getObjectOps(base);
      if (!ops)
        vmThrow(QLangErrorKind::Type,
                std::string("cannot subscript ") + base.typeName() + " value");
      frame.regs[i.a] =
          ops->getField(base, valueToString(frame.regs[i.c], m_scope), m_scope);
      break;
    }
    case Opcode::INDEXPUT: {
      // Polymorphic subscript write (SPEC §bytecodes "indexput"). The empty
      // subscript (t[]) is not handled here: the compiler lowers it to ARRAPP.
      QLangValue &base = frame.regs[i.a];
      if (base.type() == QLangValue::Type::Array) {
        int64_t idx = (long long)toInt64(frame.regs[i.b]);
        auto &elems = *base.asArray();
        if (idx < 0 || idx >= static_cast<int64_t>(elems.size()))
          vmThrow(QLangErrorKind::Runtime, "array index out of range");
        elems[static_cast<size_t>(idx)] = frame.regs[i.c];
        break;
      }
      const ObjectOps *ops = getObjectOps(base);
      if (!ops)
        vmThrow(QLangErrorKind::Type,
                std::string("cannot assign into ") + base.typeName() + " value");
      ops->setField(base, valueToString(frame.regs[i.b], m_scope),
                    frame.regs[i.c], m_scope);
      break;
    }
    case Opcode::CALL: {
      int dest = i.a;
      int argCount = i.c;
      // Instruction index for this CALL (pc was advanced past it at decode):
      // maps back to fn.loc for the builtin's error line/col.
      size_t idx = instructionIndexAt(fn, pc);
      std::vector<QLangValue> callArgs;
      callArgs.reserve(argCount);
      for (int k = 0; k < argCount; k++)
        callArgs.push_back(frame.regs[dest + 1 + k]);
      if (i.b < 0) {
        int nativeIdx = -1 - i.b;
        if (nativeIdx < 0 ||
            nativeIdx >= static_cast<int>(m_prog.nativeFuncs.size()))
          vmThrow(QLangErrorKind::Runtime, "unknown native function");
        frame.regs[dest] = m_prog.nativeFuncs[static_cast<size_t>(nativeIdx)](
            m_scope, callArgs);
      } else if (i.b == static_cast<int>(Builtin::Release)) {
        // release(x) mutates the argument register in place (drops the
        // owned shared_ptr); callBuiltin takes const copies, so it is
        // handled here (Phase 3, H2).
        releaseExternal(frame.regs[dest + 1],
                        idx < fn.loc.size() ? fn.loc[idx].first : 0,
                        idx < fn.loc.size() ? fn.loc[idx].second : 0,
                        this);
        frame.regs[dest] = QLangValue();
      } else {
        frame.regs[dest] = callBuiltin(static_cast<Builtin>(i.b), callArgs,
                                       m_scope, m_prog,
                                       idx < fn.loc.size() ? fn.loc[idx].first : 0,
                                       idx < fn.loc.size() ? fn.loc[idx].second : 0,
                                       this);
      }
      break;
    }
    case Opcode::CALLFN: {
      pushCall(i.b, frame.regs, i.a, i.a, i.c);
      // Def functions with captures are called through their def-time
      // closure (CALLCLOS); this path serves capture-free defs and, for the
      // rest, binds the callee's capture spec to cells seeded from the
      // caller frame.
      const Function &calleeFn = m_prog.functions[static_cast<size_t>(i.b)];
      bindCaptures(&m_frames[static_cast<size_t>(m_cur)], calleeFn,
                   calleeFn.captures,
                   m_frames[static_cast<size_t>(m_cur)].captures);
      continue;
    }
    case Opcode::CALLCLOS: {
      const QLangValue &cv = frame.regs[i.a];
      if (cv.type() == QLangValue::Type::BoundMethod) {
        // External C++ method (docs/EXTERNAL_TYPES.md): invoke the bound
        // member on the target object. Args sit at dest+1..dest+argCount
        // (same convention as CALL). selfMut() throws on an expired
        // borrow; C++ exceptions from the binding become runtime errors.
        const auto &bm = cv.asBoundMethod();
        ExternalRef target = bm.target; // copy: selfMut() is non-const
        void *self = target.selfMut();
        std::vector<QLangValue> args;
        args.reserve(i.c);
        for (int k = 0; k < i.c; k++)
          args.push_back(frame.regs[i.a + 1 + k]);
        try {
          frame.regs[i.b] = bm.member->invoke(self, m_scope, args);
        } catch (const QLangError &) {
          throw;
        } catch (const std::exception &e) {
          vmThrow(QLangErrorKind::Runtime, e.what());
        }
        break;
      }
      if (cv.type() == QLangValue::Type::Generator) {
        // Resume a generator instance: it runs in its own frame and
        // suspends on the next yield; the yielded value lands in dest.
        // The state is shared (heap) behind the value, so grab a pointer
        // before resuming: resumeGenerator pushes a frame, which can
        // reallocate m_frames and invalidate `frame`/`cv`.
        GeneratorState *gs = const_cast<GeneratorState *>(&cv.asGenerator());
        QLangValue result = resumeGenerator(*gs);
        m_frames[static_cast<size_t>(m_cur)].regs[static_cast<size_t>(i.b)] =
            result;
        continue;
      }
      if (cv.type() != QLangValue::Type::Function)
        vmThrow(QLangErrorKind::Type, "attempt to call a non-function value");
      // Extract the closure data before pushing: pushCall may reallocate
      // m_frames, which would dangle `frame`/`cv`. The Closure object itself
      // is heap-owned (shared_ptr), so the reference stays valid.
      const Closure &clos = cv.asFunction();
      // i.b is the result destination AND the arg base (args live at
      // i.b+1..i.b+argc). Normally i.b == i.a, the callee's home register;
      // they differ when the compiler hoisted the callee to a temp to
      // protect a capture-aliased register — the args then stay in the
      // original dest's slots.
      pushCall(clos.funcIndex, frame.regs, i.b, i.b, i.c);
      // The closure carries its bound cells; the callee frame uses them
      // directly (no re-binding at call time).
      m_frames[static_cast<size_t>(m_cur)].captures = clos.captures;
      continue;
    }
    case Opcode::EMIT: {
      // Deliver signal const[b] on r[a] with c args in r[a+1..a+c] (the same
      // operand convention as CALL). Delivery is synchronous: each connected
      // handler runs to completion, in connection order, before EMIT returns.
      std::vector<QLangValue> emitArgs;
      emitArgs.reserve(i.c);
      for (int k = 0; k < i.c; k++)
        emitArgs.push_back(frame.regs[i.a + 1 + k]);
      deliverSignal(frame.regs[i.a], consts[i.b].asString(), emitArgs);
      break;
    }
    case Opcode::MAKEFUNC: {
      const QLangValue &cv = consts[i.b];
      if (cv.type() != QLangValue::Type::Function)
        vmThrow(QLangErrorKind::Runtime, "invalid function constant");
      const Closure &spec = cv.asFunction();
      ClosurePtr closure = std::make_shared<Closure>();
      closure->funcIndex = spec.funcIndex;
      // Bind each capture to a live cell: local captures to a fresh cell
      // seeded from this frame (live-aliased until the frame pops), global
      // captures to the context's table slot (Phase 2).
      bindCaptures(&frame,
                   m_prog.functions[static_cast<size_t>(spec.funcIndex)],
                   spec.captures, closure->captures);
      // Register the prototype so host-driven calls (and CALLFN) reach the
      // def-time bindings.
      m_funcClosures[spec.funcIndex] = closure;
      frame.regs[i.a] = QLangValue::Function(closure);
      break;
    }
    case Opcode::MAKEGEN: {
      const QLangValue &cv = consts[i.b];
      if (cv.type() != QLangValue::Type::Function)
        vmThrow(QLangErrorKind::Runtime,
                           "invalid function constant for generator");
      const Closure &spec = cv.asFunction();
      // Phase 1: each generator value is its own instance. The instance
      // owns its saved frame state. Captures prefer the prototype closure's
      // cells (bound at def time, lexically correct at any call depth);
      // fall back to cells seeded from this frame (the call site).
      auto gen = std::make_shared<GeneratorState>();
      gen->funcIndex = spec.funcIndex;
      auto pit = m_funcClosures.find(spec.funcIndex);
      if (pit != m_funcClosures.end() && !pit->second->captures.empty())
        gen->captures = pit->second->captures;
      else
        bindCaptures(&frame,
                     m_prog.functions[static_cast<size_t>(spec.funcIndex)],
                     spec.captures, gen->captures);
      // Read call arguments from frame registers (already placed by
      // compiler).
      gen->regs.assign(static_cast<size_t>(i.c), QLangValue());
      for (int k = 0; k < i.c; k++)
        gen->regs[static_cast<size_t>(k)] =
            frame.regs[static_cast<size_t>(i.a + 1 + k)];
      gen->pc = 0;
      gen->exhausted = false;
      frame.regs[i.a] = QLangValue::Generator(gen);
      break;
    }
    case Opcode::REFLOAD: {
      const CaptureRef &cr = frame.captures[static_cast<size_t>(i.b)];
      frame.regs[i.a] = cellAt(cr);
      break;
    }
    case Opcode::REFSTORE: {
      const CaptureRef &cr = frame.captures[static_cast<size_t>(i.b)];
      cellAt(cr) = frame.regs[i.a];
      break;
    }
    case Opcode::RETURN: {
      m_returnValue = frame.regs.empty() ? QLangValue() : frame.regs[0];
      int callDest = frame.callDest;
      popFrame();
      if (m_cur >= 0 && callDest >= 0)
        m_frames[static_cast<size_t>(m_cur)].regs[static_cast<size_t>(callDest)] =
            m_returnValue;
      else if (m_cur >= 0)
        // A generator frame (callDest == -1) returned: stop here so the
        // caller is not run past the resume boundary.
        return RunResult::Returned;
      continue;
    }
    case Opcode::YIELD: {
      // Suspend: move this frame's state into the generator instance.
      // releaseFrame first so captured registers move into their cells
      // before the register file is moved out.
      QLangValue yielded =
          frame.regs.empty() ? QLangValue() : frame.regs[0];
      int callDest = frame.callDest;
      int funcIndex = frame.funcIndex;
      // pc already points past the YIELD instruction (advanced at decode).
      size_t resumePc = frame.pc;
      releaseFrame(frame);
      GeneratorState *gs = nullptr;
      if (m_cur >= 1 && callDest >= 0) {
        QLangValue &slot = m_frames[static_cast<size_t>(m_cur - 1)]
                               .regs[static_cast<size_t>(callDest)];
        if (slot.type() == QLangValue::Type::Generator)
          gs = &slot.asGenerator();
      }
      if (gs) {
        gs->funcIndex = funcIndex;
        gs->regs = frame.regs.takeOwned();
        gs->captures = std::move(frame.captures);
        gs->pc = resumePc;
        gs->exhausted = false;
      } else {
        // Host-driven resume (no caller frame holds the instance): stash the
        // suspended state in the resume slot for resumeGenerator().
        GeneratorState st;
        st.funcIndex = funcIndex;
        st.regs = frame.regs.takeOwned();
        st.captures = std::move(frame.captures);
        st.pc = resumePc;
        st.exhausted = false;
        m_suspended = std::move(st);
      }
      popFrame();
      m_returnValue = yielded;
      if (m_cur >= 0 && callDest >= 0)
        m_frames[static_cast<size_t>(m_cur)].regs[static_cast<size_t>(callDest)] =
            yielded;
      return RunResult::Yielded;
    }
    case Opcode::ADD:
    case Opcode::SUB:
    case Opcode::MUL:
    case Opcode::DIV:
    case Opcode::AND:
    case Opcode::OR:
    case Opcode::XOR:
    case Opcode::SHL:
    case Opcode::SHR:
    case Opcode::LAND:
    case Opcode::LOR:
    case Opcode::EQ:
    case Opcode::NE:
    case Opcode::LT:
    case Opcode::GT:
    case Opcode::LE:
    case Opcode::GE:
    case Opcode::MOD:
    case Opcode::POW:
    case Opcode::INx:
      // Arity rule (plan §2.6): a right operand that is not a register means
      // the in-place 2-operand form r[a] = r[a] op right (the stream carries no
      // trailing `c`); the register form keeps r[b], r[c].
      if (isInPlaceForm(op, i.rightKind)) {
        QLangValue rhs = operandValue(i, frame, consts, false);
        frame.regs[i.a] = doBinaryVm(op, frame.regs[i.a], rhs);
      } else {
        frame.regs[i.a] = doBinaryVm(op, frame.regs[i.b], frame.regs[i.c]);
      }
      break;
    case Opcode::CMP: {
      // cmp always compares against r[a]; only the right operand's kind varies.
      QLangValue rhs = operandValue(i, frame, consts, false);
      frame.flags = cmpFlags(frame.regs[i.a], rhs, this);
      break;
    }
    case Opcode::EQF:
    case Opcode::LTF:
    case Opcode::GTF:
    case Opcode::LTEF:
    case Opcode::GTEF: {
      // The flag compares have cmp's operand shape (left register + a right
      // operand selected by its kind: register, immediate or pool entry) but
      // the general relation rules, so a String/Float/Array/Table/object
      // comparison can set the flags without a boolean temporary. The
      // in-place arity rule means `c` is not in the stream — never read it.
      QLangValue rhs = operandValue(i, frame, consts, false);
      frame.flags = flagCompare(op, frame.regs[i.a], rhs);
      break;
    }
    case Opcode::TST:
      // Truthiness into the flags (SPEC §bytecodes "OpCode tst"): eq from
      // truth(r[a]), the other two bits cleared, so the flag jumps are
      // reachable from any value. `tst` + `je`/`jne` is the only truthiness
      // test form since Phase 3 S3 retired the truthiness jumps.
      frame.flags = isTrue(frame.regs[i.a]) ? CMP_EQ : 0;
      break;
    case Opcode::GETIMPORT: {
      // An imported global var/const (SPEC §bytecodes "import table (lookup)"):
      // operand b names an import-table entry whose `ref` is the DEFINING
      // module's shared cell (SPEC §bytecodes "technical specifications" 3).
      // The cell is read out of the context's global table rather than this
      // frame, which is what makes an imported name reachable at any depth — a
      // register operand could only name a cell of the executing module's own
      // slice. A linked program rewrites the entry's `ref` into the merged
      // absolute slot, so the same instruction serves both.
      const ImportEntry &im = importEntry(i.b, "getimport");
      if (im.kind == ExportKind::Func)
        vmThrow(QLangErrorKind::Runtime,
                "imported name '" + im.name + "' is a function, not a value");
      if (im.ref < 0 ||
          static_cast<size_t>(im.ref) >= m_scope.globalTable().slots.size())
        vmThrow(QLangErrorKind::Runtime,
                "imported name '" + im.name + "' has no cell in the global table");
      frame.regs[i.a] = m_scope.globalTable().slots[static_cast<size_t>(im.ref)];
      break;
    }
    case Opcode::CALLIMPORT: {
      // A call of a function exported by another module. A linked program has
      // no `callimport` left (the linker resolves it into `callfn`/`makegen`),
      // so this is the instruction a module image runs directly — the form a
      // future runtime registry executes (plan Phase 3c).
      const ImportEntry &im = importEntry(i.b, "callimport");
      if (im.kind != ExportKind::Func)
        vmThrow(QLangErrorKind::Runtime,
                "imported name '" + im.name + "' is not a function");
      if (im.ref < 0 ||
          static_cast<size_t>(im.ref) >= m_prog.functions.size())
        vmThrow(QLangErrorKind::Runtime,
                "imported function '" + im.name + "' is not in the function table");
      const int fi = im.ref;
      const Function &calleeFn = m_prog.functions[static_cast<size_t>(fi)];
      if (!calleeFn.isGenerator) {
        pushCall(fi, frame.regs, i.a, i.a, i.c);
        bindCaptures(&m_frames[static_cast<size_t>(m_cur)], calleeFn,
                     calleeFn.captures,
                     m_frames[static_cast<size_t>(m_cur)].captures);
        continue;
      }
      // An exported generator: the generator value is built here rather than
      // from a pool Closure spec, because the import table names the function
      // itself (same instance-per-value rule as MAKEGEN).
      auto gen = std::make_shared<GeneratorState>();
      gen->funcIndex = fi;
      auto pit = m_funcClosures.find(fi);
      if (pit != m_funcClosures.end() && !pit->second->captures.empty())
        gen->captures = pit->second->captures;
      else
        bindCaptures(&frame, calleeFn, calleeFn.captures, gen->captures);
      gen->regs.assign(static_cast<size_t>(i.c), QLangValue());
      for (int k = 0; k < i.c; k++)
        gen->regs[static_cast<size_t>(k)] =
            frame.regs[static_cast<size_t>(i.a + 1 + k)];
      gen->pc = 0;
      gen->exhausted = false;
      frame.regs[i.a] = QLangValue::Generator(gen);
      break;
    }
    case Opcode::JE:
    case Opcode::JNE:
    case Opcode::JG:
    case Opcode::JGE:
    case Opcode::JL:
    case Opcode::JLE:
      if (flagsSatisfied(op, frame.flags))
        frame.pc += static_cast<size_t>(i.jumpOffset);
      continue;
    case Opcode::JMP:
      frame.pc += static_cast<size_t>(i.jumpOffset);
      continue;
    case Opcode::CONTINUE:
      frame.pc += static_cast<size_t>(i.jumpOffset);
      continue;
    case Opcode::BREAK:
      frame.pc += static_cast<size_t>(i.jumpOffset);
      continue;
    case Opcode::Nop:
      break; // takes no action (SPEC §bytecodes "OpCode nop")
    }
  }
  return RunResult::Returned; // defensive: the frame stack ran out
}

// ---------------------------------------------------------------------------
// Host-driven function call (QLangContext::call / callClosure).
// ---------------------------------------------------------------------------

QLangValue QLangVM::callFunction(int funcIndex,
                                 const std::vector<QLangValue> &args) {
  if (funcIndex < 0 || funcIndex >= static_cast<int>(m_prog.functions.size()))
    vmThrow(QLangErrorKind::Runtime, "unknown function");

  const Function &fn = m_prog.functions[static_cast<size_t>(funcIndex)];

  // --- Generator function: create a fresh instance and run it to the first
  // --- yield. The instance owns all subsequent state (Phase 1).
  if (fn.isGenerator) {
    auto gen = std::make_shared<GeneratorState>();
    gen->funcIndex = funcIndex;
    // Prefer the def-time prototype closure's bound cells (host-driven call
    // after run()); otherwise bind fresh cells from the current top frame
    // (the call site); with no live frame, bind nil cells so REFLOAD/
    // REFSTORE stay safe.
    // Prefer the def-time prototype closure's bound cells (host-driven call
    // after run()); otherwise bind fresh cells from the current top frame
    // (the call site); with no live frame, local captures bind nil cells so
    // REFLOAD/REFSTORE stay safe (bindCaptures handles frame == nullptr).
    auto pit = m_funcClosures.find(funcIndex);
    if (pit != m_funcClosures.end() && !pit->second->captures.empty())
      gen->captures = pit->second->captures;
    else
      bindCaptures(m_frames.empty() ? nullptr : &m_frames.back(), fn,
                   fn.captures, gen->captures);
    gen->regs.assign(args.size(), QLangValue());
    for (size_t k = 0; k < args.size(); k++)
      gen->regs[k] = args[k];
    gen->pc = 0;
    gen->exhausted = false;
    return resumeGenerator(*gen);
  }

  // --- Normal (non-generator) function: push a frame and run the single
  // --- loop to the return.
  Frame frame;
  frame.funcIndex = funcIndex;
  frame.regs.resize(static_cast<size_t>(fn.registerCount), QLangValue());
  for (size_t k = 0; k < args.size() && k < frame.regs.size(); k++)
    frame.regs[k] = args[k];
  // Bind the capture spec the same way the def-time closure would: prefer
  // the prototype's cells, else bind from the top frame (nil cells when
  // there is no live frame — bindCaptures handles frame == nullptr).
  auto pit = m_funcClosures.find(funcIndex);
  if (pit != m_funcClosures.end() && !pit->second->captures.empty())
    frame.captures = pit->second->captures;
  else
    bindCaptures(m_frames.empty() ? nullptr : &m_frames.back(), fn,
                 fn.captures, frame.captures);
  frame.pc = 0;
  frame.callDest = -1;
  m_frames.push_back(std::move(frame));
  m_cur = static_cast<int>(m_frames.size()) - 1;

  CaptureReleaseGuard guard{this};
  RunResult r = runFrames();
  (void)r; // Returned (or defensive stack-exhaust): the frame was popped.
  return m_returnValue;
}

} // namespace qlang
