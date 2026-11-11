## conformance with external (C++) types

External-type bindings are written in **C++20** (the project's implementation
standard; the root `CMakeLists.txt` sets `CMAKE_CXX_STANDARD 20`).

A registered C++ type (`docs/EXTERNAL_TYPES.md`) implements this exact
model. The mapping:

| class concept        | external type binding              |
|----------------------|------------------------------------|
| class name           | `TypeDescriptor.name`              |
| `ClassName(args)`    | `TypeDescriptor.construct`         |
| `__init__`           | `TypeDescriptor.construct`         |
| field declaration    | `FieldBinding` (get + set)         |
| `const` field        | `FieldBinding` with null setter    |
| method `m(self, ...)`| `MethodBinding`                    |
| `__str__`            | `TypeDescriptor.str` (optional)    |
| `__destroy__`        | `TypeDescriptor.destroy` (optional)|
| reference count      | engine-authoritative: `weak_ptr` for handoffs, `shared_ptr` for script-constructed |
| `type(x)`            | `type_info` name                   |

Both kinds of object go through the same four dispatch operations
(GETFIELD / SETFIELD / CALLMETHOD / TOSTRING), so code written against the
class model works unchanged on external types.

# implicit \_\_str\_\_ rules

|type|example|behavior|
|--|--|--|
|numbers|| convert to string
|strings|| remain unchanged
|boolean| |"true" or "false"
|nil|| "nil"
|array|[0,1,2,3,4]| "[0, 1, 2, 3, 4]"
|table|{name="Bob", age=9}| "{name="Bob", age=9}"
|class||use provided \_\_str\_\_() method

table key order is implementation defined (depends on the underlying container).

## extracting external types

Specified 2026-11-11; **not yet implemented** (see the Open Item in
`docs/compliance/status.md`; build plan: `docs/plans/portable-module-plan.md`).

The compile-time shape `QL_OBJECT(...)` generates is extractable to a binary
**type manifest** (`QLM1`), so the compiler can validate scripts against
external types **without the engine linked** — one `qlc` front end builds
modules for any host. Extraction is registration-driven, not parse-driven (no
MOC): `QL_OBJECT` self-registers in a process-wide meta registry, a host
binary dumps it — `qlang_run --qlmeta-dump <file.qlm>` or the library
one-liner `qlang::writeTypeManifest(os)`. The manifest carries description
only: type name, members (kind, name, portable **type tags** for property
read/write, method arguments/return, signal arguments, typed-constructor
arguments), enum tables with their values, and str/ctor presence — never
closures, function pointers or mangled names. The implementation stays in the
host binary. Container layouts land in `docs/SERIALIZATION.md` with the
implementation.

### compile-time validation

The `qlc` front end consumes a manifest (`qlang_run --qlmeta <file.qlm>`);
each violation is a compile error worded like the runtime error it replaces.

| site | checked at compile time |
|-|-|
| `T.member` / `T.method(...)` | member/method exists; arity; argument tags when the argument type is inferable |
| `T(...)` | constructor exists; a **typed** `QL_CTOR` additionally checks arity + argument tags (the context-taking form is presence-only — its lambda validates its own arguments) |
| `T::M` / `T.M` enum member | exists; folds to an `Int` literal from the manifest value |
| known receiver (`p = T(...)` with a single initialization, or a declared `p:T`) | field exists; `const` field write refused; method exists + arity; ro/const wording matches the VM |
| `emit obj.sig(args)`, `connect(obj, "sig", …)` with a known receiver | signal exists + arity |

A receiver of unknown type keeps the lenient (runtime-checked) path; runtime
checks stay authoritative. Enum names injected by `registerEnum` must agree
with a manifest of the same name.

### portable module

A manifest plus the `QLC1` images it validates — byte-for-byte unchanged —
package as a **portable module** (`QPM1`, `qlang_run --package`). Loading one
(`qlang_run app.qpm` / `QLangContext::loadPortableModuleFile`) handshakes the
manifest against the host's registered types: every type present, fingerprints
equal, or a clean load error. Bytecode can never silently run against drifted
bindings.

Two containers because the consumers differ. `QLM1` exists *before* the
bytecode — it is the compile-time contract `qlc` consumes, reused across
scripts and rebuilds, the way a header is. `QPM1` exists *after* compiling —
it is the shipped deliverable, consumed once at load time. `QPM1` re-describes
nothing: it embeds the untouched `QLM1` and untouched `QLC1` images, so both
inner formats stay independently readable and neither codec moves. A single
merged container would fail at both ends: the compiler cannot consume "a
package of bytecode" before that bytecode exists, and without the envelope
every host's `main()` re-implements two-file load + handshake.

```
editor (host binary, QL_OBJECT bindings compiled in)
  ├─ qlang::writeTypeManifest / --qlmeta-dump   → map.qlm   (once per editor build)
  ├─ edit maplogic.ql
  ├─ qlang_run -c --qlmeta map.qlm maplogic.ql  → maplogic.qlc (compile-verified, no editor linked into qlc)
  └─ run/load the bare .ql or .qlc inside editor — real bindings, runtime checks
ship: --package → game.qpm (manifest + images), load-time handshake
```