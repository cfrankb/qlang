# strict typing

will be optional and can be togged on or off at the QLangContext level.

```
# assuming strict typing is enabled

def hello_world(name:str, age:int) {
}

# exception: age is not an int
hello_world("Tommy", "24")

# exception: name is not a string
hello_world(112, 24)

# exception: storage for age is too large for int
let age:long = 24
hello_world("Tommy", age)

# exception: age is a byte not an int
age:byte = 24
hello_world("Tommy", age)

def sum_int2int(a:int, b:int) -> int {
    a += b
    # exception: function must return an int
}

```

## rules

|types|rules|
|--|--|
|string| only allow a string
|long, int, short, char|coerce only allow signed integer types (see left column) of smaller storage size. implicit conversion
|qword, dword, word, byte|coerce only allow unsigned integer types (see left column) of smaller storage size. implicit conversion
|float|only allow floats|
|array|only allow arrays|
|set|only allow set|
|table|only allow table|
|class|objects of that give class
