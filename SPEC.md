overview
==========

create the skeleton for a scripting language that interfaces with C++
this language will compile to bytecode.

new class QLangContext will host the interpreter context. 

default variable scope is local

this first version will parse text within a context.

variable types
===============
string (zero terminated)
blob  (binary data)
byte (uint8_t)
word (uint16_t)
short  (int16_t)
int (int32_t)
unsigned (uint32_t)
array (contain list of mixed variable)
float
table
bool (true, false)

reserved words
===============
for
to
in
then
while
continue
break
return
string
str
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


constants
=========

const # declare a constant

const year = 2026;
const month:byte = 10;


comments 
=======================

\# comment

declaration examples
=======================

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

conditional
=======================

if a == 32 then
end

if a < 32 then
end

if  a >= 32 then
end

if (a == 32) || (b==32) then
end

if (a == 32) || (b==32) {

}

a = [1,4,6]
if (b in a) {

}

if (b in [5,7,4]) {

}

loops
=======================================

while (a > 0)
 --a
end


for x = 1 to 32 
next x

for x =1 to a
next x

for x =1 to 32 {
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


operators
===============================
=  assign
== equal
+= add or append
!= not equal
<  > <= >= comparison
<< >> bit shift
^ binary xor
& binary and
| binary or
&& logical and
|| logical or
() parenthesis
-- ++ decremental/incremental
%  modulo
! not (inverse)
;  silent separator (optional)


helpers
=========

toStr(var)       # convert to string
toInt(var)       # convert to int
type(var)        # return a string representation of the type
print(var)       # print a string
println(var)     # print a string with carriage return
printf(str, ...) # mirror C printf
len(var)         # return string, table, blob or array length; other type return 1; error on undefined/nil.
substr(str, start, lenght) # return a substring fron index start. lenght is optional
delete var      # delete variable


functions
==========

examples

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

local func1 = (name, year) {
   println("hello world")
   printf("hi %s it is %d\n", name, year)
   return true
}

func1("Frank", 2026)