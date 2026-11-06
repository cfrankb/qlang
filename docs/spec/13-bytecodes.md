# bytecodes

## disassembler

IMPORTANT: every time a change is made to the opcodes or bytecode encoding, always update the disassembler: `examples/qlang_run.cpp`.


## Instruction operand map (8-byte layout)

Implemented in `include/qlang/QLangBytecode.h` (see
[../plans/qlc-codec-plan.md](../plans/qlc-codec-plan.md)).

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

8 bytes, alignment 4, no padding. Rule: any opcode that needs a 32-bit
parameter uses `i32`; all others use `a`/`b`/`c` as `int16_t`. The single
source of truth is `usesI32(Opcode)` in `QLangBytecode.h`. Current `i32`
users: `JMP`, `JMPIFFALSE`, `JMPIFTRUE`, `CONTINUE`, `BREAK` (pc targets),
and `MOVI`/`ADDI` (32-bit integer literal; destination register in `a`).

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
| CONST | dest reg | pool index | — | | |
| MOV | dest reg | src reg | — | | |
| CONV | dest reg | src reg | type id | | |
| NEG | dest reg | src reg | — | | |
| INC | dest reg | — | — | | |
| DEC | dest reg | — | — | | |
| ADD … IN (17 binary ops) | dest reg | left reg | right reg | | ADD SUB MUL DIV MOD AND OR XOR SHL SHR LAND LOR EQ NE LT GT LE GE POW IN |
| NOT | dest reg | src reg | — | | |
| CALL | dest / arg base | builtin id / native slot | arg count | | args in r[a+1..a+c] |
| DELETE | reg to remove | — | — | | |
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
| JMPIFFALSE | cond reg | — | — | pc target | |
| JMPIFTRUE | cond reg | — | — | pc target | |
| CONTINUE | — | — | — | pc target | loop test label |
| BREAK | — | — | — | pc target | loop exit label |
| CALLFN | dest reg | function index | arg count | | args in r[a+1..a+c] |
| CALLCLOS | closure / dest reg | — | arg count | | b unused |
| RETURN | result reg | — | — | | |
| MAKEFUNC | dest reg | closure const index | — | | |
| MAKEGEN | dest reg | function const index | — | | |
| REFLOAD | dest reg | capture table index | — | | |
| REFSTORE | value reg | capture table index | — | | |
| YIELD | generator value reg | yield value reg | generator reg | | |
| MOVI | dest reg | — | — | int32 literal | `r[a] = Int(i32)`; Type::Int only |
| ADDI | dest reg | — | — | int32 literal | `r[a] = r[a] + Int(i32)`; in-place; Type::Int only |
| SUBI | dest reg | — | — | int32 literal | `r[a] = r[a] - Int(i32)`; in-place; Type::Int only |
| CMP | left reg | right reg | — | | flags = `cmp(r[a], r[b])`; independent GT/EQ/LT bits, wipes prior flags |
| CMPI | left reg | — | — | int32 literal | flags = `cmp(r[a], Int(i32))` |
| JE | — | — | — | pc target | jump when flags == EQ (`a == b`) |
| JNE | — | — | — | pc target | jump when EQ not set (`a != b`) |
| JG | — | — | — | pc target | jump when GT only (`a > b`) |
| JGE | — | — | — | pc target | jump when GT or EQ (`a >= b`) |
| JL | — | — | — | pc target | jump when LT only (`a < b`) |
| JLE | — | — | — | pc target | jump when LT or EQ (`a <= b`) |
| DEEPCOPY | dest reg | src reg | — | | `r[a] = deepcopy(r[b])` |
| INDEXGET | dest reg | base reg | key reg | | `r[a] = r[b][r[c]]`; array/table/object subscript |
| INDEXPUT | base reg | key reg | value reg | | `r[a][r[b]] = r[c]`; array/table/object subscript |

## opcode: MOVI

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


sample decompiler output
```
constants:
    [0] 0
    [1] nil
    [2] "%s is %s byte"
    [3] true
    [4] 1.24
  code:
  L 1:  local a:byte = 0
  0000  movc         01, 00     ; r1 = pool[0]                # 0
  0001  mov          00, 01     ; r0 = r1
  0002  movc         01, 01     ; r1 = pool[1]                # nil
  0003  conv         00, 00, 03 ; r0 = convert(r0, byte)      # type:byte
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


## OpCode addi

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

## alternate conditional jump OpCode

proposed jump OpCode: `je`, `jl`, `jle`, `jg`, `jge`
proposed comparator: `cmp`, `cmpi`

|shared registers||
|-|-|
|gr|    greater than
|eq|    equal
|lt|    lesser than


cmp compares Instruction.a and Instruction.b and sets `gr`, `eq` and `lt`. The
flags are wiped first, then each bit is set independently from the actual
relation: `gr = (a > b)`, `eq = (a == b)`, `lt = (a < b)`. The three relations
are mutually exclusive and exhaustive, so **exactly one** bit is set after a
cmp/cmpi. (The `a >= b`, `a <= b`, `a != b` patterns below are *jump
conditions*, not flag states — they are how the j* opcodes read a single set
bit.)

cmp opCode can only be used if a,b are both `int`
cmpi opCode can only be used if a is `int` (i32 is always int)

see which opcode can be used with which types (applies to column). 

|opCode|relation|gr|eq|lt|
|-|-|-|-|-|
|cmp a,b|a > b|1|-|-|
|cmp a,b|a == b|-|1|-|
|cmp a,b|a < b|-|-|1|

|opCode|relation|gr|eq|lt|
|-|-|-|-|-|
|cmpi a,i32|a > i32|1|-|-|
|cmpi a,i32|a == i32|-|1|-|
|cmpi a,i32|a < i32|-|-|1|


|opCode|description|jumps when|meaning|
|-|-|-|-|
|jg|jump greater|gr|a > b
|jge|jump greater equal|gr or eq|a >= b
|jl|jump lesser|lt|a < b
|jle|jump lesser equal|lt or eq|a <= b
|je|jump equal|eq|a == b
|jne|jump not equal|eq not set|a != b
|ja|jump greater|gr only set|byte, word, dword (not implemented)
|jae|jump greater equal|gr or eq|byte, word, dword (not implemented)
|jb|jump lesser|lt only set|byte, word, dword (not implemented)
|jbe|jump lesser equal|lt or eq|byte, word, dword (not implemented)

address is stored in Instruction.i32

### OpCode jg
```
  0054  gt           09, 0c, 0d ; r9 = (r12 > r13)
  0055  jmpfalse     09,  5c         ; if !truth(r9) pc = 005c
```

alternate, if r12 and r13 
```
  0054  cmp          0c, 0d ;  r12 > r13   (update shared registers)
  0055  jg           005c   ; pc=005c
```

### OpCode je
```
  004c  eq           06, 09, 0c ; r6 = (r9 == r12)
  004d  jmpfalse     06. 5c         ; if !truth(r6) pc = 005c
```

alternate, if r12 and r9 
```
  004c  cmp            09, 0c ;  r9 == r12
  004d  je     005c         ; pc = 005c
```


### OpCode jl
```
  0068  lt           09, 0d ; r14 = (r9 < r13)
  0069  jmpfalse     5c         ; if !truth(r14) pc = 005c
```
alternate, if r13 and r9 

```
  0068  cmp           09, 0d ; r9 < r13
  0069  jl     005c         ; pc = 005c
```


OpCode `jge` and `jle` work exactly like `jg` and `jl`.
`jne` is the opposite of `je`.

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
existing `.qlc` opcode numbers stay valid; the `.qlc` format version is
unchanged (v5).

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

example of external modules are `ql` files.

each module must be able to be compiled on it's own. when ingested by the `qlang` executor, it must be able to run (assuming they both share compatible bytecodes).

## export table (lookup)

includes all the global `functions`, `variables` and `constants` being exposed to other modules. these can be imported by other modules via `import` keyword.

bytecode to reference the look up table.

## import table (lookup)

this table will contain all the  global `functions`, `variables` and `constants` being imported from other modules.

bytecode to reference the look up table.

## table for build in function (qlang base)

must create a look up build in table is created from `qlang` build in functions. this table will be used in disassembly to find the exact function name. 

thus hardcoding function names in the disassembly source (which is highly discourged) will not longer be required.

## unified constant table

all the functions within a module must share a common const table or at the very least must not duplicate constants.

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

## technical specifications

1. **Link model** — both `link-time merge` and `runtime module registry`. should share a reusable solution
2. **Entry point** — global scope script only at this point. no main function. single file qlang modules in this version only.

3. **Cross-module variable semantics** — shared cell (write-through, recommended)
   
4. **Export syntax** — see keyword export

5. **Import graph rules** 

  C++ api can import `.ql modules` into a context.
  `.ql modules` may import `C++ modules`.
  `.ql modules` may import `.ql modules`. 
  C++ modules cannot load `.ql modules` at this stage. it could be a feature later.
  
  recursivity check is needed to prevent a module from importing modules that import it. avoid endless cycles.

  aliases is already a requirment. (e.g. `from m import f as g`)

6. **Compatibility rule** — at this early stage, compatible bytecode is `qlang` bytecodes generated with the exact same compiler version. 

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


## encoding proposal

[docs/plans/instruction-format-escape-byte-plan.md](../plans/instruction-format-escape-byte-plan.md)

there should be only one pool of registers and one pool of constants. extended addressing is 2 byte, default addresings is 1 byte. if OpCode top bit is not set then both left and right operand are using 1 byte encoding

127 OpCode limit (hard limit)

```
top bit of opCode byte is set to denote variants

opCode 1 byte (0-127) are opCode value. if top opCode top bit is set then an extra byte is added next to it .
extra byte (e):
- if opCode jmp
   e = 0 => jmp16 (2 byte) relative address
   e = 1 => jmp32 (4 byte) relative  address
- if opCode movi, addi, subi, movc, cmp, cmpi etc
   bit 0-3 left operand
      value 0 register 0-255
      value 1 extended register (2 bytes)
      value 2 extended register (4 bytes) // future version
   bit 4-7 right operand
      if constant (immediate)
         value 0 => 1 byte   (unsigned)
         value 1 => 2 bytes  (unsigned)
         value 2 => 4 bytes  (unsigned)
         value 3 => 8 bytes  (unsigned)
         value 4 => 1 byte   (signed)
         value 5 => 2 bytes  (signed)
         value 6 => 4 bytes  (signed)
         value 7 => 8 bytes  (signed)
         value 8 => 8 bytes  (float) // future version
         value 9 => 1 byte   (bool)  // future version
         value 10 => 0 byte  (nil)   // future version
      else if register
        value 0 register 0-255 (1 byte)
        value 1 extended register (2 bytes)
        value 2 extended register (4 bytes) // future version
      else if constant
        value 0 constant 0-255 (1 byte)
        value 1 extended constant (2 bytes)
        value 2 extended constant (4 bytes) // future version
```

## constant pool

constant in the pool must have unique values. duplicated entries should be folded into a single one.

## bytecode serialization

see [SERIALIZATION.md](../SERIALIZATION.md)

