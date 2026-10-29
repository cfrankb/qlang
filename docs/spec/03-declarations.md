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
