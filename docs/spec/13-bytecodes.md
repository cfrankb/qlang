# bytecodes

## disassembler

IMPORTANT: every time a change is made to the opcodes or bytecode encoding, always update the disassembler: `examples/qlang_run.cpp`.


## Instruction operand map (8-byte layout)

**Historical — the 8-byte struct was deleted in Round 37 (`.qlc` v5).** The instructions are
now a flat byte stream decoded by `include/qlang/QLangBytecode.h` / `src/QLangBytecode.cpp`
(see [../plans/archive/instruction-format-escape-byte-plan.md](../plans/archive/instruction-format-escape-byte-plan.md)
and `## encoding extended` below). What survives is the **operand roles**: `a`/`b`/`c` below
name the logical operands of each opcode, which is what the disassembler prints and what the
tables in this document are written against. The `i32` column is dead — 32-bit parameters are
now ordinary operands whose width is selected by the modifier byte.

See also [../plans/archive/qlc-codec-plan.md](../plans/archive/qlc-codec-plan.md).

```cpp
struct Instruction {
  Opcode op = Opcode::CONST; // uint16_t
  int16_t a;
  union {
    struct {
      int16_t b;
      int16_t c;
    };
    int32_t i32 = 0; // combined b/c for opcodes with a 32-bit parameter
  };
};
```

Note: `b`/`c` are the two 16-bit halves of `i32`; they are declared as an
anonymous struct inside the union (bare `int16_t b, c` members of a union
would alias the same storage).

(pre-v5) 8 bytes, alignment 4, no padding. Rule: any opcode that needed a 32-bit
parameter used `i32`; all others used `a`/`b`/`c` as `int16_t`. The former
`usesI32(Opcode)` source of truth is deleted — the width of every operand is
now selected by the modifier byte (`### bytecode encoding`). The opcodes that
once used the `i32` slot are `JMP`, `JMPIFFALSE`, `JMPIFTRUE`, `CONTINUE`,
`BREAK` (pc targets) and `MOVI`/`ADDI` (32-bit integer literal) — four of
those (`JMPIFFALSE`, `JMPIFTRUE`, `MOVI`, `ADDI`) were retired in `.qlc` v7.

Source location (line/col) is NOT in the instruction: it lives in the
parallel `Function::loc` side-table (`std::vector<std::pair<uint16_t,
uint16_t>>`, one entry per instruction, read only on the error path).

Conventions:

- `CALL` operand `b`: builtin id (`>= 0`) or native slot (negative;
  `slot = -1 - b`).
- Call family (`CALL`, `CALLFN`, `CALLCLOS`): arguments live in
  `r[a+1 .. a+c]`; result in `r[a]`.
- Unused operands are don't-cares (written 0 by the compiler).

| Opcode | a | b | c | i32 | Notes |
|--------|---|---|---|-----|-------|
| MOV (kind 2) | dest reg | pool index | — | | was `CONST`; the pool load is `mov` with right-operand kind 2 |
| MOV | dest reg | src reg | — | | |
| CONV | dest reg | src reg | type id | | |
| NEG | dest reg | src reg | — | | |
| INC | dest reg | — | — | | |
| DEC | dest reg | — | — | | |
| ADD … INx (17 binary ops) | dest reg | left reg | right reg | | ADD SUB MUL DIV MOD AND OR XOR SHL SHR LAND LOR EQ NE LT GT LE GE POW INx |
| NOT | dest reg | src reg | — | | |
| CALL | dest / arg base | builtin id / native slot | arg count | | args in r[a+1..a+c] |
| DELETEx | reg to remove | — | — | | |
| CAST | dest reg | src reg | type id | | |
| ARRGET | dest reg | array reg | index reg | | |
| ARRPUT | array reg | index reg | value reg | | |
| ARRAPP | array reg | — | element reg | | b unused |
| ARRAPPV | array reg | extend-array reg | — | | |
| ARRLEN | dest reg | array reg | — | | |
| LEN | dest reg | value reg | — | | string/array/table/blob |
| SEQGET | dest reg | collection reg | index reg | | array[i] or i-th table key |
| TABCREATE | dest reg | — | — | | |
| TABGET | dest reg | table reg | key reg | | |
| TABPUT | table reg | key reg | value reg | | |
| TABFINALIZE | instance reg | — | — | | install `__destroy__` deleter |
| INDGET | dest reg | table reg | key reg | | **reserved** (enum placeholder) — `$$` indirect read, not yet implemented |
| INDPUT | table reg | key reg | value reg | | **reserved** (enum placeholder) — `$$` indirect write, not yet implemented |
| EVALKEY | dest reg | value reg | — | | `$` dynamic key stringify |
| JMP | — | — | — | pc target | |
| CONTINUE | — | — | — | pc target | loop test label |
| BREAK | — | — | — | pc target | loop exit label |
| NOP | — | — | — | | no operands, no action (identity 61) |
| CALLFN | dest reg | function index | arg count | | args in r[a+1..a+c] |
| CALLCLOS | closure / dest reg | — | arg count | | b unused |
| EMIT | object reg | signal-name const index | arg count | | identity 62; args in r[a+1..a+c] |
| RETURN | result reg | — | — | | |
| MAKEFUNC | dest reg | closure const index | — | | |
| MAKEGEN | dest reg | function const index | — | | |
| REFLOAD | dest reg | capture table index | — | | |
| REFSTORE | value reg | capture table index | — | | |
| YIELD | generator value reg | yield value reg | generator reg | | |
| CMP | left reg | right reg | — | | flags = `cmp(r[a], r[b])`; independent GT/EQ/LT bits, wipes prior flags |
| JE | — | — | — | pc target | jump when flags == EQ (`a == b`) |
| JNE | — | — | — | pc target | jump when EQ not set (`a != b`) |
| JG | — | — | — | pc target | jump when GT only (`a > b`) |
| JGE | — | — | — | pc target | jump when GT or EQ (`a >= b`) |
| JL | — | — | — | pc target | jump when LT only (`a < b`) |
| JLE | — | — | — | pc target | jump when LT or EQ (`a <= b`) |
| DEEPCOPY | dest reg | src reg | — | | `r[a] = deepcopy(r[b])` |
| INDEXGET | dest reg | base reg | key reg | | `r[a] = r[b][r[c]]`; array/table/object subscript |
| INDEXPUT | base reg | key reg | value reg | | `r[a][r[b]] = r[c]`; array/table/object subscript |
| EQF | left reg | right reg | — | | identity 66; `flags = (r[a] == right) ? EQ : none`; general types; right may be immediate/register/const |
| LTF | left reg | right reg | — | | identity 67; `flags = (r[a] < right) ? LT : none` |
| GTF | left reg | right reg | — | | identity 68; `flags = (r[a] > right) ? GR : none` |
| LTEF | left reg | right reg | — | | identity 69; `flags = (r[a] < right) ? LT : (r[a] == right) ? EQ : none` (one-hot) |
| GTEF | left reg | right reg | — | | identity 70; `flags = (r[a] > right) ? GR : (r[a] == right) ? EQ : none` (one-hot) |
| TST | test reg | — | — | | identity 71; `flags = truth(r[a]) ? EQ : none`; register operand only |
| GETIMPORT | dest reg | import index | — | | identity 72; `r[a] = <imports[b].ref>`; 2-operand form — `b` names the import table, not a register |
| CALLIMPORT | dest / arg base | import index | arg count | | identity 73; `r[a] = call imports[b](r[a+1 .. a+c])`; same shape as CALLFN, `b` names the import table |

## opcode: MOVI

> **Removed in `.qlc` v7** (Phase 3, Round 60). `movi`/`movc` are the `mov`
> immediate and const-pool **kinds** since Round 56; the identities, their codec
> rows, their mnemonics and their VM handlers are gone — a v7 stream cannot
> contain them and a v6 file is rejected by the version field. The sections
> below are the historical record of the v2–v5 spellings.

**Implemented (Round 30, 2026-10-29):** `Opcode::MOVI` appended last in the
enum (existing `.qlc` opcode numbers unchanged), `usesI32()` covers it, the
VM dispatches it in all four exec loops, the compiler emits it for `Type::Int
` literals (expressions, `local` declarations, plain assignments — the
latter two inlined into the target register, saving the temp + `mov`), and
the `.qlc` writer emits format version 2 (readers accept ≤ 2).

**Step 4.4 (2026-10-30):** the compiler-generated `Type::Int` constants also
compile to `MOVI` — the `for`-loop unit/zero/direction counters, the `for-in`
index init, the class `__static_const__`/`__const_fields__` markers, the
per-field `__fields__` indices, and the `ab[]` append index. After this the
constant pool holds no `Type::Int` entries at all (user literals were already
`MOVI` in Round 30); instruction count is unchanged (1:1).

if the constant fits in a 4 bytes (32bits) integer, it should be encoded as `movi` (immediate) thus not requiring extra storage in the constant pool.  otherwise,  the opcode will be `movc` (CONST), the  constant is stored in the constant pool and the index is stored in the bytecode (current behavior)

for `movi`. the target register is stored in `Instruction.a` and the immediate 32bit value is stored in `Instruction.i32`.


sample decompiler output — refreshed to `.qlc` v8 (Round 62): the `constants:`
list is the flat **module pool** (one per program, printed above `function #0`),
and the pool load is `mov` with right-operand kind 2 (`e=0x08`); `0` is an
immediate (`e=0x40`), so neither `0` nor `nil` occupies a pool slot any more
(`tests/scripts/len.ql`, `--asm-level 4`):
```
constants:
    [0] "%s is %s byte"

function #0 'main'
  params: 0  registers: 9
  code:

  L1:  local a:byte = 0
  0000  mov          e=0x40, 00, 0 ; r0 = 0
  0004  conv         00, 00, 03 ; r0 = convert(r0, byte)      # type:byte

  L2:  println(f"{type(a)} is {len(a)} byte")
  0008  mov          e=0x08, 03, pool[00] ; r3 = pool[0]      # "%s is %s byte"
```

proposal

```
  old encoding

  0000  movc         01, 00     ; r1 = pool[0]                # 0
  0001  mov          00, 01     ; r0 = r1
  0002  movc         01, 01     ; r1 = pool[1]                # nil

  new encoding

  0000  movi         00, 0      ; r0 = i32(0)

````
net saving. -1 constant in pool. -2 Instructions.

**Deprecated by `## encoding extended` (2026-11-06); removed in `.qlc` v7 (Round 60).**
`movi`/`movc` are the same instruction
as `mov` with a right-operand *kind* of immediate / constant pool: the `i`/`c` suffix is folded
into the modifier byte and the separate identities are gone. The
"fits in 32 bits ⇒ immediate, otherwise ⇒ pool" rule survives unchanged — it becomes the
compiler's immediate-form selection (`### bytecode encoding`).


## OpCode addi

> **Removed in `.qlc` v7** (Phase 3, Round 60) — `addi` is `add` with an immediate
> right-operand kind; historical record below.

**Implemented (Round 31, 2026-10-29):** `Opcode::ADDI` appended last in the
enum (existing `.qlc` opcode numbers unchanged), `usesI32()` covers it, the
VM routes it through the ADD binary path in all four exec loops
(`frame.regs[a] = doBinary(ADD, frame.regs[a], Int(i32))`), and the compiler
emits it for compound `+=` with a `Type::Int` literal RHS — plain
(`a += 12`), captured variable (`cap += 12`), index (`a[i] += 12`) and
member (`t.k += 12`) targets — saving the temp + `movi` (−1 instruction,
−1 temp). Non-Int RHS (float/string) stays on the pool + ADD path. `.qlc`
format version stays 2 (ADDI ships in the same uncommitted v2 as MOVI).

OpCode `addi` adds the value of `i32` to register indexed by `a`

original bytecodes
```
L 9:  a += 12                      # compound assignment -> 44
  000e  movi         06, 12i    ; r6 = 12                          
  000f  add          00, 00, 06 ; r0 = r0 + r6                     
  0010  movc         06, 01     ; r6 = pool[1]          # nil
```

proposed encoding
```
  000e  addi         00, 12i    ; r0 =  r0 + 12
```

net savings: -2 instructions

##  OpCode: subi

> **Removed in `.qlc` v7** (Phase 3, Round 60) — `subi` is `sub` with an immediate
> right-operand kind; historical record below.

opcode `subi`

```
  L26:    n = n - 1
  0056  mov          0d, 06     ; r13 = r6
  0057  movi         0e, 1i     ; r14 = 1
  0058  sub          0c, 0d, 0e ; r12 = r13 - r14
  0059  mov          06, 0c     ; r6 = r12
```

```
  L26:    n = n - 1
  0056  subi         06, 1i     ; r6 = r6 - 1
```

net savings: -3 instructions

`addi`/`subi` are likewise folded into `add`/`sub` with an immediate right operand — the
in-place semantics (`r[a] = r[a] op imm`) are exactly what the non-register kinds mean
(`### bytecode encoding`). **Removed in `.qlc` v7** (Round 60), same as `movi`.

## alternate conditional jump OpCode

jump OpCode: `je`, `jl`, `jle`, `jg`, `jge`, `jne` (+ the `ja`/`jae`/`jb`/`jbe` aliases)
comparator: `cmp`, and the flag-setting `eqf`, `ltf`, `gtf`, `ltef`, `gtef`, `tst`
(`### extended OpCode eq, lt, gt, lte, gte`). `cmpi` is `cmp` with an immediate right operand
(`### bytecode encoding`), not a separate opcode in the extended encoding — its identity was
removed in `.qlc` v7 (Round 60).

### shared registers

The frame carries three comparison flags — `CMP_GR`, `CMP_EQ`, `CMP_LT`
(`include/qlang/QLangVM.h`). They are the "shared registers" referred to throughout this
document; the implementation calls them *flags*, and they live in the frame, so a nested call
starts from a clean state.

|shared register|relation it records|
|-|-|
|gr|greater than|
|eq|equal|
|lt|lesser than|

**Flag model (normative).** Each flag is an *independent relation predicate*. A comparator
wipes the three bits and then sets exactly the bits whose relation holds:
`gr = (a > b)`, `eq = (a == b)`, `lt = (a < b)`. Because the three relations are mutually
exclusive, **at most one bit is ever set** — and it can be *none* of them, which means "this
comparator's relation does not hold" (see the `*f` family below). `cmp` compares a total
order, so after `cmp` **exactly one** bit is set.

The `a >= b`, `a <= b`, `a != b` entries in the jump table below are *jump conditions*, not
flag states — they are how the `j*` opcodes read the (single) set bit. The implemented
predicates are (`QLangVM::flagsSatisfied`, `src/QLangVM.cpp`):

|jump|predicate on the flag bits `f`|
|-|-|
|`je`|`f == eq`|
|`jne`|`(f & eq) == 0`|
|`jg`|`f == gr`|
|`jge`|`(f & (gr \| eq)) != 0`|
|`jl`|`f == lt`|
|`jle`|`(f & (lt \| eq)) != 0`|


`cmp` can only be used if both operands are `int`; with an immediate right operand the
immediate is always `int`. The general (non-`int`) relations are the `*f` family below.

|opCode|relation|gr|eq|lt|
|-|-|-|-|-|
|cmp a,b|a > b|1|-|-|
|cmp a,b|a == b|-|1|-|
|cmp a,b|a < b|-|-|1|
|cmp a,imm|a > imm|1|-|-|
|cmp a,imm|a == imm|-|1|-|
|cmp a,imm|a < imm|-|-|1|

|opCode|description|jumps when|meaning|
|-|-|-|-|
|jg|jump greater|gr|a > b|
|jge|jump greater equal|gr or eq|a >= b|
|jl|jump lesser|lt|a < b|
|jle|jump lesser equal|lt or eq|a <= b|
|je|jump equal|eq|a == b|
|jne|jump not equal|eq not set|a != b|
|ja|jump above|gr|alias of `jg`|
|jae|jump above equal|gr or eq|alias of `jge`|
|jb|jump below|lt|alias of `jl`|
|jbe|jump below equal|lt or eq|alias of `jle`|

`ja`/`jae`/`jb`/`jbe` are **assembler-level aliases only**: the flags record the relation the
comparator computed, and QLang compares `int`/`unsigned`/`qword` within their own types, so
there is no unsigned-specific flag state for a distinct jump to test. They carry no distinct
encoding and no opcode identity; disassembly prints the canonical `jg`/`jge`/`jl`/`jle`. A
genuinely unsigned relation on mixed-signedness operands is deferred to a future version.

**The negative branch of a `*f` compare (normative).** A partial comparator leaves **no bit
set** when its relation does not hold, and **no jump predicate tests for the none-set state**
(`jne` is `(f & eq) == 0`, which also fires for `gr`/`lt`; `jge`/`jle` require a bit; `jg`/`jl`
require a specific one). Pairing a compare with the *same* relation and `jne` therefore takes
the branch when the condition **holds** — `ltf` + `jne` jumps on `a < b`, the opposite of the
else-path. The correct pairing is the comparator of the **dual** relation with the *same* jump
`cmp` needs, so one jump map serves both paths:

|condition|comparator|jump taken when the condition is false|
|-|-|-|
|`a == b`|`eqf`|`jne` (`f = eq` or none)|
|`a != b`|`eqf`|`je` (`f == eq`)|
|`a < b`|`gtef`|`jge` (`f = gr` \| `eq` \| none)|
|`a > b`|`ltef`|`jle` (`f = lt` \| `eq` \| none)|
|`a <= b`|`gtf`|`jg` (`f = gr` or none)|
|`a >= b`|`ltf`|`jl` (`f = lt` or none)|

The jump target is a **relative** offset (jmp8 / jmp16 / jmp32, `### bytecode encoding`), not
the pre-v5 absolute `i32` pc target.


### OpCode jg

before v7 — compare into a temporary, branch on its truthiness (2 instructions, 1 temporary):

```
0054  gt           09, 0c, 0d ; r9 = (r12 > r13)          # 4 bytes
0058  jmpfalse     09, 04     ; if !truth(r9) pc += 4     # 3 bytes, -> 005f
```

extended — flags, no temporary (2 instructions, 0 temporaries):

```
0054  gtf          0c, 0d     ; flags = cmp(r12, r13)     # 3 bytes
0057  jg           04         ; pc += 4                   # 2 bytes, -> 005d
```

### OpCode je

before v7:

```
004c  eq           06, 09, 0c ; r6 = (r9 == r12)          # 4 bytes
0050  jmpfalse     06, 04     ; if !truth(r6) pc += 4     # 3 bytes, -> 0057
```

extended:

```
004c  eqf          09, 0c     ; flags = (r9 == r12)       # 3 bytes
004f  je           04         ; pc += 4                   # 2 bytes, -> 0055
```

### OpCode jl

before v7:

```
0068  lt           0e, 09, 0d ; r14 = (r9 < r13)          # 4 bytes
006c  jmpfalse     0e, 04     ; if !truth(r14) pc += 4    # 3 bytes, -> 0073
```

extended:

```
0068  ltf          09, 0d     ; flags = cmp(r9, r13)      # 3 bytes
006b  jl           04         ; pc += 4                   # 2 bytes, -> 0071
```

### OpCode tst (replaces jmpfalse / jmptrue)

`jmpfalse`/`jmptrue` branch on the *truthiness* of a register, which the flags cannot express
on their own. `tst` bridges the gap: it sets `eq` from `truth(r[a])` and clears the other two
bits, so the flag jumps become reachable from any value.

```
v5/v6
0040  jmptrue      06, 04     ; if truth(r6) pc += 4      # 3 bytes

v7
0040  tst          06         ; flags = truth(r6) ? eq : 0 # 2 bytes
0042  je           04         ; pc += 4                   # 2 bytes, -> 0046
```

The pair is one byte longer for a bare truthiness test, which is why the compiler kept
`jmpfalse`/`jmptrue` there while both spellings decoded; it is a win whenever the condition is
already a comparison (`eqf` + `jne` replaces `eq` + `jmpfalse` **and** frees a register) or when
the flags feed a second branch.

**Status (Round 60):** `tst` is **emitted** — identity 71, codec tables, VM handler
(`flags = truth(r[a]) ? EQ : none`) and disassembly. Bare truthiness tests (`if x`, `while x`,
`&&`/`||`) lower to `tst` + `je`/`jne` (Phase 3 step 1). `jmpfalse`/`jmptrue` were **removed**
with `.qlc` v7 (Phase 3 step 3): their identities, codec rows, mnemonics and VM handlers are
gone, so the `v5/v6` listing above is the historical record only and a v7 stream can contain
only the `v7` pair.

### OpCode getimport / callimport (cross-module access)

Both name an entry of the program's **import table** (`## import table (lookup)`) instead of a
register, a pool entry or a function, which is the whole point: an operand that names a table
entry reaches another module's entity from any depth, while a register operand can only name a
cell of the module that owns the frame.

```
getimport      06, 01        ; r6 = <imports[1]>                        # imported var/const cell
callimport     06, 02, 02   ; r6 = call imports[2](r7, r8)              # dest a, import b, argCount c
```

- `GETIMPORT` is the 2-operand base form (dest, import index). Its `ref` is the **defining**
  module's shared cell (`## technical specifications` 3), read out of the global table rather
  than the executing frame. An entry naming a function is refused at run time — reading a
  function is a compile error (`'x' is a function imported from module 'm': call it instead of
  reading it`).
- `CALLIMPORT` has `CALLFN`'s shape (dest `a`, index `b`, arg count `c`). A plain function is
  called with the callee's own capture spec; an exported **generator** yields a generator value
  built from the imported function, so no `makegen` constant is needed for the cross-module
  case.
- Operand `b` is register-class for addressing (the modifier byte only selects its width) but
  its **role** is `ImportIndex`, which is what lets the reader range-check it against the
  image's own import table and the linker remap it.

**Status (Round 65):** both are **implemented and emitted** — identities 72/73
(`kOpcodeUsedCount` 72 → 74, plan **Q31**), codec tables, mnemonics, VM handlers, and the
load-time `import index out of range` check. What each means depends on whether the program was
linked:

| program | `getimport` | `callimport` |
|-|-|-|
| merged (linker output) | survives; `b` is the merged table's index and `ref` the merged cell | **never present** — the linker resolves it to `callfn`, or to `makegen` with a synthesized pool closure for an exported generator |
| module image run as-is | reads the cell its own table names | calls the function its own table names — the form a runtime module registry executes |

A merged program therefore contains no instruction that names the import table for a *call*,
but it does keep the reads; a module image keeps both, which is why a library may import another
module and still serialize.

OpCode `jge` and `jle` work exactly like `jg` and `jl`; `jne` is the complement of `je`.


## opCode divi

```
  L87:  local x = 1024 / 10
  00b2  movi         0e, 1024i  ; r14 = 1024                       
  00b3  movi         0f, 10i    ; r15 = 10                         
  00b4  div          0d, 0e, 0f ; r13 = r14 / r15                  
```
```
  L87:  local x = 1024 / 10
  00b2  movi         0d, 1024i  ; r13 = 1024                       
  00b3  divi         0d, 10i    ; r13 = r13 / 10
```

## OpCode: swap

this OpCode will swap register[Instruction.a] and register[Instruction.b]

purpose: compiler to use this opCode tp prevent/minimize fragmentation of the register pool.

```
pseudo code:

t = a
a = b
b = t
```

don't allocate a new register for `t`
use a global scratch pad


## OpCode nop 

this OpCode takes no action


## OpCode deepcopy

Implemented 2026-11-05 (Phase 1 of `docs/plans/deepcopy-plan.md`).

`deepcopy a, b` — `r[a] = deep copy of r[b]`. two register operands, no i32
slot (same shape as `mov`). appended last in the opcode enum so existing `.qlc`
opcode numbers stay valid.

the compiler lowers a `deepcopy(x)` call **directly** to this opcode: the
argument is compiled into `b` and the result is written straight into the
destination register. no `call`, no argument staging in `r[a+1..a+c]` — the
encoding guidelines ask for copying directly to the final register and for
avoiding temporaries.

the copy semantics (what is copied, what stays shared, how cycles behave) are
defined in [06-functions.md](06-functions.md) `### deepcopy`.



## OpCode indexget

Implemented 2026-11-05. Closes the table-subscript gap recorded in
[../compliance/status.md](../compliance/status.md): the language allows a
subscript on a table ([03-declarations.md](03-declarations.md):
`t["age"] = 33  # valid; equivalent to t.age`), but the compiler lowered every
postfix `[...]` to `arrget`, which rejects a non-array base.

`indexget a, b, c` — `r[a] = r[b][r[c]]`. three register operands, no `i32`
slot (the `baseOperandCount()` default). appended last in the opcode enum —
`DEEPCOPY` was 70, so this is 71 and `kOpcodeUsedCount` becomes 73 — so
existing `.qlc` opcode numbers stay valid; the `.qlc` format version was left
unchanged (v5) because no encoding rule moved. (It became v6 in Round 56, for
the modifier re-split — not for these opcodes.) *Historical identities: the v7
compaction (Round 60) moved `indexget` to 64 and `indexput` to 65, and
`kOpcodeUsedCount` to 72.*

the base register is dispatched at **runtime**:

| base of `r[b]` | behaviour |
|----------------|-----------|
| `array` | integer index; out of range → `array index out of range` |
| `table`, class instance, external object | key via `valueToString(r[c])`, read through `ObjectOps::getField` |
| anything else | `type error: cannot subscript <type> value` |

going through `ObjectOps` is what makes `t["age"]` and `t.age` the *same*
operation: `tabget` already resolves member access that way, so the subscript
form inherits the closed field set and the `const` guards instead of
re-implementing them.

the compiler emits this opcode for every postfix subscript **read**
(`ExprKind::Index`), replacing the `arrget` emission. `seqget` is unaffected —
it is the `for-in` iteration opcode, not a subscript.



## OpCode indexput

`indexput a, b, c` — `r[a][r[b]] = r[c]`. same encoding and enum placement as
`indexget` (identity 72).

| base of `r[a]` | behaviour |
|----------------|-----------|
| `array` | integer index, assigned in place; out of range → `array index out of range` |
| `table`, class instance, external object | key via `valueToString(r[b])`, written through `ObjectOps::setField` (closed field set + `const` guards on class instances) |
| anything else | `type error: cannot assign into <type> value` |

the compiler emits this opcode for every postfix subscript **write**
(`compileAssignTarget`), replacing the `arrput` emission. the empty subscript
(`ab[] = v`) is unchanged: it still lowers to `arrapp`, which appends on arrays
and sets and errors on tables (`cannot append to non-array value`) — matching
`t[] = 44  # error`.

`arrget`/`arrput` are no longer emitted by the compiler. they stay in the enum
and in the VM as the single-kind forms (`.qlc` compatibility, direct use).
`tabget`/`tabput` remain in active use for member targets (`t.name = 1`) and
for the class/statics machinery.

disambiguation: these are **not** the reserved `INDGET`/`INDPUT` placeholders
(`$$` indirect access, [05-operators.md](05-operators.md) `## operator $$`),
which remain unimplemented.

the disassembler (`examples/qlang_run.cpp`) prints both with the existing
subscript mnemonics: `r5 = r3[r4]`, `r3[r4] = r5`.



## decompiler mnemonics 

DO NOT IMPLEMENT YET

```
L 1:  local a = 1
0000  movc         01, 00     ; r1 = pool[0]                           # 1
0001  mov          00, 01     ; r0 = r1                              
0002  movc         01, 01     ; r1 = pool[1]                           # nil
L 2:  a = cast<float>(a)
0003  mov          03, 00     ; r3 = r0                              
0004  cast         02, 03, 0c ; r2 = cast(r3, float)                   # type:float
0005  mov          00, 02     ; r0 = r2                              
0006  movc         02, 01     ; r2 = pool[1]                           # nil
L 3:  println(f"a = {a} type: {type(a)} len {len(a)}") 
0007  mov          0a, 00     ; r10 = r0                             
0008  mov          0d, 00     ; r13 = r0                             
0009  mov          0c, 0d     ; r12 = r13                            
000a  call         0b, 02, 01 ; r11 = type(r12)                        # builtin:type
```

proposed improvement to the decompiler

decompiler rules
- a value denoting `register` is prefixed with `r`
- a value denoting `constant` is prefixed with `c`


example

```

line 0000    movc 01, 00     -->   movc r1, c0 
line 0004    cast 02, 03, 0c -->   cast  r2, r3, <whatever 0c presents>

```

## separating line/row from bytecode

IMPLEMENTED (2026-11-03, Round 36 — `.qlc` v4; see
`docs/plans/instruction-format-optimization-plan.md` Phase 1).

```cpp
struct Instruction {          // 8 bytes
  Opcode op;                  // u16
  int16_t a;
  union { struct { int16_t b; int16_t c; }; int32_t i32 = 0; };
};
static_assert(sizeof(Instruction) == 8);

struct SourcePos { uint16_t line, col; };

// in Function:
std::vector<Instruction> instructions;
std::vector<SourcePos> positions;   // parallel, same size
```

another upside. is that line number can be scaled from uint16_t to uint32_t at no runtime cost

# external modules

external modules are modules not included in the base `qlang` system.

each external module will have an import and export tables.

example of external modules are `qlang` source files (`.ql`) compiled into module images
(`.qlc`). The module — the thing a name refers to and an `import` binds — is the *artifact*:
its identity, export table and import table are the three sections `## keyword export` and
§4.6 of [../SERIALIZATION.md](../SERIALIZATION.md) define. The source file is how it was made.

each module must be able to be compiled on it's own. when ingested by the `qlang` executor, it must be able to run (assuming they both share compatible bytecodes).

**Implemented through Phase 3c (Round 65, 2026-11-09)** — plan
[../plans/archive/external-modules-plan-v4.md](../plans/archive/external-modules-plan-v4.md): a `.ql` compiles
standalone into a `.qlc` that carries its name and both tables (**v9**), a host loads one into a
context (`QLangContext::loadProgram` / `loadProgramFile`), an `import` resolves against its export
table at compile time, and `QLangLinker` merges the images with the entry program into one
runnable program (one function table, one folded constant pool, one global register area split
into per-module slices, **one import table**, module top levels run in dependency order).
`GETIMPORT` / `CALLIMPORT` (`### OpCode getimport / callimport`) are the instructions that name
the import table, so a module image is self-describing: it can import another module and still be
written out and linked transitively. What is *not* implemented is the second link model of
`## technical specifications` 1 as a *feature* — the two opcodes run, but nothing resolves a
module by name and hands it over at run time (a runtime module registry), and nothing finds a
module by name without a host loading it first (search paths, `07-api.md`).

## export table (lookup)

includes all the global `functions`, `variables` and `constants` being exposed to other modules. these can be imported by other modules via `import` keyword.

bytecode to reference the look up table.

**Implemented (Round 63, 2026-11-09)** — plan
[../plans/archive/external-modules-plan-v4.md](../plans/archive/external-modules-plan-v4.md) Phase 3a.
The table is `CompiledProgram::exports` (`include/qlang/QLangBytecode.h`): one
`ExportEntry {kind, name, ref}` per exposed entity, in source order, where `kind`
is `Var` / `Const` / `Func` and `ref` is the handle into this program — the
function index for a `def`, the global register (the shared cell of
`## technical specifications` 3) for a variable or a constant. The compiler builds
it in a pre-pass over the top level, so the table exists before any body compiles,
and `.qlc` **v9** serializes it
([../SERIALIZATION.md](../SERIALIZATION.md) §4.6) with every `ref` range-checked at
load. `--asm-level 5` lists it.

**Consumed by the linker since Round 64 (2026-11-09)** — plan Phase 3b: a module
that `import`s from a loaded image has its names resolved against this table at
compile time, and `QLangLinker` turns each resolved entry into a handle of the
merged program. So the table is now *read* — by the compiler and the linker.

*bytecode to reference the look up table* is **indirect**, and that is by design: an importer
never names the defining module's export entry. The compiler resolves the name at compile time
and emits `GETIMPORT` / `CALLIMPORT` naming an entry of the **importer's own** import table
(`### OpCode getimport / callimport`), whose `ref` is the export's handle. A link then turns that
entry into a merged handle and rewrites `callimport` away entirely, so a merged program contains
no instruction naming either table for a call — only `getimport` reads survive. A module image
executed without merging keeps both, which is what the runtime-registry model of
`## technical specifications` 1 needs.

## import table (lookup)

this table will contain all the  global `functions`, `variables` and `constants` being imported from other modules.

bytecode to reference the look up table.

**Implemented (Round 64, 2026-11-09)** — plan
[../plans/archive/external-modules-plan-v4.md](../plans/archive/external-modules-plan-v4.md) Phase 3a (the
section) + Phase 3b (what fills it).
`CompiledProgram::imports` holds one `ImportEntry {module, name, kind, ref, alias}`
per entity this module needs from another; `alias` is the local name when the import is
aliased — reserved, since the parser accepts the alias only on a module import
(`import m as a`, which binds the qualifier and stores no entry here), so an entry's `alias` is
always empty today and `from m import f as g` does not parse. `.qlc` v9 serializes and validates the
section (§4.6), so a module image states what it needs.

The compiler fills it when an `import` names a **loaded** `.ql` module
(`QLangContext::loadProgram` / `loadProgramFile`, `docs/SERIALIZATION.md` §5.2): the five
spec import forms resolve against that image's export table, a name in both a C++ module
and a `.ql` module is a compile error, and so is an unknown module, an unknown export, a
write through an import or an import cycle. C++-registered modules
([09-modules.md](09-modules.md)) keep resolving the way they always did — compile-time
natives in the negative-slot space of `CALL` — so they never appear here.

`import m` (a module import, [09-modules.md](09-modules.md) `## import statement`) fills the
table the same way as `from m import *`: one entry per export the module offers, keyed by the
**exported name**, so the linker dedups it against every other importer of the same cell. Only
the *local binding* differs — the compiler binds the entry under the qualified key `m.name` and
resolves a call of `m.name` through it, which is why a module import still runs the module's
`<init>` even when the program uses none of its exports.

`ref` is the **defining** module's handle (its function index, or its cell in that
module's own slice), because the importing image cannot name a merged one — it does not
know what else will be linked in. That is also why the reader does **not** range-check an
import entry's `ref` against the image carrying it ([../SERIALIZATION.md](../SERIALIZATION.md)
§4.6): the tables it names are in another file. `QLangLinker` merges every module's table into
one — deduped by (defining module, exported name), so two importers of one export share one cell
(`## technical specifications` 3) — and rewrites each `ref` into the merged handle. After a link
`ref` is merged; before one it is module-local.

*bytecode to reference the look up table* **is** implemented (Round 65, 2026-11-09):
`GETIMPORT` (identity 72) reads an entry's cell and `CALLIMPORT` (identity 73) calls the
function one names — `### OpCode getimport / callimport`. Operand `b` is the table index, its
role is `ImportIndex`, and the reader rejects one outside the image's own table
(`import index out of range`). A link remaps the index in every surviving `getimport` and
resolves every `callimport` into `callfn` / `makegen`, so the merged program's table describes
the merged program.

## table for build in function (qlang base)

**Implemented (Round 61, 2026-11-09)** — plan
[../plans/archive/external-modules-plan-v4.md](../plans/archive/external-modules-plan-v4.md) Phase 1.

one look up table is created from the `qlang` build in functions, in the library
(`k_builtin_names[]` in `src/QLangBytecode.cpp`, declared in
`include/qlang/QLangBytecode.h`). it is indexed by the `Builtin` enum order
(`toStr` = 0 … `deepcopy` = 21), one name per id; a `static_assert` fails the
build when the enum and the table drift apart.

| api | answer |
|-|-|
| `builtinCount()` | number of ids — valid ids are `[0, builtinCount())` |
| `builtinName(id)` | the canonical name, or `nullptr` when `id` is out of range |
| `findBuiltin(name)` | the id, or `-1` when the name is not a builtin |
| `builtinId(name)` | the id, or throws `QLangError` when the name is not a builtin |

`findBuiltin` is the **classification** answer: what the compiler wants when it
decides *what kind of callee* a name is, where a miss is normal (a native C++
function takes the negative-slot branch of `CALL` operand `b`, an unknown name
raises its own `unknown function '<name>'` compile error). `builtinId` is the
**resolution** answer for a caller that already knows the name is a builtin,
where a miss is a bug.

the canonical spellings are the ones `builtinName` returns (`toStr`, `toInt`);
both lookups additionally accept `str` and `int`, the short spellings of the two
converters ([06-functions.md](06-functions.md) `## build-in functions`).

**a library table, not a `.qlc` section.** `## technical specifications` 6 makes
compatible bytecode the output of the exact same compiler version, so within a
compatible pair the names cannot drift from the ids, and a serialized table
would be a section no reader could use differently. Revisit if the compatibility
rule is ever relaxed.

consumers: the compiler (name classification and `CALL` operand `b`), the
disassembler (`examples/qlang_run.cpp` — its hardcoded name switch is gone, so
both the `# builtin:<name>` note and the pseudo-mnemonic come from this table)
and any future tool. the VM's `callBuiltin` dispatch stays a `switch` on the
ids: it never needs a name.

guard: `tests/unit/builtin_table_test.cpp` (`builtin_table`) pins id → name
against an independent list, the round trip through both lookups, the aliases,
the `-1`/throw split, the ids the compiler actually emits, and the negative-slot
native path.

## unified constant table

all the functions within a module must share a common const table or at the very least must not duplicate constants.

**Implemented (Round 62, 2026-11-09)** — plan
[../plans/archive/external-modules-plan-v4.md](../plans/archive/external-modules-plan-v4.md) Phase 2.
The pool is `CompiledProgram::constants` (`include/qlang/QLangBytecode.h`): one table per
program that every function's const-pool operands index into — `Function::constants` is
deleted. The compiler owns the table and dedups by value across the whole module (one
`valueKey` map, no per-function pools), so two `def`s using the same literal share one
entry (`## constant pool`). `.qlc` v8 serializes it as the module-level pool section
([../SERIALIZATION.md](../SERIALIZATION.md) §4.2); the reader range-checks every
const-pool operand against it.

## keyword export

`export` keyword exposes functions, variables and constants to other modules.

- export keyword is only valid at global scope.
- exported entities are `readonly`.
- functions can be executed but they not modified.
- variables can be read but not modified
- entities are shared and passed by pointer or reference.

``` 
  # assume module global scope 

  # expose MODULE_NAME and greeter so they can be imported

  export const MODULE_NAME = "my_module"

  export def greeter(name) {
    return f"hello {name}"
  }

  AUTHOR = "cfrankb";
  export AUTHOR;
  export AUTHOR; # trigger an error: already exported

```

**Implemented (Round 63, 2026-11-09)** — plan
[../plans/archive/external-modules-plan-v4.md](../plans/archive/external-modules-plan-v4.md) Phase 3a. Both forms
parse: the **prefix** form (`export def`, `export const`, `export` before a variable assignment or
an arrow closure) marks the declaration it prefixes, and the **standalone** form (`export AUTHOR;`)
names a global declared earlier *or later* in the file — the table is built in a pre-pass over the
top level, before any body compiles.

How the bullets above are read (plan **Q30** settles the second and fourth):

- *only valid at global scope* — enforced twice: the parser rejects the keyword inside a `def`, an
  arrow closure or a `class` body, and the compiler's export pre-pass rejects it inside a
  global-scope `if`/`for`/`while`/`switch` block, where it would otherwise be compiled and silently
  dropped from the table.
- *exported entities are `readonly`* / *variables can be read but not modified* — **`readonly` is a
  property of the importer, not of the defining module.** The module that declares a name keeps
  writing to it: `## technical specifications` 3 makes the cell shared and write-through, and a cell
  no one may write is a constant, not a variable. What the defining module may not do is *rebind* a
  name exported as a `def` — the table holds its function index, so `f = 2` after `export def f()`
  would silently detach the export; assignment to and `++`/`--` of an exported function name are
  compile errors. Importer-side `readonly` is enforced **since Round 64 (2026-11-09)**, plan
  Phase 3b: a name bound from a loaded `.ql` module is not a variable of the importing module, so
  assigning to it, `++`/`--` on it and `delete` of it are compile errors (`guardNotImported`), and
  a closure may not capture it — reading it is what the import is for.
- *functions can be executed but they not modified* — the entry is the function index and it is
  fixed for the life of the image.
- *entities are shared and passed by pointer or reference* — `ExportEntry::ref` is the global
  register (the shared cell) for a variable or a constant, the function index for a `def`.

**Module identity** (plan **Q29**): the name is the value of the module's own
`export const MODULE_NAME = "<name>"` when it declares one — a `const` holding a constant string,
because a loader has to read the identity out of the image without running the module — else the
name the driver supplies (`qlang_run` passes the source file stem), else `main`. A **library**
compile (`QLangCompiler::setLibraryMode`) makes `MODULE_NAME` mandatory and compiles the top level
as `<init>` rather than `main` (`## technical specifications` 2, "no main function"); an application
module may omit it, and its `export` statements change only the table, not the run.

Guard: `tests/unit/export_test.cpp` (`export`, 88 checks) — both forms and their kinds, forward
references, the ten scope rejections (five inside a `def`/closure/`class`, five inside a
global-scope block), `already exported`, the exported-function rebind guard, `MODULE_NAME`
resolution, library mode, and the table through the `.qlc` v9 round trip. The importer side
(resolution against a loaded export table, the write-through-import guards, the merge, the two
opcodes and the run) is guarded by `tests/unit/linker_test.cpp` (`linker`, 130 checks).

## technical specifications

Status of each item as of Round 65 (2026-11-09) — plan
[../plans/archive/external-modules-plan-v4.md](../plans/archive/external-modules-plan-v4.md) Phases 1–3c:

1. **Link model** — both `link-time merge` and `runtime module registry`. should share a reusable solution
   — ◐ **merge implemented (Round 64, Phase 3b)**: `QLangLinker`
   (`include/qlang/QLangLinker.h`) merges the loaded images an entry imports into one program.
   The reusable units the registry needs are in place and are what the merge consumes — the
   tables (`CompiledProgram::exports`/`imports`), the registry (`QLangContext::loadProgram`,
   `loadedModule`, `loadedModules`) and resolution against the export table (compile time). Since
   Round 65 the two opcodes a module needs to run unmerged exist and are implemented in the VM
   (`### OpCode getimport / callimport`), so the registry's *execution* half is no longer missing
   — what is missing is the feature around it: nothing resolves a module by name at run time and
   hands its image over.
2. **Entry point** — global scope script only at this point. no main function. single file qlang modules in this version only.
   — ✓ **implemented (Round 63/64)**: a library compiles its top level as `<init>`
   (`QLangCompiler::setLibraryMode`), and a merged program runs every module's `<init>` exactly
   once, dependencies first, before the entry's (item 3's order, `CompiledProgram::initSequence`).
   Single-file modules only, as specified.
3. **Cross-module variable semantics** — shared cell (write-through, recommended)
   — ✓ **implemented (Round 64/65)**: one global register area per merged program, sliced per
   module, so an exported variable is one cell the defining module writes and importers read
   (`readonly` binds the importer — `## keyword export`). The defining module's writes are
   visible to the importer; a write *through* an import is a compile error. Since Round 65 the
   read is `getimport`, which names the cell through the table rather than a register, so it
   works at any depth — including inside a function of the importing module, whose registers are
   private.
4. **Export syntax** — see keyword export
   — ✓ **implemented (Round 63)**.
5. **Import graph rules**

  C++ api can import `.ql modules` into a context.
  `.ql modules` may import `C++ modules`.
  `.ql modules` may import `.ql modules`. 
  C++ modules cannot load `.ql modules` at this stage. it could be a feature later.
  
  recursivity check is needed to prevent a module from importing modules that import it. avoid endless cycles.

  aliases is already a requirment. (e.g. `from m import f as g`)

  — ✓ **implemented (Round 64/65)**: the C++ API loads `.ql` images into a context and a `.ql`
  imports from them (all five import forms, aliases included), a `.ql` keeps importing C++
  modules (compile-time natives, unchanged), C++ modules still cannot load `.ql`, and the cycle
  check is a compile error (`QLangCompiler::checkImportCycles`) with a second, loader-side half
  in the linker. *"`.ql` modules may import `.ql` modules"* holds for a **library** image too
  since Round 65: its cross-module references are `getimport`/`callimport` operands into its own
  import table, both of which the image carries, so a chain (app → middle → leaf) links and runs
  with one merged table. `import` is a module-level statement: inside a `def` body it is a
  compile error (`import of module '…' must appear at module top level`) — it used to be dropped
  silently, leaving the name unbound.
6. **Compatibility rule** — at this early stage, compatible bytecode is `qlang` bytecodes generated with the exact same compiler version. 
   — ✓ **implemented**: the reader is single-version (`version != kQlcVersion` → rejected), the
   builtin name table is library-side rather than serialized, and native slots store **names**
   re-resolved against the loading context.

## bytecode encoding guidelines

- favor using int immediates over constants 
- avoid temporary register when possible
- always copy directly to the final register 
- avoid register aliasing
- use a universal scratch pad register to avoid temporary
- when calling functions, compiler must favor passing objects by pointer when they are arrays, tables, sets, objects, blobs. primitives (fit in a pair of cpu registers) are passed by value. strings are passed by value.
- deep copy of arrays, sets, tables, objects must be explicit. string is the exception. it will be copied as it is immutable
- always favour shorter encoding sequences

### egregious examples 

here are some of the worse encodings in the current encoder.


```
  L26:    n = n - 1
  0048  mov          11, 0c     ; r17 = r12                        
  0049  movi         12, 1i     ; r18 = 1                          
  004a  sub          10, 11, 12 ; r16 = r17 - r18                  
  004b  mov          0c, 10     ; r12 = r16                        

better encoding

  L26:    n = n - 1
  0048  dec          0c        ; r12 = r12 - 1
```

```
  L3:  local s = "hello"            # string
  0003  movc         03, 00     ; r3 = pool[0]                       # "hello"
  0004  mov          02, 03     ; r2 = r3                          
  0005  movc         03, 01     ; r3 = pool[1]                       # nil

better encoding

  L3:  local s = "hello"            # string
  0003  movc         02, 00     ; r2 = pool[0]                       # "hello"
```


```
  L14:    sum = sum + i
  002c  mov          17, 03     ; r23 = r3                         
  002d  mov          18, 09     ; r24 = r9                         
  002e  add          16, 17, 18 ; r22 = r23 + r24                  
  002f  mov          03, 16     ; r3 = r22                         
  0030  movc         16, 01     ; r22 = pool[1]                      # nil

better encoding

  L14:    sum = sum + i
  002d  mov          16, 09     ; r22 = r9                         
  002e  add          03, 03, 16 ; r3 = r3 + r22                  

```


## encoding extended

Normative summary of the instruction encoding beyond the base (unescaped) forms. Four versions
are described in this document and it matters which one a passage refers to:

- **Shipped — `.qlc` v9** (Round 63): the v8 image **unchanged**, plus a module tail after
  `registerCount` — the module's name and its export and import tables
  (`CompiledProgram::moduleName` / `exports` / `imports`,
  [../SERIALIZATION.md](../SERIALIZATION.md) §4.6) — so an image describes a *module*, not only a
  runnable program; `functions[0]` may now be a library module's `<init>` instead of `main`.
  Implementation plan: [../plans/archive/external-modules-plan-v4.md](../plans/archive/external-modules-plan-v4.md)
  Phase 3a; see `## keyword export` and `## export table (lookup)`.
- **Layout — `.qlc` v8** (Round 62, superseded by v9): the v7 instruction layout **unchanged**; what
  moved is where the constants live — one module-level pool per program
  (`CompiledProgram::constants`) instead of a pool inside every function record, so the pool section
  sits after the function records ([../SERIALIZATION.md](../SERIALIZATION.md) §4.2) and every
  const-pool operand indexes it.
  Implementation plan: [../plans/archive/external-modules-plan-v4.md](../plans/archive/external-modules-plan-v4.md)
  Phase 2; see `## unified constant table`.
- **Layout — `.qlc` v7** (Round 60, superseded by v8): the v6 layout **unchanged**, with the retired
  identities removed — `movc`, `movi`, `addi`, `subi`, `cmpi`, `jmpfalse` and `jmptrue` no longer
  exist and the remaining identities are compacted to `0..71` (`kOpcodeUsedCount` 79 → 72). Bare
  truthiness tests lower to `tst` + `je`/`jne`. Implementation plan:
  [../plans/archive/phase3-opcode-retirement-plan.md](../plans/archive/phase3-opcode-retirement-plan.md);
  on-disk contract: [../SERIALIZATION.md](../SERIALIZATION.md) §4.
- **Layout — `.qlc` v6** (Round 56, superseded by v7): flat byte stream, `pc` = byte offset,
  opcode top bit = escape flag + one modifier byte, the modifier re-split into **2/2/4** fields,
  a right-operand **kind** selector (immediate / register / const pool), the full immediate set
  (signed, unsigned, float, bool, nil) and the `i`/`c` suffix opcodes no longer emitted.
  Implementation plan:
  [../plans/encoding-v6-and-indirect-plan.md](../plans/encoding-v6-and-indirect-plan.md);
  on-disk contract: [../SERIALIZATION.md](../SERIALIZATION.md) §4.5.
- **Retired — `.qlc` v5** (Round 37): the same flat stream, but the modifier carried two
  **nibbles** (low = left-operand form, high = right-operand form) and signed `Int` immediates
  only. Readers accept v9 only; the historical record stays in
  [../plans/archive/instruction-format-escape-byte-plan.md](../plans/archive/instruction-format-escape-byte-plan.md).

The flag-setting comparison family `eqf`/`ltf`/`gtf`/`ltef`/`gtef` is **implemented and
emitted** (milestone **M3**, Round 59 — identities 66–70 since the v7 compaction, 73–77 in v6;
comparison conditions in `if`/`while` position lower to a flag compare + flag jump, no temporary
— except when both operands are statically `Int`, where the compiler keeps `cmp` + flag jump: the
bytes are unchanged from pre-M3 and `cmp`'s total order keeps the flags reusable for a second
jump). `tst` (identity 71) is implemented **and emitted** since Round 60 (`### OpCode tst`).
`getimport`/`callimport` (identities 72/73, `kOpcodeUsedCount` 74) are implemented **and
emitted** since Round 65 (`### OpCode getimport / callimport`) — the identity allocation is
plan **Q31**, "append last, never renumber", so `DECL` takes 74 when **M4** lands.
Still unimplemented: the `$$` declaration table, frame name map + bitset and `DECL` (**M4**).

## opCode scope

The "v7 spelling" column is what the compiler emits. The retired identities (`movi`, `movc`,
`addi`, `subi`, `cmpi`, `jmpfalse`, `jmptrue`) no longer exist — they were removed in `.qlc` v7
(Round 60), so a v7/v8 stream cannot contain them; the first column names the *language* operations
they used to spell.

| opCode family | type | v7 spelling |
|---------------|------|-------------|
| mov, ~~movi, movc~~ | copy | `mov` — the right-operand *kind* selects register / immediate / const pool; `movi`, `movc` removed in v7 |
| add, ~~addi~~, addc | add | `add` — same kind selector; `addi` removed in v7, `addc` never added |
| sub, ~~subi~~, subc | sub | `sub` — same kind selector; `subi` removed in v7, `subc` never added |
| jmp | jump | `jmp` (jmp8 / jmp16 / jmp32 relative) |
| ja, jb, jae, jbe | conditional jump | assembler aliases of `jg`, `jl`, `jge`, `jle` — no distinct encoding |
| jl, jg, jle, jge | conditional jump | unchanged; test the shared-register flags |
| je, jne | conditional jump | unchanged; test the shared-register flags |
| cmp, ~~cmpi~~ | comparison (numbers only) | `cmp` — `cmpi` was `cmp` with an immediate right operand, removed in v7 |
| eq, lt, gt, lte, gte | comparison (general) | flag-setting `eqf`, `ltf`, `gtf`, `ltef`, `gtef` — emitted since Round 59 for comparison conditions in branch position, statically-`Int` operands excepted (`### extended OpCode eq, lt, gt, lte, gte`) |
| tst | truthiness test | sets `eq` from `truth(r[a])`; emitted since Round 60 — it replaced `jmpfalse` / `jmptrue`, which were removed in v7 |
| getimport, callimport | cross-module access | read an imported cell / call an imported function through the **import table**; emitted since Round 65 (`### OpCode getimport / callimport`) — a link resolves every `callimport` away and keeps the reads |

### bytecode encoding

[../plans/encoding-v6-and-indirect-plan.md](../plans/encoding-v6-and-indirect-plan.md)
(v6 layout) · [../plans/archive/phase3-opcode-retirement-plan.md](../plans/archive/phase3-opcode-retirement-plan.md)
(v7, shipped — identity removal only) ·
[../plans/archive/instruction-format-escape-byte-plan.md](../plans/archive/instruction-format-escape-byte-plan.md)
(v5, retired) · on-disk layout: [../SERIALIZATION.md](../SERIALIZATION.md) §4.5

there should be only one pool of registers and one pool of constants. extended addressing is
2 byte, default addressing is 1 byte. if OpCode top bit is not set then both left and right
operand are using 1 byte encoding

**encoding prioiries:**
- always favor fewer registers. 
- no constant in the pool (if possible)
- shorter encoding (in bytes)
- few register swapings and no aliasing

127 OpCode count limit (hard limit)

```
top bit of opCode byte is set to denote variants

opCode 1 byte (0-127) are opCode value. if the opCode top bit is set then an extra byte (e)
is added next to it.

extra byte (e):

- if opCode is jmp or a conditional jump (jmp, je, jne, jl, jle, jg, jge, continue, break)
    e is the offset-width selector, NOT the 2/2/4 layout below
    (base, no escape)  => jmp8  (1 byte)  relative address, +/-127
    e = 0              => jmp16 (2 bytes) relative address
    e = 1              => jmp32 (4 bytes) relative address

- if opCode is call (proposal / not finalized)
  this encoding is intended for situations where parameters cannot be
  contigious or moving them in sequence would require swaping or aliasing.

  bit 0-1 left operand (always a function)
     value 0 function -128 to 127      (1 byte)
     value 1 extended function         (2 bytes)
     value 2 extended function         (4 bytes, future version)
     value 3 reserved for future use
  bit 2-3 left operand (base register)
     value 0 base register 0 to 255    (1 byte)
     value 1 extended base register    (2 bytes)
     value 2 extended base register    (4 bytes, future version)
     value 3 reserved for future use
     this is register contains the return value
  bit 4-7
    parameter count (0-15)

  for each parameter                   (1 byte per parameter)
    encode delta index from base register for each subsequent parameter
    byte stores -128 to +127 (delta)

- otherwise (mov, add, sub, cmp, eqf/ltf/gtf/ltef/gtef, tst, ...) e is read as three fields:

  bit 0-1 left operand (always a register)
     value 0 register 0-255            (1 byte)
     value 1 extended register         (2 bytes)
     value 2 extended register         (4 bytes, future version)
     value 3 reserved
  bit 2-3 right operand kind
     value 0 right operand immediate
     value 1 right operand register
     value 2 right operand const pool
     value 3 reserved
  bit 4-7 right operand form, interpreted by the kind

    kind 0 (immediate)
       value 0  => 1 byte  (unsigned)
       value 1  => 2 bytes (unsigned)
       value 2  => 4 bytes (unsigned)
       value 3  => 8 bytes (unsigned)
       value 4  => 1 byte  (signed)     <- the base form
       value 5  => 2 bytes (signed)
       value 6  => 4 bytes (signed)
       value 7  => 8 bytes (signed)
       value 8  => 8 bytes (float)
       value 9  => 0 byte  (bool: false)   the form IS the value, no operand bytes
       value 10 => 0 byte  (bool: true)    the form IS the value, no operand bytes
       value 11 => 0 byte  (nil)           the form IS the value, no operand bytes
       value 12 reserved
       value 13 reserved
       value 14 reserved
       value 15 reserved
    kind 1 (register)
       value 0 => 1 byte  register 0-255
       value 1 => 2 bytes extended register
       value 2 => 4 bytes extended register (future version)
       value 3-15 reserved
    kind 2 (const pool)
       value 0 => 1 byte  constant 0-255
       value 1 => 2 bytes extended constant
       value 2 => 4 bytes extended constant (future version)
       value 3-15 reserved

  a kind or form that is reserved, or a kind the opCode does not accept, is a decode error:
  the reader rejects the file. the third operand c of a 3-operand op has no form field and
  stays a 1-byte register.

  arity: for a 3-operand op, kind 1 is the full form r[a] = r[b] op r[c]; kind 0 or 2 is the
  in-place form r[a] = r[a] op right (this is what addi / subi mean today).

  call is the one documented exception: always escaped, kind 0 with the form fixed to 5
  (2-byte signed function id), followed by the 1-byte argument count.
```

the base (unescaped) form is equivalent to a fixed `e` per class: register-right `e = 0x04`,
immediate-right `e = 0x40`, const-right `e = 0x08`. the escape is emitted only when an operand
exceeds its 1-byte default.

immediates replace constant-pool entries: `bool`, `nil` and `float` literals no longer occupy a
pool slot (`## constant pool`), which is the point of the wider immediate set.

### extended OpCode eq, lt, gt, lte, gte

`opCode eq` is different from `shared registers eq`: the first is an instruction, the second is
a flag bit. The flag-setting family is spelled with an `f` suffix in assembly and disassembly
(`eqf`, `ltf`, `gtf`, `ltef`, `gtef`) because `eq`/`lt`/`gt`/`le`/`ge` already denote the
**value-producing** comparisons (`r[a] = (r[b] == r[c])`), which stay — they are needed wherever
a comparison is a value rather than a branch condition. There is no `nef`: "not equal" is
`eqf` + `jne`.

The `*f` family compares **general** values (the same relation rules as the value-producing
comparisons, including `ObjectOps` dispatch for strings, tables and external objects); `cmp`
stays `int`-only (`## alternate conditional jump OpCode`).

The table lists, for each opCode, **which flags it may set** — it is a capability mask, not a
flag state. Each flag is an independent relation predicate and at most one is ever set
(`### shared registers`); "none set" means the tested relation does not hold.

|opCode|may set eq|may set lt|may set gt|sets when the relation holds|
|-|-|-|-|-|
|eqf|1|0|0|`eq` if a == b, else none|
|ltf|0|1|0|`lt` if a < b, else none|
|gtf|0|0|1|`gt` if a > b, else none|
|ltef|1|1|0|`lt` if a < b, `eq` if a == b, else none|
|gtef|1|0|1|`gt` if a > b, `eq` if a == b, else none|

A simultaneous two-bit state (`lte` setting `eq` **and** `lt` together) is **rejected**: it is
lossy and it breaks the implemented jump predicates — `je` tests `f == eq`, so it would stop
firing for `a == b` after `lte`, and `jle` would fire for `a == b` after `cmp` only by
accident. The mask reading needs no change to `QLangVM::flagsSatisfied`.

once the extended comparisons have set the `shared registers` a follow up conditional jump is
possible (`je`, `jne`, `jl`, `jle`, `jg`, `jge`, and the `ja`/`jb`/`jae`/`jbe` aliases).

```
v5/v6 spelling, reg, reg (comparison into a temporary; `jmpfalse` removed in v7)

0075  eq           11, 0c, 0d ; r17 = (r12 == r13)      # 4 bytes
0079  jmpfalse     11, 03     ; if !truth(r17) pc += 3  # 3 bytes, -> 007f

new encoding, right operand is a register (kind 1, form 0)

0075  eqf          0c, 0d     ; flags = (r12 == r13)    # 3 bytes
0078  jne          03         ; pc += 3                 # 2 bytes, -> 007d

new encoding, right operand is a signed 1-byte immediate (kind 0, form 4) — the modifier MUST
be emitted: a base (unescaped) form can only mean the opcode's default right kind, which for
every `*f` compare is a register, so an immediate right operand is always announced (`e=0x40`)

0075  eqf   e=0x40 0c, 05    ; flags = (r12 == 5)       # 4 bytes (escaped: e + 1-byte imm)
0079  jne          03         ; pc += 3                 # 2 bytes, -> 007e
```

these new encodings reduce the number of active temporary registers: the comparison writes
flags instead of a register, so the temporary `r17` disappears, and the right operand no longer
needs a register of its own.


## constant pool

constant in the pool must have unique values. duplicated entries should be folded into a single one.

the pool holds what cannot be an immediate: **strings**, **blobs** and the **closure / function
constants** used by `makefunc` / `makegen`. `int` literals left the pool with `movi` (Round
30/34); `bool`, `nil` and `float` literals left it with the v6 immediate forms (Round 56,
`### bytecode encoding`), which is the "freeing slots in the constant pool" goal.


## bytecode encoding and tests

all tests will have to validated again whenever new encoding guidelines are implemented.

## future plans

Phasing of the extended encoding. Full design, impact assessment and verification gate:
[../plans/encoding-v6-and-indirect-plan.md](../plans/encoding-v6-and-indirect-plan.md).

**Phase 1 — `.qlc` v6.** *landed — Round 56 (encoding, pipeline) and Round 59 (the `*f`
family and the `ja`/`jae`/`jb`/`jbe` aliases).*

- the modifier byte becomes the 2/2/4 layout (`### bytecode encoding`), which carries a
  right-operand **kind**: immediate / register / const pool.
- `mov`, `movc` and `movi` are reconciled as `mov` with the extra byte; `add`/`addi`,
  `sub`/`subi` and `cmp`/`cmpi` the same way. `addc`/`subc` are never added — kind 2 already
  means "const pool".
- immediates include signed, unsigned, float, bool and nil, thus freeing slots in the constant
  pool (`## constant pool`).
- the flag-setting comparisons `eqf`/`ltf`/`gtf`/`ltef`/`gtef` and `tst` are added
  (`### extended OpCode eq, lt, gt, lte, gte`, `### OpCode tst`). **— landed Round 59**
  (`tst` emitted since Round 60, Phase 3 S1)
- `ja`/`jae`/`jb`/`jbe` exist as assembler aliases only. **— landed Round 59** as
  specification + disassembly rules: the repo has no assembler, so an alias has no code
  surface, and the opcode table carries no `JA`/`JAE`/`JB`/`JBE` identity by design.
- the deprecated spellings (`movi`, `movc`, `addi`, `subi`, `cmpi`, `jmpfalse`, `jmptrue`) kept
  decoding in **v6**, so a v6 stream could contain either spelling while the compiler emitted
  only the new ones. They were **removed in `.qlc` v7** (Phase 3, Round 60).

**Phase 2 — compiler follow-up.** the compiler stops emitting the deprecated spellings *(done,
Round 56 — it now emits `mov`/`add`/`sub`/`cmp` with a kind and an immediate form)*, widened
right operands are extended beyond the `## opCode scope` families, and the pool is re-measured.

**Phase 3 — removal.** *landed — Round 60.* the `i`/`c` suffix opcodes (`movc`, `movi`, `addi`,
`subi`, `cmpi`) are removed together with `jmpfalse` and `jmptrue` (`tst` + `je`/`jne` subsume
them), the remaining identities are compacted to `0..71` (`kOpcodeUsedCount` 79 → 72) and the
version field becomes **7** — a second version bump with no layout change, landed once nothing
emitted the old spellings any more. Plan:
[../plans/archive/phase3-opcode-retirement-plan.md](../plans/archive/phase3-opcode-retirement-plan.md)
(S1 `b86460b`, S3 `801498c`, S4 `a6737b9`).

reducing temporary registers is a priority: the flag comparisons and the in-place immediate
forms exist to remove the compare-to-temporary + `jmpfalse` shape and the arithmetic
temporaries (`### egregious examples`).

**`$$` inside the new framework — resolved 2026-11-06.** the declaration bit is *not* encoded.
It is per-*event* state (whether a declaration has executed in this frame), not
per-instruction state, so no operand field can express it — spare or not. It becomes a
per-frame bitset indexed by declaration-site id, set by a `DECL siteId` marker opcode executed
where the declaration executes, with the frame name map and the declaration table as function
metadata in the `.qlc` function record (v8 as of Round 62 — v8 moved only the constant pool out of
the function record). `indget`/`indput` are ordinary register-class
opcodes. `kind 3` and immediate forms 12–15 therefore stay reserved. See
[../plans/encoding-v6-and-indirect-plan.md](../plans/encoding-v6-and-indirect-plan.md) §7 and
[05-operators.md](05-operators.md) `## operator $$`.


## bytecode serialization

only retain a serializer and decoder for the current qlc codec.

see [SERIALIZATION.md](../SERIALIZATION.md)

