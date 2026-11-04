# strict typing

the enforcement is at the QLangContext level. C++ api to be provided


## enforcement levels

|enforcement level||
|-|-|
|0|no type checking
|1|check type if declared|
|2|enforce type checking and throw an error if type is not declared

nil type is implicit and doesn't need to be declared

when strict typing is on, type declarations are strictly enforced.

type declarations are optional. no enforcement.

## example of regular functions
```
# assuming strict typing is level 1

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
## lambda examples

```
let hello_word = (name:str, age:int)[]->str{
    return f"hello {name}. you are {age} years old"
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
