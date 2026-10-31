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

# function and local scope

the variable, lambda, constants and other entity that reside within a function are considered to be within the `function scope`. the reach and life cycle of those entities also considered `local` since they expire upon closure.

```

a = 10

def make_test() {
    a ++
}

# expected: a = 10

b = 20

if (b == 20) {
    local a = 20
}

# expected: a = 10

```