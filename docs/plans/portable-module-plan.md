# Plan: portable modules — `QL_OBJECT` meta extraction, `QLM1` manifest, compile-time external-type validation, `QPM1` packaging

Status: **PROPOSED** (2026-11-11). The requirements it closes — the OPEN TOPIC
`§ extracting external types` in `docs/spec/11-external-types.md` — were
finalized as normative spec text the same day (compliance Round 74); that
section flips to implemented only when P1–P5 below are green.
Baseline: Round 73 HEAD, `.qlc` **v9**. **No `.qlc` version bump
anywhere in this plan** — `QLM1` and `QPM1` are new containers with their own
magics; the `QLC1` images inside them stay byte-for-byte unchanged.

Builds on: `docs/plans/archive/external-modules-plan-v4.md` (linker, loader,
export/import tables — reused untouched), Round 41's `addExternalEnums` (the
injection precedent for P4), Round 40's signals (manifest carries signals).

## Goal

Extract the metadata `QL_OBJECT(...)` already generates at compile time into a
binary **type manifest (`QLM1`)**. The `qlc` front end (`qlang_run -c/-a`)
consumes a manifest and validates `.ql` sources against the declared external
shapes **without the engine being linked in** — one compiler builds modules for
any host. Package a manifest + untouched `QLC1` images as a **portable module
(`QPM1`)**; at load time the host's registered types are handshaked against the
manifest (fingerprint match or refusal), so a `.qlc` can never silently run
against a drifted native library.

Decisions settled with the user (2026-11-11):
- Extraction is driven from the host C++ binary (static registry, no MOC, no
  source parsing) exposed through `qlang_run --qlmeta-dump` + a library API.
- New outer packaging (`QPM1`) wrapping untouched `QLC1` images — not an
  in-image v10 table, not a trailer chunk.
- **`QLM1` and `QPM1` stay distinct containers — role, not redundancy:**
  `QLM1` is the *compile-time input* (exists before any bytecode; the
  compiler validates against it with no engine linked; one manifest is reused
  across scripts and rebuilds, like a header). `QPM1` is the *load-time
  output* (exists after compiling; the shipped deliverable the host
  handshakes against). `QPM1` re-describes nothing — it embeds the untouched
  `QLM1` + untouched `QLC1` images, so inner formats stay independently
  readable and neither codec moves. A single merged format fails at both
  ends: the compiler cannot consume "a package of bytecode" before that
  bytecode exists, and without the envelope every host's `main()`
  re-implements two-file load + handshake. The envelope is the lazy option.
- The manifest carries **type tags** (P1), not just names + kinds — this is
  what powers the added compile-time type checking.
- **Amendment (same day):** a **typed `QL_CTOR` form** lands in P1. A probe of
  the construction path found `d->construct(c, a)` dispatched with **no arity
  guard** (`QLangContext.cpp:1144`) — a short-arity call is UB in host code.
  The typed form generates the guard + conversions + manifest ctor tags in
  one template, closing both the safety hole and the compile-time ctor
  limitation; the context-taking form stays working, documented as
  self-validating. Receiver-type depth settled at single-init inference +
  declared annotations (P4b); flow inference belongs to strict typing
  (module 16), which absorbs the whole channel later.

## Hard constraint (why the container is a *manifest*, not an implementation)

`TypeMeta` erases all behavior into `std::function` (`Member::get/set/invoke`,
`SignalMeta::argChecks`, `construct`, `typeCheck`). Closures, function pointers
and mangled `typeid` names are **not serializable and not portable across
compilers**. The container therefore carries the interface *shape* only; the
host binary still supplies the implementation, and the handshake (P5) is what
ties the two together. Tags, never mangled names, are the portable vocabulary.

## Development workflow (loose files)

The dev loop (e.g. a map editor with user scripts) uses **loose `.qlm` /
`.qlc` / `.ql` files — no `QPM1`**; packaging is the ship step, never the dev
step.

1. Host rebuilds → re-dump the manifest: the host calls
   `qlang::writeTypeManifest` on its own registry (a build step or an
   "export type defs" action); `--qlmeta-dump` covers standalone dumpers.
   The `.qlm` is a header artifact of that host build.
2. Author edits `maplogic.ql`; compile-validates with the plain front end —
   `qlang_run -c --qlmeta map.qlm maplogic.ql`. A script-only dev needs only
   the `.qlm`, no engine.
3. Run/load the `.qlc` (or bare `.ql`) **inside the host** — real bindings,
   runtime checks authoritative (today's workflow, unchanged).

Loose files carry **no integrity link** between image and manifest: drift is
caught only at first runtime use. Dev discipline is "re-dump `.qlm` on every
host build, recompile scripts after." `QPM1`'s load-time handshake exists
precisely to close that hole for shipped bundles, where loose files can
silently mix versions.

---

## Phase 1 — describable meta (`include/qlang/QLangMeta.h`, `include/qlang/QLangTypes.h`) ✓ DONE (Round 75, 2026-11-11)

The `meta_detail` templates already know every property/argument/return type
at compile time and erase it. Capture it as plain data, additively — the
runtime dispatch paths do not change.

1. `enum class QTypeTag : uint8_t` in `QLangMeta.h`:
   `Unknown = 0x00` (a type outside the `as<T>()` set — the tag exists, the
   checks involving it stay silent), `Void = 0xFF`, and one tag per supported
   scalar mirroring the `qval_of()` overloads: `Bool, Char (int8), Short, Int
   (int32), Long (int64), Byte (uint8), Word (uint16), Dword (uint32), Qword
   (uint64), Float (double), Str (std::string), External (opaque — the tag of
   any other T, arg types of cross-object methods; no name is portable)`.
   `template <class T> constexpr QTypeTag qtype_tag()` — mapping exactly the
   set `QLangValue::as<T>()` accepts; anything else yields `Unknown`, never a
   hard error (a binding over an exotic member type must keep compiling).
2. Extend `Member`: `QTypeTag getTag = Unknown; QTypeTag setTag = Unknown;`
   (properties; `setTag` stays `Unknown` on read-only) and `std::vector<QTypeTag>
   argTags; QTypeTag retTag = Unknown;` (methods). Filled in `prop()` (from `M`
   / the getter's `R` / the setter's `M`), and `method()` (from the pack `A...`
   and `R`; void → `Void`).
3. Extend `SignalMeta`: `std::vector<QTypeTag> argTags;` filled beside
   `argChecks` in `signal_args<Sig>()` from the same pack.
4. **Typed `QL_CTOR` overload** (replaces the earlier presence-only ctor
   plan — a safety finding forced it). `QLangContext.cpp:1144` calls
   `d->construct(c, a)` with **no arity guard**: the documented
   context-taking lambda form does `a[0].as<double>()` directly, so a
   short-arity script call (`Point()`) is an out-of-bounds vector read —
   **UB in host code**, not a clean type error. Fix it where the information
   lives: overload `ctor_clause` with `requires` on
   `std::is_invocable_v<F, QLangContext&, const std::vector<QLangValue>&>`:
   - *invocable with (ctx, vec)* → today's behavior, unchanged (tags stay
     `Unknown`); documented rule: **this form must validate `a.size()`
     itself** (kept for ctors that genuinely need the context).
   - *typed form* — `QL_CTOR([](double x, double y) { return
     std::make_shared<Point>(x, y); })` — deduces `R(A...)` from
     `&F::operator()` (the `SignalArgs` trick) and generates the wrapper:
     arity check (`constructor expects N argument(s), got M`), `as<A>()`
     conversion per argument (identical rules to methods), then fills
     `TypeMeta::ctorArgTags = {qtype_tag<A>()...}`. `add()` sets `hasCtor`
     from either form (and `hasStr` from the `StrClause`, as today). No
     generic lambdas in `QL_CTOR` (same restriction `method()` already
     implies — documented).
   `TypeMeta` gains `bool hasStr`, `bool hasCtor`,
   `std::vector<QTypeTag> ctorArgTags` (empty + `hasCtor` ⇒ the context
   form ⇒ P4 checks presence only, arity/types stay runtime).
5. `TypeDescriptor` copies the new fields in `QLangContext::registerType`.

**Verify:** `cmake -B build && cmake --build build && ctest --test-dir build`
all green (no behavior change expected anywhere); extend
`tests/unit/test_external.cpp`: `Point::x (double)` → `getTag==Float`;
ro property → `setTag==Unknown`; a rw READ/WRITE accessor pair whose getter
and setter types differ → distinct tags; `translate(double,double) void` →
`argTags=={Float,Float}, retTag==Void`; `QL_SIGNAL(levelChanged)` over `int` →
`argTags=={Int}`; an enum-only type keeps every tag `Unknown`; a typed
`QL_CTOR([](double,double){...})` → `ctorArgTags=={Float,Float}` and
`Point(1)` from a script is a clean `constructor expects 2 argument(s),
got 1` (was UB); the context-taking form still compiles and runs unchanged.
**Commit:** `Add portable type tags to QL_OBJECT meta (qtype_tag<T>, describable
Member/SignalMeta fields, typed QL_CTOR with arity guard)`

## Phase 2 — `QLM1` type-manifest codec (`include/qlang/QLangManifest.h`,
`src/QLangManifest.cpp`, docs) ✓ DONE (Round 76, 2026-11-11)

One serializer, one code path, used by the dumper (P3), the compiler input
(P4) and the package writer (P5) — fingerprint equality between host and
manifest is therefore equality by construction.

Layout (little-endian, same writer/`Reader` discipline as
`QLangBytecodeIO.cpp`; strict version like `.qlc`):

```
magic            4s   "QLM1"
version          u32  = 1 (kQlmetaVersion; readers accept 1 only)
typeCount        u32
types            typeCount × Type
```

`Type`:
```
fingerprint      u64       FNV-1a 64 over the entry body bytes (§below)
name             String    u32 size + bytes
flags            u8        bit0 hasStr, bit1 hasCtor
memberCount      u32
members          each: kind u8 (0 rw-prop, 1 ro-prop, 2 method)
                         name String
                         prop:   getTag u8, setTag u8
                         method: retTag u8, argCount u8, argTags u8×
enumCount        u32
enums            each: typeName String, entryCount u32,
                       entries: name String + value i64
signalCount      u32
signals          each: name String, argCount u8, argTags u8×
ctorArgs         when flags bit1: argCount u8, argTags u8×
                 (absent ⇒ context-taking QL_CTOR: presence-only checks)
```

API (`QLangManifest.h`):
- `struct ManifestType` — the serializable mirror of `TypeMeta` (no closures).
- `std::string serializeTypeEntry(const ManifestType&)` / `parse` — **one
  canonical byte form**; `fingerprint = fnv1a64(entry body)`, computed by the
  serializer itself (host side from a `TypeMeta`/`TypeDescriptor` converted to
  a `ManifestType`, reader side recomputed and verified). Enum values carry the
  full `long` (`QL_ENUM` stores `long`); entries are self-delimiting so a
  future v2 can append fields.
- `writeManifest(std::ostream&, const std::vector<ManifestType>&)`,
  `readManifest(std::istream&) -> std::vector<ManifestType>` — the reader
  treats input as untrusted (every read bounds-checked,
  `QLangError(Io, "qlm: … (byte offset N)")`, counts capped at the §8 caps,
  duplicate type name → error, out-of-range tag/kind byte → error).
- `manifestTypeFrom(const TypeMeta&)` (and a `TypeDescriptor` overload).

**Verify:** new `tests/unit/manifest_check.cpp` + `add_test(manifest_check)`:
round-trip of a hand-built manifest (all kinds, enums, signals, ctor args,
empty lists; context-form ctor ⇒ absent `ctorArgs` section);
truncation matrix (every section boundary) and version 0/2 + bad magic
rejection at the right field; fingerprint stable across serialize→parse→
reserialize and sensitive to any single-byte perturbation (flip one tag, one
name char); `Unknown` tag round-trips. Full `ctest` green. Docs:
`docs/SERIALIZATION.md` gains `## type manifest (QLM1)` (layout, load
contract, the fingerprint rule, "tags not mangled names" rationale); record
in the §4 *version allocation* paragraph that this plan allocates **no**
`.qlc` version.
**Commit:** `QLM1 type-manifest codec (QLangManifest, fingerprint, manifest_check suite)`

## Phase 3 — extraction: static meta registry + `qlmeta_dump`
(`include/qlang/QLangMeta.h`, `src/QLangManifest.cpp`, `examples/qlang_run.cpp`) ✓ DONE (Round 77, 2026-11-11)

No MOC, no list to maintain: `QL_OBJECT` self-registers, Qt-style.

1. `QL_OBJECT(...)` gains one line beside `qlang_meta()`:
   `inline static const bool qlang_meta_registered_ = ::qlang::meta_detail::registry_add(&qlang_meta);`
   (C++17 inline static member → exactly one instance; the initializer runs at
   dynamic init). `meta_detail::registry_add` stores the *accessor pointer*
   (`const TypeMeta& (*)()`) in the construct-on-first-use function-local
   static `qlang::metaRegistry()` — no SIOF. Two entries with the same
   `name()`: the registry keeps both, `writeTypeManifest` refuses with
   `QLangError` naming the duplicate (never throw at static-init time).
2. `void writeTypeManifest(std::ostream&)` — walks the registry →
   `ManifestType`s → `writeManifest`. This is the host one-liner for binaries
   that are not `qlang_run`.
3. `qlang_run`: `--qlmeta-dump <file.qlm>` — a standalone mode: dump the
   process registry, report the type count to stdout, exit 0; no source file
   required, mutually exclusive with compile/run modes. Update the usage text.

Caveat, documented in `docs/EXTERNAL_TYPES.md`: the registrar emits in
translation units that *reference* the class — a dumper binary must include
and touch its bound types (normal for a host; a TU that only forward-declares
contributes nothing). `metaRegistry()` size is observable, so a host can assert
its full list is present.

**Verify:** `manifest_check` gains: dumping inside the test binary (which
declares `Point`/`Sensor` via `test_external` fixtures) → `readManifest`
round-trip → entries match the hand-built `manifestTypeFrom(T::qlang_meta())`
fingerprints; duplicate-name refusal. CLI smoke as a new `add_test`
(`qlmeta_dump` entry): `qlang_run --qlmeta-dump <tmp>` exits 0 and the file
parses. Full `ctest` green; corpus scripts byte-identical (no codegen change).
**Commit:** `QL_OBJECT static meta self-registration + qlang_run --qlmeta-dump (QLM1 extraction)`

## Phase 4 — compile-time external-type validation from a manifest
(`include/qlang/QLangCompiler.h`, `src/QLangCompiler.cpp`, `examples/qlang_run.cpp`)

`qlang_run --qlmeta <file.qlm>` loads a manifest and feeds the compiler
`addExternalTypes(const std::vector<ManifestType>&)` (the `addExternalEnums`
pattern). Engine-agnostic compile: with a manifest, `qlang_run -c app.ql`
compiles on a machine that has **none** of the bindings linked. Compiler-side
state: `m_extTypes` name → `const ManifestType*`; the type names resolve
through the existing host-injected-const path (`m_externalVars`-style register
declaration) so codegen is unchanged and the VM still seeds the `TypeRef` from
ctx at run start when the module executes on the real host.

Checks, in escalation order (each is a `QLangError(Parse)` naming the manifest
entry; message style follows the runtime wording so compile-time and runtime
report alike):

1. **Base is a type name** (statically known — `T.member`, `T.method(...)`,
   `T(...)` construction, `T::M` / `T.M`):
   - unknown member / method → `type 'T' has no field 'x'` /
     `... has no method 'x'` (previously compiles, fails at runtime);
   - `T(...)` → refusal with `type 'T' has no constructor` when
     `!hasCtor`; with a **typed** `QL_CTOR` (P1) → full arity + argument
     tag checks like any method; the context-taking form → presence only
     (arity/types stay runtime, the lambda owns its own validation);
   - enum member → **folded to an Int literal from the manifest** (values are
     data; consistent with the injected-enum fold of Round 41);
   - reading a method bare as a value → error naming the call spelling (the
     module-entity rule's shape).
2. **Receiver statically known** — v1 keeps this conservative: a register's
   *single* initialization `let p = T(...)` (or `p = T(...)`) with **no other
   assignment** anywhere in the compiling module; anything else → type
   unknown → today's lenient behavior. On a known receiver: field exists,
   `ro` property write rejected (`cannot assign to const field 'x'`, same
   wording as the VM), method exists + **arity** (`method expects N
   argument(s), got M`), argument **tag** mismatches reported when the
   argument type is inferable (literals and `cast` — reuse `inferType`; any
   dynamic argument stays silent, the runtime check remains authoritative).
3. **Signals**: `emit obj.sig(args)` with a known receiver → signal exists +
   arity; `connect(o, "sig", …)` / `disconnect` with a statically-known string
   name + known receiver → signal exists.
4. Manifest enums vs `registerEnum`-injected enums: if the same name is both,
   values must agree or compile fails (cheap loop, catches host/dump drift).

**P4b (optional, deferrable):** accept `p:T` declarations/parameters where `T`
is a manifest-known external type (parser: a class-type annotation **only**
against `m_extTypes`, otherwise today's error), store it on the symbol, and
treat annotated variables as known receivers — scoped strictly to this
validation, it is *not* the strict-typing enforcement of
`docs/spec/16-strict_typing.md` and must not claim to be; if strict typing
lands later, it absorbs this.

**Verify:** new `tests/unit/manifest_compile.cpp` (`manifest_compile` ctest):
drives `QLangCompiler` directly with a fixture `ManifestType` list — positive:
a full valid script compiles with **no ctx registration**; negative matrix:
unknown field, unknown method, ro write, wrong arity, literal tag mismatch,
`T(...)` without ctor, wrong-arity / wrong-tag `T(...)` on a typed-`QL_CTOR`
type (presence-only on a context-form one), unknown signal, bad `T::M`;
receiver-liveness: the same
call checks when `p` has one init, stays lenient after a second assignment.
CLI: `qlang_run --qlmeta m.qlm -c t.ql` green / red fixtures (extend the P3
dump fixture). Corpus regression: `tests/scripts/*.ql` unchanged (no manifest
→ no new checks; today's behavior is the no-manifest path).
**Commit:** `compile-time external-type validation from QLM1 manifests (addExternalTypes, --qlmeta)`

## Phase 5 — `QPM1` portable module + load handshake
(`src/QLangManifest.cpp`, `include/qlang/QLangContext.h`, `src/QLangContext.cpp`,
`examples/qlang_run.cpp`)

```
magic        4s   "QPM1"
version      u32  = 1 (kQpmVersion)
entryName    String        // which image is the entry
manifestSize u32
manifest     bytes         // a full QLM1 (magic + version included)
imageCount   u32
images       each: name String, size u32, bytes   // untouched QLC1 images
```

- `writeQpm` / `readQpm` in `QLangManifest` — the reader validates *through
  the existing readers*: the embedded `QLM1` parses via `readManifest`, every
  image via `readQlc`, before anything touches the context; embedded payloads
  are never re-encoded (the linker's rule stands: `writeQlc` refuses a linked
  program, so packages carry **module images** + the entry, and the entry
  links the embedded libraries through the existing `loadProgram`/`QLangLinker`
  path).
- `QLangContext::loadPortableModuleFile(path)` → **handshake first**:
  every `ManifestType` must have a ctx-registered descriptor of that name
  (`missing type 'T' required by portable module`) with an equal fingerprint
  (`external type 'T' does not match the portable module`) — then register
  embedded library images, then load the entry (existing module-search-path /
  linker machinery).
- `qlang_run`: `--package <file.qpm>` (with `--qlmeta` + `-c`) bundles the
  entry image + every library image the invocation used; `qlang_run app.qpm`
  loads a package directly. Usage text updated in the same commit.

**Verify:** new `tests/unit/portable_module_test.cpp` (`portable_module`
ctest): build manifest + image in-process → `writeQpm` → `readQpm` round-trip;
handshake refusal on missing type and on a one-tag-drifted manifest; success
path loads, links, **runs** and prints the expected output for a fixture
script over `Point`. Truncation/magic/version matrix. `qlang_run` script
regression: package an existing `tests/scripts` fixture, run the `.qpm`,
stdout byte-identical to the `.ql` run. Full `ctest` green.
**Commit:** `QPM1 portable module (package, load handshake, qlang_run --package / .qpm)`

## Docs round (after P5, before declaring the topic closed)

- `docs/spec/11-external-types.md`: ✓ **done ahead of the phases (2026-11-11,
  Round 74)** — the OPEN TOPIC was replaced with the normative text above
  (marked *specified, not yet implemented*). On P5 completion: drop the
  status marker, reconcile wording with the shipped diagnostics, and re-record
  the hash row.
- `docs/SERIALIZATION.md`: `## type manifest (QLM1)` + `## portable module
  (QPM1)` sections (already drafted in P2/P5), §10 pointers.
- `docs/EXTERNAL_TYPES.md`: new `§portable modules` (dump workflow, registry
  caveat, one-liner for host dumpers, `qlang_run` flags) + the `QL_CTOR`
  hygiene rule (the context-taking form must validate `a.size()` itself;
  the typed form is preferred — guard, conversions and manifest tags come
  with it).
- Compliance: `status.md` round entry + tally rows (new suites
  `manifest_check`, `manifest_compile`, `portable_module`, `qlmeta_dump`),
  flip the external-types module row's Open note, `tests.md`/`harness.md`
  tallies; `./update-doc-hashes.sh` **after** green (never bump hashes before
  testing).
- `docs/plans/archive/` this plan once verified.

## Explicitly skipped (add when the trigger fires)

- Shape-only **runtime** stubs (compile today's v1 needs no ctx registration;
  *running* against stubs) — needs deferred-use errors everywhere; only a
  real "interpret without bindings" demand justifies it.
- Flow-insensitive receiver inference beyond single-init (parameters,
  returns, branches) — that is strict typing (module 16), its own project;
  P4b's declared annotations are the absorption point, and at enforcement
  level 2 the lenient path shrinks to nothing without any inference.
- Manifest signing / tamper-evidence — fingerprint detects drift, not
  malice; add when distribution crosses a trust boundary.
- constexpr static-blob emission (manifest compiled into the binary with no
  runtime walk) — `T::qlang_meta()` + the registry already is the extraction
  point.
