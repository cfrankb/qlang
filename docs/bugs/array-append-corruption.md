# RESOLVED: "Array append corrupts element 0" was a test-harness false positive

Status: **closed — not an engine bug.** The reported corruption was a
dangling reference in the C++ test harness, not a defect in the QLang engine.
Array append (`arr[] = v`) is correct.

A real (minor) documentation bug found while investigating — the `ARRAPP`
operand was documented as `c` but is `b` — **is fixed** in this change.

## The original (incorrect) report

`local out = []; out[] = 1; out[] = 2; out[] = 3` was reported to produce a
garbage element 0 (`typeName()` → `"?"`) with shifted values. That was wrong.

## Root cause of the false positive

The harness read the array like this:

```cpp
const auto &arr = ctx.get("out").asArray();   // DANGLES
```

`QLangContext::get()` returns a `QLangValue` **by value** (`src/QLangContext.cpp:152`).
`asArray()` is a plain `const` member (not ref-qualified) that returns a
reference to the `m_storage` member *inside* that temporary. Binding a
reference to the result of a **member-function call** on a temporary does NOT
extend the temporary's lifetime (that extension only applies to direct member
access `obj.member`, or to ref-qualified member functions). So the temporary
`QLangValue` is destroyed at the end of the full expression and `arr` dangles
into dead stack memory. Reading it yields freed-stack garbage — including a
bogus `m_type` that `typeName()` renders as `"?"`.

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

The engine path is clean end to end: the `[]` literal builds an empty
`ArrayValue` via the constant pool (`src/QLangCompiler.cpp:2806-2813`); the
bare-append statement emits `ARRAPP baseR, valR, 0`
(`src/QLangCompiler.cpp:3226`); the VM handler does a plain
`asArray().push_back(val)` (`src/QLangVM.cpp:320-326, 1481-1483`). No
off-by-one, no raw-byte copy.

## The real bug found & fixed: `ARRAPP` operand documented as `c`, is `b`

While tracing, the disassembly of the append read `arrapp 00, 00` (base, 0),
which *looked* like a self-append and is what first pointed the investigation
at "append writing at the wrong index." In reality the value is in operand
**b** (the compiler emits it there and the VM reads it there); operand **c**
is unused/zero. Three sites wrongly referenced `c`:

| File | Line | Before | After |
|------|------|--------|-------|
| `include/qlang/QLangBytecode.h` | 101 | `arrays[a].push_back(c)` | `arrays[a].push_back(b)` |
| `examples/qlang_run.cpp` | 341 | `rn(ins.a) << " += " << rn(ins.c)` | `rn(ins.a) << " += " << rn(ins.b)` |
| `examples/qlang_run.cpp` | 465-467 | `ARRAPP` in the `A + C` mask group | `ARRAPP` → `A + B` (separated from `CALLCLOS`) |

After the fix the listing reads `arrapp 00, 03` / `arrapp 00, 05` (base,
value-register), matching the emitted bytecode. Runtime behavior is unchanged
(it was already correct); only the documentation and the `.qla` listing are
fixed.

## Guidance for C++ harnesses

Never bind a reference to a member of a by-value return in the same full
expression. Copy first:

```cpp
QLangValue v = ctx.get(name);      // always
const auto &arr = v.asArray();     // safe
```

## Regression

Full suite green after the doc/disassembly fix: `ctest` 5/5, `spec_check2`
421/421, `range_check` 22/22, `test_module` OK, `qlang_example` OK.
