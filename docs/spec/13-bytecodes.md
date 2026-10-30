# bytecodes

## Instruction operand map (12-byte layout)

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
  uint16_t line, col;
};
```

Note: `b`/`c` are the two 16-bit halves of `i32`; they are declared as an
anonymous struct inside the union (bare `int16_t b, c` members of a union
would alias the same storage).

12 bytes, alignment 4, no padding. Rule: any opcode that needs a 32-bit
parameter uses `i32`; all others use `a`/`b`/`c` as `int16_t`. The single
source of truth is `usesI32(Opcode)` in `QLangBytecode.h`. Current `i32`
users: `JMP`, `JMPIFFALSE`, `JMPIFTRUE`, `CONTINUE`, `BREAK` (pc targets),
and `MOVI`/`ADDI` (32-bit integer literal; destination register in `a`).

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
| INDGET | dest reg | table reg | key reg | | `$$` indirect read |
| INDPUT | table reg | key reg | value reg | | `$$` indirect write |
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

## new opcode: MOVI

**Implemented (Round 30, 2026-10-29):** `Opcode::MOVI` appended last in the
enum (existing `.qlc` opcode numbers unchanged), `usesI32()` covers it, the
VM dispatches it in all four exec loops, the compiler emits it for `Type::Int
` literals (expressions, `local` declarations, plain assignments — the
latter two inlined into the target register, saving the temp + `mov`), and
the `.qlc` writer emits format version 2 (readers accept ≤ 2).

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


## new OpCode addi

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

PROPOSAL. DOT NOT IMPLEMENT

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