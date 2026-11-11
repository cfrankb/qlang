# Module export/import audit (2026-11-10)

Status: **open** — findings record, no fix attempted. Found while auditing what is still missing
for `export`/`import` after **C8b** (Round 69, `.ql` discovery + nested compile). Everything here
reproduces on `HEAD` after C8b; none of it is a C8b regression (the const defect reproduces on the
`.qlc` path, which predates C8b, and on `--import`, which predates it too).

Implementation plan: [../plans/module-export-import-plan.md](../plans/module-export-import-plan.md).

Probes: ~25 two-file QLang pairs (`main.ql` + a module beside it), run as
`build/qlang_run main.ql` with **no** `--module-path` (C8b's entry-directory rule makes the module
findable). The ones worth keeping are quoted below; F1 is the minimal repro of the defect.

## F1 — a module `const` that a `def` reads is unreachable at run time  **(defect)**

`export const` is unusable in any module whose `def` bodies read it. Both the exported cell and the
`def` read nil; the module's own top level is the only code that sees the value.

```ql
# kv.ql                                                  # F1 (p43)
export const MODULE_NAME = "kv"
export const TAG    = "tagged"   # read by a def below
export const SECOND = "two"      # no def touches it
export const THIRD  = "three"    # no def touches it
export def tag()
  return "fn:" + TAG
end
```
```ql
# main.ql
import kv
println("TAG    [" + kv.TAG    + "]")   # -> []            WRONG
println("SECOND [" + kv.SECOND + "]")   # -> [two]         ok
println("THIRD  [" + kv.THIRD  + "]")   # -> [three]       ok
```

`from kv import TAG` reads nil the same way, and `tag()` returns `"fn:"` — the value is gone for
every reader except the defining module's top level. The matrix that bounds the bug:

| # | shape | result |
|---|---|---|
| p43 | one module, captured const + two untouched consts | captured → nil, others fine |
| p34 | module with exported consts, **no `def` at all** | all read fine |
| p36 | `def` present but does **not** read the const | fine |
| p37 | module-**private** `const` read by a `def` (not exported) | `tag()` → `"fn:"` nil |
| p45 | same as F1 + `println(TAG)` as the library's **last** top-level line | `late read [tagged]` |
| p39/p44 | same shape as a plain **script** (`const` + `def` reading it, before/after the call) | correct, before and after |
| p41 | same as F1 with `global TAG` inside the `def` | still nil — `global` does not help |
| p14 | module-level **var** read by a `def` without `global` | compile error `use of undeclared variable 'counter'` |
| p32 | `linker_test`'s `tally` shape (`global counter`, no def reads a const) | correct |

A mutable module variable reached through an explicit `global` (p32, and the `tally` guard's shared
cell) is fine, so the failure is specific to **a top-level immutable cell reached as a capture**.
`QLangCompiler.cpp:1806-1824` is that path, and its own comment states the assumption that fails:

```
// A capture whose home is a module-level IMMUTABLE cell — a top-level const ...
// At module top level the register IS the slot (declareVar)
```

which holds for a script (the main frame aliases the table at base 0) and not for a library, whose
top level is `<init>` — a function frame aliasing the merged table at `regBase`. Sibling defect in
the same family, recorded before this audit: `docs/plans/todo.md` §6, "a top-level `const`
interpolated inside an f-string in a function body evaluates to nil".

### What the merged image says (this is the puzzle)

`qlang_run -a --asm-level 5 main.ql`, F1 (entry width 6, so `regBase = 6`; module `kv` = fn#1,
registers 6..10):

```
function #1 '<init>'                  # kv's top level
  0004  mov  e=0x08, 01, pool[05]     ; r1 = "tagged"        -> merged slot 7
  0008  mov  e=0x08, 02, pool[06]     ; r2 = "two"           -> merged slot 8
  0010  makefunc 04, pool[09]         ; r4 = closure(tag)
function #2 'tag'
  captures: r-8                       ; -1 - 7  -> merged slot 7
  0004  refload 02, 00                ; r2 = cap[0]

  exports:  TAG  const r/f1 -> r7     # export ref            -> merged slot 7
  imports:  TAG  <- kv  cell r7       # entry's import entry  -> merged slot 7
```

Store, export ref, import ref and capture operand **all name merged slot 7**, and `<init>`'s frame
aliases the same storage the importer reads (`QLangContext.h:107-115`: `globalSlots()` *is*
`globalTable().slots`, and `GETIMPORT` reads `globalTable().slots[ref]`, `QLangVM.cpp:2540-2556`).
So the static tables are self-consistent and the cell is nevertheless nil at the reader. The bug is
therefore not visible in any image: it needs the run, and it is not an operand-numbering mistake the
linker could have made.

### Excluded (do not re-check these)

| hypothesis | why it is out |
|---|---|
| linker forgot to rebase the export/import ref or the capture | level-5 listing above: `regBase + slot` everywhere, including `rebaseConstant`'s `-1 - slot` shift (`QLangLinker.cpp:117-131`, `:300-306`, `:454`) |
| module `<init>` never ran, or ran after the entry | F1 + `println` in the library prints first (`init sees [tagged]` before any of main's output); `QLangVM.cpp:1993-2004` runs `initSequence` before the main frame |
| init frame ignores `regs.base` | a sibling const at `base + 2` is exported and read correctly in the same module (p43) |
| two different slot vectors (host scope vs table) | same storage, see above |
| the cell is nilled by capture release | `releaseFrame`/`releaseAllCaptures` only move values **into** `Env` cells; a global capture has `env == nullptr` (`QLangVM.cpp:1835-1861`, `:2369-2386`) — and p44 (script) reads the const unchanged after the call |
| pool folding collapsed the literal | the four literals differ; folding is keyed by value |
| import-index mapping is off by one | a shift would print a sibling's value, not nil |
| `getimport` of a `Const` reads somewhere else | one handler for both kinds, reads `im.ref` |

### Where the fix has to be (unproven, ordered by suspicion)

1. **Two identities for a captured module const.** The store in `<init>` uses the frame register the
   free-list/declare path handed out (`QLangCompiler.cpp:670-690`, `declareVar`), while the capture
   and the export `ref` use the slot recorded when the capture was built
   (`:3800-3828`, `m_globalRegMap[name] = m_scope->globalSlotOf(name)`). For a library compiled on a
   scratch context those two numbers are taken at different moments; if they diverge, the module is
   self-consistent (p45) and every outside reader is wrong (F1) — exactly what is observed. The
   image-level listing cannot show it because both prints are *labels*, and the module's own
   numbering is used for both.
2. **The `immutableCell` capture rule itself** (`:1806-1824`): a library-mode module const could be
   reached the way `global` reaches a var (an absolute cell operand the linker already rewrites)
   instead of a capture, which would remove the second identity instead of reconciling it.
3. Not the linker: it faithfully rebases whatever the compiler recorded.

## F2 — two `from … import *` can never coexist  **(defect, needs a decision)**

`MODULE_NAME` is an ordinary exported const, so it is a public name, so two star-imports collide:

```ql
from a import *      # a.ql, b.ql both `export const MODULE_NAME`
from b import *      # -> 'MODULE_NAME' is already imported from module 'a'
```

`from m import *` is specified to bind "every public name `m` exposes"
([../spec/09-modules.md](../spec/09-modules.md)), so the compiler is literal-correct and the idiom
unusable. Identity or entity is an open decision — plan **Q40**.

## F3 — per-name import alias has never parsed  **(spec requirement, unimplemented)**

`from m import f as g` → `parse error … unexpected token 'as'`. `13-bytecodes.md`
`## technical specifications` 5 states "aliases is already a requirment. (e.g.
`from m import f as g`)", `09-modules.md` states it is *not* parsed, and `ImportEntry::alias` exists
and is permanently empty. Recorded as "recorded, not fixed" in `history.md` (Round 68). Plan P2.

## F4 — classes cannot be exported  **(unspecified)**

`export class Point { … }` → `export can only prefix def, const or a variable assignment`.
`## keyword export` lists functions, variables and constants only, so a module that wants to offer a
type has no answer in the spec. Plan Q42 (default: out of scope until the spec says otherwise).

## F5 — diagnostics that mislead

- Reading through a module the entry never imported (pulled in only transitively) reports
  `'leaf' is a module, not a value: reach what it offers as 'leaf.<name>'` — advice that cannot
  work, the module is not bound here.
- A `def` reading a module-level **var** without `global` reports `use of undeclared variable`,
  while the same read of a module-level **const** silently compiles to nil (F1). The const path
  should resolve or report, never both-fail-quietly.
- The cross-module capture guard's message is good and *is* the shape F1 needs:
  `imported function 'outer' captures a local of its own module and cannot be called across a
  module boundary: its captures are seeded from the defining module's frame` — it fires for *local*
  captures; a *global* capture is allowed through on the assumption F1 breaks.

## F6 — surface that is correct (probed, keep as regression anchors)

`import m as a`; legacy `import m.f`; `from m import f`; `from m import *`; exported generator
across the boundary; exported `def` using its own lambda captures; a `.ql` library importing a C++
module (`import math` inside `kv.ql`); the spec's standalone `export AUTHOR` form; duplicate
`export A` → `'A' is already exported`; `kv.CONST = x` → `cannot assign to module member`;
two `.ql` modules with different names in one run; a `.qlc` as the entry file.

## Follow-ups

- Plan: [../plans/module-export-import-plan.md](../plans/module-export-import-plan.md) (P0 = F1).
- `docs/plans/todo.md` §6 carries F1 as a defect line with a pointer here.
- Probe sources: F1 above; the rest of the matrix is small enough to re-type from the shape column.
