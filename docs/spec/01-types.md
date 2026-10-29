# language

scripting language that interfaces with C++
this language will compile to bytecode.

QLang uses a python-like syntax.

QLangContext hosts the interpreter context. 

default variable scope is local

everything is case sensitive.

SPEC.md is a living document.

## data types

|type|size|notes|status|
|-|-|-|-|
|string|varying| zero terminated | deprecated
|str|varying| zero terminated| alias string
|blob|varying|  binary data
|char|1 byte| int8_t
|byte|1 byte| uint8_t
|word|2 bytes| uint16_t
|short|2 bytes| int16_t
|int|4 bytes| int32_t
|unsigned|4bytes| uint32_t |equivalent to dword
|dword|4bytes| uint32_t
|long|8 bytes| int64_t
|qword|8 bytes| uint64_t
|array|varying| contain list of mixed variable|
|float|8 bytes||
|table|varying||
|bool|1 byte| true, false
|set|varying|unique value container|

**NOTES**: type `dword` is another alias for `unsigned` both are 32bits (4 bytes) unsigned int (uint32_t). the original documentation was in error when `dword` was used  for `8 bytes`.


## strings

string are zero terminated arrays of bytes.

`QLang` to implement `Python f-strings` style

```python
price = 19.99
print(f"Total: ${price:.2f}")  # Output: Total: $19.99
```
qlang also has standard `printf` and `sprintf` functions.

## sets

sets are container of `unique values`.

sets can contains numbers and strings.

possible extensions may include: arrays, tables and other sets. tbd

**declaration**

```
local set_a = set(1,5,7,9)  # set constructor
local set_b:set = [1,2,4.7] # array coerced into set
local set_c = set([1,6,46,5]) # array coerced into set
```

sets have their own `__str__()` function for string coersion.

```
local set_a = set(1,2,3)
println("{set_a}") # expected: "set(1, 2, 3)"

# append

local set_a = set(1,5,7,8,9,5) # expected: 1,5,7,8,9
set_a[] = 11  # expected: 1,5,7,8,9,11
```

**sets and in keyword**

|example||behavior|
|-------|-|-|
|for x in set(1,2,3) {...}|similar to | for x in [1,2,3] {...}
|while x in set(1,2,3) {...}|similar to | while x in [1,2,3] {...}
|if x in set(1,2,3) {...}|similar to | if x in [1,2,3] {...}


value order is dependant on the implementation.
TODO: please advise on options


