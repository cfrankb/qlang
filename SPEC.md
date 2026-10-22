overview
==========

scripting language that interfaces with C++
this language will compile to bytecode.

new class QLangContext will host the interpreter context. 

default variable scope is local

this first version will parse text within a context.

SPEC_COMPLIANCE.md should track the hash of SPEC.md

variable types
===============
```
string/str (zero terminated)
blob  (binary data)
char (int8_t)
byte (uint8_t)
word (uint16_t)
short (int16_t)
int (int32_t)
unsigned (uint32_t)
dword (uint64_t)
array (contain list of mixed variable)
float
table
bool (true, false)
```

reserved words
===============
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
range => keyword
slice => build in
goto
char
dword
switch
case
default
type
join
```

constants
=========

```
const # declare a constant

const year = 2026;
const month:byte = 10;
```

comments 
=======================

```
# comment
```

boolean evaluation
=======================

|type||
|----|-|
|boolean| true or false|
|string| empty => false; non-empty => true
|array| empty => false; non-empty => true
|table| empty => false; non-empty => true
|number| 0 => false; non-zero => true



declarations
=======================

```
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

local z # this nil 
local f = 1.13 # float

c = a  # copy a to c
a += 12 # add to variable
s += "abc" # append to a string

sv = 123
sv += "test" # error

sx = "test"
sx += 123 # sx = test123

sz = year # valid. constants can be copied to vars
year = sx # error; constants cannot be assigned to

# anonymous functions inside a table
local events:table = {
  onLoad = (name)[] {
    printf("""onLOaD -->%s\n""");
  }, 
  onDeath = ()[] {
  }
}

```

conditional
=======================

```

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

```

switch
=======
```
switch (var) {
   case "string2":
   case "string1":
      break;
   case 123:
      break;
   case in [1,2,3]:
   case in range(10,20):   
   default:
}
```

loops
=======================================

```

for x = 1 to 32
next x

for x in range (0,4)
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

# loops i from 1 to 10
for i in range(1,10) {

}
```

label/goto
================

```

a = 10

:label1
--a

if a > 1 goto label1

```

label and goto must reside in the same scope.
if they cross a boundary, trigger an error.

operators
==========

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
| \|\| |logical or
|() |parenthesis
|-- ++ |decremental/incremental
|%  |modulo
|/ |division
|* |multiplication
|** |exponential
|! |not (inverse)
|;  |silent separator (optional)

operator +=

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



build-in functions
===================

|helpers||
|--------|-|
|toStr(var)     |  convert to string|
|toInt(var)     |  convert to int|
|type(var)      |   return a string representation of the type|
|print(var)       | print a string|
|println(var)     | print a string with carriage return
|printf(fmt, ...) | mirror C printf
|sprintf(fmt, ...) |same format as sprintf. return new string
|len(var)         | return size of string, table, blob or array |length; other type return 1; error on undefined/nil.
|substr(str, start, lenght) | return a substring fron index start. lenght is optional
|delete var      | delete variable
|str(var)        | return string representation of variable
|int(var)        | return variable cast to int; string are parsed as decimal. other types return 0
|slice(array, start, length) | return an array slice
|isset(var)| return true if variable, array index or table field exists. otherwise returns false
|join(separator, arr)| join the array element using separator

```
a = [1,2,3]
println(join(", ", a))
# output: 1, 2, 3

a = [1,2,3,4,5]
b = slice(a, 0, 2)
# b = [1,2]


```


functions (qlang native)
==================

```

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


C++ interface
========

Injected host variables are available to QLang code via the
`QLangContext` API. Two injection modes exist:

```
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
qval_dword(uint64_t)
qval_float(double)
qval_str(const char*) / qval_str(const std::string&)
qval_blob(const std::vector<uint8_t>&)
qval_array(ArrayValue)
qval_table(TableValue)
```

Const-injected variables seed the VM register file at start (readable from
QLang) but are excluded from the end-of-run scope sync, so QLang code cannot
mutate them. The compiler guards against assignment with a parse-time error.

TODO: inject C++ function (native qlang) into the context.

global scope visibility
======================

global const and functions are visible everywhere
