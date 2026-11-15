# RESOLVED: "Array append corrupts element 0" was a test-harness false positive

Status: **closed — not an engine bug.** The reported corruption was a
dangling reference in the C++ test harness, not a defect in the QLang engine.
Array append (`arr[] = v`) is correct.

A real (minor) documentation bug found while investigating — the `ARRAPP`
operand was documented as `c` but is `b`. **The code and the `.qla` listing
were fixed then; the normative operand map in `docs/spec/13-bytecodes.md` was
not, and still said `b unused` — fixed 2026-11-11** (see "`ARRAPP` operand:
documented as `c`, is `b`").

Line references below were re-checked against the tree on 2026-11-11; the
original write-up cited the line numbers as they stood then, and several have
since moved.

## The original (incorrect) report

`local out = []; out[] = 1; out[] = 2; out[] = 3` was reported to produce a
garbage element 0 (`typeName()` → `"?"`) with shifted values. That was wrong.

## Root cause of the false positive

The harness read the array like this:

```cpp
const auto &arr = ctx.get("out").asArray();   // DANGLES
```

`QLangContext::get()` returns a `QLangValue` **by value**
(`src/QLangContext.cpp:839-846`). `asArray()` is a plain `const` member (not
ref-qualified) that returns a reference to the payload handle *inside* that
temporary. Binding a reference to the result of a **member-function call** on
a temporary does NOT extend the temporary's lifetime (that extension only
applies to direct member access `obj.member`, or to ref-qualified member
functions). So the temporary `QLangValue` is destroyed at the end of the full
expression and `arr` dangles into dead stack memory. Reading it yields
freed-stack garbage — including a bogus `m_type` that `typeName()` renders as
`"?"`.

AddressSanitizer confirms it:

```
ERROR: AddressSanitizer: stack-use-after-scope ... READ of size 8
  #0 ... std::vector<qlang::QLangValue>::size() const
  #1 ... main  (the arr.size() read)
Address ... in stack of thread T0 ... 'ctx' frame   <== dead temporary
```

## Correct behavior (verified)

Using the safe pattern — copy the value out first, then take the reference:

```cpp
QLangValue v = ctx.get("out");   // copy the temporary into a named object
const auto &arr = v.asArray();   // safe: v outlives arr
```

- `out[] = 1;2;3` → `size=3  [0]=1 [1]=2 [2]=3` (all `int`)
- `for i in range(0,3) { out[] = i }` → `size=4  [0]=0 [1]=1 [2]=2 [3]=3`
  (range is **inclusive** per spec 04-control-flow, so 4 elements is correct)

The engine path is clean end to end:

- the `[]` literal is a **program constant-pool entry** loaded by one `mov`
  (`src/QLangCompiler.cpp:4654-4690`, `case ExprKind::Array`; the pool insert
  is `emitConst` → `m_pool.add` at `src/QLangCompiler.cpp:888-889`, and
  `evalConstExpr` builds the container at `:5109-5114`). A pool load hands out
  a **fresh copy** of the payload, so appending through the loaded register
  cannot corrupt the shared pool entry. A literal with non-constant elements
  takes the runtime path in the same case: load an EMPTY array, then `ARRAPP`
  each element.
- the bare-append statement emits `ARRAPP baseR, valR, 0`
  (`src/QLangCompiler.cpp:5199-5200`); `+=` on an array selects
  `ARRAPP`/`ARRAPPV` at `src/QLangCompiler.cpp:2093-2102`.
- the VM handler (`src/QLangVM.cpp:2142-2145`) delegates to `appendValue`
  (`src/QLangVM.cpp:413-427`), a plain `(*base.asArray()).push_back(val)`
  (sets dedup through the same helper). No off-by-one, no raw-byte copy.

## `ARRAPP` operand: documented as `c`, is `b`

While tracing, the disassembly of the append read `arrapp 00, 00` (base, 0),
which *looked* like a self-append and is what first pointed the investigation
at "append writing at the wrong index." In reality the value is in operand
**b** (the compiler emits it there and the VM reads it there); operand **c**
is unused/zero. Four sites wrongly referenced `c`. Then and now:

| Site | Then | Now |
|------|------|-----|
| `include/qlang/QLangBytecode.h` | 101: `arrays[a].push_back(c)` | 198: `arrays[a].push_back(b)` |
| `examples/qlang_run.cpp` listing | 341: `rn(ins.a) << " += " << rn(ins.c)` | 752-754: `rn(ins.a) << " += " << right()` |
| `examples/qlang_run.cpp` operand group | 465-467: `ARRAPP` moved out of the `A + C` mask group into `A + B` | **gone** — the mask groups were replaced by the kind-aware `right()` lambda (`:573-586`), which reads `ins.b` for a register operand |
| `docs/spec/13-bytecodes.md` operand map | **never fixed** — `\| ARRAPP \| array reg \| — \| element reg \| \| b unused \|` survived until 2026-11-11, in the one document a reader trusts | 73: `\| ARRAPP \| array reg \| element reg \| — \| \| \|` (spec hash re-recorded in `docs/spec/README.md`) |

After the fix the listing reads `arrapp 00, 02` (base, value-register),
matching the emitted bytecode. Runtime behavior is unchanged (it was already
correct); only the documentation and the `.qla` listing are fixed.

## Residual site (fixed 2026-11-11)

The dangling pattern survived in one place: the deepcopy identity block of
`tests/unit/spec_check2.cpp` bound `const auto &umap =
dctx.get("u").asTable();` (`:1385` then, `:1387` now) and read the freed stack.
Same class, same fix — `asTable()` hands back a `shared_ptr` payload handle
(`include/qlang/QLangVariant.h:82`), so `const auto umap = …` (no `&`) keeps
the payload alive and still compares identities with `.get()`. ASAN on the old
idiom: `stack-use-after-scope`, READ of size 8 in `__shared_ptr::get()`; the
same probe without `&` exits clean.

No extra guard needed: `-Wdangling-reference` (already in the project's
`-Wall`, `CMakeLists.txt:48`) flags every one of these at build time —
1 warning before the fix, 0 after.

## Guidance for C++ harnesses

Never bind a reference to a member of a by-value return in the same full
expression. Copy first:

```cpp
QLangValue v = ctx.get(name);      // always
const auto &arr = v.asArray();     // safe
```

For the container accessors the shorter form is enough, because the accessor
returns a shared handle rather than the payload: `const auto arr =
ctx.get(name).asArray();`.

## Regression

Full suite green after the doc/disassembly fix, re-run 2026-11-11 after the
operand-map correction: `ctest` 58/58 (incl. `doc_hashes`, with the
`docs/spec/13-bytecodes.md` row re-recorded), `spec_check2` 529/529,
`range_check` 22/22, `test_module` OK, `qlang_example` OK.
