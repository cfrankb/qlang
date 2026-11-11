#pragma once

// Declarative meta for exposing C++ types to QLang (docs/EXTERNAL_TYPES.md).
//
// Qt-style, one declarative line per binding, no MOC, no code generation.
// The binding lines need no class name: the class is named **once**, as the
// CRTP base `qlang::QLObject<Point>`, which exposes a `SelfType` alias. The
// entry macros form *qualified* member pointers (`&SelfType::x`,
// `&SelfType::translate`) — unqualified `&x` is not a pointer-to-member in
// C++ (it is ill-formed in a static context and yields a bound `int*`/is
// rejected for methods in a non-static one), so a type is always required.
// The one exception is QL_SIGNAL: its member function is declared but never
// defined, so the macro names its *type* (`decltype(&SelfType::name)`) instead
// of forming a pointer to it — taking the address would odr-use a definition
// that does not exist, which MSVC reports as an unresolved external.
// The script-visible type *name* is recovered from the platform "current
// function" string (QL_PRETTY_FUNCTION: __PRETTY_FUNCTION__ on
// GCC/Clang/MinGW/emscripten, __FUNCSIG__ on MSVC; `__CLASS__` is MSVC-only
// and not used). The macros avoid the GNU `##__VA_ARGS__` extension so the
// header is portable across g++/Clang/MinGW/MSVC/emscripten. Everything is
// `QL_`-prefixed (or in `namespace qlang`) so a translation unit can include
// this header alongside Qt headers without macro collisions.
//
// Usage:
//   struct Point : qlang::QLObject<Point> {
//     double x = 0, y = 0;                          // public -> read-write
//     double dist() const { ... }                   // const getter -> read-only
//     void translate(double dx, double dy) { ... }
//     QL_OBJECT(
//       QL_PROPERTY(x),
//       QL_PROPERTY(dist),
//       QL_INVOKABLE(translate),
//       QL_ENUM(PointKind, KindA, KindB),
//       QL_STR(str),
//       QL_CTOR([](double x, double y) {
//         return std::make_shared<Point>(x, y); })
//     );
//   };
//   ctx.import<Point>();
// The typed QL_CTOR form is preferred: the generated wrapper checks arity,
// applies the method-side conversions, and records the argument tags for the
// type manifest. The context-taking form —
// (QLangContext&, const std::vector<qlang::QValue>&) — also works and must
// validate a.size() itself.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "qlang/QLangErrors.h"
#include "qlang/QValue.h"

// Cross-platform "current function" string. GCC/Clang/MinGW/emscripten
// provide __PRETTY_FUNCTION__; MSVC provides __FUNCSIG__ instead. Both forms
// contain the substring "::qlang_meta" for the meta accessor, so
// meta_detail::className() parses either.
#if defined(_MSC_VER)
#define QL_PRETTY_FUNCTION __FUNCSIG__
#else
#define QL_PRETTY_FUNCTION __PRETTY_FUNCTION__
#endif

namespace qlang {

// Forward declaration for QLangContext (used in method/ctor signatures).
class QLangContext;

// Portable type tags for the type manifest (portable-module-plan P1; the
// `QLM1` codec in P2 is the first consumer). The set mirrors exactly what
// `qval_of()` overloads and `QLangValue::as<T>()` accepts; tags are
// descriptive metadata only — no runtime dispatch path consults them, and
// an unrecognized type yields `Unknown` rather than a compile error, so a
// binding over an exotic member type keeps compiling.
enum class QTypeTag : uint8_t {
  Unknown = 0x00,  // outside the supported set — related checks stay silent
  Bool = 0x01,
  Char = 0x02,   // int8_t / char / signed char
  Short = 0x03,  // int16_t
  Int = 0x04,    // int32_t / int
  Long = 0x05,   // int64_t / long long
  Byte = 0x06,   // uint8_t
  Word = 0x07,   // uint16_t
  Dword = 0x08,  // uint32_t / unsigned int
  Qword = 0x09,  // uint64_t / unsigned long long
  Float = 0x0A,  // double / float (float values land in the double)
  Str = 0x0B,    // std::string / const char*
  External = 0xFE,  // opaque C++ object — the name is not portable
  Void = 0xFF,
};

// Compile-time tag of one C++ type: exact match against the scalar set
// `qval_of()`/`as<T>()` speak; anything pointer- or class-shaped is the
// opaque `External`, the rest is `Unknown`. Never a hard error.
template <class T> constexpr QTypeTag qtype_tag() {
  using D = std::decay_t<T>;
  if constexpr (std::is_same_v<D, void>)
    return QTypeTag::Void;
  else if constexpr (std::is_same_v<D, bool>)
    return QTypeTag::Bool;
  else if constexpr (std::is_same_v<D, int8_t> ||
                     std::is_same_v<D, signed char> || std::is_same_v<D, char>)
    return QTypeTag::Char;
  else if constexpr (std::is_same_v<D, int16_t>)
    return QTypeTag::Short;
  else if constexpr (std::is_same_v<D, int32_t>)
    return QTypeTag::Int;
  else if constexpr (std::is_same_v<D, int64_t> || std::is_same_v<D, long long>)
    return QTypeTag::Long;
  else if constexpr (std::is_same_v<D, uint8_t>)
    return QTypeTag::Byte;
  else if constexpr (std::is_same_v<D, uint16_t>)
    return QTypeTag::Word;
  else if constexpr (std::is_same_v<D, uint32_t>)
    return QTypeTag::Dword;
  else if constexpr (std::is_same_v<D, uint64_t> ||
                     std::is_same_v<D, unsigned long long>)
    return QTypeTag::Qword;
  else if constexpr (std::is_same_v<D, double> || std::is_same_v<D, float>)
    return QTypeTag::Float;
  else if constexpr (std::is_same_v<D, std::string> ||
                     std::is_same_v<D, const char *> ||
                     std::is_same_v<D, char *>)
    return QTypeTag::Str;
  else if constexpr (std::is_pointer_v<D> || std::is_class_v<D>)
    return QTypeTag::External;
  else
    return QTypeTag::Unknown;
}

// List of parameter tags for a tuple of argument types (methods, signals,
// typed constructors share the vocabulary).
template <class Args, std::size_t... I>
std::vector<QTypeTag> qtype_tags(std::index_sequence<I...>) {
  return {qtype_tag<std::tuple_element_t<I, Args>>()...};
}


// QLObject (the CRTP base for external types) lives in qlang/QLangObject.h;
// it is included transitively via QValue.h above.

// One bound member of an external type.
//   kind 0 = read-write property (data member, or READ+WRITE accessors)
//   kind 1 = read-only property  (const getter, or READ-only accessor)
//   kind 2 = method (QL_INVOKABLE / QL_SLOTS)
struct Member {
  int kind = 0;
  std::string name;
  std::function<QLangValue(const void*)> get;
  std::function<void(void*, const QLangValue&)> set;
  std::function<QLangValue(void*, QLangContext&, const std::vector<QLangValue>&)>
      invoke;
  // Describable shape (portable-module-plan P1): properties fill getTag
  // (and setTag unless read-only), methods fill argTags/retTag. Pure
  // metadata for the manifest — dispatch never consults them.
  QTypeTag getTag = QTypeTag::Unknown;
  QTypeTag setTag = QTypeTag::Unknown;
  std::vector<QTypeTag> argTags;
  QTypeTag retTag = QTypeTag::Unknown;
};

// One enumerator entry: the script-visible name plus the C++ value.
struct EnumEntry {
  std::string name;
  long value;
};

// One enum/flag table declared by QL_ENUM / QL_FLAGS.
struct EnumMeta {
  std::string typeName;
  std::vector<EnumEntry> entries;
};

// One signal declared by QL_SIGNAL: the script-visible name plus a
// convertibility checker per parameter. The checkers apply the same
// as<A>() conversion rules a method call uses, so a signal argument that
// would be accepted by a same-signature method is accepted by emit.
struct SignalMeta {
  std::string name;
  std::vector<std::function<bool(const QLangValue&)>> argChecks;
  std::vector<QTypeTag> argTags;  // describable shape (P1), same order
};

// Compile-time meta for one external type, produced by QL_OBJECT(...).
struct TypeMeta {
  std::string name;
  std::vector<Member> members;
  std::function<QLangValue(const void*)> toString;
  std::function<std::shared_ptr<void>(QLangContext&, const std::vector<QLangValue>&)>
      construct;
  std::vector<EnumMeta> enums;
  std::vector<SignalMeta> signals;
  // Concrete C++ type of constructed objects (Phase 3): deduced from the
  // QL_CTOR lambda's return type. `typeCheck` validates a resolved object
  // pointer against it (dynamic_cast at the binding site); both are null
  // when the lambda returns a void-typed shared_ptr.
  const std::type_info* typeInfo = nullptr;
  std::function<bool(const void*)> typeCheck = nullptr;
  // Describable shape (portable-module-plan P1). hasStr/hasCtor mirror the
  // clause presence (set by add()); ctorArgTags is filled only by the
  // **typed** QL_CTOR form — empty + hasCtor means the context-taking form,
  // whose signature is invisible to the manifest (presence-only checks).
  bool hasStr = false;
  bool hasCtor = false;
  std::vector<QTypeTag> ctorArgTags;
};

namespace meta_detail {

enum class Acc { read, write };

template <class P> struct AccPtr {
  Acc k;
  P p;
};
template <class P> AccPtr<P> acc(P p, Acc k) {
  return {k, p};
}

// --- QL_PROPERTY overloads (overload resolution by member-pointer type) ---

// Data member -> read-write property.
template <class T, class M> Member prop(const char* n, M T::*m) {
  Member mem{0, n,
             [m](const void* s) {
               return qval_of(static_cast<const T*>(s)->*m);
             },
             [m](void* s, const QLangValue& v) {
               static_cast<T*>(s)->*m = v.as<M>();
             }};
  mem.getTag = mem.setTag = qtype_tag<M>();
  return mem;
}

// Const getter listed bare -> read-only property.
template <class T, class R> Member prop(const char* n, R (T::*g)() const) {
  Member mem{1, n,
             [g](const void* s) {
               return qval_of((static_cast<const T*>(s)->*g)());
             },
             nullptr, nullptr};
  mem.getTag = qtype_tag<R>();  // read-only: setTag stays Unknown
  return mem;
}

// QL_PROPERTY(name, READ g) -> computed read-only property.
template <class T, class R>
Member prop(const char* n, AccPtr<R (T::*)() const> g) {
  Member mem{1, n,
             [g](const void* s) {
               return qval_of((static_cast<const T*>(s)->*g.p)());
             },
             nullptr, nullptr};
  mem.getTag = qtype_tag<R>();
  return mem;
}

// QL_PROPERTY(name, READ g, WRITE s) -> computed read-write property.
// The setter is `void (T::*)(M)`: Qt-style setters return void, so the
// parameter type M is the only deduced type (return == parameter would
// reject every real setter). The tags may therefore differ per direction.
template <class T, class R, class M>
Member prop(const char* n, AccPtr<R (T::*)() const> g,
            AccPtr<void (T::*)(M)> s) {
  Member mem{0, n,
             [g](const void* o) {
               return qval_of((static_cast<const T*>(o)->*g.p)());
             },
             [s](void* o, const QLangValue& v) {
               (static_cast<T*>(o)->*s.p)(v.as<M>());
             }};
  mem.getTag = qtype_tag<R>();
  mem.setTag = qtype_tag<M>();
  return mem;
}

// --- QL_INVOKABLE / QL_SLOTS: member function -> method ------------------

// Index-sequence argument converter: converts the script args via as<A>()
// and throws a Type error on an arity mismatch.
template <class T, class R, class... A, std::size_t... I>
QLangValue call(R (T::*f)(A...), T* self, const std::vector<QLangValue>& a,
                std::index_sequence<I...>) {
  if (a.size() != sizeof...(A))
    throw QLangError(QLangErrorKind::Type, 0, 0,
                     "method expects " + std::to_string(sizeof...(A)) +
                         " argument(s), got " + std::to_string(a.size()));
  if constexpr (std::is_void_v<R>) {
    (self->*f)(a[I].as<A>()...);
    return QValue();
  } else {
    return qval_of((self->*f)(a[I].as<A>()...));
  }
}

template <class T, class R, class... A, std::size_t... I>
QLangValue call(R (T::*f)(A...) const, const T* self,
                const std::vector<QLangValue>& a, std::index_sequence<I...>) {
  if (a.size() != sizeof...(A))
    throw QLangError(QLangErrorKind::Type, 0, 0,
                     "method expects " + std::to_string(sizeof...(A)) +
                         " argument(s), got " + std::to_string(a.size()));
  if constexpr (std::is_void_v<R>) {
    (self->*f)(a[I].as<A>()...);
    return QValue();
  } else {
    return qval_of((self->*f)(a[I].as<A>()...));
  }
}

// Non-const member function -> method.
template <class T, class R, class... A>
Member method(const char* n, R (T::*f)(A...)) {
  Member mem{2, n, nullptr, nullptr,
             [f](void* s, QLangContext& c, const std::vector<QLangValue>& a) {
               return call(f, static_cast<T*>(s), a,
                           std::index_sequence_for<A...>{});
             }};
  mem.argTags = {qtype_tag<A>()...};
  mem.retTag = qtype_tag<R>();  // void -> Void (qtype_tag maps it)
  return mem;
}

// Const member function -> method.
template <class T, class R, class... A>
Member method(const char* n, R (T::*f)(A...) const) {
  Member mem{2, n, nullptr, nullptr,
             [f](void* s, QLangContext& c, const std::vector<QLangValue>& a) {
               return call(f, static_cast<const T*>(s), a,
                           std::index_sequence_for<A...>{});
             }};
  mem.argTags = {qtype_tag<A>()...};
  mem.retTag = qtype_tag<R>();
  return mem;
}

// --- QL_SIGNAL: declared member function -> signal -----------------------
// The signal's member function is declared (never implemented); only its
// signature is used. The parameter types become per-argument
// convertibility checkers for emit / emitSignal.

// Per-argument convertibility check: true when the value converts to A
// under the same rules a method call applies (QLangValue::as<A>()).
template <class A>
std::function<bool(const QLangValue&)> arg_check() {
  return [](const QLangValue& v) {
    try {
      v.as<A>();
      return true;
    } catch (const QLangError&) {
      return false;
    }
  };
}

// Parameter types of a signal's member-function type (non-const and const).
template <class Sig> struct SignalArgs {
  static_assert(sizeof(Sig) == 0,
                "QL_SIGNAL requires the name of a member function");
};
template <class T, class R, class... A> struct SignalArgs<R (T::*)(A...)> {
  using type = std::tuple<A...>;
};
template <class T, class R, class... A>
struct SignalArgs<R (T::*)(A...) const> {
  using type = std::tuple<A...>;
};

// Signature-only signal builder. `Sig` is the member-function *type*
// (`decltype(&SelfType::name)`), never its address: taking the address of the
// declared-but-never-defined signal function odr-uses it, and MSVC then emits
// an external reference to a definition that does not exist (LNK2019). Naming
// the type in `decltype` is an unevaluated context, so no definition is
// required on any compiler.
template <class Sig, std::size_t... I>
SignalMeta signal_args(const char* n, std::index_sequence<I...>) {
  SignalMeta s;
  s.name = n;
  using Args = typename SignalArgs<Sig>::type;
  (s.argChecks.push_back(arg_check<std::tuple_element_t<I, Args>>()), ...);
  (s.argTags.push_back(qtype_tag<std::tuple_element_t<I, Args>>()), ...);
  return s;
}

template <class Sig>
SignalMeta signal(const char* n) {
  return signal_args<Sig>(
      n, std::make_index_sequence<
             std::tuple_size_v<typename SignalArgs<Sig>::type>>{});
}

// --- Whole-group enum tables (QL_ENUM / QL_FLAGS) -------------------------

// Whole-group table from explicit (name, value) pairs; the QL_ENUM/QL_FLAGS
// macros form the pairs from bare enumerator names (E::name + #name).
// The pair *types* are deduced directly: a non-pack enum type cannot appear
// in the pack element type `std::pair<const char*, E>...` (GCC rejects it),
// and the macro is the only call site so no extra type check is needed.
template <class... Pairs>
EnumMeta enum_table(const char* tn, Pairs... ps) {
  return {tn, {{ps.first, static_cast<long>(ps.second)}...}};
}

// --- str / ctor clauses ----------------------------------------------------

struct StrClause {
  std::function<QLangValue(const void*)> f;
};
struct CtorClause {
  std::function<std::shared_ptr<void>(QLangContext&, const std::vector<QLangValue>&)>
      f;
  const std::type_info* typeInfo = nullptr;
  std::function<bool(const void*)> check = nullptr;
  std::vector<QTypeTag> argTags;  // typed form only; empty = context-taking
};

template <class T, class R> StrClause str_clause(R (T::*f)() const) {
  return {[f](const void* s) {
            return qval_of((static_cast<const T*>(s)->*f)());
          }};
}

// --- The two QL_CTOR forms (portable-module-plan P1) ----------------------
// Discrimination is by invocability: a callable invocable with
// (QLangContext&, const std::vector<QLangValue>&) is the *context-taking*
// form, unchanged. Note its signature is opaque to any template and the
// wrapper cannot guard it: **this form must validate a.size() itself** —
// a short-arity call used to be an out-of-bounds read of `a` inside the
// host lambda (UB, not a clean type error). It stays for ctors that
// genuinely need the context.
// The *typed* form declares the real parameter types,
//   QL_CTOR([](double x, double y) { return std::make_shared<Point>(x, y); })
// and its R(A...) signature is deduced here: the generated wrapper checks
// arity, applies the method-side as<A>() conversions, and records the
// argument tags for the manifest. Generic/overloaded callables are rejected
// by the static_assert (the same restriction method() already implies).

// Peel R / (A...) off a plain callable: lambda object, member operator(),
// or function pointer. `ok == false` for anything else (generic lambda).
template <class F, class = void> struct fn_traits {
  static constexpr bool ok = false;
};
template <class F>
struct fn_traits<F, std::void_t<decltype(&F::operator())>>
    : fn_traits<std::decay_t<decltype(&F::operator())>> {};
template <class C, class R, class... A>
struct fn_traits<R (C::*)(A...) const> {
  static constexpr bool ok = true;
  using ret = R;
  using args = std::tuple<A...>;
};
template <class C, class R, class... A>
struct fn_traits<R (C::*)(A...)> : fn_traits<R (C::*)(A...) const> {};
template <class R, class... A> struct fn_traits<R (*)(A...)> {
  static constexpr bool ok = true;
  using ret = R;
  using args = std::tuple<A...>;
};

// Typed-form wrapper: arity guard first, then the same as<A>() conversions
// a method call applies.
template <class Args, std::size_t... I>
std::shared_ptr<void> ctor_call_typed(const auto& g,
                                      const std::vector<QLangValue>& a,
                                      std::index_sequence<I...>) {
  if (a.size() != sizeof...(I))
    throw QLangError(QLangErrorKind::Type, 0, 0,
                     "constructor expects " + std::to_string(sizeof...(I)) +
                         " argument(s), got " + std::to_string(a.size()));
  return g(a[I].template as<std::tuple_element_t<I, Args>>()...);
}

// Context-taking form. The lambda's return type is deduced (shared_ptr<T>,
// T the concrete object type) before erasure to shared_ptr<void>: T is
// known here, so the descriptor can validate constructed/handed-off
// pointers with a clean type error instead of UB (Phase 3). A void-typed
// return carries no check.
template <class F>
  requires std::is_invocable_v<F, QLangContext&,
                               const std::vector<QLangValue>&>
CtorClause ctor_clause(F f) {
  using R = std::decay_t<std::invoke_result_t<F, QLangContext&,
                                              const std::vector<QLangValue>&>>;
  if constexpr (std::is_void_v<typename R::element_type>) {
    return CtorClause{
        [g = std::move(f)](QLangContext& c,
                           const std::vector<QLangValue>& a) -> std::shared_ptr<void> {
          return g(c, a);
        },
        nullptr, nullptr};
  } else {
    using T = typename R::element_type;
    return CtorClause{
        [g = std::move(f)](QLangContext& c,
                           const std::vector<QLangValue>& a) -> std::shared_ptr<void> {
          return g(c, a);
        },
        &typeid(T),
        // dynamic_cast cannot source from void*: go through the polymorphic
        // QLObject<T> base (the static_cast is safe — p is a T by
        // construction; the dynamic_cast validates the dynamic type).
        [](const void* p) {
          return dynamic_cast<const T*>(static_cast<const QLObject<T>*>(p)) !=
                 nullptr;
        }};
  }
}

// Typed form: the wrapper is generated from the declared R(A...), and the
// Phase 3 typeInfo/check logic applies unchanged (R is still shared_ptr<T>).
template <class F>
  requires(!std::is_invocable_v<F, QLangContext&,
                                const std::vector<QLangValue>&>)
CtorClause ctor_clause(F f) {
  static_assert(fn_traits<F>::ok,
                "QL_CTOR: the typed form needs a non-generic lambda or a "
                "function pointer — QL_CTOR([](double x, double y) { return "
                "std::make_shared<Point>(x, y); }); to use the context, take "
                "(QLangContext&, const std::vector<QLangValue>&) — that form "
                "must validate a.size() itself");
  using R = typename fn_traits<F>::ret;
  using Args = typename fn_traits<F>::args;
  static_assert(std::is_convertible_v<R, std::shared_ptr<void>>,
                "QL_CTOR: the typed form must return std::shared_ptr<T>");
  CtorClause cl;
  cl.f = [g = std::move(f)](QLangContext&,
                            const std::vector<QLangValue>& a) mutable
         -> std::shared_ptr<void> {
    return ctor_call_typed<Args>(
        g, a, std::make_index_sequence<std::tuple_size_v<Args>>{});
  };
  if constexpr (!std::is_void_v<typename R::element_type>) {
    using T = typename R::element_type;
    cl.typeInfo = &typeid(T);
    cl.check = [](const void* p) {
      return dynamic_cast<const T*>(static_cast<const QLObject<T>*>(p)) !=
             nullptr;
    };
  }
  cl.argTags = qtype_tags<Args>(
      std::make_index_sequence<std::tuple_size_v<Args>>{});
  return cl;
}

// --- Assembly: fold the heterogeneous entry list into a TypeMeta ----------

inline void add(TypeMeta& m, Member e) { m.members.push_back(std::move(e)); }
inline void add(TypeMeta& m, EnumMeta e) { m.enums.push_back(std::move(e)); }
inline void add(TypeMeta& m, SignalMeta e) {
  m.signals.push_back(std::move(e));
}
inline void add(TypeMeta& m, StrClause e) {
  m.toString = std::move(e.f);
  m.hasStr = true;
}
inline void add(TypeMeta& m, CtorClause e) {
  m.construct = std::move(e.f);
  m.typeInfo = e.typeInfo;
  m.typeCheck = std::move(e.check);
  m.hasCtor = true;
  m.ctorArgTags = std::move(e.argTags);  // empty for the context-taking form
}

// Recover the (innermost) class name without `__CLASS__`. `__CLASS__` is an
// MSVC extension and is NOT available in GCC/Clang, so we derive the name
// from `__PRETTY_FUNCTION__` instead. For
//   static const qlang::TypeMeta& A::qlang_meta()
// GCC emits "static const qlang::TypeMeta& A::qlang_meta()", so the class
// name is the identifier token immediately before "::qlang_meta". Nested
// scopes (NS::B, Outer::Inner) yield the innermost name.
inline std::string className(const char* pf) {
  const char* m = std::strstr(pf, "::qlang_meta");
  if (!m)
    return std::string();
  const char* end = m;  // the ':' of "::qlang_meta"
  const char* start = end;
  auto isId = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
  };
  while (start > pf && isId(*(start - 1)))
    --start;
  return std::string(start, static_cast<size_t>(end - start));
}

template <class... Ts> TypeMeta assemble(std::string name, Ts... es) {
  TypeMeta m{std::move(name)};
  ((add(m, es), ...));
  return m;
}
}  // namespace meta_detail

// --- Process-wide meta registry (portable-module-plan P3) ------------------
// QL_OBJECT self-registers, Qt-style: no MOC, no list to maintain. The
// registry stores only the *accessor pointers*, never built TypeMetas, so
// nothing a host registers runs at static-init time (the inline-static
// registrar inside QL_OBJECT passes just a function address; qlang_meta()
// assembles lazily on first call, always after main starts).
using MetaAccessor = const TypeMeta& (*)();

// Construct-on-first-use function-local static: no static-init order can be
// observed, and one instance per process (inline function). Observers walk
// it; duplicate names are kept here and refused by writeTypeManifest (P3)
// and readManifest (P2) — never silently dropped, never thrown at init time.
inline std::vector<MetaAccessor>& metaRegistry() {
  static std::vector<MetaAccessor> reg;
  return reg;
}

namespace meta_detail {
inline bool registry_add(MetaAccessor a) {
  metaRegistry().push_back(a);
  return true;
}
}  // namespace meta_detail

} // namespace qlang

// === Entry macros — pure token transformers, one declarative line each ===

// Data member -> read-write property (public member: script gets a setter).
// Computed: QL_PROPERTY(name, READ getter)               -> read-only
//           QL_PROPERTY(name, READ getter, WRITE setter) -> read-write
// Const getter listed bare -> read-only property.
// `SelfType` is the inherited alias from `qlang::QLObject<Self>`.
// The optional READ/WRITE tail is handled by an arity dispatch. It counts the
// TOTAL args (the name included) so the zero-tail `QL_PROPERTY(x)` case needs
// no GNU `##__VA_ARGS__` comma-swallow — portable to MSVC. READ/WRITE expand
// to acc(...) calls whose internal commas are not top-level, so they count as
// single args.
#define QL_PROPERTY(...) \
  QL_PROPERTY_DISPATCH(QL_PROP_NARG(__VA_ARGS__), __VA_ARGS__)
#define QL_PROP_NARG(...) QL_PROP_NARG_IMPL(__VA_ARGS__, 3, 2, 1, 0)
#define QL_PROP_NARG_IMPL(_1, _2, _3, N, ...) N
#define QL_PROPERTY_DISPATCH(N, ...) QL_PROPERTY_SEL(N)(__VA_ARGS__)
#define QL_PROPERTY_SEL(N) QL_PROPERTY_SEL_IMPL(N)
#define QL_PROPERTY_SEL_IMPL(N) QL_PROPERTY_CASE##N
#define QL_PROPERTY_CASE1(name) \
  qlang::meta_detail::prop(#name, &SelfType::name)
// 2/3-arg forms are computed properties: the accessor overloads take only
// the name plus the READ/WRITE accessor(s) — no data-member pointer.
#define QL_PROPERTY_CASE2(name, a) \
  qlang::meta_detail::prop(#name, a)
#define QL_PROPERTY_CASE3(name, a, b) \
  qlang::meta_detail::prop(#name, a, b)
#define READ(a) qlang::meta_detail::acc(&SelfType::a, qlang::meta_detail::Acc::read)
#define WRITE(a) qlang::meta_detail::acc(&SelfType::a, qlang::meta_detail::Acc::write)

// Method (non-const and const member functions). Slots and invokables are
// the same thing to the VM.
#define QL_INVOKABLE(name) qlang::meta_detail::method(#name, &SelfType::name)
#define QL_SLOTS(name) qlang::meta_detail::method(#name, &SelfType::name)

// Signal: the member function is declared (never implemented); its signature
// defines the emit argument types. The *type* is named via decltype — taking
// &SelfType::name would odr-use a function that has no definition.
#define QL_SIGNAL(name) \
  qlang::meta_detail::signal<decltype(&SelfType::name)>(#name)

// Whole enum/flag group in one line: bare enumerator names. The macro forms
// one (name, value) pair per enumerator (C++17 has no dependent NTTP packs,
// so the values cannot be template arguments of a single call). Up to 16
// enumerators. The argument count selects the matching `QL_FE2_N` fold so
// the recursion stops exactly at the last real enumerator (a fixed
// `QL_FE2_16` would recurse into empty tail args and emit garbage pairs).
#define QL_ENUM_PAIR(E, name) ::std::pair<const char*, E>{#name, E::name}
#define QL_FE2_1(m, e, x) m(e, x)
#define QL_FE2_2(m, e, x, ...) m(e, x), QL_FE2_1(m, e, __VA_ARGS__)
#define QL_FE2_3(m, e, x, ...) m(e, x), QL_FE2_2(m, e, __VA_ARGS__)
#define QL_FE2_4(m, e, x, ...) m(e, x), QL_FE2_3(m, e, __VA_ARGS__)
#define QL_FE2_5(m, e, x, ...) m(e, x), QL_FE2_4(m, e, __VA_ARGS__)
#define QL_FE2_6(m, e, x, ...) m(e, x), QL_FE2_5(m, e, __VA_ARGS__)
#define QL_FE2_7(m, e, x, ...) m(e, x), QL_FE2_6(m, e, __VA_ARGS__)
#define QL_FE2_8(m, e, x, ...) m(e, x), QL_FE2_7(m, e, __VA_ARGS__)
#define QL_FE2_9(m, e, x, ...) m(e, x), QL_FE2_8(m, e, __VA_ARGS__)
#define QL_FE2_10(m, e, x, ...) m(e, x), QL_FE2_9(m, e, __VA_ARGS__)
#define QL_FE2_11(m, e, x, ...) m(e, x), QL_FE2_10(m, e, __VA_ARGS__)
#define QL_FE2_12(m, e, x, ...) m(e, x), QL_FE2_11(m, e, __VA_ARGS__)
#define QL_FE2_13(m, e, x, ...) m(e, x), QL_FE2_12(m, e, __VA_ARGS__)
#define QL_FE2_14(m, e, x, ...) m(e, x), QL_FE2_13(m, e, __VA_ARGS__)
#define QL_FE2_15(m, e, x, ...) m(e, x), QL_FE2_14(m, e, __VA_ARGS__)
#define QL_FE2_16(m, e, x, ...) m(e, x), QL_FE2_15(m, e, __VA_ARGS__)
// Count the enumerator list (1..16) and pick the matching fold. No GNU
// `##__VA_ARGS__` comma-swallow (MSVC lacks it); the degenerate 0-enumerator
// case is the only one affected and is never used.
#define QL_ENUM_COUNT(...) \
  QL_ENUM_COUNT_IMPL(_, __VA_ARGS__, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, \
                     6, 5, 4, 3, 2, 1, 0)
#define QL_ENUM_COUNT_IMPL(_, _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, \
                           _12, _13, _14, _15, _16, N, ...) N
// Two-level indirection: ## suppresses argument expansion, so the count must
// be expanded by a normal (non-pasting) argument pass first, then pasted.
#define QL_ENUM_SELECT(N) QL_ENUM_PASTE(N)
#define QL_ENUM_PASTE(N) QL_FE2_##N
#define QL_ENUM(E, ...) \
  qlang::meta_detail::enum_table( \
      #E, QL_ENUM_SELECT(QL_ENUM_COUNT(__VA_ARGS__))(QL_ENUM_PAIR, E, __VA_ARGS__))
#define QL_FLAGS(F, ...) \
  qlang::meta_detail::enum_table( \
      #F, QL_ENUM_SELECT(QL_ENUM_COUNT(__VA_ARGS__))(QL_ENUM_PAIR, F, __VA_ARGS__))

// str()/f-string conversion, and script-side construction.
#define QL_STR(name) qlang::meta_detail::str_clause(&SelfType::name)
#define QL_CTOR(expr) qlang::meta_detail::ctor_clause(expr)

// === The one assembly point (what Qt's MOC generates) ===
// The class name is recovered from the platform "current function" string
// (QL_PRETTY_FUNCTION: __PRETTY_FUNCTION__ on GCC/Clang/MinGW/emscripten,
// __FUNCSIG__ on MSVC). __CLASS__ is an MSVC-only extension and is not used.
#define QL_OBJECT(...)                                       \
  public:                                                    \
  static const qlang::TypeMeta& qlang_meta() {               \
    static const qlang::TypeMeta m_{                          \
        qlang::meta_detail::assemble(                          \
            qlang::meta_detail::className(QL_PRETTY_FUNCTION), \
            __VA_ARGS__)};                                    \
    return m_;                                               \
  }                                                          \
  /* Self-registration (portable-module-plan P3): the C++17 inline static    \
     member has exactly one instance per program, and its initializer runs    \
     at dynamic init — it stores only the accessor pointer, so no TypeMeta    \
     is assembled and nothing can throw at static-init time. The registrar    \
     is emitted in every translation unit that references the class's meta;   \
     the inline guarantees a single dynamic initializer. */                   \
  inline static const bool qlang_meta_registered_ =                           \
      ::qlang::meta_detail::registry_add(&qlang_meta)
