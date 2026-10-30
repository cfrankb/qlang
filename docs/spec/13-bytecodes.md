# bytecodes

## Instruction operand map (proposed 12-byte layout)

**Proposed — not yet implemented.** See [../plans/qlc-codec-plan.md](../plans/qlc-codec-plan.md).

```cpp
struct Instruction {
  Opcode op = Opcode::CONST; // uint16_t
  int16_t a;
  union {
    int16_t b, c;
    int32_t i32;  // combined b/c for opcodes with a 32-bit parameter
  };
  uint16_t line, col;
};
```

12 bytes, alignment 4, no padding. Rule: any opcode that needs a 32-bit
parameter uses `i32`; all others use `a`/`b`/`c` as `int16_t`. The single
source of truth is `usesI32(Opcode)` in `QLangBytecode.h`. Current `i32`
users: `JMP`, `JMPIFFALSE`, `JMPIFTRUE`, `CONTINUE`, `BREAK` (pc targets).

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

## new opcode: MOVI

**this information preliminary and subject to changes**

DO NOT IMPLEMENT YET

if the constant fits in a 4 bytes (32bits) integer, it should be encoded as `movi` (immediate) thus not requiring extra storage in the constant pool.  otherwise,  the opcode will be `movc` (CONST), the  constant is stored in the constant pool and the index is stored in the bytecode (current behavior)

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

decompiler rules
- a value denoting `register` is prefixed with `r`
- a value denoting `constant` is prefixed with `c`

```
examples

line 0000    movc 01, 00     -->   movc r1, c0 
line 0004    cast 02, 03, 0c -->   cast  r2, r3, 0c

```