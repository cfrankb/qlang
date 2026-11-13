# Qt6 integration

**Status: draft (2026-11-13) — not implemented.** The module is reserved by
`docs/plans/qt-bridge-plan.md` (pending approval; its Phase-0 spike decides
feasibility). The behavior below is the contract that plan commits to; the
status flips to final with the P1 compliance round (hash re-recorded then).

When the host binary is built with Qt support — the CMake option
`QLANG_WITH_QT`, which defines the C++ macro of the same name; default **off**
— a hosted script gains access to Qt's own `Q_OBJECT` classes beside the
host's `QL_OBJECT` types ([11-external-types.md](11-external-types.md)):
dialogs and widgets, `QFile` (including `:/` resource paths), Qt signals and
slots. Built without it, nothing in this module exists: no Qt class is
importable and no Qt symbol links into the binary — the language is exactly
modules 01–18.

## availability

- Qt types are **external types** (11) as far as scripts are concerned: dot
  access, property read/write, method calls, `T.Constant` enum folding,
  `connect`/`emit` ([17-signals.md](17-signals.md)) behave identically to
  hand-bound types.
- The binding shape comes from Qt's own runtime introspection (the class's
  meta-object), not from per-class `QL_OBJECT` declarations. Consequences a
  script can observe:
  - **Inherited members are flattened in**: a `QPushButton` answers everything
    its Qt base chain declares (`setText` from `QPushButton`, `click` from
    `QAbstractButton`, `setParent` from `QWidget`). Inherited enum constants
    travel with each class (`QFile.ReadOnly` — the flag is declared by the
    unregistered `QIODevice`).
  - **Overloads collapse into one method**: a call is matched against the
    overload set by argument count and convertibility; a no-match call is a
    type error naming the candidates.
  - **Default arguments do not exist**: meta-objects carry no default values,
    so every declared parameter must be passed.
  - Slots and `Q_INVOKABLE` methods are the same thing to the script (both
    callable — consistent with 17's treatment of signals).
- The available classes are a **curated registration list**, never a runtime
  enumeration (Qt cannot enumerate the meta-objects in a binary, and the list
  doubles as the security boundary — process/network classes are deliberately
  absent). The host widens the list explicitly (`qlang::qt::registerAll(ctx)`
  or per-class opt-in); nothing is consulted from Qt for a class that is not
  listed. The honest reading of "all Q_OBJECT": **every member of every
  registered class, as Qt itself describes it** — not every symbol in Qt.
- Default set (P1): QtCore — `QFile`, `QDir`; QtWidgets — `QMessageBox`,
  `QDialog`, `QWidget`, `QPushButton`, `QLabel`, `QLineEdit`, `QCheckBox`,
  `QRadioButton`, `QComboBox`, `QSlider`, `QProgressBar`, `QGroupBox`,
  `QVBoxLayout`, `QHBoxLayout`, `QGridLayout`.
- Host-owned classes keep `QL_OBJECT` (11); a host class is never imported
  from its meta-object just because it has one.

## construction and ownership

- Script-side construction like any external type (11 §6):
  `let box = QMessageBox(parent, "Title", "Text", buttons, flags)`. The script
  reference owns the object.
- Closing a window does not destroy the object (Qt does not delete closed
  dialogs by default); dropping the last script reference does — including a
  window that is still visible (the window goes away). A host wanting
  Qt-side lifetime reparents the widget into the C++ tree (next bullet).
- `setParent` moves ownership per Qt's rules; the script view tracks the
  object's liveness and thereafter reports
  `runtime error: object destroyed: <type>` (the expired-borrow error, 11 §3)
  — a C++-side delete is never a dangling access.
- `deleteLater()` is a public slot and callable from script; the same
  `object destroyed` rule applies after the deletion runs.

## signals and slots

- `connect(button, "clicked", handler)` — both handler forms and both handle
  forms of 17 apply; the signal name resolves against the meta-object chain,
  so a signal declared on a base (`QAbstractButton::clicked`) connects from
  any derived class.
- Handlers receive the Qt signal's arguments converted per the boundary table.
- `disconnect` (handle / sender+signal) and the auto-disconnect on sender
  destruction follow 17 unchanged.
- `emit obj.signal(args)` on a Qt type **activates the real Qt signal**: C++
  subscribers see the emission; the script-visible checks (unknown signal,
  arity, convertibility) mirror 17.
- `connect(a, "signal", b, "method")` where both `a` and `b` are Qt objects
  connects **directly** — delivery stays inside Qt and never enters the VM.
- A handler connected to a widget inside a modal `exec()` loop (GUI contract)
  runs **nested** inside the blocking call; runaway nesting is contained by
  the step budget (17's re-entrancy rule), not by a re-entrancy guard.

## boundary

Value conversion follows 11 §7, extended for Qt types:

| Qt | QLang |
|---|---|
| `bool`, integral, `double`/`float` metatypes | `Bool`, int family, `Float` |
| `QString` | `Str` (UTF-8) |
| `QByteArray` | `Str` (bytes pass through) |
| Qt enums and flag types | `Int` — flag combination is native int bit-ops (11 §6 precedent) |
| a registered Qt class (any base-compatible value) | `External`; a call argument naming a destroyed object is a type error |

A call or property whose signature involves a metatype outside the table
(`QColor`, `QPoint`, `QModelIndex`, …) is a **type error naming the
metatype** — never a silent pass-through or opaque value.

## QFile and resource paths

- Paths use Qt's native semantics: `:/…` addresses the **Qt Resource
  System** — resources compiled into the host binary are readable by script
  with no host callback: `let f = QFile(":/data/config.ini")`. Relative paths
  resolve against the host process's working directory.
- Resource files are **read-only**: `open(ReadOnly)` succeeds on `:/…`, any
  write-opening fails. I/O failure is a `false` return from `open`/`write`,
  never a QLang error (the no-exception rule of 11 for failed I/O;
  `errorString()` explains).
- `open` takes the mode flag constants promoted onto `QFile`
  (`QFile.ReadOnly | QFile.Text` — inherited-enum rule, flag bit-ops);
  `open`/`close`/`isOpen`/`read(max)`/`readAll`/`write(str)`/`size`/`resize`/
  `exists`/`remove`/`rename`/`copy`/`fileName`/`errorString` behave as Qt
  documents for them.

## GUI contract

- Script code touching GUI classes runs on the Qt **GUI thread** — a host
  obligation (same class of contract as `docs/EVENT_HANDLERS.md` §8.2); the
  runtime does not diagnose its violation (Qt asserts).
- `dialog.exec()` blocks inside one VM instruction, driving Qt's own event
  loop; the dialog's built-in buttons work regardless of script handlers.
  Non-modal `show()` returns immediately.
- Event-driven hosting: after `show()` + `connect` calls the script body may
  end; connections keep firing while the host keeps the context alive. That
  is a host pattern (the demo), not a VM mode.

## portability

- Qt object values are `External` (tag 20) — non-durable, never serialized
  (11 §3, `docs/SERIALIZATION.md`).
- A `.qlc`/QPM manifest built from a Qt-enabled binary names the Qt types it
  uses; a host built **without** Qt support refuses such an image at load —
  `missing type 'QMessageBox' required by portable module` — and runs nothing
  (11 §14). Qt-version shape drift is caught by the same byte comparison.

## restrictions (v1)

- default set + explicit host opt-ins only — no script-side discovery of
  unlisted classes;
- no Qt **value types** (`QColor`, `QFont`, `QPixmap`, `QRect`, …): calls
  involving them are type errors (plan P3);
- no models/views, no `QTimer`, no script-side subclassing of Qt classes, no
  queued/cross-thread connections, no signal-to-signal connections beyond 17;
- `qlang_run` stays Qt-free (18-qlang-run.md unchanged) — Qt hosting is the
  embedding application's job.

## tests

Dedicated suite `qt_bridge` (registered only with `QLANG_WITH_QT=ON`,
`QT_QPA_PLATFORM=offscreen`): QFile write/read/exists/remove round-trip; a
`:/` resource read and a `:/` write-opening failure; property read/write and
enum constants on `QPushButton`; `click()` delivering to a connected handler
through a base-class signal; `textChanged(const QString&)` argument
conversion; overload no-match listing candidates; `emit` reaching a C++
subscriber; Qt↔Qt direct connect; access after `deleteLater` →
`object destroyed`; non-Qt-host load rejection of a Qt-typed package (the
portable-module gate).
