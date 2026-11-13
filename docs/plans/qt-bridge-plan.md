# Plan: Qt bridge — Q_OBJECT classes for hosted scripts

Status: **draft** (2026-11-13) — pending approval; Phase 0 spike not yet run.
Spec: `docs/spec/19-Qt6_integration.md` — **draft authored 2026-11-13**
(hash `6ff5575b…`, recorded in `docs/spec/README.md`); it flips to final with
the P1 round.
Related: `docs/spec/11-external-types.md`, `docs/spec/17-signals.md`,
`docs/EXTERNAL_TYPES.md` (§3 lifetime, §9 dispatch, §14 portable-module
handshake), `docs/briefs/add-qlmeta-to-cpp.md` (the "no hand-written
`TypeMeta`" rule — this plan adds one sanctioned exception, D2).

## Goal

When QLang is built **with Qt support explicitly enabled** (CMake option,
macro-defined), a hosted script gains access not only to `QL_OBJECT`-annotated
host types but to **Qt's own `Q_OBJECT` classes**: create dialogs and widgets
(`QMessageBox`, `QPushButton`, …), read/write their properties, call their
methods and slots, use their enum constants, `connect` their signals to script
handlers, and `emit` Qt signals from script — plus `QFile` for file I/O.

When the option is OFF (default), nothing changes anywhere: `libqlang.a` never
links Qt, no Qt symbol appears in it, and the emscripten/MinGW/MSVC portability
story of `QLangMeta.h` is untouched.

## What the bridge is

A **QMetaObject-driven binding builder** in a new `qlang_qt` library: at
registration it walks `T::staticMetaObject` (methods, signals, slots,
properties, enums, **including the full `superClass` chain**) and synthesizes
the existing `TypeMeta`/`Member`/`SignalMeta`/`EnumMeta` structures, then feeds
them to the existing `QLangContext::registerType()` (`include/qlang/QLangContext.h:336`).
The VM, compiler, bytecode, `.qlc` format and the script-level
`connect`/`emit`/`TABGET`/`CALLCLOS` paths are **unchanged** — Qt classes
arrive as ordinary external types whose bindings happen to be built at runtime
from Qt's own introspection data instead of at compile time from `QL_OBJECT`.

Rejected alternatives (settled by the 2026-11-13 exploration):

- **Hand-written `QLObject` wrappers per class** — does not scale to "all
  Q_OBJECT"; dozens of classes × hundreds of members.
- **Making `QL_OBJECT` emit `Q_OBJECT` / moc-aware headers** — solves the wrong
  direction (host types becoming QObjects); moc textual scanning + leftmost
  base rules + lambda parse failures for no script-visible gain.
- **QML/QtScript-style generated wrappers** — a codegen project, not a bridge;
  QMetaObject already carries the interface at runtime.

## Design decisions

| # | Topic | Decision | Rationale / consequence |
|---|-------|----------|-------------------------|
| **D1** | Build shape | New static target `libqlang_qt.a` (`src/qt/*.cpp`, public header `include/qlang/QLangQt.h`), built only when `option(QLANG_WITH_QT OFF)`; defines `QLANG_WITH_QT=1` PUBLIC and links `Qt6::Core`/`Qt6::Gui`/`Qt6::Widgets`. `libqlang.a` gains **no** Qt dependency. | Mirrors the CMake-layout rule (separate targets, `examples/` demos only). "Linked against Qt" is not detectable by the preprocessor — the option **is** the contract. |
| **D2** | Binding source | `TypeMeta` synthesized from `QMetaObject` at registration (pure data + `std::function` closures — `QLangMeta.h` structs are exactly that). `add-qlmeta-to-cpp.md` gets a one-paragraph exception: hand-built `TypeMeta` is sanctioned **only** inside `qlang_qt`. | Keeps every dispatch path (external_ops, registerType validation, TypeRef seeding) untouched. |
| **D3** | Type availability | **Explicit registration list**, one macro line per class (`QL_QT_TYPE(QMessageBox)` …), shipped by `qlang_qt`; host opts in per class or takes the module default set. | There is **no runtime enumeration** of all linked `QMetaObject`s — "all Q_OBJECT" is necessarily a build-time list. The list doubles as the **security boundary** (an uncurated "all" would include `QProcess`-adjacent classes; none are in the default set). |
| **D4** | Inheritance | **Flatten at registration**: walk `superClass` + `methodOffset/propertyOffset/enumeratorOffset` into one flat member/signal/enum list (nearest declaration wins). | `ObjectOps::base` stays reserved (deviation unchanged); Qt meta *is* an inheritance chain, so flattening is the only v1-viable model, and it matches the flat registry that rejects duplicate names. |
| **D5** | Overloads | Collapse same-name methods into **one `Member`** whose `invoke` dispatches over the overload set internally (try each candidate's metatype conversion, then arity error listing all candidates). | Registration's duplicate-name rejection (§11(b)) stays intact; Qt APIs are overload-heavy (`QMessageBox::information` ×4). |
| **D6** | Construction | **Explicit ctor lambdas in the registration list** (per class, full overload set handled in plain C++). `QMetaObject::construct`/moc `Construct` is **not** relied on. | The moc `Construct` emission is Designer territory with version-dependent coverage — betting the ctor path on it imports a silent failure mode for ~15 lambdas of saved code. The P0 spike documents reality but the default stands. |
| **D7** | Lifetime | New third flavor in `ExternalRef`: an optional `std::function<void*()>` **alive-resolver**; `self()`/`selfMut()` consult it first — null result → the existing `runtime error: object destroyed: <type>`. `qlang_qt` fills it with a `QPointer` capture (Qt deletes widgets under us: `deleteLater`, dialog close, reparenting). Qt-type validation becomes `metaObject()->inherits(registered)` instead of the `QLObject<T>`-based `dynamic_cast` (`QValue.h:83,96`) — sound because `QObject` is the primary base of every Q_OBJECT class, so the stored most-derived `void*` **is** the `QObject*`. | This is the plan's **one core seam** (D7 tests land in the Qt-free `external` suite with a fake resolver). `weak_ptr`/`shared_ptr` semantics cannot model the Qt parent tree; `QPointer` is the platform answer. |
| **D8** | Qt signals → script | Script `connect(qtObj, "clicked", handler)`: the bridge lazily builds a **relay** `QObject` per sender+signal and wires `QMetaObject::connect(sender, signalIndex, relay, slotIndex)`; the relay's `qt_metacall(InvokeMetaMethod)` marshals the `void**` args via `QMetaType` and enters the **existing** connection-registry delivery (`deliverSignal` path, `17-signals.md`). Sender destruction: Qt auto-disconnects the relay; the registry's weak-sender equivalence uses the D7 resolver. | `QMetaObject::connect` (index-based, public, static) is exactly the scripting-binding mechanism (QtScript/qtruby precedent); the relay `qt_metacall` handles arbitrary parameter types without codegen. Signal index may live on a base class (`QAbstractButton::clicked`) — resolve via `metaObject()->indexOfSignal` on the chain (normalized signature from `QMetaMethod::methodSignature`, not string guessing). |
| **D9** | Script → Qt signals | `emit qtObj.sig(args)` on a Qt-flavored descriptor routes through `QMetaObject::activate` (args marshalled out to `void**` storage per D10). Script may also `connect` two Qt objects directly (`QMetaObject::connect`, no relay) — pure win, no handler involved. | Mirrors §17 semantics from the script's side: same `connect`/`disconnect` builtins, same handle semantics. |
| **D10** | Marshalling | Fixed table, metatype ↔ `QLangValue`: bool/int family/qreal ↔ `QMetaType::Bool…Double`; `QString`↔Str (`toStdString`/`fromUtf8`); `QByteArray`↔Str; `QMetaEnum`-backed enums/flags ↔ Int (flag combination stays native int bit-ops, §6 precedent `DockArea`); `QObject*`-family ↔ `External` (resolved through D7, dead object → type error). Anything unmappable: `QTypeTag::Unknown` in the manifest, **hard type error at the call boundary** naming the metatype. Qt value types (`QColor`, `QPoint`, …) are P3. | Bounds the surface; mirrors the scalar-only §7 boundary philosophy with a Qt-shaped second table owned by `qlang_qt` (no new `as<T>()` in core). |
| **D11** | Modality & re-entrancy | Contract: hosted Qt scripts run on the **GUI thread**. Dialog `exec()` blocks a Qt event loop **inside a VM instruction** — v1 permits it (stock QMessageBox buttons are served by Qt's own internal slots — no script re-entry). If the **script** connected handlers into widgets that live inside the modal loop, delivery re-enters `deliverSignal` recursively: allowed, contained by the step budget (the §17 re-entrancy rule), documented as a host hazard. VM-aware non-blocking dialog forms are **out of scope**. | A new class of re-entry (nested event loop, same context) deserves one contract paragraph in spec 19, not a VM redesign. |
| **D12** | Manifests / portability | Qt-enabled builds register the synthesized metas' accessors into `metaRegistry()` at `qlang_qt` init, so `--qlmeta-dump` / `--qlmeta` compile checks / QPM packaging work unchanged; the §14 handshake already refuses a package carrying `QMessageBox` on a non-Qt host with `missing type 'QMessageBox' required by portable module` and runs nothing. | Zero new machinery; Qt-version shape drift is policed by the existing byte comparison. |
| **D13** | v1 scope | QObject classes from the P1 list only: `QFile` (+ `QDir`), `QMessageBox`, `QDialog`, `QWidget`, `QPushButton`, `QLabel`, `QLineEdit`, `QCheckBox`, `QRadioButton`, `QComboBox`, `QSlider`, `QProgressBar`, `QGroupBox`, the three box layouts, `QApplication` access (instance provided by host, read-only `activeWindow`-style helpers). **Out:** parent-tree ownership transfer, `QTimer`/event-loop-tied classes, models/views, value types, custom script-implemented QObjects, threading. | File I/O + dialogs + basic widgets is the stated goal; everything else is a named P3 backlog item, not a promise. |

## Impact assessment

| Area | File(s) | Change | Risk |
|------|---------|--------|------|
| Core lifetime seam | `include/qlang/QLangValue.h` / `QValue.h`, `src/QLangValue.cpp` (or inline equivalents), `src/QLangObjects.cpp` (`external_ops::getField/setField`, `resolveExternal`) | `ExternalRef` gains optional `resolve` closure; `self()`/`selfMut()` consult it (null → `object destroyed`); validation short-circuits to `check` when set | **Medium — the only core change.** Existing owned/borrowed flavors untouched; tested Qt-free in `external` with a fake resolver. |
| Connection registry | `include/qlang/QLangContext.h`, `src/QLangContext.cpp` | Weak-sender reference gains the resolver flavor (Qt senders have no `weak_ptr`); delivery path unchanged | Medium. |
| Bridge | **new** `include/qlang/QLangQt.h`, `src/qt/QLangQtBridge.{h,cpp}` (meta→TypeMeta builder, marshalling table, relay), `src/qt/QLangQtTypes.cpp` (registration list + ctor lambdas), `src/qt/QLangQtEmit.cpp` (activate path, P2) | ~new code only; uses public `registerType`, `metaRegistry`, `deliverSignal`/connection APIs | High volume, isolated behind the option; the relay/marshalling correctness is the sharp edge — pinned by the P0 spike before any of it is written. |
| CMake | `CMakeLists.txt` | `option(QLANG_WITH_QT OFF)`; `find_package(Qt6 COMPONENTS Core Gui Widgets)`; `add_library(qlang_qt …)`; demo + test targets guarded by the option | Low — default build byte-identical. |
| Tests | `tests/unit/qt_bridge.cpp` (new ctest, option-guarded, `QT_QPA_PLATFORM=offscreen`); additions to `tests/unit/test_external.cpp` (resolver seam, Qt-free) | see Test plan | — |
| Demo | `examples/qlang_qt_host.cpp` + `examples/qt_demo.ql` | host: `QApplication` + context + `qlang::qt::registerAll(ctx)` + `show()`-style non-modal demo (P2) | — |
| Docs | `docs/spec/19-Qt6_integration.md` (finalize draft), `docs/spec/README.md` (hash re-record), `docs/EXTERNAL_TYPES.md` (§3 resolver flavor addendum), `docs/briefs/add-qlmeta-to-cpp.md` (D2 exception), `docs/compliance/status.md` + `history.md` (round entries) | per AGENTS.md: spec finalized **with** the implementation round, **test first, then record hashes**. | — |

Not affected: lexer/parser/compiler/bytecode (no new syntax or opcodes — Qt
classes are plain registered external types), `.qlc` version, `QValue` tags
(External 20 reused), `QLangMeta.h` macros, non-Qt build in its entirety.

## Phases

### P0 — spike (throwaway, under `/tmp/opencode`; results recorded in `history.md` and the Q-table)

1. On the target Qt 6: relay `QObject` + `QMetaObject::connect(btn,
   indexOfSignal("clicked()"), relay, slotIndex)` — verify `qt_metacall`
   receives `void**` args of the declared types (bool for `clicked`).
2. Same for a **base-class** signal (`QAbstractButton::clicked` from a
   `QPushButton`) and a typed signal (`QLineEdit::textChanged(const QString&)`).
3. `QMetaObject::activate` round-trip (emit reaches a connected relay).
4. `QApplication` + `QT_QPA_PLATFORM=offscreen` widget creation under `ctest`
   (headless CI feasibility).
5. (Informational only, per D6) whether `QMetaObject::construct` works on
   `&QPushButton::staticMetaObject` in this Qt version.

**Gate:** items 1–4 must pass. If 1 fails on the target Qt, the plan stops
here — there is no cheaper signal-plumbing fallback worth keeping.

### P1 — core bridge (one compliance round)

1. `ExternalRef` resolver seam + Qt-free tests in `external` suite.
2. `qlang_qt` target, `QLANG_WITH_QT` plumbing.
3. Meta→TypeMeta builder (D2, D4 flatten, D5 overload collapse, D10
   marshalling incl. `QString`/`QByteArray`/`QMetaEnum`).
4. Registration list (D3, D6 ctor lambdas), `qlang::qt::registerAll(ctx, opts)`
   + per-class opt-in.
5. `QFile` (owned; `open/read/write/readAll/close/exists/remove/size`,
   `QIODevice::OpenModeFlag` enums through `EnumMeta`, **native `:/` resource
   paths read-only** — spec 19 `## QFile and resource paths`) — the minimal
   proof the whole chain works **without** any GUI dependency (QtCore only).
6. Script `connect` → relay → `deliverSignal` (D8); auto-disconnect on sender
   destruction.
7. Spec 19 finalized (draft authored 2026-11-13; feature contract, GUI-thread
   rule, scope list, security note already drafted); `EXTERNAL_TYPES.md` §3
   addendum; compliance round + hash re-record.

### P2 — GUI + emit (one compliance round)

1. QtWidgets default set (D13 list), layouts, dialogs; `exec()` stance (D11)
   with a re-entrant-delivery stress test.
2. `emit` → `QMetaObject::activate` (D9); Qt↔Qt direct connect from script.
3. Manifest integration (D12): `metaRegistry` accessors, `--qlmeta-dump` smoke
   test, QPM round-trip on/off Qt hosts (the `missing type` refusal).
4. Event-driven host pattern documented (context lifetime, handlers outliving
   the script body) + `examples/qlang_qt_host.cpp`.

### P3 — backlog (no commitments)

Value types (`QColor`/`QPoint`/`QRect` chains), models/views, `QTimer`,
parent-tree ownership transfer, script-implemented QObjects (custom slots),
per-host class opt-out policies.

## Test plan

- `external` (default build): resolver-flavor checks — dead resolver →
  `object destroyed: T`, resolver + `check` interaction, owned/borrowed
  regression untouched.
- `qt_bridge` (option ON): QFile round-trip script (write/read/exists/remove);
  QPushButton property/method/enum checks; script `connect(btn,"clicked",h)`
  fires h from `btn.click()`; base-class + typed signal delivery; overload
  dispatch; `emit` activate (P2); dead-widget access error; GUI-thread
  contract not enforceable in-suite — documented.
- Everything under `QT_QPA_PLATFORM=offscreen`; suite registered with
  `add_test` + `ENVIRONMENT` property; **default (Qt-off) ctest tally must be
  unchanged**.
- Per AGENTS.md before any "verified" claim: `cmake -B build && cmake --build
  build` + `ctest --test-dir build`, both option states.

## Open questions (each with a default — adopt on silence)

| # | Question | Default |
|---|----------|---------|
| Q1 | Stub name cleanup (`19-Qt _integration.md` — stray space)? | **Resolved 2026-11-13**: renamed to `19-Qt6_integration.md` (underscore precedent: `16-strict_typing.md`; "Qt6" kept per the index row the host authored) |
| Q2 | Target Qt version floor? | Qt 6.5 LTS (metatype `name()` availability, `QMetaType` id stability) |
| Q3 | Bridge name in scripts: Qt's (`QMessageBox`, `QPushButton`) — collides with nothing today but pins future built-ins | Keep Qt names verbatim; collision policy = the existing import-shadowing rules (§6) |
| Q4 | `connect` overload for Qt↔Qt from script in P2 or P3? | P2 (it's a few lines once the relay exists) |
| Q5 | Should `qlang_run` grow a Qt mode (host a Qt event loop itself)? | **No** — `qlang_run` stays Qt-free; hosting is the app's job (18-qlang-run.md untouched). The demo covers the pattern |
| Q6 | Do app-owned (non-Qt-class) QObjects get the bridge too? | Out of scope: app types stay `QL_OBJECT`; if a host wants both faces on one class, that is the *old* rejected `Q_OBJECT`-in-`QL_OBJECT` idea and needs its own plan |
| Q7 | Widget deletion initiated by script (`w.deleteLater()` — it is a slot, so the bridge exposes it by default)? | Expose it (it's just a slot); document that script-side deletion + held references then hit the `object destroyed` path — which D7 exists to make clean |

## Commits / rounds

P0 findings + this plan amended per spike results (docs-only).
P1: core seam + tests, bridge, QFile, spec 19, `EXTERNAL_TYPES.md`, compliance
round — stage core seam separately from `qlang_qt`. P2: GUI + emit round.
Suggested P1 commit name:
`feat(qt): QMetaObject bridge — meta-driven Qt type imports behind QLANG_WITH_QT`.
