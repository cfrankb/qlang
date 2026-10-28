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


## reserved words
```
for
to
in
then
while
continue
break
return
string
str (alias string)
byte
blob
word
short
int
unsigned
array
float
table
bool
true
false
def
let (alias local)
else
range
goto
char
qword
switch
case
default
type
join
global
const
end
cast
assert
class
yield
fetch
step
if
next
import
from
static
isset
set
long
enum
```

## constants

```python
const # declare a constant

const year = 2026;
const month:byte = 10;
```

const expressions cannot be redeclared and are immutable. trying to redeclare an existing constant will trigger and error. trying to assign a value to a constant will trigger an error.

global constants are viewable in every scope without needing the `global` keyword. 


## enum

enum are collection of immutable values

enum type default to int

```
enum Animal {
   Dog, # implied 0
   Cat, # implied 1
   Horse # implied 2
}

# enum type: byte
enum SoftDrimk:byte {
   CocaCola , # implied 0
   Pepsi, # implied 1
   SevenUp # implied 2
}

# trigger error: cannot redeclare enum Animal
enum Animal {
   Turle, # implied 0
   Fish, # implied 1
   Moose # implied 2
}

enum Language {
   Cpp = 100, 
   Python = 101, 
   Pascal # implied 102
}

println(f"language Cpp = {Language::Cpp}")

# both notation is valid
let python = Language::Python # python = 101
let dog = Animal.Dog # dog = 0

println("size: {len(Language)}") # expected: "size: 3"
```

restrictions and special considerations

- enum are binded to the current scope (global, function, lambda). 
- using local or let qualifier, enum can be restricted to block scope.
- global enums are visible everywhere.
- enum cannot be redefined. 
- C++ api needed to inject enums to the global context (needs discussion)
- trying to reference an unknown enum, triggers an error
- valueless enum triggers an error

## comments 

```python
# comment
```

## boolean evaluation

|type||
|----|-|
|boolean| true or false|
|string| empty => false; non-empty => true
|array| empty => false; non-empty => true
|table| empty => false; non-empty => true
|blob| empty => false; non-empty => true
|number| 0 => false; non-zero => true
|nil| false
|generator| true
|function, lambda|truthiness evaluated based on its return value



## declarations and assignments

the compiler tries to guess the type based on right hand.
you can coerse the type using `var:type` modifier.

```python
local a = 0xff             # int (4 bytes)
local b:byte = 0xff        # byte (1 byte)
local c = cast<short>(b)   # short (2 bytes)
local d                    # implicit int. default value 0
```

here are some general purpose cases to illustrate how variables and constants are declared and assign.

```python
local a1  # default variable type int. default value 0
local result = true # boolean
result = false # boolean
local a = 32;  # int 
local b:byte = 32 # byte
local bb = ' ' # byte
local s ="this is a string"
local ms = """ 
   multilines string
"""
ms = ''' 
   multilines string
'''

local num = 12 # assign 12 
local num = 0xc # assign 12 (hexadecimal)
local num = -124 # assign -124

local aa:array = [1,2,3]    # declare array
local ab = [0,4,5]          # declare array
const acon = [1,27,7]      # constant array (immutable)
local t:table = {name="Frank", age=99, year=2026, character:byte=' '}

t.name = "John"  # valid
t.age=101  # valid
t.tokens = 14455 # valid
t["age"] = 33 # valid; equivalent to t.age
t[] = 44  # error

ab[0] = 55 # assign value to index 0; throw out of range error if applicable
ab[] = 4 # append to array
ab += 5 # append to array

local ac = aa + ab # append both array into a new array (ac)
ac += [6,7] # append new array to ac

local z # int, default 0
local f = 1.13 # float

c = a  # copy a to c
a += 12 # add to variable
s = "hello"
s += "abc" # append to a string

sv = 123
sv += "test" # error

sx = "test"
sx += 123 # sx = test123

sz = year # valid. constants can be copied to vars
year = sx # error; constants cannot be assigned to

local xbit = 1
if true {
    local xbit = 2   
}

t = 5
local t = 9   # warning: duplicate declaration → compile warning

# anonymous functions inside a table
local events:table = {
  version = 0x101,
  name = "loader",
  onLoad = (name)[] {
    printf("""onLOaD -->%s\n""");
  }, 
  onDeath = ()[] {
  }
}

# implicit declaration/assignment
events = {
  version = 0x101,
  name = "loader",
  onLoad = (name)[] {
    printf("""onLOaD -->%s\n""");
  }, 
  onDeath = ()[] {
  }
}

# floats use a special notation
local my_float = 1.0f

# unsigned int
local my_uint = 44u

```
# Control flow

## conditions

```python

if (a == 32) || (b==32) {

}


a = [1,4,6]
if (b in a) {

}

if (b in [5,7,4]) {

}

if b in [1,3,5] {

} else if b in [2,6,7] {

}

if (a == 4) {

} else {

}

# check if a between 1 and 10 (inclusive)
if a in range (1,10) {

}

if a == 1 return false;

if true return false

if true {

}

```

## range

```python
for x = 0 to 10 step 1 {
   ...
}

if i in range (0,10) {
   ...
}
```

rules
- step keyword is optional
- evaluate the relation ship between start and end value
- the direction determine the step (+1 or -1)
- enter the loop once
- continue to apply step until you are passed the end value

**guard rail.** if step is user specified and step is heading in the wrong direction then enter loop once before leaving
otherwise apply the same guard logic as above. 
step 0 creates an infinite loop similar to while true {}

infinite loops are not valid. implement a timeout guard that result in a runtime exception. make the message concise so that tracking the bug will be easier.

```python
# range keyword has different encoding based on context

arr = range(0, 10)

# encode: arr = [0,1,2,3,4,5,6,7,8,9,10]

for i in range (0, 10) {
   yield i
}

# encode: i = 0; while (i <= 10) { yield i; ++i }

while i in range (0,10) {
   ...
}

# encode: while i >=0 && i <= 10 {...}
# no increment here
# if i not defined throw error

let b = 3
let x = b in range(0,10)
# encode: x = (b >= 0) && (b <= 10)

if a in range (1,10) {
   ...
}

# encode if a >=1 && a <=10 { ... }

```

if used, the keyword `continue`  will apply the step. this is implicit

## switch

switch cases fallthrough until they reach break, return, goto or end of switch. 

```python
switch (var) {
   case "string2":
   case "string1":
      # var value is either "string1" or "string2" here
      break;
   case 123:
      break;
   case in [1,2,3]:
   case in range(10,20):   
   default:
}
```

## loops

the step keyword applies to ranges in for loops only and is explicitly not allowed when looping on a string or collection.

```python
for x = 1 to 32
next x

for x = 10 to 0 step -1
next x

for x in range (0,4)
next x

for x in range (10,0) step - 1
next x

for x in [1,25,6,7]
next x

# next can increment counter implicitely
# replicating the same behavior as basic

for x = 1 to 32
next # assume x

for y = 1 to 32
   for x = 1 to 32
   next # assume x
next # assume y

a = [1,4,6]
for x in a
next # assume x

for x in [1,4,6] step 2 {
   # error. you can't step into arrays
}


for i in range (10, 0) step - 2 {
}

for y in range (0,10) {
   for x in range (0,10) {
   }
}

for x = 1 to 32 {
   if x == 4 {
      continue # skip iteration
   }

   if (x == 2) {
      break #exit loop
   }
   printf("%d\n", x)
}

while (a > 0) {
 --a
}

a = [1,4,6]

for i in a {
   printf("i=%d\n", i) # can look up an array
}

for i in [4,6,8,9] {
   printf("i=%d\n", i) # can look up an array
}

for c in "string" {
   printf("char: %c\n", c)
}

# loops i from 1 to 10 (inclusive)
for i in range(1,10) {

}
```

## label / goto
```python
a = 10

:label1
--a

if a > 1 goto label1
```

label and goto must reside in the same scope.
if they cross a boundary, trigger an error.

# syntax

type coersion between numbers is always guided by the left operand.

## operators

|operator|role|
|---------|--|
|=  | assign |
|== |equal |
|+= |add or append
|!= |not equal
|<  > <= >= |comparison
|<< >> |bit shift
|^ |binary xor
|& |binary and
|\| |binary or
|&& |logical and
|\|\| |logical or
|() |parenthesis
|-- ++ |decremental/incremental
|%  |modulo
|/ |division
|* |multiplication
|** |exponential
|! |not (inverse)
|;  |silent separator (optional)
|cast|change datatype at runtime
|$| dynamic key resolution operator
|$$| indirect access operator
|::| scope resolution operator

## operators < and >

|left hand|right hand|analysis
|-|-|-|
|number|number|numeric comparison
|number|float|coerce and numeric comparison|
|number|string|error: invalid comparison
|number|table, array, class, object|error: invalid comparison
|string|string|binary comparison
|string|table, class, array, object|binary comparison with __str__() for table, class and array
|nil|any|error: invalid value for comparison
|array|array|comparison of each value at their relative indexes
|table|table|comparison of each key value
|table|class, object|error: objects cannot be compared
|table|array|error: invalid comparison
|class, object|any|error: objects cannot be compared

same rules apply to `operator <=` and `operator >=`

## operator **

pow is only valid when left and right operands are numbers.
in this implementation, all other cases will return an error.

```
a = 2
b = 3
c = a ** b
println("{c}") # expected: 8

d = 2 ** [1,2] # expected: error
```


## operator *

|left hand|right hand|operation|example|expected|notes|
|-|-|-|-|-|-|
|number|number|multiply|2 * 2|4
|float|float|multiply|1.5 * 1.2|1.8
|int|float|multiply|1 * 1.2|1|float coerced to int
|float|int|multiply|1.2 * 1|1.2|int coerced to float
|string|number|"toto" * 2|error|
|array|number|multiply|[1,2,3]*2|[2,4,6]|
|table|any|n/a|error
|class|number||error|
|number|string|copy|2 * "abc"|"abcabc"

## operator +=

|left-hand|operator|right-hand|result
|-|-|-|-|
|"string"| +=| "_string"|"string_string"
|"string"| +=| 123|"string123"
|"string"| +=| true|"stringtrue"
|"string"| +=| 1.23|"string1.23"
|"string"| +=| any other types|string<\type>
|123| +=| 123|246
|123| +=| 1.23|124
|123| +=| any other types|error
|1.23| +=| 123|124.23
|1.23| +=| 1.23|2.46
|1.23| +=| any other types|error
|[1,2,3]|+=|[5,6]|[1,2,3,5,6]
|[1,2,3]|+=|4|[1,2,3,4]
|[1,2,3]|+=|"s"|[1,2,3,"s"]
|[1,2,3]|+=|1.23|[1,2,3,1.23]
|[1,2,3]|+=|{name="toto"}|error
|[1,2,3]|+=|any other types|error
|{name="toto"}|+=|{age=23}|{name="toto", age=23}
|{name="toto"}|+=|any other types|error

## operator cast
|examples|results|notes|
|-----|-----|-|
|cast\<int>(1.12)| 1| casting to 1. truncate decimals
|cast\<float>(1)|1.0f| make the int into a floating point
|cast\<string>(13)|"13"| turns int into string
|cast\<string>(1.3)|"1.3"| turns float into string
|cast\<word>(-1)|65535| converting int to word removes sign
|cast\<string>([])|depends on \_\_str__()| collections fallback on their string representations
|cast\<int>([])|n/a| error: collections cannot be cast to int
|cast\<array>(123)|n/a| error: numbers cannot be cast to collections
|cast\<array>(set)|n/a| sets are converted to arrays
|cast\<set>(array)|n/a| arrays are converted to sets
|cast\<any>()|n/a| error: empty source. nothing to cast
|cast\<>(123)|n/a| error: empty cast. nothing to do

## operator == and !=
|var type|operator|var type|result
|-|-|-|-|
|nil| ==| nil | true
|nil| ==| number (int, float etc) | false
|nil| ==| collection | false
|nil| ==| table | false
|nil| !=| nil | false
|nil| !=| number | true
|nil| !=| collection | true
|nil| !=| table | true

*important*: `nil` is ONLY equal to itself. ``collection`` implies (table, array, class, set)

|expr|operator|expr|result|notes
|-|-|-|-|-|
|1|==|1|true||
|1|==|1.0|see notes|only if type coercion is possible and results in equivalency|
|3.0|==|3.0|true||
|"3"|==|3|false|strings are not numbers. there is no coersion|
|' '|==|32|true|byte ' ' is 32
|-1|==|(byte)0xFF|false|comparison between signed and unsigned must match intended value 
|1|==|(unsigned)1|true|signed and unsigned int are equal when sign bit is not set
|1|==|object|false|objects are not primitives
|string|==|object|see notes|true only if the __str__ value of the object matches the string
|"toto"|==|"toto"|true|string is equal to itself
|table|==|table|depends|only true if all kv pairs must match. no stray keys
|array|==|array|depends|only true if array is an exact match
|set|==|set|depends|only true if set is an exact match

equivalency works both ways. `operator !=` is the `boolean` inversion of `operator ==`.

## The \$ Operator (Evaluation / Dynamic Key Resolution)

The `$` operator evaluates an identifier at runtime and forces its value into a string representation. It is primarily used for dynamic property key resolution during table literal construction and dynamic expression formatting.

### Constraints and Type Restrictions
Unlike the explicit `cast<string>()` operator which falls back on default string representations for all types, the `$` operator is strictly a **scalar-to-string evaluator**. 

* **Supported Types:** `string/str`, `char`, `byte`, `word`, `short`, `int`, `unsigned`, `qword`, `float`, `bool`, and `nil`.
* **Unsupported Types:** `array`, `table`, and `class`. Invoking the `$` operator on an aggregate collection type will halt execution and depeding on when evaluation is taking place, it will throw either an exception or a compilation error. 

### Behavior Matrix

| Operand Type | Value | Evaluation Result | Status |
|---|---|---|---|
| `int` | `33` | `"33"` | Valid |
| `float` | `19.99` | `"19.99"` | Valid |
| `string` | `"gender"` | `"gender"` | Valid |
| `bool` | `true` | `"true"` | Valid |
| `nil` | `nil` | `"nil"` | Valid |
| `array` | `[1, 2, 3]` | `error: collections not supported` | **Compile/Runtime Error** |
| `table` | `{age=33}` | `error: collections not supported` | **Compile/Runtime Error** |

### Code Examples

#### 1. Valid Scalar Resolution
```python
local age = 33
local str_age = $age  # str_age becomes "33"

local property = "gender"
local t:table = {name="Bob", age=99, $property="male"}
# Resulting table: {name="Bob", age=99, gender="male"}
```

#### 2. Invalid Collection Evaluation
```python
local a = [1, 2, 3]
local err1 = \$a       # Error: collections not supported

local t = {x=1, y=2}
local err2 = \$t       # Error: collections not supported
```

### Compiler and VM Implementation Notes
1. **Scope Boundaries:** The `$` operator respects QLang scope lookup invariants. If an identifier is located in the global scope, it must be explicitly exposed using the `global` keyword before a local `$` evaluation can resolve it inside a function body.
2. **Bytecode Generation:** The compiler must emit a specialized `OP_EVAL_KEY` opcode. This opcode is responsible for checking the target variable type tag before performing the stringification step. If the type tag matches an aggregate or compound collection signature, the VM must raise an execution panic.

## operator $$

The `$$` operator is an **indirect access operator**: it resolves a variable name
at runtime from a string and reads or assigns the referenced variable. Unlike the
`$` operator (scalar-to-string evaluation), `$$` does **not** coerce its operand —
the operand must already be a string.

`$$name` is a first-class expression: it may appear on the right-hand side of
assignments, as a function argument, in conditions, and as a table value.

```python
local a = "b"
local b # b = nil (implicit)
$$a = "toto"
println(a) # expected; "b"
println(b) # expected: "toto"

local c = "d"
$$c = "etc"  # expected: indirect variable error, `d` not declared

local e = 123
$$e = "x" # expected: indirect variable error, invalid name `123`

f = $$a
println(b) # expected: "toto"
g = $$c # expected: indirect variable error, `d` not declared

# assume global module_owner = "cfrankb"
global module_owner
h = "module_owner"
println($$h) # expected: "cfrankb"

local aa = "bb"
$$aa = 1   # expected: indirect variable error, `bb` is not declared
local bb

def foo() { return 1 }
local a = "foo"
x = $$a   # expected: indirect variable error, `foo` is a function
$$a = 5   # expected: indirect variable error, `foo` is a function

const year = 2026
local y = "year"
z = $$y   # z = 2026 (local const read is valid)
$$y = 1   # expected: indirect variable error, `year` is a constant

global module_owner
$$h = "x" # expected: indirect variable error, global write denied
```

### restrictions

- the string must hold a valid variable name.
- assignment requires the variable reference to exist in the local scope. read requires the variable to be visible (local, global declared)
- no creation of new variables
- cannot be applied to any function or lambda
- cannot reference an operator or a reserved word
- assignment is denied global scope access
- can read from global variables and const (if exposed via global keyword)
- can read local const
- cannot read global functions
- declarations are sequential

### resolution and check pipeline

Every `$$` expression runs the same staged pipeline; the first failing stage halts
execution with its error. All messages use the prefix `indirect variable error:`.

| stage | check | failure message |
|---|---|---|
| 1 | operand is a string | `invalid name <value>` |
| 2 | string lexes as a valid identifier | `invalid name <value>` |
| 3 | name is not a reserved word | `reserved word <name>` |
| 4 | name is declared and visible (resolution order below) | `<name> not declared` |
| 5 | binding is not a function/lambda (reads and writes), not a const (writes) | `<name> is a function` / `<name> is a constant` |
| 6 | (writes only) binding is local, not global | `global write denied` |

**resolution order**

- **writes:** current frame locals only. A local that shadows a global resolves to
  the local.
- **reads:** current frame locals, then globals and const exposed via `global` in
  the current scope. Global const is visible everywhere (per the global scope
  rules) and does not require `global`.

A read that misses every level is an error — `$$` never returns nil for an
unresolved name.

### declaration state (sequential declarations)

A name is targetable only after its **declaration event** has executed. The first
declaration of a name is either an explicit `local`/`let` or its first plain
assignment; a later explicit `local`/`let` for the same name is a compile warning
declares into the enclosing function frame, so a `$$` targeting a name declared
in a branch that has not run is a `not declared` error:

```python
local a = "x"
if cond {
   local x = 5
}
$$a = 1   # legal only if cond executed true
```

`$$` never creates variables, so a name whose declaration event has not executed
cannot be introduced by indirection.

**asymmetry note:** direct references resolve frame-wide at compile time, so a
direct use of a name before its declaration reads its default value; `$$`
indirection is deliberately stricter and raises `not declared` until the
declaration event has executed.

### const and function bindings

- **const** (local and global) is readable via `$$`; assignment through `$$` to a
  const binding is an error, consistent with direct assignment.
- **function and lambda** bindings (user `def`, lambda literals, global and
  native functions) are never valid `$$` targets, for reads or writes. Functions
  hosted in tables or classes are table entries, not scope names — they remain
  mutable through ordinary dynamic table access (`t[key]`).

### compiler and VM implementation notes

1. **frame name map:** at compile time the compiler builds, per frame, a static
   map of name → (register, binding kind, declaration bit). Binding kind is one of
   `var`, `const`, `function`, `lambda`. The map covers every name directly
   declared in the frame; a `$$` target absent from the map (and not an exposed
   global) can only be reached through a dynamic name string and fails at stage 4.
2. **declaration bit:** a flag on the existing store/init opcode, set once at the
   name's unique declaration site (explicit `local`/`let` or first plain
   assignment). No new opcode is required for declaration state.
3. **new opcodes:** two — `OP_INDIRECT_READ nameReg → dstReg` and
   `OP_INDIRECT_WRITE nameReg, srcReg`. Both run the pipeline above: string check,
   identifier lexeme check, reserved-word check, map lookup (frame, then exposed
   globals for reads), declaration-bit test, binding-kind test, and (writes) the
   local-only test.
4. **compile-time fast path (optional):** if a literal form `$$"name"` is ever
   supported, stages 2–6 for a constant name resolve at compile time, turning
   unknown names and function/const/global-write violations into compile errors;
   dynamic names keep the runtime path. Semantics are identical.

## build-in functions

|helpers|-|-|
|--------|-|-|
|toStr(var)     |  convert to string| deprecated
|toInt(var)     |  convert to int| deprecated
|type(var)      |   return a string representation of the type|
|print(var)       | print a string|
|println(var)     | print a string with carriage return
|printf(fmt, ...) | mirror C printf
|sprintf(fmt, ...) |same format as sprintf. return new string
|len(var)         | structures return storage sizes, collections return their length and other types return 0
|substr(str, start, length)| return a substring from index start. length is optional
|delete var      | delete variable. error when trying to delete external variables/structures or nil.
|str(var)        | return string representation of variable
|int(var)        | return variable cast to int; string are parsed as decimal. other types return 0
|slice(array, start, length) | return an array slice
|isset(var)| return true if variable, array index or table field exists. otherwise returns false
|join(separator, arr)| join the array element using separator
|assert(bool, str)| if bool is false thrown an exception using text provided by str


```python
a = [1,2,3]
println(join(", ", a))
# output: 1, 2, 3

a = [1,2,3,4,5]
b = slice(a, 0, 2)
# b = [1,2]
```


## functions (qlang native)

```python
def function_name(param1, param2)
end

def hello(name, year)
   println("hello world")
   printf("hi %s it is %d\n", name, year)
   return true
end

hello("Frank", 2026)

def hello2(name, year) {
   println("hello world")
   printf("hi %s it is %d\n", name, year)
   return true
}

# def is only valid in global scope. no closure.

local func1 = (name, year)[] { # no capture
   println("hello world")
   printf("hi %s it is %d\n", name, year)
   return true
}
func1("Frank", 2026)

local world = "world"
# this is the lambda implementation
local func2 = (name, year)[world] {  # explicit capture passed by reference
   println("hello " + world)
   printf("hi %s it is %d\n", name, year)
   return true
}

func2("Nana", "2003")

```

only explicit capture being passed from caller context;
closure list is mandatory []. closure only available in function scope.
reference still maintained even after reassignment 
return value is optiona. defaults to nil
local always apply to the current scope.

functions, lamdas and generators without a return statement will return nil (nil type)

# C++ api

Injected host variables are available to QLang code via the
`QLangContext` API. Two injection modes exist:

```cpp
ctx.set("name", value, false)   // writable from QLang 
ctx.set("name", value, true)    // read-only from QLang; assignment is a compile error
```

Host code constructs values via the `QValue` type (alias for `QLangValue`) and
free helpers:

```cpp
#include "qlang/QValue.h"
using namespace qlang;

ctx.set("greeting", qval_str("hello"));
ctx.set("pi",       qval_float(3.14159), true);
ctx.set("MAX",      qval_int(99), true);

// QValue helpers cover all scalar types plus aggregate types:
qval_nil()
qval_bool(bool)
qval_byte(uint8_t)
qval_char(int8_t)
qval_word(uint16_t)
qval_short(int16_t)
qval_int(int32_t)
qval_unsigned(uint32_t)
qval_qword(uint64_t)
qval_float(double)
qval_str(const char*) / qval_str(const std::string&)
qval_blob(const std::vector<uint8_t>&)
qval_array(ArrayValue)
qval_table(TableValue)
```

Const-injected variables seed the VM register file at start (readable from
QLang) but are excluded from the end-of-run scope sync, so QLang code cannot
mutate them. The compiler guards against assignment with a parse-time error.

Native (C++) functions are injected with `setFunc()`. The callback receives the
context as its first argument (Lua convention) followed by the QLang arguments:

```cpp
ctx.setFunc("double", [](QLangContext& ctx, const std::vector<QLangValue>& args) -> QLangValue {
    return qval_int(static_cast<int32_t>(args[0].asInt()) * 2);
});

ctx.setFunc("add", [](QLangContext& ctx, const std::vector<QLangValue>& args) -> QLangValue {
    return qval_int(static_cast<int32_t>(args[0].asInt()) + static_cast<int32_t>(args[1].asInt()));
});
```

Called from QLang like any builtin: `r = double(21)`. The return value is
assigned to the destination register; returning `qval_nil()` means no result.
Native funcs are resolved at compile time (negative opcode indices) and stored
in `CompiledProgram::nativeFuncs` — no name lookup at runtime.

## Module registration

System modules are auto-registered on `QLangContext` construction. Call
`registerBuiltinModules(ctx)` to register additional or override modules before
the first `run()`. Call `ctx.loadBuiltinModules()` to reload the default modules
after `ctx.clear()`:

```cpp
#include "qlang/QLangModules.h"
QLangContext ctx;
registerBuiltinModules(ctx);  // math module (min, max, sum) is now available

ctx.clear();
ctx.loadBuiltinModules();     // math module is available again
```

Two methods control module availability:

### `ctx.registerModule(name, module)`

Defines and stores a new module in the context's registry. This is a **setup**
operation — it does not import anything into QLang scope. QLang code can use
`import <name>` to pull the functions in on the next `run()`.

```cpp
QLangContext::Module myMod;
myMod.name = "math";
myMod.funcs["min"] = [](QLangContext&, const std::vector<QLangValue>& args) -> QLangValue {
    if (args.empty()) return QLangValue::Int(0);
    int64_t v = toInt64(args[0]);
    for (size_t i = 1; i < args.size(); i++) {
        int64_t a = toInt64(args[i]);
        if (a < v) v = a;
    }
    return QLangValue::Int(static_cast<int32_t>(v));
};
myMod.funcs["max"] = [](QLangContext&, const std::vector<QLangValue>& args) -> QLangValue { ... };
ctx.registerModule("math", myMod);
```

Registering a module with a name that already exists appends a new entry; the
last registration wins at compile time.

### `ctx.importModule(name, alias)`

Finds an already-registered module by `name` and re-registers it under
`alias` (or the same `name` if alias is empty). This is the C++ equivalent of
the QLang `import <name>` statement — it makes the module's functions
available for compile-time import resolution.

```cpp
ctx.importModule("math");           // equivalent to QLang `import math`
ctx.importModule("math", "m");      // equivalent to QLang `import m`
```

Throws `runtime error: unknown module '...'` if the source module is not found.
Useful when you want to expose the same module under multiple names or override
the default name.

### QLang import syntax

Modules are resolved at compile time — no VM changes needed for flat import.

```python
import math                   # import module into the namespace
                              # use math.min and math.max

import math as m              # m.min, m.max

from math import *            # import all public names from the math module
from math import min, max     # import `min` and `max` only
```

| Form | Behaviour |
|---|---|
| `import math` | Register the module under `math`; call as `math.min(...)`, `math.max(...)` (qualified access) |
| `import math as m` | Register the module under alias `m`; call as `m.min(...)` |
| `from math import *` | Flat import: copy all public names into the current scope |
| `from math import min, max` | Flat import: copy only the listed names into the current scope |

Qualified access (`math.min(...)`) is a future extension — v1 supports flat
import only (`from math import *` / `from math import min, max`). The module
is still resolved at compile time and each imported function is registered as
an external (native) function in the current scope.

## ctx.clear()

`ctx.clear()` resets the context state for reuse:

```cpp
ctx.clear();
// equivalent to:
//   m_vars.clear();
//   m_injectedConsts.clear();
//   m_nativeFuncs.clear();
//   m_modules.clear();
//   m_pimpl->compiled.reset();
```

After `clear()`, all injected vars, consts, native funcs, and modules are
removed. The compiled program cache is invalidated. Call
`registerBuiltinModules(ctx)` again if you need the built-in modules.

`ctx.remove(name)` deletes a single variable.
`ctx.has(name)` / `ctx.isset(name)` check whether a variable exists.

`QLangContext::call(name, args)` invokes a QLang or native function by name
from C++. It checks native funcs first, then scans compiled user functions by
name. The compiled program is cached after the first `run()` call so subsequent
`call()` invocations reuse it.

```cpp
ctx.run("def square(x) { return x * x }\n");
QLangValue r = ctx.call("square", {qval_int(7)});  // r.asInt() == 49

ctx.setFunc("dbl", [](QLangContext&, const std::vector<QLangValue>& a) {
  return qval_int(a[0].asInt() * 2);
});
QLangValue r2 = ctx.call("dbl", {qval_int(21)});  // 42
```

TODO: 
- add methods to rerived the bytecodes() from the context 
- upload new bytecodes into the context
- version bytecodes to avoid loading incompatible bytecodes

## verbose level

**Priority one** — an application-wide verbose level that is adjustable with a single function call.

The verbose level gates *diagnostic trace output only* (printf-style lines written to **stderr**). It never affects *program output* (`print` / `println` / `printf` to stdout). The level is process-global: one call changes it for the whole interpreter, and it can be toggled at runtime from either QLang or C++.

### Granularity

| Level | Trace detail emitted |
|-------|----------------------|
| `0` | Off (default). No VM trace emitted. |
| `1` | Frame / call events (`[pushCall] …`). |
| `2` | VM loop + opcodes + returns (`[VM] …`, `[DBG-RET] …`). |
| `3` | Full detail: generators, table ops, bytecode (`[DBG-GEN-EXIT] …`, `[VM-TABCREATE] …`). |

Compiler diagnostics (`[DBG-COMPILER]`, `[DBG-COMP]`, `[DBG-CONST]`, `[DBG-CALL]`, …) are emitted by `QLangCompiler` and are independent of this level.

### QLang API

`set_verbose(level)` sets the level and returns the new value.

```python
set_verbose(1)          # frame/call trace on stderr
println(foo())          # program output on stdout — unaffected by verbose
set_verbose(0)          # turn VM trace back off at runtime
```

```python
def f() { local x = 0; x = 1 + 2; return x }

set_verbose(2)          # print the opcode stream to stderr
println(f())            # >> 3   (stdout, unaffected)
set_verbose(0)
```

```
# stderr (level 2, truncated):
# [VM] func=1 pc=2 op=0 a=3 b=1 c=0 regs=6
# [VM] func=1 pc=3 op=0 a=4 b=2 c=0 regs=6
# [VM] func=1 pc=4 op=6 a=2 b=3 c=4 regs=6
```

### C++ API

The level lives on `QLangContext` and is readable/writable at any time:

```cpp
#include "qlang/QLangContext.h"
using namespace qlang;

QLangContext ctx;
ctx.setVerboseLevel(2);        // 0 = off, 1 = frames, 2 = VM loop, 3 = full
int lvl = ctx.verboseLevel();  // read back the current level
bool on = ctx.isAt(2);         // true when current level >= 2 (used to gate trace)

QLangValue r = ctx.run("println(1)");
```

### Implementation notes

- `QLangContext` owns the level (`m_verboseLevel`, default `0`) and exposes `setVerboseLevel(int)`, `verboseLevel()`, and `isAt(int)`.
- `set_verbose` is registered as a native function (`QLangContext.cpp`). It accepts an int/qword/float argument and returns the resulting level.
- `QLangVM` holds a gated emit helper `trace(minLevel, fmt, …)` (declared inline in `QLangVM.h`). Every diagnostic call site in `QLangVM.cpp` was converted from `fprintf(stderr, …)` to `trace(T, …)` with its tier `T` (1/2/3); sites inside the free function `callBuiltin` call `vm->trace(T, …)`.
- `trace()` returns immediately when `m_scope.isAt(minLevel)` is false, so no formatting cost is incurred when a tier is disabled.

### Example session

```
$ ./vtest prog.lang            # prog.lang: set_verbose(1); print(foo())
42                             # <- stdout: program output
[pushCall] calleeIndex=1 …     # <- stderr: trace, only when level >= 1
```



# global scope 

global const and functions are visible everywhere (global, local, function). 
global variables (native and injected) are not visble in function scope unless declared via global keyword. (see example)

```python
const HELLO = "Hello World"

a = [1,2,3] # global a
b = "John"  # global b

def test() {
   println(HELLO) # HELLO is visible here

   # error, a is a global var/invisible by default
   printf("[test] len: %d\n", len(a)) 

   global a, b;
   printf("[test] len: %d\n", len(a)) 
   # output >> [test] len: 3

   global c; # error: cannot bind unknown global `c`

   a += [4,5,6] # modify global a
}

printf("[global] len: %d\n", len(a)) 
# output >> [global] len 3

test()

printf("[global] len: %d\n", len(a)) 
# output >> [global] len 6
```


# block scope

variable explicitly defined via `local` or `let` keyword while inside a block scope belong to that scope.

```
local i = 1
if true {
   local i = 3  # local declared inside block scope
}
println($i) # expected: "1"
```

example - crossing the block scope

```
local i = 1
if true {
   a = 2
   i = 3  # assign to variable outside block scope
}
println($i) # expected: "3"
println($a) # expected: "2"
```

block scope extends to if (conditions), loops (for, while, etc) and switch body.

```
local j = 0
for i in range(0, 10) {
   local j = i
}

println($j) # expected "0"

local i = 0
while (i < 10) {
   ++i;
   local j = i;
}

println($j) # expected "0"

```


# generators

Generators are created by defining a function that uses `yield`. Each call to
`yield` pauses the generator and returns the yielded value. A subsequent call
advances to the next `yield`. When the generator is exhausted, `yield` returns
`nil` (the eof sentinel).

## PROTOTYPE 1


```python
def gen_range(mi, mx) {
   for i = mi to mx {
      yield i
   }
}

local gen = gen_range(1, 3)
local v = gen()        # yields 1
while v != nil {
   println(v)
   v = gen()          # yields 2, then 3, then nil
}
# output:
# 1
# 2
# 3
```


The generator value is a function-like callable that captures its local state.
Each invocation runs the generator body from its last yield point until the next
`yield` or until the body returns/exhausts.

## Syntax

```
yield expr      ; yield a value
yield           ; yield nil (empty yield, equivalent to `return` from generator)
```

`yield` is a reserved word; it cannot be used as an identifier.

## EOF sentinel

When a generator is exhausted (body returns or falls off the end), the next call
returns `nil`. This is the EOF sentinel — distinguishable from a yielded `nil`
because a yielded `nil` would have been produced by an explicit `yield` statement
inside the generator body, while the EOF `nil` is produced by the VM's exhaustion
check.

# modules (internal)

## import statement

```python
import math                   # import module into the namespace
                              # use math.min and matn.max

import math as m              # m.min, m.max

from math import *            # import all public names from the math 
from math import min, max     # import `min` and `max` 

```

Modules are registered from C++ via `ctx.registerModule(name, module)`. The
`import` statement is resolved at compile time: each function in the module
is registered as an external (native) function in the current scope. Qualified
access (`math.min(...)`) is a future extension; v1 uses flat import only.

Module can be written in C++ or Qlang. System modules (C++) are part of the
qlang library.

qlang context (ctx) parses the source. it creates the `main` module.
when a module is registered, it will provide it's own name.
all errors messages will reference the module name.

## math module

```cpp
ctx.registerModule("math", {
  {"min", [](QLangContext&, const std::vector<QLangValue>& args) -> QLangValue { ... }},
  {"max", ...},
  {"sum", ...}
});
```

|functions|-|-|
|--------|-|-|
|min(number...\|array)| returns the smallest value in a group number (int, float, byte etc). |
|max(number...\|array)| returns the largest value in the group of numbers (int, float, byte etc). |
|sum(number...\|array)| returns a sum of the group of numbers (int, float, byte etc). |
|rand()|return a random int value|

```
import math
c = min(4, 1)  # c = 1
c = min(2, 0, 4, 3)  # c = 0
c = min([2.5,4,1]) # c = 1
c = min([3,5,6,1], [2,5,1,2]) # errror: only 1 array allowed
#import min and max accept: numbers (int, char, float) or an array ONLY
#min and max refuse data if types are mixed (int and float)
#min and max refuse data if types have mixed signed and unsigned types (char and byte)
#max used the same rules as min

d = sum(1,10,20) # d = 31
d = sum([1,4,20]) # d = 25
d = sum([1,4,20], [50,2]) # d = 77
# sum accepts: numbers (int, char, float etc) or arrays ONLY
# sum return float if a type float is present. coerce non-float to float
# sum return int   if data is mixed signed and unsigned types (char and byte) and no float are present
```

# user defined classes

Classes give qlang its object model. A class instance, a table, and an
external (C++) type all go through the **same object dispatch** — field
get, field set, method call, string conversion. A script cannot tell them
apart syntactically. (Dispatch design: `docs/EXTERNAL_TYPES.md` §9.)

```python
class Money {
   static banner = "GreatAccount";
   total = 0

   # constructor
   __init__(self, amount)[] {
      self.total = amount
   }

   # destructor
   __destroy__(self)[] {
   }

   # string representation
   __str__(self)[] {
      return f"balance: {self.total} at {banner}"
   }

   receive(self, amount)[] {
      self.total += amount
   }

   spend(self, amount)[] {
      self.total -= amount
   }

   balance(self)[] {
      return self.total
   }

   static getBanner()[] {
      return banner;
   }
   static setBanner(newBanner)[] {
      banner = newBanner;
   }
}

let money = Money(125)
money.receive(25)
println(f"balance: {money.balance()}")   # balance: 150
money.spend(15)
println(f"balance: {money.balance()}")   # balance: 135

println(money) # expected: output from _str__()
println(f"{money}") # expected: output from _str__()

Money::setBanner("OrangePumpkin")
println(Money::getBanner()) # expected: "OrangePumpkin"
println(f"{Money::banner}") # expected: "OrangePumpkin"

printf(f"banner: {money.banner}") #expected: "OrangePumpkin"
money.banner = "Turtle"
printf(f"banner: {money.banner}") #expected: "Turtle"
printf(f"banner: {Money::banner}") #expected: "Turtle"


```

## static

can only exists inside a class.

`static variables` are bound to the entire class 
they can be read and modified by functions in the class.

`static functions` can be addressed via the class name.

static can combined with const.
static cannot be combined with local or let.

`static` (and the `::` scope resolution operator) is a **new extension** beyond v1 —
v1 classes have no static members (see `## v1 restrictions`).

## class body

- a class is declared in **global scope only** (same rule as `def`).
- the body contains **field declarations** (`name = value`) and **methods**
  (`name(self, args)[] { }`). `self` is always the first method parameter.
- field declarations are **defaults**: every instance starts with a copy of
  the declared fields.
- a field declared `const` is read-only for the life of the instance;
  `x.f = 1` on a const field is a runtime (Type) error naming the field.
  This matches external types, where a field binding without a setter is
  read-only.
- method names are **unique** within a class; duplicate method or dunder
  names are compile-time errors. **No overloading in v1.**
- dunders, at most one each, all optional:
  - `__init__(self, ...)` — constructor
  - `__destroy__(self)` — finalization, runs exactly once
  - `__str__(self)` — string representation

## construction

`let x = ClassName(args)` creates an instance:

1. the instance is created holding the declared field defaults.
2. if `__init__` exists, it is called with `self` + args.
3. a class without `__init__` is constructed with **zero args only**;
   passing args to it is a compile-time error at the call site.
4. wrong arg count for an existing `__init__` is a compile-time error
   (methods have fixed arity — the compiler knows it for source classes,
   and the binding table knows it for external types).

## field access

- `obj.field` reads the instance field. An undeclared field name is a
  runtime error naming the field and the class.
- `obj.field = v` writes it. The field must be **declared**: classes have a
  **closed** field set — unlike tables, there is no open key insertion.
- fields hold qlang values of any type, including other instances and
  external objects.

## method calls

- `obj.method(args)` binds `self` to the instance implicitly.
- an unknown method is a runtime error naming the method and the class.
- methods may call other methods of the same instance via `self`.

## string conversion

- `str(obj)` and f-string interpolation of a whole object (`f"{obj}"`) call
  `__str__` when present.
- a class without `__str__` converts to its class name (`"Money"`).
  (Conformance note: an external type without a `__str__` binding is a Type
  error — it has no sensible default. See the conformance table.)
- f-string field/method interpolation (`f"{money.balance()}"`) goes through
  the same object dispatch.

## lifetime and ownership

- instances are **reference counted**. Assigning, passing, and returning
  share the instance — never a copy.
- `__destroy__` runs exactly once, when the count reaches zero.
- class instances are always **script-created and script-owned** — the
  engine never hands one in, so reference counting is the complete model.
  External (C++) objects follow the engine-authoritative rule instead: the
  engine owns, and the script holds a weak reference to handed-in objects
  (`docs/EXTERNAL_TYPES.md` §3).

## type identity

- `type(x)` returns the class name string for an instance
  (`type(money) == "Money"`). For external objects it returns the
  registered type name.

## v1 restrictions

- no inheritance (no base clause; the dispatch reserves a `base` hook for a
  later version)
- no method overloading, no operator overloading
- no static / class methods
- instances are not tables: the `$` dynamic-key pipeline does not apply to
  them; field access is by declared name only

## conformance with external (C++) types

A registered C++ type (`docs/EXTERNAL_TYPES.md`) implements this exact
model. The mapping:

| class concept        | external type binding              |
|----------------------|------------------------------------|
| class name           | `TypeDescriptor.name`              |
| `ClassName(args)`    | `TypeDescriptor.construct`         |
| `__init__`           | `TypeDescriptor.construct`         |
| field declaration    | `FieldBinding` (get + set)         |
| `const` field        | `FieldBinding` with null setter    |
| method `m(self, ...)`| `MethodBinding`                    |
| `__str__`            | `TypeDescriptor.str` (optional)    |
| `__destroy__`        | `TypeDescriptor.destroy` (optional)|
| reference count      | engine-authoritative: `weak_ptr` for handoffs, `shared_ptr` for script-constructed |
| `type(x)`            | `type_info` name                   |

Both kinds of object go through the same four dispatch operations
(GETFIELD / SETFIELD / CALLMETHOD / TOSTRING), so code written against the
class model works unchanged on external types.

# implicit \_\_str\_\_ rules

|type|example|behavior|
|--|--|--|
|numbers|| convert to string
|strings|| remain unchanged
|boolean| |"true" or "false"
|nil|| "nil"
|array|[0,1,2,3,4]| "[0, 1, 2, 3, 4]"
|table|{name="Bob", age=9}| "{name="Bob", age=9}"
|class||use provided \_\_str\_\_() method

table key order is implementation defined (depends on the underlying container).

# warning 

compiler and interpreter warning can be togged off.

all warnings defered until further notice.

# error reporting

Clear and concise error reporting is important to track down bugs.

## requirements

- must report line and column where the error occured (when available)
- must report the module
- must report the class/function/table if applicable

## execution trace

provide the stack frame and execution chain when available.
each node to provide line number, module/class/function/table if applicable.