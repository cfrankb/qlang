# BUG: Array append (`arr[] = v`) corrupts element 0 and shifts appends

Status: **open** — pre-existing, independent of the Task 4 (external-type
display) work. Not caused by, and not fixed by, the register-allocator
changes in `src/QLangCompiler.cpp`.

Severity: **high** (silent data corruption: a garbage element of an
unrecognized type enters user data).

## Symptom

Appending to an empty array literal produces a leading element whose
`QLangValue` type is not a valid enum value (`QLangValue::typeName()` →
`"?"`), and the appended values land shifted by one slot.

```
local out = []
out[] = 1
out[] = 2
out[] = 3
# expected: [1, 2, 3]
# actual:   size=3, [0]=<type "?">, [1]=int, [2]=int   (one value lost)
```

Inside a loop the same pattern yields an extra trailing element:

```
local out = []
for i in range(0, 3) {   # range is inclusive -> i = 0,1,2,3 (4 iterations)
  out[] = i
}
# expected: [0, 1, 2, 3]
# actual:   size=4, [0]=<type "?">, [1]=int, [2]=int, [3]=int
```

The `?` type is the "no matching enum case" default returned by
`QLangValue::typeName()` (`src/QLangValue.cpp:34`). It means the `QLangValue`
stored in the array has a **corrupted / uninitialized `m_type`** — this is a
memory-level defect, not a value the language is supposed to produce.

## Reproduction

Minimal harness (builds against `build/libqlang.a`):

```cpp
#include <iostream>
#include "qlang/QLangContext.h"
using namespace qlang;

int main() {
  QLangContext ctx;
  ctx.run("local out = []\nout[] = 1\nout[] = 2\nout[] = 3\n");
  const auto &arr = ctx.get("out").asArray();
  std::cout << "size=" << arr.size();
  for (size_t i = 0; i < arr.size(); i++)
    std::cout << " [" << i << "]=" << arr[i].typeName();
  std::cout << "\n";   // prints: size=3 [0]=? [1]=int [2]=int
  return 0;
}
```

Compile: `g++ -std=c++17 -I include <file> build/libqlang.a -o <bin>`

A second probe form (index assignment) does not even parse, so the only
working append path is the bare `arr[] = v`:

```
local out = [0, 0, 0]
for i in range(0, 3) { out[i] = i * 10 }
# parse error: unsupported operator ''
```

## Confirmed pre-existing

Reproduced identically with the Task 4 / register-allocator changes stashed
(`git stash push src/QLangCompiler.cpp include/qlang/QLangCompiler.h`,
rebuild, run). The corruption is present on the unmodified compiler, so it
is **not a regression** introduced by the current work.

## What is *not* the cause

- The f-string / external-type display path (Task 4) — the corruption occurs
  with plain integer appends, no externals and no f-strings involved.
- The register allocator — verified identical behavior with those changes
  stashed.

## Where to look (suggested)

The append path for `arr[] = v` (bare-append / `TABPUT`-with-empty-key, or a
dedicated append opcode) and the `Array` storage growth in
`QLangValue`/`ArrayValue`. Likely candidates:

1. The empty literal `[]` initializes the `ArrayValue` with a stray
   default-constructed (zeroed) element, so index 0 is a `QLangValue` whose
   `m_type` is 0-ish garbage rather than `Type::Nil`.
2. The append writes at `size` but `size` is computed before/after the push
   inconsistently, giving the one-slot shift and the lost/extra element.
3. A `QLangValue` is copied/moved into the vector by raw bytes (e.g. a
   `memcpy` or a placement issue) leaving `m_type` unset for the first slot.

The asymmetry between the no-loop case (size 3, one value lost) and the
loop case (size 4, extra trailing slot) suggests the off-by-one interacts
with how the loop body re-reads `out` each iteration — worth a targeted
disassembly of the append instruction sequence.

## Impact

Any script that builds arrays by repeated `arr[] = v` gets a poisoned
leading element and a count/value mismatch. Downstream `str`/`join`/indexing
on such arrays will misbehave or, for the `?` element, hit the type switch's
default path. This blocks the common "accumulate into a list" idiom.

## Acceptance criteria for a fix

- `local out = []; out[] = 1; out[] = 2; out[] = 3` → exactly `[1, 2, 3]`,
  every element `typeName() == "int"`.
- Loop accumulation yields the expected element count with no `?` element.
- No `QLangValue` in a script-owned array can have an `m_type` outside the
  declared enum (consider a debug assertion in the `Array` factory/accessors).
- Full suite green: `ctest`, `spec_check2`, `range_check`, `test_module`,
  `qlang_example`.
