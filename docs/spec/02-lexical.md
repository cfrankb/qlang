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

