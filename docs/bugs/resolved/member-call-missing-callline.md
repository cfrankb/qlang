# Member calls (`obj.method(...)`) compile with `callLine`/`callCol` = 0 — disassembly locs point at the wrong line and the listing interleaves source headers

**Date:** 2026-11-15
**Severity:** medium (diagnostics correctness — wrong locs in the image's `locs:` section and in the `-a` disassembly listing; error positions for member-call failures may also report 0:0)
**Status:** fixed — `parsePostfix()` and the keyword-builtin Call sites now stamp
`callLine`/`callCol` (member calls via new `Member::memberLine/memberCol`
copied from the member-name token); regression test
`tests/unit/member_call_loc_test.cpp` (`ctest -R member_call_loc`)
**Cited code at commit:** `a8e7896`

## Symptom

`build/qlcc -a --asm-level 4 tests/scripts/neo.ql` renders the L7/L8
statements with a stray `L6:` header and repeated `L7:`/`L8:` headers,
splitting each statement's instructions across mismatched source lines:

```
  L7:  println(f"min: {math.min([66,1,7,9])}")
  0029  mov          e=0x08, 03, pool[01] ; r3 = pool[1]    # "min: %s"
  002d  mov          e=0x08, 05, pool[02] ; r5 = pool[2]    # [66, 1, 7, 9]

  L6:                                                        # <-- blank line 6!
  0031  call         e=0x50, 04, -7, 01 ; r4 = native6(r5)  # native:6

  L7:  println(f"min: {math.min([66,1,7,9])}")               # <-- duplicated
  0037  call         e=0x50, 02, 0e, 02 ; r2 = format(r3, r4)
  003d  call         e=0x50, 01, 04, 01 ; r1 = println(r2)

  L8:  println(f"min: {math.min(5,8,4,5,1)}")
  0043  mov          e=0x08, 03, pool[01] ; r3 = pool[1]
  ...
  L7:  println(f"min: {math.min([66,1,7,9])}")               # <-- L7 again
  005b  call         e=0x50, 04, -7, 05 ; r4 = native6(r5, r6, r7, r8, r9)

  L8:  println(f"min: {math.min(5,8,4,5,1)}")                # <-- duplicated
  0061  call         e=0x50, 02, 0e, 02 ; r2 = format(r3, r4)
```

The `locs:` line in `neo.qla` shows the actual per-instruction stamps — the
`native:6` CALLs carry `6:0` and `7:0` (both column 0), while the rest of
each statement carries the correct `7:1` / `8:1`:

```
locs: ... 7:1 7:1 6:0 7:1 7:1 8:1 8:1 8:1 8:1 8:1 8:1 7:0 8:1 8:1 ...
```

Minimal repro without any f-string (`/tmp/opencode/mc.ql`):

```ql
import math

local a = math.min(66,1,7)
print(a)
```

```
locs: 3:1 3:1 3:1 0:0 4:1 4:1 4:1
                  ^^^^ the math.min CALL has loc 0:0
```

So the defect is not the f-string path — the CALL itself is stamped `(0, 0)`.

## Root cause

`QLangParser::parsePostfix()` (src/QLangParser.cpp:1401–1418) builds the
`Call` node for `obj.method(args)` but never assigns `callLine`/`callCol`,
which default to `0` (include/qlang/QLangParser.h:65–66):

```cpp
// src/QLangParser.cpp:1405-1409 — no callLine/callCol here:
auto node = std::make_shared<Expr>();
node->kind = ExprKind::Call;
node->callee = e->memberName;
node->calleeMember = e;
```

Every other `Call` construction site sets them from the callee token, e.g.
`node->callLine = tok.line; node->callCol = tok.col;` at
src/QLangParser.cpp:1554–1557 (identifier call), 1639–1642 (`type()`),
1758–1761 (`assert()`), 1781–1784 (`isset()`).

The compiler then propagates the unset pair onto the emitted CALL for
runtime error reporting (src/QLangCompiler.cpp:5795–5796 for the
module-member native path, and 6295–6296 generally):

```cpp
m_emitter.setLastLoc(static_cast<uint16_t>(e->callLine),
                     static_cast<uint16_t>(e->callCol));
```

Inside an f-string the effect is worse than `(0,0)`: the interpolation is
parsed by a fresh sub-parser starting at line 1 and re-based with
`shiftExprLines(*innerExpr, tok.line - 1)` (src/QLangParser.cpp:1872),
whose `Call` case is `e.callLine += delta` (line 127). So `0 + (7 - 1) = 6`
— the CALL looks stamped with the line *above* the statement, with col 0.

The confusing listing (stray/duplicated `L7:`/`L8:` headers) is
downstream, not a separate bug: `qlcc::disassemble` prints a source header
whenever the loc *changes* (`insLine != lastLine`,
examples/qlcc.cpp:1360–1365), so one statement holding both `7:1` and
`6:0` instructions toggles the header back and forth. Once the CALL carries
the right loc, the listing groups correctly with no qlcc change.

## Suggested fix

In `parsePostfix()`, stamp the Call node from the member-name token (or the
`'('` token, matching the other sites):

```cpp
node->callee = e->memberName;
node->calleeMember = e;
node->callLine = e->line ? e->line : tok_line_of_name;  // see note
node->callCol  = ...;
```

The Member node `e` has no line field today, so the cheapest correct stamp
is to capture the member-name token's position when the Dot/ScopeRes branch
consumes it (src/QLangParser.cpp:1385–1394, `nameTok`), e.g. stash it on the
Member node (`node->memberLine = nameTok.line; node->memberCol =
nameTok.col;`) and copy it into the Call node here — or thread it through a
local in `parsePostfix` if the member is the outermost postfix base.

## Verification

After the fix, re-run:

- `build/qlcc -a --asm-level 4 tests/scripts/neo.ql` — no `L6:` header, no
  duplicated `L7:`/`L8:` headers, `locs:` shows `7:1 ... 7:c` and
  `8:1 ... 8:c` with no `0:0`/`6:0`/`7:0`.
- `build/qlcc -a --asm-level 4 /tmp/opencode/mc.ql` — the CALL's loc is
  `3:<col>`, not `0:0`.
- `ctest --test-dir build` (spec suites covering locs/`.qlc` round-trip).
