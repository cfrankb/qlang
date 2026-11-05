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
//       QL_CTOR([](qlang::QLangContext& c, const std::vector<qlang::QValue>& a) {
//         return std::make_shared<Point>(a[0].as<double>(), a[1].as<double>()); })
//     );
//   };
//   ctx.import<Point>();

#include <cstddef>
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
  return {0, n,
          [m](const void* s) {
            return qval_of(static_cast<const T*>(s)->*m);
          },
          [m](void* s, const QLangValue& v) {
            static_cast<T*>(s)->*m = v.as<M>();
          }};
}

// Const getter listed bare -> read-only property.
template <class T, class R> Member prop(const char* n, R (T::*g)() const) {
  return {1, n,
          [g](const void* s) {
            return qval_of((static_cast<const T*>(s)->*g)());
          },
          nullptr, nullptr};
}

// QL_PROPERTY(name, READ g) -> computed read-only property.
template <class T, class R>
Member prop(const char* n, AccPtr<R (T::*)() const> g) {
  return {1, n,
          [g](const void* s) {
            return qval_of((static_cast<const T*>(s)->*g.p)());
          },
          nullptr, nullptr};
}

// QL_PROPERTY(name, READ g, WRITE s) -> computed read-write property.
// The setter is `void (T::*)(M)`: Qt-style setters return void, so the
// parameter type M is the only deduced type (return == parameter would
// reject every real setter).
template <class T, class R, class M>
Member prop(const char* n, AccPtr<R (T::*)() const> g,
            AccPtr<void (T::*)(M)> s) {
  return {0, n,
          [g](const void* o) {
            return qval_of((static_cast<const T*>(o)->*g.p)());
          },
          [s](void* o, const QLangValue& v) {
            (static_cast<T*>(o)->*s.p)(v.as<M>());
          }};
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
  return {2, n, nullptr, nullptr,
          [f](void* s, QLangContext& c, const std::vector<QLangValue>& a) {
            return call(f, static_cast<T*>(s), a,
                        std::index_sequence_for<A...>{});
          }};
}

// Const member function -> method.
template <class T, class R, class... A>
Member method(const char* n, R (T::*f)(A...) const) {
  return {2, n, nullptr, nullptr,
          [f](void* s, QLangContext& c, const std::vector<QLangValue>& a) {
            return call(f, static_cast<const T*>(s), a,
                        std::index_sequence_for<A...>{});
          }};
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
  (s.argChecks.push_back(
       arg_check<std::tuple_element_t<I, typename SignalArgs<Sig>::type>>()),
   ...);
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
};

template <class T, class R> StrClause str_clause(R (T::*f)() const) {
  return {[f](const void* s) {
            return qval_of((static_cast<const T*>(s)->*f)());
          }};
}

// The lambda's return type is deduced (shared_ptr<T>, T the concrete object
// type) before erasure to shared_ptr<void>: T is known here, so the
// descriptor can validate constructed/handed-off pointers with a clean type
// error instead of UB (Phase 3). A void-typed return carries no check.
template <class F> CtorClause ctor_clause(F f) {
  using R = std::decay_t<std::invoke_result_t<F, QLangContext &,
                                              const std::vector<QLangValue> &>>;
  if constexpr (std::is_void_v<typename R::element_type>) {
    return CtorClause{
        [g = std::move(f)](QLangContext &c,
                           const std::vector<QLangValue> &a) -> std::shared_ptr<void> {
          return g(c, a);
        },
        nullptr, nullptr};
  } else {
    using T = typename R::element_type;
    return CtorClause{
        [g = std::move(f)](QLangContext &c,
                           const std::vector<QLangValue> &a) -> std::shared_ptr<void> {
          return g(c, a);
        },
        &typeid(T),
        // dynamic_cast cannot source from void*: go through the polymorphic
        // QLObject<T> base (the static_cast is safe — p is a T by
        // construction; the dynamic_cast validates the dynamic type).
        [](const void *p) {
          return dynamic_cast<const T *>(static_cast<const QLObject<T> *>(p)) !=
                 nullptr;
        }};
  }
}

// --- Assembly: fold the heterogeneous entry list into a TypeMeta ----------

inline void add(TypeMeta& m, Member e) { m.members.push_back(std::move(e)); }
inline void add(TypeMeta& m, EnumMeta e) { m.enums.push_back(std::move(e)); }
inline void add(TypeMeta& m, SignalMeta e) {
  m.signals.push_back(std::move(e));
}
inline void add(TypeMeta& m, StrClause e) { m.toString = std::move(e.f); }
inline void add(TypeMeta& m, CtorClause e) {
  m.construct = std::move(e.f);
  m.typeInfo = e.typeInfo;
  m.typeCheck = std::move(e.check);
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

} // namespace meta_detail
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
  }
