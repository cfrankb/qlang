overview
==========

create the skeleton for a scripting language that interfaces with C++
this language will compile to bytecode.

new class QLangContext will host the interpreter context. 

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

local aa:array = [1,2,3]    # declare array
local ab = [0,4,5]          # declare array
local t:table = {name="Frank", age=99, year=2026, char:byte=' '}

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


for x = 1 to 32 
next x

for x =1 to a
next x

if a == 32 then
end

if a < 32 then
end

if  a >= 32 then
end

if (a == 32) || (b==32) then
end

while (a > 0)
 --a
end

operators
=  assign
== equal
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


helpers
=========

toStr(var)       # convert to string
toInt(var)       # convert to int
type(var)        # return a string representation of the type
print(var)       # print a string
println(var)     # print a string with carriage return
printf(str, ...) # mirror C printf

delete var      # delete variable
