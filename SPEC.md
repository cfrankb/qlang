# instructions

scripting language that interfaces with C++
this language will compile to bytecode.

QLang uses a python-like syntax.

new class QLangContext will host the interpreter context. 

default variable scope is local

this first version will parse text within a context.

`SPEC_COMPLIANCE.md` should track the hash of SPEC.md
always update `SPEC_COMPLIANCE.md`.

maintain separate targets in the CMakeLists.
- linkage library src/*.cpp
- demo application (e,g, examples/hello.cpp)
- test sets for all the features listed below. 

suggest commit name but don't commit automatically.

everything is case sensitive.

always ask for permission before touching this file.

# language

## variable types
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
range => keyword
goto
char
dword
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
```

## constants

```python
const # declare a constant

const year = 2026;
const month:byte = 10;
```

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
|function, lambda|depends on return value and value type



## declarations and assignments


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

local z # this nil 
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

```

## range

range keyword has different encoding based on context


```
if i in range (0,10) {
   ...
}
# encode: if i >=0  and i <= 10 { ... }

for i in range (0,10) {
   ...
}

# encode: i = 0; while (i <= 10) { ... }

arr = range(0, 10)

# encode: arr = [0,1,2,3,4,5,6,7,8,9,10]

for i in range (0, 10) {
   yield i
}

# encode: i = 0; while (i <= 10) { yield i }


```

## switch
```python
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

## loops
```python
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
| \|\| |logical or
|() |parenthesis
|-- ++ |decremental/incremental
|%  |modulo
|/ |division
|* |multiplication
|** |exponential
|! |not (inverse)
|;  |silent separator (optional)
|cast|change datatype at runtime

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

operator cast
|examples|results|notes|
|-----|-----|-|
|cast\<int>(1.12)| 1| casting to 1. trunkate decimals
|cast\<float>(1)|1.0f| make the iht into a floating point
|cast\<string>(13)|"13"| turns int into string
|cast\<string>(1.3)|"1.3"| turns float into string
|cast\<word>(-1)|65535| converting int to word removes signage
|cast\<string>([])|"\<string>"| collections fallback on their string representations
|cast\<int>([])|n/a| error: collections cannot be cast to int
|cast\<array>(123)|n/a| error: numbers cannot be cast to collections
|cast\<any>()|n/a| error: empty source. nothing to cast
|cast\<>(123)|n/a| error: empty cast. nothing to do

operator == and !=
|var type|operator|var type|result
|-|-|-|-|
|nil| ==| nil | true
|nil| ==| number (int, float etc) | false
|nil| ==| collection (array, table, class etc) | false
|nil| !=| nil | false
|nil| !=| number | true
|nil| !=| collection | true
|nil| !=| number | true

*important*: nil is ONLY equal to iself

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
|len(var)         | return size of string, table, blob or array; other type return 1; error on undefined/nil.
|substr(str, start, lenght)| return a substring fron index start. lenght is optional
|delete var      | delete variable
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


functions (qlang native)
==================

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

##  verbose level

**Priority one**:
application wide verbose level that can adjusted with one function call. 



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

   a += [4,5,6] # modify global a
}

printf("[global] len: %d\n", len(a)) 
# output >> [global] len 3

test()

printf("[global] len: %d\n", len(a)) 
# output >> [global] len 6
```


# strings

`QLang` to implement `Python f-strings` style

```python
price = 19.99
print(f"Total: ${price:.2f}")  # Output: Total: $19.99
```

# generators

Generators are created by defining a function that uses `yield`. Each call to
`yield` pauses the generator and returns the yielded value. A subsequent call
advances to the next `yield`. When the generator is exhausted, `yield` returns
`nil` (the eof sentinel).

## PROTOTYPE 1


```

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

i would favor a variation on prototype 2.

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
|min(number...|array)| returns the smallest value in a group number (int, float, byte etc). |
|max(number...|array)| returns the largest value in the group of numbers (int, float, byte etc). |
|sum(number...|array)| returns a sum of the group of numbers (int, float, byte etc). |


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
# sum accepts: numbers (int, char, float) or arrays ONLY
# sum return float if a type float is present
# sum return int   if data is mixed signed and unsigned types (char and byte) and no float are present
```

## string module

|functions|-|-|
|--------|-|-|
|tba||||

# user defined classes

TODO: propose a class definition based on the prototype herein


```python
class Money {
   total = 0

   # constructor
   __init__ (self, amount)[] {
      self.total = amount
   }

   # destructor
   __destroy__(self)[] {
   }

   # string representation
   __str__(self)[] {
      return f"balance: {self.total}"
   }

   receive(self, amount)[] {
      self.total += amount
   }

   spend(self, amount)[] {
      self.total += amount
   }

   balance(self)[] {
      return self.total
   }
}

let money = Money(125)
money.receive(25)
println(f"balance: {money.balance()}")
money.spend(15)
println(f"balance: {money.balance()}")
```
